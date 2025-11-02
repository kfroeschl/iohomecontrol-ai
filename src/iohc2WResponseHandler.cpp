/*
   Copyright (c) 2024. CRIDP https://github.com/cridp

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

           http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */

#include "iohc2WResponseHandler.h"
#include "iohcDevice2W.h"
#include "iohcCryptoHelpers.h"
#include "crypto2Wutils.h"
#include "Aes.h"
#include <Arduino.h>
#include "user_config.h"

IOHC2WResponseHandler* IOHC2WResponseHandler::_instance = nullptr;

IOHC2WResponseHandler::IOHC2WResponseHandler() : _radioInstance(nullptr) {
}

IOHC2WResponseHandler* IOHC2WResponseHandler::getInstance() {
    if (!_instance) {
        _instance = new IOHC2WResponseHandler();
    }
    return _instance;
}

void IOHC2WResponseHandler::setRadioInstance(IOHC::iohcRadio* radio) {
    _radioInstance = radio;
}

bool IOHC2WResponseHandler::handleChallenge(IOHC::iohcPacket* iohc) {
    auto* devMgr = Device2WManager::getInstance();
    Device2W* device = devMgr->getDevice(iohc->payload.packet.header.source);
    
    if (!device || device->pairingState != PairingState::PAIRED) {
        return false; // Not a paired device
    }
    
    // Store the challenge
    if (iohc->payload.buffer[8] >= 6) {  // Check payload length
        devMgr->storeChallenge(device->nodeAddress, &iohc->payload.buffer[9], 6);
        Serial.printf("� Received challenge from device %s: %02X%02X%02X%02X%02X%02X\n",
                     device->addressStr.c_str(),
                     device->lastChallenge[0], device->lastChallenge[1], 
                     device->lastChallenge[2], device->lastChallenge[3],
                     device->lastChallenge[4], device->lastChallenge[5]);
        
        // Automatically send authentication response
        if (!_radioInstance) {
            Serial.println("ERROR: No radio instance for authentication");
            return true;
        }
        
        if (device->lastCommandLen > 0) {
            Serial.println("🔐 Sending automatic authentication response...");
            
            // Debug: Show system key
            Serial.print("[Auth] System Key: ");
            for (int i = 0; i < 16; i++) {
                Serial.printf("%02X", device->systemKey[i]);
            }
            Serial.println();
            
            // Build frame data for CMD 0x3D authentication
            // Frame data should be JUST the CMD 0x3D byte (not the original command!)
            std::vector<uint8_t> frame_data;
            frame_data.push_back(0x3D);  // CMD 0x3D response byte
            
            // Debug: Show frame data
            Serial.print("[Auth] Frame Data: ");
            for (uint8_t byte : frame_data) {
                Serial.printf("%02X", byte);
            }
            Serial.println();
            
            // Calculate MAC
            uint8_t mac[6];
            iohcCrypto::create_2W_hmac(mac, device->lastChallenge, device->systemKey, frame_data);
            
            // Create and send CMD 0x3D packet
            IOHC::iohcPacket* packet = new IOHC::iohcPacket();
            packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(IOHC::_header) + 5;
            packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
            packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
            packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
            packet->buffer_length = 15;
            packet->payload.packet.header.CtrlByte2.asByte = 0;
            
            address myAddr = CONTROLLER_ADDRESS;
            memcpy(packet->payload.packet.header.source, myAddr, 3);
            memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
            
            packet->payload.packet.header.cmd = 0x3D;
            memcpy(packet->payload.buffer + 9, mac, 6);
            
            packet->frequency = CHANNEL2;
            packet->repeatTime = 25;
            packet->repeat = 0;
            packet->lock = false;
            packet->shortPreamble = true;
            
            std::vector<IOHC::iohcPacket*> packets;
            packets.push_back(packet);
            _radioInstance->send(packets);
            
        
            Serial.printf("✅ Sent CMD 0x3D authentication (MAC: %02X%02X%02X%02X%02X%02X)\n",
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            Serial.println("⏳ Waiting for CMD 0x04 confirmation...");
        } else {
            Serial.println("⚠️  No command stored - cannot authenticate");
        }
    }
    
    return true;
}

bool IOHC2WResponseHandler::handleConfirmation(IOHC::iohcPacket* iohc) {
    auto* devMgr = Device2WManager::getInstance();
    Device2W* device = devMgr->getDevice(iohc->payload.packet.header.source);
    
    if (!device || device->pairingState != PairingState::PAIRED) {
        return false; // Not a paired device
    }
    
    Serial.printf("✅ CMD 0x04 response from %s: ", device->addressStr.c_str());
    for (int i = 0; i < iohc->payload.buffer[8]; i++) {
        Serial.printf("%02X", iohc->payload.buffer[9 + i]);
    }
    Serial.println();
    
    return true;
}
