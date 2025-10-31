#include "iohcDevice2W.h"
#include "iohcPacket.h"
#include "fileSystemHelpers.h"
#include "log_buffer.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

using namespace IOHC;

// Static instance
Device2WManager* Device2WManager::instance = nullptr;

// Device2W implementation

String Device2W::toJson() const {
    JsonDocument doc;
    
    // Basic info
    doc["description"] = description;
    doc["pairing_state"] = getPairingStateStr();
    doc["last_seen"] = lastSeen;
    
    // Capabilities
    doc["node_type"] = capabilities.nodeType;
    doc["node_subtype"] = capabilities.nodeSubtype;
    doc["manufacturer"] = capabilities.manufacturer;
    doc["multi_info"] = capabilities.multiInfo;
    doc["timestamp"] = capabilities.timestamp;
    doc["name"] = capabilities.name;
    
    // Keys (stored as hex strings)
    if (hasSystemKey) {
        char keyHex[33];
        for (int i = 0; i < 16; i++) {
            snprintf(keyHex + i * 2, 3, "%02x", systemKey[i]);
        }
        keyHex[32] = '\0';
        doc["system_key"] = String(keyHex);
    }
    
    if (hasSessionKey) {
        char keyHex[33];
        for (int i = 0; i < 16; i++) {
            snprintf(keyHex + i * 2, 3, "%02x", sessionKey[i]);
        }
        keyHex[32] = '\0';
        doc["session_key"] = String(keyHex);
    }
    
    doc["sequence"] = sequenceNumber;
    
    // General info (if available)
    if (capabilities.hasGeneralInfo1) {
        char hex[29];
        for (int i = 0; i < 14; i++) {
            snprintf(hex + i * 2, 3, "%02x", capabilities.generalInfo1[i]);
        }
        hex[28] = '\0';
        doc["general_info1"] = String(hex);
    }
    
    if (capabilities.hasGeneralInfo2) {
        char hex[33];
        for (int i = 0; i < 16; i++) {
            snprintf(hex + i * 2, 3, "%02x", capabilities.generalInfo2[i]);
        }
        hex[32] = '\0';
        doc["general_info2"] = String(hex);
    }
    
    String output;
    serializeJson(doc, output);
    return output;
}

bool Device2W::fromJson(const String& addressKey, const String& jsonStr) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonStr);
    
    if (error) {
        addLogMessage(("Failed to parse device JSON for " + addressKey).c_str());
        return false;
    }
    
    // Parse address
    if (addressKey.length() != 6) return false;
    
    uint8_t addr[3];
    for (int i = 0; i < 3; i++) {
        String byteStr = addressKey.substring(i * 2, i * 2 + 2);
        addr[i] = strtoul(byteStr.c_str(), nullptr, 16);
    }
    setAddress(addr);
    
    // Basic info
    description = doc["description"] | "";
    
    String stateStr = doc["pairing_state"] | "UNPAIRED";
    if (stateStr == "PAIRED") pairingState = PairingState::PAIRED;
    else if (stateStr == "KEY_EXCHANGED") pairingState = PairingState::KEY_EXCHANGED;
    else if (stateStr == "CHALLENGE_SENT") pairingState = PairingState::CHALLENGE_SENT;
    else if (stateStr == "DISCOVERING") pairingState = PairingState::DISCOVERING;
    else if (stateStr == "PAIRING_FAILED") pairingState = PairingState::PAIRING_FAILED;
    else pairingState = PairingState::UNPAIRED;
    
    lastSeen = doc["last_seen"] | 0;
    
    // Capabilities
    capabilities.nodeType = doc["node_type"] | 0;
    capabilities.nodeSubtype = doc["node_subtype"] | 0;
    capabilities.manufacturer = doc["manufacturer"] | 0;
    capabilities.multiInfo = doc["multi_info"] | 0;
    capabilities.timestamp = doc["timestamp"] | 0;
    capabilities.name = doc["name"] | "";
    
    // Keys
    if (doc.containsKey("system_key")) {
        String keyHex = doc["system_key"];
        if (keyHex.length() == 32) {
            for (int i = 0; i < 16; i++) {
                String byteStr = keyHex.substring(i * 2, i * 2 + 2);
                systemKey[i] = strtoul(byteStr.c_str(), nullptr, 16);
            }
            hasSystemKey = true;
        }
    }
    
    if (doc.containsKey("session_key")) {
        String keyHex = doc["session_key"];
        if (keyHex.length() == 32) {
            for (int i = 0; i < 16; i++) {
                String byteStr = keyHex.substring(i * 2, i * 2 + 2);
                sessionKey[i] = strtoul(byteStr.c_str(), nullptr, 16);
            }
            hasSessionKey = true;
        }
    }
    
    sequenceNumber = doc["sequence"] | 0;
    
    // General info
    if (doc.containsKey("general_info1")) {
        String hex = doc["general_info1"];
        if (hex.length() == 28) {
            for (int i = 0; i < 14; i++) {
                String byteStr = hex.substring(i * 2, i * 2 + 2);
                capabilities.generalInfo1[i] = strtoul(byteStr.c_str(), nullptr, 16);
            }
            capabilities.hasGeneralInfo1 = true;
        }
    }
    
    if (doc.containsKey("general_info2")) {
        String hex = doc["general_info2"];
        if (hex.length() == 32) {
            for (int i = 0; i < 16; i++) {
                String byteStr = hex.substring(i * 2, i * 2 + 2);
                capabilities.generalInfo2[i] = strtoul(byteStr.c_str(), nullptr, 16);
            }
            capabilities.hasGeneralInfo2 = true;
        }
    }
    
    return true;
}

// Device2WManager implementation

Device2W* Device2WManager::addDevice(const address& addr) {
    char addrStr[7];
    snprintf(addrStr, sizeof(addrStr), "%02x%02x%02x", addr[0], addr[1], addr[2]);
    String addrKey = String(addrStr);
    
    // Check if already exists
    auto it = devices.find(addrKey);
    if (it != devices.end()) {
        return it->second;
    }
    
    // Create new device
    Device2W* device = new Device2W(addr);
    devices[addrKey] = device;
    
    addLogMessage(("Added 2W device: " + addrKey).c_str());
    return device;
}

Device2W* Device2WManager::getDevice(const address& addr) {
    char addrStr[7];
    snprintf(addrStr, sizeof(addrStr), "%02x%02x%02x", addr[0], addr[1], addr[2]);
    return getDevice(String(addrStr));
}

Device2W* Device2WManager::getDevice(const String& addrStr) {
    auto it = devices.find(addrStr);
    if (it != devices.end()) {
        return it->second;
    }
    return nullptr;
}

bool Device2WManager::removeDevice(const address& addr) {
    char addrStr[7];
    snprintf(addrStr, sizeof(addrStr), "%02x%02x%02x", addr[0], addr[1], addr[2]);
    return removeDevice(String(addrStr));
}

bool Device2WManager::removeDevice(const String& addrStr) {
    auto it = devices.find(addrStr);
    if (it != devices.end()) {
        delete it->second;
        devices.erase(it);
        addLogMessage(("Removed 2W device: " + addrStr).c_str());
        return true;
    }
    return false;
}

std::vector<Device2W*> Device2WManager::getAllDevices() {
    std::vector<Device2W*> result;
    for (auto& pair : devices) {
        result.push_back(pair.second);
    }
    return result;
}

std::vector<Device2W*> Device2WManager::getDevicesByState(PairingState state) {
    std::vector<Device2W*> result;
    for (auto& pair : devices) {
        if (pair.second->pairingState == state) {
            result.push_back(pair.second);
        }
    }
    return result;
}

Device2W* Device2WManager::findDeviceInPairing() {
    for (auto& pair : devices) {
        if (pair.second->isPairingInProgress()) {
            return pair.second;
        }
    }
    return nullptr;
}

bool Device2WManager::loadFromFile() {
    if (!LittleFS.exists(jsonFilePath.c_str())) {
        addLogMessage("No 2W device database found, starting fresh");
        return false;
    }
    
    fs::File f = LittleFS.open(jsonFilePath.c_str(), "r");
    if (!f) {
        addLogMessage("Failed to open 2W.json for reading");
        return false;
    }
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, f);
    f.close();
    
    if (error) {
        addLogMessage(("Failed to parse 2W.json: " + String(error.c_str())).c_str());
        return false;
    }
    
    // Parse each device
    int count = 0;
    for (JsonPair kv : doc.as<JsonObject>()) {
        String addrKey = kv.key().c_str();
        String deviceJson;
        serializeJson(kv.value(), deviceJson);
        
        Device2W* device = new Device2W();
        if (device->fromJson(addrKey, deviceJson)) {
            devices[addrKey] = device;
            count++;
        } else {
            delete device;
        }
    }
    
    addLogMessage(("Loaded " + String(count) + " devices from 2W.json").c_str());
    return true;
}

bool Device2WManager::saveToFile() {
    JsonDocument doc;
    
    for (auto& pair : devices) {
        String deviceJson = pair.second->toJson();
        JsonDocument deviceDoc;
        deserializeJson(deviceDoc, deviceJson);
        doc[pair.first] = deviceDoc;
    }
    
    fs::File f = LittleFS.open(jsonFilePath.c_str(), "w");
    if (!f) {
        addLogMessage("Failed to open 2W.json for writing");
        return false;
    }
    
    serializeJsonPretty(doc, f);
    f.close();
    
    addLogMessage(("Saved " + String(devices.size()) + " devices to 2W.json").c_str());
    return true;
}

bool Device2WManager::startPairing(const address& addr) {
    Device2W* device = getDevice(addr);
    if (!device) {
        device = addDevice(addr);
    }
    
    device->pairingState = PairingState::DISCOVERING;
    device->pairingStartTime = millis();
    device->touch();
    
    addLogMessage(("Started pairing for " + device->addressStr).c_str());
    return true;
}

bool Device2WManager::completePairing(const address& addr) {
    Device2W* device = getDevice(addr);
    if (!device) return false;
    
    device->pairingState = PairingState::PAIRED;
    device->touch();
    saveToFile();
    
    addLogMessage(("Completed pairing for " + device->addressStr).c_str());
    return true;
}

bool Device2WManager::failPairing(const address& addr) {
    Device2W* device = getDevice(addr);
    if (!device) return false;
    
    device->pairingState = PairingState::PAIRING_FAILED;
    device->touch();
    
    addLogMessage(("Pairing failed for " + device->addressStr).c_str());
    return true;
}

bool Device2WManager::updateFromDiscoveryAnswer(const address& addr, const uint8_t* data, size_t len) {
    if (len < 9) return false;
    
    Device2W* device = getDevice(addr);
    if (!device) {
        device = addDevice(addr);
    }
    
    // Parse CMD 0x29 format: nodeType(2) + address(3) + manufacturer(1) + multiInfo(1) + timestamp(2)
    uint16_t typeAndSubtype = (data[0] << 8) | data[1];
    device->capabilities.nodeType = (typeAndSubtype >> 6) & 0x3FF;      // 10 bits
    device->capabilities.nodeSubtype = typeAndSubtype & 0x3F;            // 6 bits
    device->capabilities.manufacturer = data[5];
    device->capabilities.multiInfo = data[6];
    device->capabilities.timestamp = (data[7] << 8) | data[8];
    
    device->touch();
    
    addLogMessage(("Updated discovery info for " + device->addressStr + 
                       " Type:" + String(device->capabilities.nodeType) +
                       " Subtype:" + String(device->capabilities.nodeSubtype)).c_str());
    return true;
}

bool Device2WManager::updateFromNameAnswer(const address& addr, const String& name) {
    Device2W* device = getDevice(addr);
    if (!device) return false;
    
    device->capabilities.name = name;
    device->touch();
    
    addLogMessage(("Updated name for " + device->addressStr + ": " + name).c_str());
    return true;
}

bool Device2WManager::updateFromGeneralInfo1(const address& addr, const uint8_t* data, size_t len) {
    if (len < 14) return false;
    
    Device2W* device = getDevice(addr);
    if (!device) return false;
    
    memcpy(device->capabilities.generalInfo1, data, 14);
    device->capabilities.hasGeneralInfo1 = true;
    device->touch();
    
    addLogMessage(("Updated General Info 1 for " + device->addressStr).c_str());
    return true;
}

bool Device2WManager::updateFromGeneralInfo2(const address& addr, const uint8_t* data, size_t len) {
    if (len < 16) return false;
    
    Device2W* device = getDevice(addr);
    if (!device) return false;
    
    memcpy(device->capabilities.generalInfo2, data, 16);
    device->capabilities.hasGeneralInfo2 = true;
    device->touch();
    
    addLogMessage(("Updated General Info 2 for " + device->addressStr).c_str());
    return true;
}

bool Device2WManager::storeChallenge(const address& addr, const uint8_t* challenge, size_t len) {
    if (len < 6) return false;
    
    Device2W* device = getDevice(addr);
    if (!device) return false;
    
    memcpy(device->lastChallenge, challenge, 6);
    device->hasPendingChallenge = true;
    device->touch();
    
    return true;
}

bool Device2WManager::storeResponse(const address& addr, const uint8_t* response, size_t len) {
    if (len < 6) return false;
    
    Device2W* device = getDevice(addr);
    if (!device) return false;
    
    memcpy(device->lastResponse, response, 6);
    device->hasPendingChallenge = false;
    device->touch();
    
    return true;
}

bool Device2WManager::storeSystemKey(const address& addr, const uint8_t* key, size_t len) {
    if (len < 16) return false;
    
    Device2W* device = getDevice(addr);
    if (!device) return false;
    
    memcpy(device->systemKey, key, 16);
    device->hasSystemKey = true;
    device->touch();
    saveToFile();
    
    addLogMessage(("Stored system key for " + device->addressStr).c_str());
    return true;
}

bool Device2WManager::storeSessionKey(const address& addr, const uint8_t* key, size_t len) {
    if (len < 16) return false;
    
    Device2W* device = getDevice(addr);
    if (!device) return false;
    
    memcpy(device->sessionKey, key, 16);
    device->hasSessionKey = true;
    device->touch();
    
    addLogMessage(("Stored session key for " + device->addressStr).c_str());
    return true;
}

void Device2WManager::removeTimedOutDevices() {
    std::vector<String> toRemove;
    
    for (auto& pair : devices) {
        if (pair.second->hasPairingTimedOut()) {
            pair.second->pairingState = PairingState::PAIRING_FAILED;
            addLogMessage(("Pairing timeout for " + pair.first).c_str());
        }
    }
}

void Device2WManager::clear() {
    for (auto& pair : devices) {
        delete pair.second;
    }
    devices.clear();
}
