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

#include <iohc2WCommands.h>
#include <iohcDevice2W.h>
#include <iohcPairingController.h>
#include <iohcPacket.h>
#include <iohcRadio.h>

// External radio instance from main.cpp
extern IOHC::iohcRadio *radioInstance;

namespace IOHC2WCommands {

// Pairing and device discovery
void pair2W(Tokens *cmd) {
        if (cmd->size() < 2) {
            Serial.println("Usage: pair2W <address>");
            Serial.println("Example: pair2W fe90ee");
            return;
        }
        
        address deviceAddr{};
        if (hexStringToBytes(cmd->at(1), deviceAddr) != 3) {
            Serial.println("Invalid address - must be 6 hex characters (3 bytes)");
            return;
        }
        
        auto* pairingCtrl = PairingController::getInstance();
        if (pairingCtrl->startPairing(deviceAddr)) {
            char addrStr[7];
            snprintf(addrStr, sizeof(addrStr), "%02x%02x%02x", deviceAddr[0], deviceAddr[1], deviceAddr[2]);
            Serial.printf("Started pairing device %s\n", addrStr);
            Serial.println("Pairing process will complete automatically.");
        } else {
            Serial.println("Failed to start pairing (already in progress?)");
        }
}

void cancelPair2W(Tokens *cmd) {
    auto* pairingCtrl = PairingController::getInstance();
    pairingCtrl->cancelPairing();
    Serial.println("Pairing cancelled");
}

// Device management
void list2W(Tokens *cmd) {
        auto* devMgr = Device2WManager::getInstance();
        auto devices = devMgr->getAllDevices();
        
        if (devices.empty()) {
            Serial.println("No 2W devices found");
            return;
        }
        
        Serial.printf("Found %d 2W device(s):\n", devices.size());
        Serial.println("Address  | State          | Type | Name          | Last Seen");
        Serial.println("---------|----------------|------|---------------|----------");
        
        for (auto* dev : devices) {
            uint32_t lastSeenSec = (millis() - dev->lastSeen) / 1000;
            Serial.printf("%s | %-14s | %04X | %-13s | %us ago\n",
                         dev->addressStr.c_str(),
                         dev->getPairingStateStr().c_str(),
                         dev->capabilities.nodeType,
                         dev->capabilities.name.c_str(),
                         lastSeenSec);
        }
}

void info2W(Tokens *cmd) {
        if (cmd->size() < 2) {
            Serial.println("Usage: info2W <address>");
            return;
        }
        
        auto* devMgr = Device2WManager::getInstance();
        auto* device = devMgr->getDevice(String(cmd->at(1).c_str()));
        
        if (!device) {
            Serial.println("Device not found");
            return;
        }
        
        Serial.printf("\n=== Device %s ===\n", device->addressStr.c_str());
        Serial.printf("Pairing State: %s\n", device->getPairingStateStr().c_str());
        Serial.printf("Description:   %s\n", device->description.c_str());
        Serial.printf("Last Seen:     %u seconds ago\n", (millis() - device->lastSeen) / 1000);
        
        Serial.printf("\nCapabilities:\n");
        Serial.printf("  Node Type:     0x%04X (%u)\n", device->capabilities.nodeType, device->capabilities.nodeType);
        Serial.printf("  Node Subtype:  0x%02X (%u)\n", device->capabilities.nodeSubtype, device->capabilities.nodeSubtype);
        Serial.printf("  Manufacturer:  0x%02X\n", device->capabilities.manufacturer);
        Serial.printf("  Multi Info:    0x%02X\n", device->capabilities.multiInfo);
        Serial.printf("  Timestamp:     %u\n", device->capabilities.timestamp);
        Serial.printf("  Name:          %s\n", device->capabilities.name.c_str());
        
        Serial.printf("\nCrypto State:\n");
        Serial.printf("  Has System Key:  %s\n", device->hasSystemKey ? "Yes" : "No");
        Serial.printf("  Has Session Key: %s\n", device->hasSessionKey ? "Yes" : "No");
        Serial.printf("  Sequence Number: %u\n", device->sequenceNumber);
        
        if (device->capabilities.hasGeneralInfo1) {
            Serial.printf("\nGeneral Info 1: ");
            for (int i = 0; i < 14; i++) {
                Serial.printf("%02X ", device->capabilities.generalInfo1[i]);
            }
            Serial.println();
        }
        
        if (device->capabilities.hasGeneralInfo2) {
            Serial.printf("General Info 2: ");
            for (int i = 0; i < 16; i++) {
                Serial.printf("%02X ", device->capabilities.generalInfo2[i]);
            }
            Serial.println();
        }
        Serial.println();
}

void del2W(Tokens *cmd) {
        if (cmd->size() < 2) {
            Serial.println("Usage: del2W <address>");
            return;
        }
        
        auto* devMgr = Device2WManager::getInstance();
        if (devMgr->removeDevice(String(cmd->at(1).c_str()))) {
            Serial.printf("Device %s removed\n", cmd->at(1).c_str());
            devMgr->saveToFile();
        } else {
            Serial.println("Device not found");
        }
}

void save2W(Tokens *cmd) {
        auto* devMgr = Device2WManager::getInstance();
        if (devMgr->saveToFile()) {
            Serial.println("2W devices saved successfully");
        } else {
            Serial.println("Failed to save 2W devices");
        }
}

void reload2W(Tokens *cmd) {
        auto* devMgr = Device2WManager::getInstance();
        devMgr->clear();
        if (devMgr->loadFromFile()) {
            Serial.println("2W devices reloaded successfully");
        } else {
            Serial.println("Failed to reload 2W devices");
        }
}

// Device control commands
void on2W(Tokens *cmd) {
        if (cmd->size() < 2) {
            Serial.println("Usage: on2W <address>");
            return;
        }
        
        auto* devMgr = Device2WManager::getInstance();
        auto* device = devMgr->getDevice(String(cmd->at(1).c_str()));
        
        if (!device) {
            Serial.println("Device not found. Use list2W to see paired devices.");
            return;
        }
        
        if (device->pairingState != PairingState::PAIRED) {
            Serial.printf("Device %s is not paired (state: %s)\n", 
                         device->addressStr.c_str(), device->getPairingStateStr().c_str());
            return;
        }
        
        // ON/OFF plug control: CMD 0x00 with 6-byte payload (from TaHoma logs)
        // Format: 01 e7 00 00 00 00 for ON, 01 e7 c8 00 00 00 for OFF
        iohcPacket* packet = new iohcPacket();
        
        packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) + 5;
        packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
        packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
        packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
        packet->buffer_length = 14;
        
        packet->payload.packet.header.CtrlByte2.asByte = 0;
        
        address myAddr = {0xBA, 0x11, 0xAD};
        memcpy(packet->payload.packet.header.source, myAddr, 3);
        memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
        
        packet->payload.packet.header.cmd = 0x00;
        packet->payload.buffer[8] = 0x01;   // Originator type
        packet->payload.buffer[9] = 0xe7;   // ACEI (actuator class/event identifier)
        packet->payload.buffer[10] = 0x00;  // Main parameter (0x00 = ON)
        packet->payload.buffer[11] = 0x00;  // Functional param 1
        packet->payload.buffer[12] = 0x00;  // Functional param 2
        packet->payload.buffer[13] = 0x00;  // Functional param 3
        
        // Store command for later MAC calculation
        device->lastCommandLen = 7;
        device->lastCommand[0] = 0x00;
        device->lastCommand[1] = 0x01;
        device->lastCommand[2] = 0xe7;
        device->lastCommand[3] = 0x00;  // ON = 0x00
        device->lastCommand[4] = 0x00;
        device->lastCommand[5] = 0x00;
        device->lastCommand[6] = 0x00;
        
        packet->frequency = CHANNEL2;
        packet->repeatTime = 25;
        packet->repeat = 0;
        packet->lock = false;
        packet->shortPreamble = true;
        
        std::vector<iohcPacket*> packets;
        packets.push_back(packet);
        radioInstance->send(packets);
        
        Serial.printf("Sent ON command to device %s\n", device->addressStr.c_str());
        Serial.println("Device will challenge - authentication is automatic");
}

void off2W(Tokens *cmd) {
        if (cmd->size() < 2) {
            Serial.println("Usage: off2W <address>");
            return;
        }
        
        auto* devMgr = Device2WManager::getInstance();
        auto* device = devMgr->getDevice(String(cmd->at(1).c_str()));
        
        if (!device) {
            Serial.println("Device not found. Use list2W to see paired devices.");
            return;
        }
        
        if (device->pairingState != PairingState::PAIRED) {
            Serial.printf("Device %s is not paired (state: %s)\n", 
                         device->addressStr.c_str(), device->getPairingStateStr().c_str());
            return;
        }
        
        // ON/OFF plug control: CMD 0x00 with 6-byte payload (from TaHoma logs)
        // Format: 01 e7 00 00 00 00 for ON, 01 e7 c8 00 00 00 for OFF
        iohcPacket* packet = new iohcPacket();
        
        packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) + 5;
        packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
        packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
        packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
        packet->buffer_length = 14;
        
        packet->payload.packet.header.CtrlByte2.asByte = 0;
        
        address myAddr = {0xBA, 0x11, 0xAD};
        memcpy(packet->payload.packet.header.source, myAddr, 3);
        memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
        
        packet->payload.packet.header.cmd = 0x00;
        packet->payload.buffer[8] = 0x01;   // Originator type
        packet->payload.buffer[9] = 0xe7;   // ACEI (actuator class/event identifier)
        packet->payload.buffer[10] = 0xc8;  // Main parameter (0xc8 = OFF)
        packet->payload.buffer[11] = 0x00;  // Functional param 1
        packet->payload.buffer[12] = 0x00;  // Functional param 2
        packet->payload.buffer[13] = 0x00;  // Functional param 3
        
        // Store command for later MAC calculation
        device->lastCommandLen = 7;
        device->lastCommand[0] = 0x00;
        device->lastCommand[1] = 0x01;
        device->lastCommand[2] = 0xe7;
        device->lastCommand[3] = 0xc8;  // OFF = 0xc8
        device->lastCommand[4] = 0x00;
        device->lastCommand[5] = 0x00;
        device->lastCommand[6] = 0x00;
        
        packet->frequency = CHANNEL2;
        packet->repeatTime = 25;
        packet->repeat = 0;
        packet->lock = false;
        packet->shortPreamble = true;
        
        std::vector<iohcPacket*> packets;
        packets.push_back(packet);
        radioInstance->send(packets);
        
        Serial.printf("Sent OFF command to device %s\n", device->addressStr.c_str());
        Serial.println("Device will challenge - authentication is automatic");
}

void status2W(Tokens *cmd) {
        if (cmd->size() < 2) {
            Serial.println("Usage: status2W <address>");
            return;
        }
        
        auto* devMgr = Device2WManager::getInstance();
        auto* device = devMgr->getDevice(String(cmd->at(1).c_str()));
        
        if (!device) {
            Serial.println("Device not found. Use list2W to see paired devices.");
            return;
        }
        
        if (device->pairingState != PairingState::PAIRED) {
            Serial.printf("Device %s is not paired (state: %s)\n", 
                         device->addressStr.c_str(), device->getPairingStateStr().c_str());
            return;
        }
        
        // Send CMD 0x03 with payload 030000 to query status
        iohcPacket* packet = new iohcPacket();
        
        packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) + 2;
        packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
        packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
        packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
        packet->buffer_length = 11;
        
        packet->payload.packet.header.CtrlByte2.asByte = 0;
        
        address myAddr = {0xBA, 0x11, 0xAD};
        memcpy(packet->payload.packet.header.source, myAddr, 3);
        memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
        
        packet->payload.packet.header.cmd = 0x03;
        packet->payload.buffer[8] = 0x03;
        packet->payload.buffer[9] = 0x00;
        packet->payload.buffer[10] = 0x00;
        
        packet->frequency = CHANNEL2;
        packet->repeatTime = 25;
        packet->repeat = 0;
        packet->lock = false;
        packet->shortPreamble = true;
        
        std::vector<iohcPacket*> packets;
        packets.push_back(packet);
        radioInstance->send(packets);
        
        Serial.printf("Sent status query to device %s (check logs for CMD 0x04 response)\n", device->addressStr.c_str());
}

void test2W(Tokens *cmd) {
        if (cmd->size() < 5) {
            Serial.println("Usage: test2W <address> <cmd> <byte1> <byte2> <byte3> [byte4] [byte5] [byte6]");
            Serial.println("Example: test2W 4c79dc 00 01 e7 00 00 00 00  (CMD 0x00 with 6 bytes)");
            Serial.println("Example: test2W 4c79dc 03 2d 01 c8  (CMD 0x03 with 3 bytes)");
            return;
        }
        
        auto* devMgr = Device2WManager::getInstance();
        auto* device = devMgr->getDevice(String(cmd->at(1).c_str()));
        
        if (!device) {
            Serial.println("Device not found. Use list2W to see paired devices.");
            return;
        }
        
        // Parse command byte and payload
        uint8_t cmdByte = (uint8_t)strtoul(cmd->at(2).c_str(), nullptr, 16);
        uint8_t byte1 = (uint8_t)strtoul(cmd->at(3).c_str(), nullptr, 16);
        uint8_t byte2 = (uint8_t)strtoul(cmd->at(4).c_str(), nullptr, 16);
        uint8_t byte3 = (uint8_t)strtoul(cmd->at(5).c_str(), nullptr, 16);
        
        // Optional bytes for longer payloads
        uint8_t byte4 = (cmd->size() > 6) ? (uint8_t)strtoul(cmd->at(6).c_str(), nullptr, 16) : 0x00;
        uint8_t byte5 = (cmd->size() > 7) ? (uint8_t)strtoul(cmd->at(7).c_str(), nullptr, 16) : 0x00;
        uint8_t byte6 = (cmd->size() > 8) ? (uint8_t)strtoul(cmd->at(8).c_str(), nullptr, 16) : 0x00;
        
        int dataLen = (cmd->size() > 6) ? 6 : 3;
        
        iohcPacket* packet = new iohcPacket();
        
        packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) + dataLen - 1;
        packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
        packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
        packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
        packet->buffer_length = 8 + dataLen;
        
        packet->payload.packet.header.CtrlByte2.asByte = 0;
        
        address myAddr = {0xBA, 0x11, 0xAD};
        memcpy(packet->payload.packet.header.source, myAddr, 3);
        memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
        
        packet->payload.packet.header.cmd = cmdByte;
        packet->payload.buffer[8] = byte1;
        packet->payload.buffer[9] = byte2;
        packet->payload.buffer[10] = byte3;
        if (dataLen == 6) {
            packet->payload.buffer[11] = byte4;
            packet->payload.buffer[12] = byte5;
            packet->payload.buffer[13] = byte6;
        }
        
        packet->frequency = CHANNEL2;
        packet->repeatTime = 25;
        packet->repeat = 0;
        packet->lock = false;
        packet->shortPreamble = true;
        
        std::vector<iohcPacket*> packets;
        packets.push_back(packet);
        radioInstance->send(packets);
        
        if (dataLen == 6) {
            Serial.printf("Sent CMD 0x%02X with payload %02X %02X %02X %02X %02X %02X to device %s\n", 
                         cmdByte, byte1, byte2, byte3, byte4, byte5, byte6, device->addressStr.c_str());
        } else {
            Serial.printf("Sent CMD 0x%02X with payload %02X %02X %02X to device %s\n", 
                         cmdByte, byte1, byte2, byte3, device->addressStr.c_str());
        }
}

void verifyCrypto(Tokens *cmd) {
        auto* pairingCtrl = PairingController::getInstance();
        Serial.println("Running crypto verification test...");
        pairingCtrl->verifyCryptoImplementation();
        Serial.println("Check logs for results");
}

} // namespace IOHC2WCommands
