/*
 ******************************************************************************
  Copyright (c) 2015 Particle Industries, Inc.  All rights reserved.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation, either
  version 3 of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this program; if not, see <http://www.gnu.org/licenses/>.
  ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "application.h"

#define SCAN_RESULT_COUNT       5
#define BLE_ADV_DATA_MAX        31

SYSTEM_MODE(MANUAL);

SerialLogHandler log(115200, LOG_LEVEL_ALL);

BleScanResult results[SCAN_RESULT_COUNT];


void setup() {

}

void loop() {
    int count = BLE.scan(results, SCAN_RESULT_COUNT);

    if (count > 0) {
        uint8_t buf[BleAdvertisingData::MAX_LEN];
        size_t len;

        Log.trace("%d devices are found:", count);
        for (int i = 0; i < count; i++) {
            Log.trace("devices %d: %d - %02X:%02X:%02X:%02X:%02X:%02X", i, results[i].rssi,
                    results[i].address[0], results[i].address[1], results[i].address[2],
                    results[i].address[3], results[i].address[4], results[i].address[5]);

            len = results[i].advertisingData(buf, sizeof(buf));
            if (len > 0) {
                Log.trace("Advertising data:");
                for (size_t j = 0; j < len; j++) {
                    Log.printf("0x%02x, ", buf[j]);
                }
                Log.print("\r\n");
            }

            len = results[i].scanResponse(buf, sizeof(buf));
            if (len > 0) {
                Log.trace("Scan response data:");
                for (size_t j = 0; j < len; j++) {
                    Log.printf("0x%02x, ", buf[j]);
                }
                Log.print("\r\n");
            }
        }
    }

    delay(3000);
}
