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
#include <fileSystemHelpers.h>
#include <iohcRemote1W.h>
#include <iohcCozyDevice2W.h>
#include <iohcOtherDevice2W.h>
#include <iohcDevice2W.h>
#include <iohcPairingController.h>
#include <iohcRemoteMap.h>
#include <iohcPacket.h>
#include <interact.h>
#include <wifi_helper.h>
#include <oled_display.h>
#include <iohcCryptoHelpers.h>
#include <cstdlib>
#if defined(MQTT)
#include <mqtt_handler.h>
#endif
#include <nvs_helpers.h>

// External radio instance from main.cpp
extern IOHC::iohcRadio *radioInstance;

ConnState mqttStatus = ConnState::Disconnected;

// Helper function to send CMD 0x3D challenge response for authenticated control
// TEMPORARILY DISABLED TO SAVE MEMORY - Will re-enable after memory optimization
/*
bool sendChallengeResponseForControl(Device2W* device, const uint8_t* originalCommand, size_t cmdLen) {
    if (!device->hasPendingChallenge) {
        Serial.println("ERROR: No pending challenge to respond to");
        return false;
    }
    
    if (!device->hasSystemKey) {
        Serial.println("ERROR: Device has no system key for authentication");
        return false;
    }
    
    // Create frame data from the original command for MAC calculation
    std::vector<uint8_t> frame_data;
    for (size_t i = 0; i < cmdLen; i++) {
        frame_data.push_back(originalCommand[i]);
    }
    
    // Generate MAC using system key and challenge
    uint8_t mac[6];
    iohcCrypto::create_2W_hmac(mac, device->lastChallenge, device->systemKey, frame_data);
    
    // Create CMD 0x3D packet
    iohcPacket* packet = new iohcPacket();
    packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) - 1;
    packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
    packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
    packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
    packet->payload.packet.header.CtrlByte1.asByte += 6;
    memcpy(packet->payload.buffer + 9, mac, 6);
    packet->buffer_length = 6 + 9;
    
    packet->payload.packet.header.CtrlByte2.asByte = 0;
    
    address myAddr = {0xBA, 0x11, 0xAD};
    memcpy(packet->payload.packet.header.source, myAddr, 3);
    memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
    
    packet->payload.packet.header.cmd = 0x3D;
    packet->frequency = CHANNEL2;
    packet->repeatTime = 25;
    packet->repeat = 0;
    packet->lock = false;
    packet->shortPreamble = true;
    
    std::vector<iohcPacket*> packets;
    packets.push_back(packet);
    radioInstance->send(packets);
    
    // Store response and clear pending flag
    auto* devMgr = Device2WManager::getInstance();
    devMgr->storeResponse(device->nodeAddress, mac, 6);
    
    Serial.printf("✅ Sent CMD 0x3D authentication (MAC: %02X%02X%02X%02X%02X%02X)\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    return true;
}
*/

// Pre-allocate command handlers instead of using malloc to avoid heap fragmentation
_cmdEntry _cmdHandlerStorage[MAXCMDS];
_cmdEntry* _cmdHandler[MAXCMDS];
uint8_t lastEntry = 0;


void tokenize(std::string const &str, const char delim, Tokens &out) {
  std::stringstream ss(str);
  std::string s;
  while (std::getline(ss, s, delim)) {
    out.push_back(s);
  }
}


namespace Cmd {
bool verbosity = true;
bool pairMode = false;
bool scanMode = false;
#if defined(ESP32)
TimersUS::TickerUsESP32 kbd_tick;
#endif
TimerHandle_t consoleTimer;

static char _rxbuffer[512];
static uint8_t _len = 0;
static uint8_t _avail = 0;
/**
 * The function `createCommands()` initializes and adds various command handlers for controlling
 * different devices and functionalities.
 */
void createCommands() {
    // Atlantic 2W - COMMENTED OUT TO SAVE MEMORY
    /*
    Cmd::addHandler((char *) "powerOn", (char *) "Permit to retrieve paired devices", [](Tokens *cmd)-> void {
        IOHC::iohcCozyDevice2W::getInstance()->cmd(IOHC::DeviceButton::powerOn, nullptr);
    });
    Cmd::addHandler((char *) "setTemp", (char *) "7.0 to 28.0 - 0 get actual temp", [](Tokens *cmd)-> void {
        IOHC::iohcCozyDevice2W::getInstance()->cmd(IOHC::DeviceButton::setTemp, cmd);
    });
    Cmd::addHandler((char *) "setMode", (char *) "auto prog manual off - FF to get actual mode",
                    [](Tokens *cmd)-> void {
                        IOHC::iohcCozyDevice2W::getInstance()->cmd(IOHC::DeviceButton::setMode, cmd);
                    });
    Cmd::addHandler((char *) "setPresence", (char *) "on off", [](Tokens *cmd)-> void {
        IOHC::iohcCozyDevice2W::getInstance()->cmd(IOHC::DeviceButton::setPresence, cmd);
    });
    Cmd::addHandler((char *) "setWindow", (char *) "open close", [](Tokens *cmd)-> void {
        IOHC::iohcCozyDevice2W::getInstance()->cmd(IOHC::DeviceButton::setWindow, cmd);
    });
    Cmd::addHandler((char *) "midnight", (char *) "Synchro Paired", [](Tokens *cmd)-> void {
        IOHC::iohcCozyDevice2W::getInstance()->cmd(IOHC::DeviceButton::midnight, nullptr);
    });
    Cmd::addHandler((char *) "associate", (char *) "Synchro Paired", [](Tokens *cmd)-> void {
        IOHC::iohcCozyDevice2W::getInstance()->cmd(IOHC::DeviceButton::associate, nullptr);
    });
    */
    Cmd::addHandler((char *) "custom", (char *) "test unknown commands", [](Tokens *cmd)-> void {
        /*scanMode = true;*/
        IOHC::iohcOtherDevice2W::getInstance()->cmd(IOHC::Other2WButton::custom, cmd /*cmd->at(1).c_str()*/);
    });
    Cmd::addHandler((char *) "custom60", (char *) "test 0x60 commands", [](Tokens *cmd)-> void {
        /*scanMode = true;*/
        IOHC::iohcOtherDevice2W::getInstance()->cmd(IOHC::Other2WButton::custom60, cmd /*cmd->at(1).c_str()*/);
    });
    // 1W
    Cmd::addHandler((char *) "pair", (char *) "1W put device in pair mode", [](Tokens *cmd)-> void {
        IOHC::iohcRemote1W::getInstance()->cmd(IOHC::RemoteButton::Pair, cmd);
    });
    Cmd::addHandler((char *) "add", (char *) "1W add controller to device", [](Tokens *cmd)-> void {
        IOHC::iohcRemote1W::getInstance()->cmd(IOHC::RemoteButton::Add, cmd);
    });
    Cmd::addHandler((char *) "remove", (char *) "1W remove controller from device", [](Tokens *cmd)-> void {
        IOHC::iohcRemote1W::getInstance()->cmd(IOHC::RemoteButton::Remove, cmd);
    });
    Cmd::addHandler((char *) "open", (char *) "1W open device", [](Tokens *cmd)-> void {
        IOHC::iohcRemote1W::getInstance()->cmd(IOHC::RemoteButton::Open, cmd);
    });
    Cmd::addHandler((char *) "close", (char *) "1W close device", [](Tokens *cmd)-> void {
        IOHC::iohcRemote1W::getInstance()->cmd(IOHC::RemoteButton::Close, cmd);
    });
    Cmd::addHandler((char *) "stop", (char *) "1W stop device", [](Tokens *cmd)-> void {
        IOHC::iohcRemote1W::getInstance()->cmd(IOHC::RemoteButton::Stop, cmd);
    });
    Cmd::addHandler((char *) "position", (char *) "1W set position 0-100", [](Tokens *cmd)-> void {
        IOHC::iohcRemote1W::getInstance()->cmd(IOHC::RemoteButton::Position, cmd);
    });
    Cmd::addHandler((char *) "absolute", (char *) "1W set absolute position 0-100", [](Tokens *cmd)-> void {
        IOHC::iohcRemote1W::getInstance()->cmd(IOHC::RemoteButton::Absolute, cmd);
    });
    // Less common 1W commands - COMMENTED OUT TO SAVE MEMORY
    /*
    Cmd::addHandler((char *) "vent", (char *) "1W vent device", [](Tokens *cmd)-> void {
        IOHC::iohcRemote1W::getInstance()->cmd(IOHC::RemoteButton::Vent, cmd);
    });
    Cmd::addHandler((char *) "force", (char *) "1W force device open", [](Tokens *cmd)-> void {
        IOHC::iohcRemote1W::getInstance()->cmd(IOHC::RemoteButton::ForceOpen, cmd);
    });
    Cmd::addHandler((char *) "mode1", (char *) "1W Mode1", [](Tokens *cmd)-> void {
        IOHC::iohcRemote1W::getInstance()->cmd(IOHC::RemoteButton::Mode1, cmd);
    });
    Cmd::addHandler((char *) "mode2", (char *) "1W Mode2", [](Tokens *cmd)-> void {
        IOHC::iohcRemote1W::getInstance()->cmd(IOHC::RemoteButton::Mode2, cmd);
    });
    Cmd::addHandler((char *) "mode3", (char *) "1W Mode3", [](Tokens *cmd)-> void {
        IOHC::iohcRemote1W::getInstance()->cmd(IOHC::RemoteButton::Mode3, cmd);
    });
    Cmd::addHandler((char *) "mode4", (char *) "1W Mode4", [](Tokens *cmd)-> void {
        IOHC::iohcRemote1W::getInstance()->cmd(IOHC::RemoteButton::Mode4, cmd);
    });
    */
    Cmd::addHandler((char *) "new1W", (char *) "Add new 1W device", [](Tokens *cmd)-> void {
        if (cmd->size() < 2) {
            Serial.println("Usage: new1W <name>");
            return;
        }
        std::string name = cmd->at(1);
        for (size_t i = 2; i < cmd->size(); ++i) {
            name += " " + cmd->at(i);
        }
        IOHC::iohcRemote1W::getInstance()->addRemote(name);
    });
    Cmd::addHandler((char *) "del1W", (char *) "Remove 1W device", [](Tokens *cmd)-> void {
        if (cmd->size() < 2) {
            Serial.println("Usage: del1W <description>");
            return;
        }
        IOHC::iohcRemote1W::getInstance()->removeRemote(cmd->at(1));
    });
    Cmd::addHandler((char *) "edit1W", (char *) "Edit 1W device name", [](Tokens *cmd)-> void {
        if (cmd->size() < 3) {
            Serial.println("Usage: edit1W <description> <name>");
            return;
        }
        std::string name = cmd->at(2);
        for (size_t i = 3; i < cmd->size(); ++i) {
            name += " " + cmd->at(i);
        }
        IOHC::iohcRemote1W::getInstance()->renameRemote(cmd->at(1), name);
    });
    Cmd::addHandler((char *) "time1W", (char *) "Set 1W device travel time", [](Tokens *cmd)-> void {
        if (cmd->size() < 3) {
            Serial.println("Usage: time1W <description> <seconds>");
            return;
        }
        uint32_t t = strtoul(cmd->at(2).c_str(), nullptr, 10);
        IOHC::iohcRemote1W::getInstance()->setTravelTime(cmd->at(1), t);
    });
    Cmd::addHandler((char *) "list1W", (char *) "List 1W devices", [](Tokens *cmd)-> void {
        const auto &remotes = IOHC::iohcRemote1W::getInstance()->getRemotes();
        for (const auto &r : remotes) {
            Serial.printf("%s: %s %u %s\n",
                          r.description.c_str(),
                          r.name.c_str(),
                          r.travelTime,
                          r.paired ? "paired" : "unpaired");
        }
    });
    // Remote map
    Cmd::addHandler((char *) "newRemote", (char *) "Create remote with address and name", [](Tokens *cmd)-> void {
        if (cmd->size() < 3) {
            Serial.println("Usage: newRemote <address> <name>");
            return;
        }
        IOHC::address node{};
        if (hexStringToBytes(cmd->at(1), node) != sizeof(IOHC::address)) {
            Serial.println("Invalid address");
            return;
        }
        std::string name = cmd->at(2);
        for (size_t i = 3; i < cmd->size(); ++i) {
            name += " " + cmd->at(i);
        }
        IOHC::iohcRemoteMap::getInstance()->add(node, name);
    });
    Cmd::addHandler((char *) "linkRemote", (char *) "Link device to remote", [](Tokens *cmd)-> void {
        if (cmd->size() < 3) {
            Serial.println("Usage: linkRemote <address> <device>");
            return;
        }
        IOHC::address node{};
        if (hexStringToBytes(cmd->at(1), node) != sizeof(IOHC::address)) {
            Serial.println("Invalid address");
            return;
        }
        IOHC::iohcRemoteMap::getInstance()->linkDevice(node, cmd->at(2));
    });
    Cmd::addHandler((char *) "unlinkRemote", (char *) "Remove device from remote", [](Tokens *cmd)-> void {
        if (cmd->size() < 3) {
            Serial.println("Usage: unlinkRemote <address> <device>");
            return;
        }
        IOHC::address node{};
        if (hexStringToBytes(cmd->at(1), node) != sizeof(IOHC::address)) {
            Serial.println("Invalid address");
            return;
        }
        IOHC::iohcRemoteMap::getInstance()->unlinkDevice(node, cmd->at(2));
    });
    Cmd::addHandler((char *) "delRemote", (char *) "Remove remote", [](Tokens *cmd)-> void {
        if (cmd->size() < 2) {
            Serial.println("Usage: delRemote <address>");
            return;
        }
        IOHC::address node{};
        if (hexStringToBytes(cmd->at(1), node) != sizeof(IOHC::address)) {
            Serial.println("Invalid address");
            return;
        }
        IOHC::iohcRemoteMap::getInstance()->remove(node);
    });
    // Other 2W
    Cmd::addHandler((char *) "discovery", (char *) "Send discovery on air", [](Tokens *cmd)-> void {
        Serial.println("Sending 2W discovery broadcast...");
        Serial.println("Listening for devices (press device pairing button now)...");
        Serial.println("Device addresses will be shown when they respond.");
        Serial.println("Use 'pair2W <address>' to pair a discovered device.");
        IOHC::iohcOtherDevice2W::getInstance()->cmd(IOHC::Other2WButton::discovery, nullptr);
    });
    Cmd::addHandler((char *) "getName", (char *) "Name Of A Device", [](Tokens *cmd)-> void {
        IOHC::iohcOtherDevice2W::getInstance()->cmd(IOHC::Other2WButton::getName, cmd);
    });
    Cmd::addHandler((char *) "scanMode", (char *) "scanMode", [](Tokens *cmd)-> void {
        scanMode = true;
        IOHC::iohcOtherDevice2W::getInstance()->cmd(IOHC::Other2WButton::checkCmd, nullptr);
    });
    
    // New 2W Device Management
    Cmd::addHandler((char *) "pair2W", (char *) "Pair 2W device <address>", [](Tokens *cmd)-> void {
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
    });
    
    Cmd::addHandler((char *) "cancelPair2W", (char *) "Cancel ongoing pairing", [](Tokens *cmd)-> void {
        auto* pairingCtrl = PairingController::getInstance();
        pairingCtrl->cancelPairing();
        Serial.println("Pairing cancelled");
    });
    
    Cmd::addHandler((char *) "verifyCrypto", (char *) "Verify crypto implementation with test vectors", [](Tokens *cmd)-> void {
        auto* pairingCtrl = PairingController::getInstance();
        Serial.println("Running crypto verification test...");
        pairingCtrl->verifyCryptoImplementation();
        Serial.println("Check logs for results");
    });
    
    Cmd::addHandler((char *) "list2W", (char *) "List all 2W devices", [](Tokens *cmd)-> void {
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
    });
    
    Cmd::addHandler((char *) "info2W", (char *) "Show detailed info for 2W device <address>", [](Tokens *cmd)-> void {
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
    });
    
    Cmd::addHandler((char *) "del2W", (char *) "Remove 2W device <address>", [](Tokens *cmd)-> void {
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
    });
    
    Cmd::addHandler((char *) "save2W", (char *) "Save 2W devices to file", [](Tokens *cmd)-> void {
        auto* devMgr = Device2WManager::getInstance();
        if (devMgr->saveToFile()) {
            Serial.println("2W devices saved successfully");
        } else {
            Serial.println("Failed to save 2W devices");
        }
    });
    
    Cmd::addHandler((char *) "reload2W", (char *) "Reload 2W devices from file", [](Tokens *cmd)-> void {
        auto* devMgr = Device2WManager::getInstance();
        devMgr->clear();
        if (devMgr->loadFromFile()) {
            Serial.println("2W devices reloaded successfully");
        } else {
            Serial.println("Failed to reload 2W devices");
        }
    });
    
    // 2W Device Control Commands
    Cmd::addHandler((char *) "on2W", (char *) "Turn ON 2W device <address>", [](Tokens *cmd)-> void {
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
        // Format: 01 e7 00 00 00 00 for OFF, 01 e7 c8 00 00 00 for ON
        iohcPacket* packet = new iohcPacket();
        
        packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) + 5; // header + 6 bytes data - 1
        packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
        packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
        packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
        packet->buffer_length = 14; // 8 byte header + 6 bytes data
        
        packet->payload.packet.header.CtrlByte2.asByte = 0;
        
        address myAddr = {0xBA, 0x11, 0xAD};
        memcpy(packet->payload.packet.header.source, myAddr, 3);
        memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
        
        packet->payload.packet.header.cmd = 0x00;
        // Write data bytes directly to buffer after header (8 bytes)
        packet->payload.buffer[8] = 0x01;   // Originator type
        packet->payload.buffer[9] = 0xe7;   // ACEI (actuator class/event identifier)
        packet->payload.buffer[10] = 0x00;  // Main parameter (0x00 = ON for this device!)
        packet->payload.buffer[11] = 0x00;  // Functional param 1
        packet->payload.buffer[12] = 0x00;  // Functional param 2
        packet->payload.buffer[13] = 0x00;  // Functional param 3
        
        // Store command for later MAC calculation
        device->lastCommandLen = 7;  // CMD byte + 6 data bytes
        device->lastCommand[0] = 0x00;  // CMD
        device->lastCommand[1] = 0x01;  // Payload bytes
        device->lastCommand[2] = 0xe7;
        device->lastCommand[3] = 0x00;  // ON = 0x00 (counter-intuitive but correct!)
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
        
        Serial.printf("Sent ON command (CMD 0x00: 01 e7 c8 00 00 00) to device %s\n", device->addressStr.c_str());
        Serial.println("Device should respond with CMD 0x3C challenge - you'll need to authenticate with auth2W");
    });
    
    Cmd::addHandler((char *) "off2W", (char *) "Turn OFF 2W device <address>", [](Tokens *cmd)-> void {
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
        // Format: 01 e7 00 00 00 00 for OFF, 01 e7 c8 00 00 00 for ON
        iohcPacket* packet = new iohcPacket();
        
        packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) + 5; // header + 6 bytes data - 1
        packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
        packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
        packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
        packet->buffer_length = 14; // 8 byte header + 6 bytes data
        
        packet->payload.packet.header.CtrlByte2.asByte = 0;
        
        address myAddr = {0xBA, 0x11, 0xAD};
        memcpy(packet->payload.packet.header.source, myAddr, 3);
        memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
        
        packet->payload.packet.header.cmd = 0x00;
        // Write data bytes directly to buffer after header (8 bytes)
        packet->payload.buffer[8] = 0x01;   // Originator type
        packet->payload.buffer[9] = 0xe7;   // ACEI (actuator class/event identifier)
        packet->payload.buffer[10] = 0x00;  // Main parameter (0x00 = 0 = OFF)
        packet->payload.buffer[11] = 0x00;  // Functional param 1
        packet->payload.buffer[12] = 0x00;  // Functional param 2
        packet->payload.buffer[13] = 0x00;  // Functional param 3
        
        // Store command for later MAC calculation
        device->lastCommandLen = 7;  // CMD byte + 6 data bytes
        device->lastCommand[0] = 0x00;  // CMD
        device->lastCommand[1] = 0x01;  // Payload bytes
        device->lastCommand[2] = 0xe7;
        device->lastCommand[3] = 0x00;  // OFF = 0x00
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
        
        Serial.printf("Sent OFF command (CMD 0x00: 01 e7 00 00 00 00) to device %s\n", device->addressStr.c_str());
        Serial.println("Device should respond with CMD 0x3C challenge - you'll need to authenticate with auth2W");
    });
    
    Cmd::addHandler((char *) "status2W", (char *) "Query status of 2W device <address>", [](Tokens *cmd)-> void {
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
        
        packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) + 2; // header + 3 bytes data - 1
        packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
        packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
        packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
        packet->buffer_length = 11; // 8 byte header + 3 bytes data
        
        packet->payload.packet.header.CtrlByte2.asByte = 0;
        
        address myAddr = {0xBA, 0x11, 0xAD};
        memcpy(packet->payload.packet.header.source, myAddr, 3);
        memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
        
        packet->payload.packet.header.cmd = 0x03;
        // Write data bytes directly to buffer after header (8 bytes)
        packet->payload.buffer[8] = 0x03;   // Status query
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
    });
    
    Cmd::addHandler((char *) "test2W", (char *) "Test command with custom payload <address> <cmd> <byte1> <byte2> <byte3> [byte4] [byte5] [byte6]", [](Tokens *cmd)-> void {
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
    });
    
    // Authentication command for 2W devices
    Cmd::addHandler((char *) "auth2W", (char *) "Authenticate to 2W device <address>", [](Tokens *cmd)-> void {
        if (cmd->size() < 2) {
            Serial.println("Usage: auth2W <address>");
            Serial.println("This responds to a CMD 0x3C challenge from a device");
            return;
        }
        
        auto* devMgr = Device2WManager::getInstance();
        auto* device = devMgr->getDevice(String(cmd->at(1).c_str()));
        
        if (!device) {
            Serial.println("Device not found. Use list2W to see paired devices.");
            return;
        }
        
        if (!device->hasPendingChallenge) {
            Serial.println("ERROR: No pending challenge from this device");
            Serial.println("Send a command first (like on2W or off2W) to trigger a challenge");
            return;
        }
        
        if (!device->hasSystemKey) {
            Serial.println("ERROR: Device has no system key");
            Serial.println("The device must be paired first");
            return;
        }
        
        if (device->lastCommandLen == 0) {
            Serial.println("ERROR: No command stored");
            Serial.println("Send a command first (like on2W or off2W)");
            return;
        }
        
        // Generate MAC using the stored challenge, system key, and ORIGINAL COMMAND
        // Frame data is the original command that triggered the challenge
        std::vector<uint8_t> frame_data;
        for (uint8_t i = 0; i < device->lastCommandLen; i++) {
            frame_data.push_back(device->lastCommand[i]);
        }
        
        Serial.print("[auth2W] Using frame data: ");
        for (uint8_t byte : frame_data) {
            Serial.printf("%02X", byte);
        }
        Serial.println();
        
        uint8_t mac[6];
        iohcCrypto::create_2W_hmac(mac, device->lastChallenge, device->systemKey, frame_data);
        
        // Create CMD 0x3D packet
        iohcPacket* packet = new iohcPacket();
        
        packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) + 5; // header + 6 bytes - 1
        packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
        packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
        packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
        packet->buffer_length = 14; // 8 byte header + 6 bytes MAC
        
        packet->payload.packet.header.CtrlByte2.asByte = 0;
        
        address myAddr = {0xBA, 0x11, 0xAD};
        memcpy(packet->payload.packet.header.source, myAddr, 3);
        memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
        
        packet->payload.packet.header.cmd = 0x3D;
        memcpy(packet->payload.buffer + 8, mac, 6);
        
        packet->frequency = CHANNEL2;
        packet->repeatTime = 25;
        packet->repeat = 0;
        packet->lock = false;
        packet->shortPreamble = true;
        
        std::vector<iohcPacket*> packets;
        packets.push_back(packet);
        radioInstance->send(packets);
        
        // Clear pending challenge flag
        device->hasPendingChallenge = false;
        
        Serial.printf("✅ Sent CMD 0x3D authentication (MAC: %02X%02X%02X%02X%02X%02X)\n",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        Serial.println("Device should respond with CMD 0x04 confirming the action");
    });
    
    // General commands (existing)
    Cmd::addHandler((char *) "scanDump", (char *) "Dump Scan Results", [](Tokens *cmd)-> void {
        scanMode = false;
        IOHC::iohcOtherDevice2W::getInstance()->scanDump();
    });
    Cmd::addHandler((char *) "verbose", (char *) "Toggle verbose output on packets list",
                    [](Tokens *cmd)-> void { verbosity = !verbosity; });

    Cmd::addHandler((char *) "pairMode", (char *) "pairMode", [](Tokens *cmd)-> void { pairMode = !pairMode; });

    // Utils
    Cmd::addHandler((char *) "dump", (char *) "Dump Transceiver registers", [](Tokens *cmd)-> void {
        Radio::dump();
//        Serial.printf("*%d packets in memory\t", nextPacket);
//        Serial.printf("*%d devices discovered\n\n", sysTable->size());
    });
    /*    
    //    Cmd::addHandler((char *)"dump2", (char *)"Dump Transceiver registers 1Col", [](Tokens*cmd)->void {Radio::dump2(); Serial.printf("*%d packets in memory\t", nextPacket); Serial.printf("*%d devices discovered\n\n", sysTable->size());});
    Cmd::addHandler((char *) "list1W", (char *) "List received packets", [](Tokens *cmd)-> void {
        for (uint8_t i = 0; i < nextPacket; i++) msgRcvd(radioPackets[i]);
        sysTable->dump1W();
    });
    Cmd::addHandler((char *) "save", (char *) "Saves Objects table", [](Tokens *cmd)-> void {
        sysTable->save(true); });
    Cmd::addHandler((char *) "erase", (char *) "Erase received packets", [](Tokens *cmd)-> void {
        for (uint8_t i = 0; i < nextPacket; i++) free(radioPackets[i]);
        nextPacket = 0;
    });
    Cmd::addHandler((char *) "send", (char *) "Send packet from cmd line",
                    [](Tokens *cmd)-> void { txUserBuffer(cmd); });
*/
    Cmd::addHandler((char *) "ls", (char *) "List filesystem", [](Tokens *cmd)-> void { listFS(); });
    Cmd::addHandler((char *) "cat", (char *) "Print file content", [](Tokens *cmd)-> void { cat(cmd->at(1).c_str()); });
    Cmd::addHandler((char *) "rm", (char *) "Remove file", [](Tokens *cmd)-> void { rm(cmd->at(1).c_str()); });
    Cmd::addHandler((char *) "lastAddr", (char *) "Show last received address", [](Tokens *cmd)-> void {
        Serial.println(bytesToHexString(IOHC::lastFromAddress, sizeof(IOHC::lastFromAddress)).c_str());
    });
#if defined(MQTT)
    Cmd::addHandler((char *) "mqttIp", (char *) "Set MQTT server IP", [](Tokens *cmd)-> void {
        if (cmd->size() < 2) {
            Serial.println("Usage: mqttIp <ip>");
            return;
        }
        mqtt_server = cmd->at(1);

        nvs_write_string(NVS_KEY_MQTT_SERVER, mqtt_server);

        mqttClient.disconnect();
        mqttClient.setServer(mqtt_server.c_str(), 1883);
        connectToMqtt();
    });
    Cmd::addHandler((char *) "mqttUser", (char *) "Set MQTT username", [](Tokens *cmd)-> void {
        if (cmd->size() < 2) {
            Serial.println("Usage: mqttUser <username>");
            return;
        }
        mqtt_user = cmd->at(1);

        nvs_write_string(NVS_KEY_MQTT_USER, mqtt_user);

        mqttClient.disconnect();
        mqttClient.setCredentials(mqtt_user.c_str(), mqtt_password.c_str());
        connectToMqtt();
    });
    Cmd::addHandler((char *) "mqttPass", (char *) "Set MQTT password", [](Tokens *cmd)-> void {
        if (cmd->size() < 2) {
            Serial.println("Usage: mqttPass <password>");
            return;
        }
        mqtt_password = cmd->at(1);

        nvs_write_string(NVS_KEY_MQTT_PASSWORD, mqtt_password);

        mqttClient.disconnect();
        mqttClient.setCredentials(mqtt_user.c_str(), mqtt_password.c_str());
        connectToMqtt();
    });
    Cmd::addHandler((char *) "mqttDiscovery", (char *) "Set MQTT discovery topic", [](Tokens *cmd)-> void {
        if (cmd->size() < 2) {
            Serial.println("Usage: mqttDiscovery <topic>");
            return;
        }
        mqtt_discovery_topic = cmd->at(1);

        nvs_write_string(NVS_KEY_MQTT_DISCOVERY, mqtt_discovery_topic);

        if (mqttStatus == ConnState::Connected)
            handleMqttConnect();
    });
#endif
/*
    Cmd::addHandler((char *) "list2W", (char *) "List received packets", [](Tokens *cmd)-> void {
        for (uint8_t i = 0; i < nextPacket; i++) msgRcvd(radioPackets[i]);
        sysTable->dump2W();
    });
*/    // Unnecessary just for test
    Cmd::addHandler((char *) "discover28", (char *) "discover28", [](Tokens *cmd)-> void {
        IOHC::iohcOtherDevice2W::getInstance()->cmd(IOHC::Other2WButton::discover28, nullptr);
    });

    Cmd::addHandler((char *) "discover2A", (char *) "discover2A", [](Tokens *cmd)-> void {
        IOHC::iohcOtherDevice2W::getInstance()->cmd(IOHC::Other2WButton::discover2A, nullptr);
    });
/*
    Cmd::addHandler((char *) "fake0", (char *) "fake0", [](Tokens *cmd)-> void {
        IOHC::iohcCozyDevice2W::getInstance()->cmd(IOHC::DeviceButton::fake0, nullptr);
    });
    Cmd::addHandler((char *) "ack", (char *) "ack33", [](Tokens *cmd)-> void {
        IOHC::iohcCozyDevice2W::getInstance()->cmd(IOHC::DeviceButton::ack, nullptr);
    });
*/
    /*
        options.add_options()
          ("d,debug", "Enable debugging") // a bool parameter
          ("i,integer", "Int param", cxxopts::value<int>())
          ("f,file", "File name", cxxopts::value<std::string>())
          ("v,verbose", "Verbose output", cxxopts::value<bool>()->default_value("false"))
        options.add_options()
                ("b,bar", "Param bar", cxxopts::value<std::string>())
                ("d,debug", "Enable debugging", cxxopts::value<bool>()->default_value("false"))
                ("f,foo", "Param foo", cxxopts::value<int>()->default_value("10"))
                ("h,help", "Print usage")
    */
    //Customize the options of the console object. See https://github.com/jarro2783/cxxopts for explaination
    /*
        OptionsConsoleCommand powerOn("powerOn", [](int argc, char **argv, ParseResult result, Options options)-> int {
            IOHC::iohcCozyDevice2W::getInstance()->cmd(IOHC::DeviceButton::powerOn, nullptr);
            return EXIT_SUCCESS;
        }, "Permit to retrieve paired devices", "v.0.0.1", "");
        //powerOn.options.add_options()("i,integer", "Int param", cxxopts::value<int>());
        //Register it like any other command
        console.registerCommand(powerOn);
        OptionsConsoleCommand setTemp("t", [](int argc, char **argv, ParseResult result, Options options)-> int {
            // auto tempTok = new Tokens();
            // auto temp = static_cast<String>(result["t"].as<float>());
            // tempTok->push_back(temp.c_str());
        printf(result["t"].as<String>().c_str());
        //    IOHC::iohcCozyDevice2W::getInstance()->cmd(IOHC::DeviceButton::setTemp, result["t"].as<Tokens>() );
            return EXIT_SUCCESS;
        }, ".0 to 28.0 - 0 get actual temp", "v.0.0.1", "");
        //Customize the options of the console object. See https://github.com/jarro2783/cxxopts for explaination
        setTemp.options.add_options()("t", "Temperature", cxxopts::value< std::vector<std::string> >());
        //Register it like any other command
        console.registerCommand(setTemp);
    */
}

bool addHandler(char *cmd, char *description, void (*handler)(Tokens*)) {
  for (uint8_t idx = 0; idx < MAXCMDS; ++idx) {
    if (_cmdHandler[idx] != nullptr) {
    } else {
      // Use pre-allocated storage instead of malloc to avoid heap fragmentation
      _cmdHandler[idx] = &_cmdHandlerStorage[idx];
      memset(_cmdHandler[idx], 0, sizeof(struct _cmdEntry));
      strncpy(_cmdHandler[idx]->cmd, cmd,
              strlen(cmd) < sizeof(_cmdHandler[idx]->cmd) ? strlen(cmd)
                                                          : sizeof(_cmdHandler[idx]->cmd) - 1);
      strncpy(_cmdHandler[idx]->description, description,
              strlen(cmd) < sizeof(_cmdHandler[idx]->description)
                  ? strlen(description)
                  : sizeof(_cmdHandler[idx]->description) - 1);
      _cmdHandler[idx]->handler = handler;

      if (idx > lastEntry)
        lastEntry = idx;
      return true;
    }
  }
  return false;
}

char *cmdReceived(bool echo) {
  _avail = Serial.available();
  if (_avail) {
    _len += Serial.readBytes(&_rxbuffer[_len], _avail);
    if (echo) {
      _rxbuffer[_len] = '\0';
      Serial.printf("%s", &_rxbuffer[_len - _avail]);
    }
  }
  if (_rxbuffer[_len - 1] == 0x0a) {
    _rxbuffer[_len - 2] = '\0';
    _len = 0;
    return _rxbuffer;
  }
  return nullptr;
}

void cmdFuncHandler() {
  constexpr char delim = ' ';
  Tokens segments;

  char *cmd = cmdReceived(true);
  if (!cmd)
    return;
  if (!strlen(cmd))
    return;

  tokenize(cmd, delim, segments);
  if (strcmp((char *)"help", segments[0].c_str()) == 0) {
    Serial.printf("\nRegistered commands:\n");
    for (uint8_t idx = 0; idx <= lastEntry; ++idx) {
      if (_cmdHandler[idx] == nullptr)
        continue;
      Serial.printf("- %s\t%s\n", _cmdHandler[idx]->cmd, _cmdHandler[idx]->description);
    }
    Serial.printf("- %s\t%s\n\n", (char *)"help", (char *)"This command");
    Serial.printf("\n");
    return;
  }
  for (uint8_t idx = 0; idx <= lastEntry; ++idx) {
    if (_cmdHandler[idx] == nullptr)
      continue;
    if (strcmp(_cmdHandler[idx]->cmd, segments[0].c_str()) == 0) {
      _cmdHandler[idx]->handler(&segments);
      return;
    }
  }
  Serial.printf("*> Unknown <*\n");
}

void init() {
//#if defined(MQTT)
//initMqtt();
//#endif

//  initWifi();

  kbd_tick.attach_ms(500, cmdFuncHandler);
}
}
