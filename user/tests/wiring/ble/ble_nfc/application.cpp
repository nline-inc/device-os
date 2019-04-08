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

#define UART_TX_BUF_SIZE        20

SYSTEM_MODE(MANUAL);

Serial1LogHandler log(115200, LOG_LEVEL_ALL);

void onDataReceived(const uint8_t* data, size_t len);

const char* serviceUuid = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
const char* rxUuid = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
const char* txUuid = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

BleCharacteristic txCharacteristic("tx", PROPERTY::NOTIFY, txUuid, serviceUuid);
BleCharacteristic rxCharacteristic("rx", PROPERTY::WRITE_WO_RSP, rxUuid, serviceUuid, onDataReceived);

uint8_t txBuf[UART_TX_BUF_SIZE];
size_t txLen = 0;

static void nfcEventCallback(nfc_event_type_t type, nfc_event_t* event, void* ctx) {
    switch (type) {
        case NFC_EVENT_FIELD_ON: {
            digitalWrite(D7, 1);
            break;
        }
        case NFC_EVENT_FIELD_OFF: {
            digitalWrite(D7, 0);
            digitalWrite(D0, 0);
            break;
        }
        case NFC_EVENT_READ: {
            digitalWrite(D0, 1);
            break;
        }
        default:
            break;
    }
}

void onDataReceived(const uint8_t* data, size_t len) {
    WITH_LOCK(Serial) {
        for (uint8_t i = 0; i < len; i++) {
            Serial.write(data[i]);
        }
    }

    char text[len + 1] = {0};
    memcpy(text, data, len);
    NFC.setText(text, "en");
    NFC.update();
}

void setup() {
    pinMode(D0, OUTPUT);
    digitalWrite(D0, 0);
    pinMode(D7, OUTPUT);
    digitalWrite(D7, 0);

    Serial.begin(115200);

    NFC.setText("Hello Particle!", "en");
    NFC.on(nfcEventCallback);

    BLE.addCharacteristic(txCharacteristic);
    BLE.addCharacteristic(rxCharacteristic);

    BleAdvertisingData advData;
    advData.appendServiceUUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
    BLE.advertise(&advData);
}

void loop() {
    if (BLE.connected()) {
        WITH_LOCK(Serial) {
            while (Serial.available() && txLen < UART_TX_BUF_SIZE) {
                txBuf[txLen++] = Serial.read();
            }
        }

        if (txLen > 0) {
            txCharacteristic.setValue(txBuf, txLen);
            txLen = 0;
        }
    }
}
