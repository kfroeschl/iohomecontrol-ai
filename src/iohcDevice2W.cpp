#include "iohcDevice2W.h"
#include "iohcPacket.h"
#include "fileSystemHelpers.h"
#include "log_buffer.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

using namespace IOHC;

// Static instance
Device2WManager* Device2WManager::instance = nullptr;

// DeviceCapabilities helper implementations

String DeviceCapabilities::getManufacturerName() const {
    switch (manufacturer) {
        case 0x00: return "No Type";
        case 0x01: return "Velux";
        case 0x02: return "Somfy";
        case 0x03: return "Honeywell";
        case 0x04: return "Hörmann";
        case 0x05: return "ASSA ABLOY";
        case 0x06: return "Niko";
        case 0x07: return "Window Master";
        case 0x08: return "Renson";
        case 0x09: return "CIAT";
        case 0x0A: return "Secuyou";
        case 0x0B: return "Overkiz";
        case 0x0C: return "Atlantic Group";
        case 0x0D: return "Zehnder Group";
        default: return "Unknown";
    }
}

String DeviceCapabilities::getNodeTypeName() const {
    // Combine type and subtype into a 16-bit value for easier matching
    uint16_t combined = (nodeType << 6) | nodeSubtype;
    
    switch (combined) {
        case 0x0000: return "All Nodes except Controller";
        case 0x0033: return "Smart Plug";
        case 0x0040: return "Interior Venetian Blind (IVB)";
        case 0x006A: return "Light Sensor";
        case 0x0080: return "Roller Shutter";
        case 0x0081: return "Roller Shutter with Adjustable Slats";
        case 0x0082: return "Roller Shutter with Projection";
        case 0x00C0: return "Vertical Exterior Awning (Terrace)";
        case 0x00CA: return "Window Covering Device";
        case 0x00CB: return "Window Covering Controller";
        case 0x0100: return "Window Opener";
        case 0x0101: return "Window Opener with Integrated Rain Sensor";
        case 0x012E: return "Temp and Humidity Sensor";
        case 0x0140: return "Garage Door Opener";
        case 0x017A: return "Garage Door Opener: Open/Close Only";
        case 0x0180: return "Light: On/Off + Dimming";
        case 0x0192: return "IAS Zone";
        case 0x01BA: return "Light: On/Off Only";
        case 0x01C0: return "Gate Opener";
        case 0x01FA: return "Gate Opener: Open/Close Only";
        case 0x0200: return "Rolling Door Opener";
        case 0x0240: return "Door Lock / Motorized Bolt";
        case 0x0241: return "Window Lock";
        case 0x0280: return "Vertical Interior Blind";
        case 0x0290: return "Secure Configuration Device (SCD)";
        case 0x0300: return "Beacon (Gateway/Repeater)";
        case 0x0340: return "Dual Roller Shutter";
        case 0x0380: return "Heating Temperature Interface";
        case 0x03C0: return "Switch: On/Off";
        case 0x0400: return "Horizontal Awning";
        case 0x0401: return "Pergola Rail Guided Awning";
        case 0x0440: return "Exterior Venetian Blind (EVB)";
        case 0x0480: return "Louver Blind";
        case 0x04C0: return "Curtain Track";
        case 0x0500: return "Ventilation Point";
        case 0x0501: return "Air Inlet";
        case 0x0502: return "Air Transfer";
        case 0x0503: return "Air Outlet";
        case 0x0540: return "Exterior Heating";
        case 0x057A: return "Exterior Heating: On/Off Only";
        case 0x0580: return "Heat Pump";
        case 0x05C0: return "Intrusion Alarm System";
        case 0x0600: return "Swinging Shutter";
        case 0x0601: return "Swinging Shutter with Independent Handling of Leaves";
        case 0x06C0: return "Sliding Window";
        case 0x0700: return "Zone Control Generator";
        case 0x0740: return "Bioclimatic Pergola";
        case 0x0780: return "Indoor Siren";
        case 0x0CC0: return "Domestic Hot Water";
        case 0x0D00: return "Electrical Heater";
        case 0x0D40: return "Heat Recovery Ventilation";
        case 0x3FC0: return "Central House Control";
        case 0xFC00: return "Test and Evaluation (RD)";
        case 0xFFC0: return "Remote Controller (RC)";
        default:
            // Return generic description with type and subtype
            return "Type " + String(nodeType) + "." + String(nodeSubtype);
    }
}

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
    doc["node_type_name"] = capabilities.getNodeTypeName();
    doc["manufacturer"] = capabilities.manufacturer;
    doc["manufacturer_name"] = capabilities.getManufacturerName();
    doc["multi_info"] = capabilities.multiInfo;
    doc["timestamp"] = capabilities.timestamp;
    doc["name"] = capabilities.name;
    
    // Decoded multiInfo fields
    doc["actuator_turnaround_time"] = capabilities.actuatorTurnaroundTime;
    doc["sync_ctrl_grp"] = capabilities.syncCtrlGrp;
    doc["rf_support"] = capabilities.rfSupport;
    doc["io_membership"] = capabilities.ioMembership;
    doc["power_save_mode"] = capabilities.powerSaveMode;
    
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
    if (hasStackKey) {
        char keyHex[33];
        for (int i = 0; i < 16; i++) {
            snprintf(keyHex + i * 2, 3, "%02x", stackKey[i]);
        }
        keyHex[32] = '\0';
        doc["stack_key"] = String(keyHex);
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
    else if (stateStr == "PAIRING_CONFIRMED") pairingState = PairingState::PAIRING_CONFIRMED;
    else if (stateStr == "CHALLENGE_RECEIVED") pairingState = PairingState::CHALLENGE_RECEIVED;
    else if (stateStr == "LEARNING_MODE") pairingState = PairingState::LEARNING_MODE;
    else if (stateStr == "ALIVE_CHECK") pairingState = PairingState::ALIVE_CHECK;
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
    
    // Decoded multiInfo fields (use defaults if not present)
    capabilities.actuatorTurnaroundTime = doc["actuator_turnaround_time"] | 0;
    capabilities.syncCtrlGrp = doc["sync_ctrl_grp"] | false;
    capabilities.rfSupport = doc["rf_support"] | true;
    capabilities.ioMembership = doc["io_membership"] | true;
    capabilities.powerSaveMode = doc["power_save_mode"] | 0;
    
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
    
    if (doc.containsKey("stack_key")) {
        String keyHex = doc["stack_key"];
        if (keyHex.length() == 32) {
            for (int i = 0; i < 16; i++) {
                String byteStr = keyHex.substring(i * 2, i * 2 + 2);
                stackKey[i] = strtoul(byteStr.c_str(), nullptr, 16);
            }
            hasStackKey = true;
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
        if (pair.second->isPairing()) {
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
    
    // Decode multiInfo byte
    uint8_t multiInfo = data[6];
    device->capabilities.actuatorTurnaroundTime = (multiInfo >> 6) & 0x03;  // bits 7-6
    device->capabilities.syncCtrlGrp = (multiInfo & 0x20) != 0;             // bit 5
    device->capabilities.rfSupport = (multiInfo & 0x08) == 0;               // bit 3 (inverted: 0=Yes, 1=No)
    device->capabilities.ioMembership = (multiInfo & 0x04) == 0;            // bit 2 (inverted: 0=Yes, 1=No)
    device->capabilities.powerSaveMode = multiInfo & 0x03;                  // bits 1-0
    
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

bool Device2WManager::storeStackKey(const address& addr, const uint8_t* key, size_t len) {
    if (len < 16) return false;
    
    Device2W* device = getDevice(addr);
    if (!device) return false;
    
    memcpy(device->stackKey, key, 16);
    device->hasStackKey = true;
    device->touch();
    
    addLogMessage(("Stored stack key for " + device->addressStr).c_str());
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
