/*
 * Copyright (c) 2018 Particle Industries, Inc.  All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "mesh.h"

#if SYSTEM_CONTROL_ENABLED

#include "system_openthread.h"

#include "common.h"

#include "concurrent_hal.h"
#include "delay_hal.h"

#include "bytes2hexbuf.h"
#include "hex_to_bytes.h"
#include "random.h"
#include "preprocessor.h"
#include "logging.h"

#include "spark_wiring_vector.h"

#include "openthread/thread.h"
#include "openthread/thread_ftd.h"
#include "openthread/instance.h"
#include "openthread/commissioner.h"
#include "openthread/joiner.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/dataset.h"
#include "openthread/dataset_ftd.h"

#include "proto/mesh.pb.h"

#include "system_commands.h"
#include <mutex>
#include <cstdlib>

#define CHECK_THREAD(_expr) \
        do { \
            const auto ret = _expr; \
            if (ret != OT_ERROR_NONE) { \
                LOG(ERROR, #_expr " failed: %d", (int)ret); \
                return threadToSystemError(ret); \
            } \
        } while (false)

#define THREAD_LOCK(_name) \
        ThreadLock _name##Mutex; \
        std::unique_lock<ThreadLock> _name(_name##Mutex)

#define PB(_name) particle_ctrl_mesh_##_name

using spark::Vector;

namespace particle {

using namespace control::common;
using namespace system;

namespace ctrl {

namespace mesh {

namespace {

// Default IEEE 802.15.4 channel
const unsigned DEFAULT_CHANNEL = 11;

// Timeout is seconds after which the commissioner role is automatically stopped
const unsigned COMMISSIONER_TIMEOUT = 120;

// Timeout in seconds after which a joiner is automatically removed from the commissioner's list
const unsigned JOINER_TIMEOUT = 120;

// Minimum size of the joining device credential
const size_t JOINER_PASSWORD_MIN_SIZE = 6;

// Maximum size of the joining device credential
const size_t JOINER_PASSWORD_MAX_SIZE = 32;

// Time in milliseconds to spend scanning each channel during an active scan
const unsigned ACTIVE_SCAN_DURATION = 0; // Use Thread's default timeout

// Time in milliseconds to spend scanning each channel during an energy scan
const unsigned ENERGY_SCAN_DURATION = 200;

// Vendor data
const char* const VENDOR_NAME = "Particle";
const char* const VENDOR_MODEL = PP_STR(PLATFORM_NAME);
const char* const VENDOR_SW_VERSION = PP_STR(SYSTEM_VERSION_STRING);
const char* const VENDOR_DATA = "";

// Current joining device credential
char g_joinPwd[JOINER_PASSWORD_MAX_SIZE + 1] = {}; // +1 character for term. null

// Commissioner role timer
os_timer_t g_commTimer = nullptr;

class Random: public particle::Random {
public:
    void genBase32Thread(char* data, size_t size) {
        // base32-thread isn't really defined anywhere, but otbr-commissioner explicitly forbids using
        // I, O, Q and Z in the joiner passphrase
        static const char alpha[32] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'J', 'K', 'L', 'M', 'N', 'P', 'R', 'S',
                'T', 'U', 'V', 'W', 'X', 'Y', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9' };
        genAlpha(data, size, alpha, sizeof(alpha));
    }
};

void commissionerTimeout(os_timer_t timer);

void stopCommissionerTimer() {
    if (g_commTimer) {
        os_timer_destroy(g_commTimer, nullptr);
        g_commTimer = nullptr;
    }
}

int startCommissionerTimer() {
    if (!g_commTimer) {
        const int ret = os_timer_create(&g_commTimer, COMMISSIONER_TIMEOUT * 1000, commissionerTimeout, nullptr, true, nullptr);
        if (ret != 0) {
            return SYSTEM_ERROR_NO_MEMORY;
        }
    }
    const int ret = os_timer_change(g_commTimer, OS_TIMER_CHANGE_START, false, 0, 0xffffffff, nullptr);
    if (ret != 0) {
        stopCommissionerTimer();
        return SYSTEM_ERROR_UNKNOWN;
    }
    LOG_DEBUG(TRACE, "Commissioner timer started");
    return 0;
}

void commissionerTimeout(os_timer_t timer) {
    THREAD_LOCK(lock);
    LOG_DEBUG(TRACE, "Commissioner timeout");
    stopCommissionerTimer();
    const auto thread = threadInstance();
    if (otCommissionerGetState(thread) != OT_COMMISSIONER_STATE_DISABLED) {
        const auto ret = otCommissionerStop(thread);
        if (ret != OT_ERROR_NONE) {
            LOG(WARN, "otCommissionerStop() failed: %d", (int)ret);
        }
    }
}

int threadToSystemError(otError error) {
    switch (error) {
    case OT_ERROR_NONE:
        return SYSTEM_ERROR_NONE;
    case OT_ERROR_SECURITY:
        return SYSTEM_ERROR_NOT_ALLOWED;
    case OT_ERROR_NOT_FOUND:
        return SYSTEM_ERROR_NOT_FOUND;
    case OT_ERROR_RESPONSE_TIMEOUT:
        return SYSTEM_ERROR_TIMEOUT;
    case OT_ERROR_NO_BUFS:
        return SYSTEM_ERROR_NO_MEMORY;
    case OT_ERROR_BUSY:
        return SYSTEM_ERROR_BUSY;
    case OT_ERROR_ABORT:
        return SYSTEM_ERROR_ABORTED;
    case OT_ERROR_INVALID_STATE:
        return SYSTEM_ERROR_INVALID_STATE;
    default:
        return SYSTEM_ERROR_UNKNOWN;
    }
}

} // particle::ctrl::mesh::

int auth(ctrl_request* req) {
    THREAD_LOCK(lock);
    const auto thread = threadInstance();
    if (!thread) {
        return SYSTEM_ERROR_INVALID_STATE;
    }
    // Parse request
    PB(AuthRequest) pbReq = {};
    DecodedCString dPwd(&pbReq.password); // Commissioning credential
    int ret = decodeRequestMessage(req, PB(AuthRequest_fields), &pbReq);
    if (ret != 0) {
        return ret;
    }
    // Get network name, extended PAN ID and current PSKc
    const char* name = otThreadGetNetworkName(thread);
    const uint8_t* extPanId = otThreadGetExtendedPanId(thread);
    const uint8_t* curPskc = otThreadGetPSKc(thread);
    if (!name || !extPanId || !curPskc) {
        return SYSTEM_ERROR_INVALID_STATE;
    }
    // Generate PSKc for the provided commissioning credential
    uint8_t pskc[OT_PSKC_MAX_SIZE] = {};
    CHECK_THREAD(otCommissionerGeneratePSKc(thread, dPwd.data, name, extPanId, pskc));
    if (memcmp(pskc, curPskc, OT_PSKC_MAX_SIZE) != 0) {
        return SYSTEM_ERROR_NOT_ALLOWED;
    }
    return 0;
}

using namespace MeshCommand;
int notifyNetworkUpdated(int flags) {
    NotifyMeshNetworkUpdated cmd;
    NetworkInfo& ni = cmd.ni;
    // todo - consolidate with getNetworkInfo - decouple fetching the network info from the
    // control request decoding/result encoding.
    THREAD_LOCK(lock);
    const auto thread = threadInstance();
    if (!thread) {
        return SYSTEM_ERROR_INVALID_STATE;
    }
    if (flags & NetworkInfo::NAME_VALID) {
        // Network name
        const char* name = otThreadGetNetworkName(thread);
        if (!name) {
            LOG(ERROR, "Unable to retrieve thread network name");
            return SYSTEM_ERROR_UNKNOWN;
        }
        size_t length = strlen(name);
        strncpy(ni.name, name, MAX_NETWORK_NAME_LENGTH);
        ni.name_length = length;
    }
    if (flags & NetworkInfo::CHANNEL_VALID) {
    // Channel
        ni.channel = otLinkGetChannel(thread);
    }
    if (flags & NetworkInfo::PANID_VALID) {
        // PAN ID
        const otPanId panId = otLinkGetPanId(thread);
        ni.panid[0] = panId>>8;
        ni.panid[1] = panId&0xFF;
    }
    const uint8_t* extPanId = otThreadGetExtendedPanId(thread);
    if (!extPanId) {
        LOG(ERROR, "Unable to retrieve thread XPAN ID");
        return SYSTEM_ERROR_UNKNOWN;
    }
    memcpy(ni.update.id, extPanId, sizeof(ni.xpanid));
    if (flags & NetworkInfo::XPANID_VALID) {
        // Extended PAN ID
        memcpy(ni.xpanid, extPanId, sizeof(ni.xpanid));
    }
    if (flags & NetworkInfo::ON_MESH_PREFIX_VALID) {
        const uint8_t* prefix = otThreadGetMeshLocalPrefix(thread);
        if (!prefix) {
            LOG(ERROR, "Unable to retrieve thread network local prefix");
            return SYSTEM_ERROR_UNKNOWN;
        }
        memcpy(ni.on_mesh_prefix, prefix, 8);
    }

    ni.update.size = sizeof(ni);
    ni.flags = flags;
    int result = system_command_enqueue(cmd, sizeof(cmd));
    if (result) {
        LOG(ERROR, "Unable to add notification to system command queue %d", result);
    }
    return result;
}

int createNetwork(ctrl_request* req) {
    THREAD_LOCK(lock);
    const auto thread = threadInstance();
    if (!thread) {
        return SYSTEM_ERROR_INVALID_STATE;
    }
    // Parse request
    PB(CreateNetworkRequest) pbReq = {};
    DecodedCString dName(&pbReq.name); // Network name
    DecodedCString dPwd(&pbReq.password); // Commissioning credential
    int ret = decodeRequestMessage(req, PB(CreateNetworkRequest_fields), &pbReq);
    if (ret != 0) {
        return ret;
    }
    // Network name: up to 16 characters, UTF-8 encoded
    // Commissioning credential: 6 to 255 characters, UTF-8 encoded
    if (dName.size == 0 || dName.size >= OT_NETWORK_NAME_MAX_SIZE || dPwd.size < OT_COMMISSIONING_PASSPHRASE_MIN_SIZE ||
            dPwd.size > OT_COMMISSIONING_PASSPHRASE_MAX_SIZE) {
        return SYSTEM_ERROR_INVALID_ARGUMENT;
    }
    CHECK_THREAD(otThreadSetEnabled(thread, false));
    CHECK_THREAD(otIp6SetEnabled(thread, false));
    unsigned channel = pbReq.channel; // IEEE 802.15.4 channel
    if (channel == 0) {
        // Perform an energy scan
        struct EnergyScanResult {
            unsigned channel;
            int minRssi;
            volatile bool done;
        };
        EnergyScanResult enScan = {};
        // For now, excluding channels 25 and 26 which are not allowed for use in most countries which
        // have radio frequency regulations
        const unsigned channelMask = OT_CHANNEL_ALL & ~OT_CHANNEL_25_MASK & ~OT_CHANNEL_26_MASK;
        LOG_DEBUG(TRACE, "Performing energy scan");
        CHECK_THREAD(otLinkEnergyScan(thread, channelMask, ENERGY_SCAN_DURATION,
                [](otEnergyScanResult* result, void* data) {
            const auto scan = (EnergyScanResult*)data;
            if (!result) {
                LOG_DEBUG(TRACE, "Energy scan done");
                scan->done = true;
                return;
            }
            LOG_DEBUG(TRACE, "Channel: %u; RSSI: %d", (unsigned)result->mChannel, (int)result->mMaxRssi);
            if (result->mMaxRssi < scan->minRssi) {
                scan->minRssi = result->mMaxRssi;
                scan->channel = result->mChannel;
            }
        }, &enScan));
        // FIXME: Make this handler asynchronous
        lock.unlock();
        while (!enScan.done) {
            os_thread_yield();
        }
        lock.lock();
        channel = (enScan.channel != 0) ? enScan.channel : DEFAULT_CHANNEL; // Just in case
    }
    LOG(TRACE, "Using channel %u", channel);
    CHECK_THREAD(otLinkSetChannel(thread, channel));
    // Perform an IEEE 802.15.4 active scan
    struct ActiveScanResult {
        Vector<uint64_t> extPanIds;
        Vector<uint16_t> panIds;
        int result;
        volatile bool done;
    };
    ActiveScanResult actScan = {};
    if (!actScan.extPanIds.reserve(4) || !actScan.panIds.reserve(4)) {
        return SYSTEM_ERROR_NO_MEMORY;
    }
    LOG_DEBUG(TRACE, "Performing active scan");
    CHECK_THREAD(otLinkActiveScan(thread, (uint32_t)1 << channel, ACTIVE_SCAN_DURATION,
            [](otActiveScanResult* result, void* data) {
        const auto scan = (ActiveScanResult*)data;
        if (!result) {
            LOG_DEBUG(TRACE, "Active scan done");
            scan->done = true;
            return;
        }
        if (scan->result != 0) {
            return;
        }
        uint64_t extPanId = 0;
        static_assert(sizeof(result->mExtendedPanId) == sizeof(extPanId), "");
        memcpy(&extPanId, &result->mExtendedPanId, sizeof(extPanId));
        if (!scan->extPanIds.contains(extPanId)) {
            if (!scan->extPanIds.append(extPanId)) {
                scan->result = SYSTEM_ERROR_NO_MEMORY;
            }
        }
        const uint16_t panId = result->mPanId;
        if (!scan->panIds.contains(panId)) {
            if (!scan->panIds.append(panId)) {
                scan->result = SYSTEM_ERROR_NO_MEMORY;
            }
        }
#ifdef DEBUG_BUILD
        char extPanIdStr[sizeof(extPanId) * 2] = {};
        bytes2hexbuf_lower_case((const uint8_t*)&extPanId, sizeof(extPanId), extPanIdStr);
        LOG_DEBUG(TRACE, "Name: %s; PAN ID: 0x%04x; Extended PAN ID: 0x%s", result->mNetworkName, (unsigned)panId, extPanIdStr);
#endif
    }, &actScan));
    // FIXME: Make this handler asynchronous
    lock.unlock();
    while (!actScan.done) {
        os_thread_yield();
    }
    lock.lock();
    if (actScan.result != 0) {
        return actScan.result;
    }
    // Generate PAN ID
    Random rand;
    uint16_t panId = 0;
    do {
        panId = rand.gen<uint16_t>();
    } while (panId == 0xffff || actScan.panIds.contains(panId));
    CHECK_THREAD(otLinkSetPanId(thread, panId));
    // Generate extended PAN ID
    uint64_t extPanId = 0;
    do {
        extPanId = rand.gen<uint64_t>();
    } while (actScan.extPanIds.contains(extPanId));
    static_assert(sizeof(extPanId) == OT_EXT_PAN_ID_SIZE, "");
    CHECK_THREAD(otThreadSetExtendedPanId(thread, (const uint8_t*)&extPanId));
    // Set network name
    CHECK_THREAD(otThreadSetNetworkName(thread, dName.data));
    // Generate mesh-local prefix (see section 3 of RFC 4193)
    uint8_t prefix[OT_MESH_LOCAL_PREFIX_SIZE] = {
            0xfd, // Prefix, L
            0x00, 0x00, 0x00, 0x00, 0x00, // Global ID
            0x00, 0x00 }; // Subnet ID
    rand.gen((char*)prefix + 1, 5); // Generate global ID
    CHECK_THREAD(otThreadSetMeshLocalPrefix(thread, prefix));
    // Generate master key
    otMasterKey key = {};
    rand.genSecure((char*)&key, sizeof(key));
    CHECK_THREAD(otThreadSetMasterKey(thread, &key));
    // Set PSKc
    uint8_t pskc[OT_PSKC_MAX_SIZE] = {};
    CHECK_THREAD(otCommissionerGeneratePSKc(thread, dPwd.data, dName.data, (const uint8_t*)&extPanId, pskc));
    CHECK_THREAD(otThreadSetPSKc(thread, pskc));
    // Enable Thread
    CHECK_THREAD(otIp6SetEnabled(thread, true));
    CHECK_THREAD(otThreadSetEnabled(thread, true));
    int notifyResult = notifyNetworkUpdated(NetworkInfo::NETWORK_CREATED|NetworkInfo::PANID_VALID|NetworkInfo::XPANID_VALID|NetworkInfo::CHANNEL_VALID|NetworkInfo::ON_MESH_PREFIX_VALID|NetworkInfo::NAME_VALID);
    if (notifyResult<0) {
        LOG(ERROR, "Unable to notify network change %d", notifyResult);
    }

    // Encode a reply
    char extPanIdStr[sizeof(extPanId) * 2] = {};
    bytes2hexbuf_lower_case((const uint8_t*)&extPanId, sizeof(extPanId), extPanIdStr);
    PB(CreateNetworkReply) pbRep = {};
    EncodedString eName(&pbRep.network.name, dName.data, dName.size);
    EncodedString eExtPanId(&pbRep.network.ext_pan_id, extPanIdStr, sizeof(extPanIdStr));
    pbRep.network.pan_id = panId;
    pbRep.network.channel = channel;
    ret = encodeReplyMessage(req, PB(CreateNetworkReply_fields), &pbRep);
    if (ret != 0) {
        return ret;
    }
    return 0;
}

int startCommissioner(ctrl_request* req) {
    THREAD_LOCK(lock);
    const auto thread = threadInstance();
    if (!thread) {
        return SYSTEM_ERROR_INVALID_STATE;
    }
    CHECK_THREAD(otIp6SetEnabled(thread, true));
    CHECK_THREAD(otThreadSetEnabled(thread, true));
    // FIXME: Subscribe to OpenThread events instead of polling
    for (;;) {
        const auto role = otThreadGetDeviceRole(thread);
        if (role != OT_DEVICE_ROLE_DETACHED) {
            break;
        }
        lock.unlock();
        HAL_Delay_Milliseconds(500);
        lock.lock();
    }
    otCommissionerState state = otCommissionerGetState(thread);
    if (state == OT_COMMISSIONER_STATE_DISABLED) {
        CHECK_THREAD(otCommissionerStart(thread));
    }
    for (;;) {
        state = otCommissionerGetState(thread);
        if (state != OT_COMMISSIONER_STATE_PETITION) {
            break;
        }
        lock.unlock();
        HAL_Delay_Milliseconds(500);
        lock.lock();
    }
    if (state != OT_COMMISSIONER_STATE_ACTIVE) {
        return SYSTEM_ERROR_TIMEOUT;
    }
    startCommissionerTimer();
    return 0;
}

int stopCommissioner(ctrl_request* req) {
    THREAD_LOCK(lock);
    const auto thread = threadInstance();
    if (!thread) {
        return SYSTEM_ERROR_INVALID_STATE;
    }
    const auto state = otCommissionerGetState(thread);
    if (state != OT_COMMISSIONER_STATE_DISABLED) {
        CHECK_THREAD(otCommissionerStop(thread));
    }
    stopCommissionerTimer();
    return 0;
}

int prepareJoiner(ctrl_request* req) {
    THREAD_LOCK(lock);
    const auto thread = threadInstance();
    if (!thread) {
        return SYSTEM_ERROR_INVALID_STATE;
    }
    // Parse request
    PB(PrepareJoinerRequest) pbReq = {};
    int ret = decodeRequestMessage(req, PB(PrepareJoinerRequest_fields), &pbReq);
    if (ret != 0) {
        return ret;
    }
    // Disable Thread
    CHECK_THREAD(otThreadSetEnabled(thread, false));
    CHECK_THREAD(otIp6SetEnabled(thread, false));
    // Clear master key (invalidates active and pending datasets)
    otMasterKey key = {};
    CHECK_THREAD(otThreadSetMasterKey(thread, &key));
    // Erase persistent data
    CHECK_THREAD(otInstanceErasePersistentInfo(thread));
    // Set PAN ID
    // https://github.com/openthread/openthread/pull/613
    CHECK_THREAD(otLinkSetPanId(thread, pbReq.network.pan_id));
    // Get factory-assigned EUI-64
    otExtAddress eui64 = {}; // OT_EXT_ADDRESS_SIZE
    otLinkGetFactoryAssignedIeeeEui64(thread, &eui64);
    char eui64Str[sizeof(eui64) * 2] = {};
    bytes2hexbuf_lower_case((const uint8_t*)&eui64, sizeof(eui64), eui64Str);
    // Generate joining device credential
    Random rand;
    rand.genBase32Thread(g_joinPwd, JOINER_PASSWORD_MAX_SIZE);
    // Encode a reply
    PB(PrepareJoinerReply) pbRep = {};
    EncodedString eEuiStr(&pbRep.eui64, eui64Str, sizeof(eui64Str));
    EncodedString eJoinPwd(&pbRep.password, g_joinPwd, JOINER_PASSWORD_MAX_SIZE);
    ret = encodeReplyMessage(req, PB(PrepareJoinerReply_fields), &pbRep);
    if (ret != 0) {
        return ret;
    }
    return 0;
}

int addJoiner(ctrl_request* req) {
    THREAD_LOCK(lock);
    const auto thread = threadInstance();
    if (!thread) {
        return SYSTEM_ERROR_INVALID_STATE;
    }
    // Parse request
    PB(AddJoinerRequest) pbReq = {};
    DecodedCString dEui64Str(&pbReq.eui64);
    DecodedCString dJoinPwd(&pbReq.password);
    int ret = decodeRequestMessage(req, PB(AddJoinerRequest_fields), &pbReq);
    if (ret != 0) {
        return ret;
    }
    if (dEui64Str.size != sizeof(otExtAddress) * 2 || dJoinPwd.size < JOINER_PASSWORD_MIN_SIZE ||
            dJoinPwd.size > JOINER_PASSWORD_MAX_SIZE) {
        return SYSTEM_ERROR_INVALID_ARGUMENT;
    }
    // Add joiner
    otExtAddress eui64 = {};
    hexToBytes(dEui64Str.data, (char*)&eui64, sizeof(otExtAddress));
    CHECK_THREAD(otCommissionerAddJoiner(thread, &eui64, dJoinPwd.data, JOINER_TIMEOUT));
    return 0;
}

int removeJoiner(ctrl_request* req) {
    THREAD_LOCK(lock);
    const auto thread = threadInstance();
    if (!thread) {
        return SYSTEM_ERROR_INVALID_STATE;
    }
    // Parse request
    PB(RemoveJoinerRequest) pbReq = {};
    DecodedCString dEui64Str(&pbReq.eui64);
    int ret = decodeRequestMessage(req, PB(RemoveJoinerRequest_fields), &pbReq);
    if (ret != 0) {
        return ret;
    }
    if (dEui64Str.size != sizeof(otExtAddress) * 2) {
        return SYSTEM_ERROR_INVALID_ARGUMENT;
    }
    // Remove joiner
    otExtAddress eui64 = {};
    hexToBytes(dEui64Str.data, (char*)&eui64, sizeof(otExtAddress));
    CHECK_THREAD(otCommissionerRemoveJoiner(thread, &eui64));
    return 0;
}

int notifyJoined(bool joined) {
    THREAD_LOCK(lock);
    const auto thread = threadInstance();
    const uint8_t* extPanId = otThreadGetExtendedPanId(thread);
    if (!extPanId) {
        return SYSTEM_ERROR_UNKNOWN;
    }

    NotifyMeshNetworkJoined cmd;
    cmd.nu.size = sizeof(cmd.nu);
    memcpy(cmd.nu.id, extPanId, sizeof(cmd.nu.id));
    cmd.joined = joined;

    return system_command_enqueue(cmd, sizeof(cmd));
}

void joinNetwork(ctrl_request* req) {
    THREAD_LOCK(lock);
    const auto thread = threadInstance();
    if (!thread) {
        system_ctrl_set_result(req, SYSTEM_ERROR_INVALID_STATE, nullptr, nullptr, nullptr);
        return;
    }
    auto tRet = otIp6SetEnabled(thread, true);
    if (tRet != OT_ERROR_NONE) {
        LOG(ERROR, "otIp6SetEnabled() failed: %u", (unsigned)tRet);
        system_ctrl_set_result(req, threadToSystemError(tRet), nullptr, nullptr, nullptr);
        return;
    }
    otJoinerCallback cb = [](otError tRet, void* ctx) {
        const auto req = (ctrl_request*)ctx;
        if (tRet == OT_ERROR_NONE) {
            const auto thread = threadInstance();
            tRet = otThreadSetEnabled(thread, true);
            if (tRet != OT_ERROR_NONE) {
                LOG(ERROR, "otThreadSetEnabled() failed: %u", (unsigned)tRet);
            } else {
                notifyJoined(true);
            }
        } else {
            LOG(ERROR, "otJoinerStart() failed: %u", (unsigned)tRet);
        }
        memset(g_joinPwd, 0, sizeof(g_joinPwd));
        system_ctrl_set_result(req, threadToSystemError(tRet), nullptr, nullptr, nullptr);
    };
    tRet = otJoinerStart(thread, g_joinPwd, nullptr, VENDOR_NAME, VENDOR_MODEL, VENDOR_SW_VERSION,
            VENDOR_DATA, cb, req);
    if (tRet != OT_ERROR_NONE) {
        LOG(ERROR, "otJoinerStart() failed: %u", (unsigned)tRet);
        system_ctrl_set_result(req, threadToSystemError(tRet), nullptr, nullptr, nullptr);
    }
}

int leaveNetwork(ctrl_request* req) {
    THREAD_LOCK(lock);
    const auto thread = threadInstance();
    if (!thread) {
        return SYSTEM_ERROR_INVALID_STATE;
    }
    system_command_clear();
    notifyJoined(false);
    // Disable Thread protocol
    CHECK_THREAD(otThreadSetEnabled(thread, false));
    // Clear master key (invalidates active and pending datasets)
    otMasterKey key = {};
    CHECK_THREAD(otThreadSetMasterKey(thread, &key));
    // Erase persistent data
    CHECK_THREAD(otInstanceErasePersistentInfo(thread));
    return 0;
}

int getNetworkInfo(ctrl_request* req) {
    THREAD_LOCK(lock);
    const auto thread = threadInstance();
    if (!thread) {
        return SYSTEM_ERROR_INVALID_STATE;
    }
    if (!otDatasetIsCommissioned(thread)) {
        return SYSTEM_ERROR_NOT_FOUND;
    }
    // Network name
    const char* name = otThreadGetNetworkName(thread);
    if (!name) {
        return SYSTEM_ERROR_UNKNOWN;
    }
    // Channel
    const uint8_t channel = otLinkGetChannel(thread);
    // PAN ID
    const otPanId panId = otLinkGetPanId(thread);
    // Extended PAN ID
    const uint8_t* extPanId = otThreadGetExtendedPanId(thread);
    if (!extPanId) {
        return SYSTEM_ERROR_UNKNOWN;
    }
    char extPanIdStr[OT_EXT_PAN_ID_SIZE * 2] = {};
    bytes2hexbuf_lower_case(extPanId, OT_EXT_PAN_ID_SIZE, extPanIdStr);
    // Encode a reply
    PB(GetNetworkInfoReply) pbRep = {};
    EncodedString eName(&pbRep.network.name, name, strlen(name));
    EncodedString eExtPanIdStr(&pbRep.network.ext_pan_id, extPanIdStr, sizeof(extPanIdStr));
    pbRep.network.channel = channel;
    pbRep.network.pan_id = panId;
    const int ret = encodeReplyMessage(req, PB(GetNetworkInfoReply_fields), &pbRep);
    if (ret != 0) {
        return ret;
    }
    return 0;
}

int scanNetworks(ctrl_request* req) {
    THREAD_LOCK(lock);
    const auto thread = threadInstance();
    if (!thread) {
        return SYSTEM_ERROR_INVALID_STATE;
    }
    struct Network {
        char name[OT_NETWORK_NAME_MAX_SIZE + 1]; // Network name (null-terminated)
        char extPanId[OT_EXT_PAN_ID_SIZE * 2]; // Extended PAN ID in hex
        uint16_t panId; // PAN ID
        uint8_t channel; // Channel number
    };
    struct ScanResult {
        Vector<Network> networks;
        int result;
        volatile bool done;
    };
    ScanResult scan = {};
    CHECK_THREAD(otLinkActiveScan(thread, OT_CHANNEL_ALL, ACTIVE_SCAN_DURATION,
            [](otActiveScanResult* result, void* data) {
        const auto scan = (ScanResult*)data;
        if (!result) {
            scan->done = true;
            return;
        }
        if (scan->result != 0) {
            return;
        }
        Network network = {};
        // Network name
        static_assert(sizeof(result->mNetworkName) <= sizeof(Network::name), "");
        memcpy(&network.name, &result->mNetworkName, sizeof(result->mNetworkName));
        // Extended PAN ID
        static_assert(sizeof(result->mExtendedPanId) * 2 == sizeof(Network::extPanId), "");
        bytes2hexbuf_lower_case((const uint8_t*)&result->mExtendedPanId, sizeof(result->mExtendedPanId), network.extPanId);
        // PAN ID
        network.panId = result->mPanId;
        // Channel number
        network.channel = result->mChannel;
        if (!scan->networks.append(std::move(network))) {
            scan->result = SYSTEM_ERROR_NO_MEMORY;
        }
    }, &scan));
    // FIXME: Make this handler asynchronous
    lock.unlock();
    while (!scan.done) {
        os_thread_yield();
    }
    lock.lock();
    if (scan.result != 0) {
        return scan.result;
    }
    // Encode a reply
    PB(ScanNetworksReply) pbRep = {};
    pbRep.networks.arg = &scan;
    pbRep.networks.funcs.encode = [](pb_ostream_t* strm, const pb_field_t* field, void* const* arg) {
        const auto scan = (const ScanResult*)*arg;
        for (int i = 0; i < scan->networks.size(); ++i) {
            const Network& network = scan->networks.at(i);
            PB(NetworkInfo) pbNetwork = {};
            EncodedString eName(&pbNetwork.name, network.name, strlen(network.name));
            EncodedString eExtPanId(&pbNetwork.ext_pan_id, network.extPanId, sizeof(network.extPanId));
            pbNetwork.pan_id = network.panId;
            pbNetwork.channel = network.channel;
            if (!pb_encode_tag_for_field(strm, field)) {
                return false;
            }
            if (!pb_encode_submessage(strm, PB(NetworkInfo_fields), &pbNetwork)) {
                return false;
            }
        }
        return true;
    };
    const int ret = encodeReplyMessage(req, PB(ScanNetworksReply_fields), &pbRep);
    if (ret != 0) {
        return ret;
    }
    return 0;
}

int test(ctrl_request* req) {
    const int ret = system_ctrl_alloc_reply_data(req, req->request_size, nullptr);
    if (ret != 0) {
        return ret;
    }
    memcpy(req->reply_data, req->request_data, req->request_size);
    return 0;
}

} // particle::ctrl::mesh

} // particle::ctrl

} // particle

#endif // SYSTEM_CONTROL_ENABLED
