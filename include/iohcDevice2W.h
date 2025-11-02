#ifndef IOHCDEVICE2W_H
#define IOHCDEVICE2W_H

#include <Arduino.h>
#include <vector>
#include <map>

// Forward declarations
namespace IOHC {
    typedef uint8_t address[3];
    class iohcPacket;
}

using namespace IOHC;

// Device pairing states
enum class PairingState {
    UNPAIRED,           // Not yet paired
    DISCOVERING,        // Discovery broadcast sent (CMD 0x28 → waiting for CMD 0x29)
    ALIVE_CHECK,        // Alive check sent (CMD 0x2C → waiting for CMD 0x2D)
    BROADCASTING_2A,    // Broadcasting CMD 0x2A (4 times) after alive check
    WAITING_BEFORE_LEARNING, // After CMD 0x2A broadcasts → send CMD 0x36 (Priority Address Request)
    LEARNING_MODE,      // After CMD 0x37 → send CMD 0x3C (Challenge)
    CHALLENGE_SENT,     // Challenge sent (CMD 0x3C → waiting for CMD 0x3D response)
    CHALLENGE_RECEIVED, // Challenge received (CMD 0x3C → need to send CMD 0x3D) - OLD FLOW
    PAIRING_CONFIRMED,  // Pairing confirmed (CMD 0x2F received) - OLD FLOW
    ASKING_CHALLENGE,   // CMD 0x31 sent → waiting for CMD 0x3C for key exchange - OLD FLOW
    KEY_EXCHANGED,      // Challenge authenticated, ready to request device info
    PAIRED,             // Fully paired and operational
    PAIRING_FAILED      // Pairing process failed
};

// Device capabilities from General Info
struct DeviceCapabilities {
    uint16_t nodeType;      // Type from CMD 0x29 (10 bits)
    uint8_t nodeSubtype;    // Subtype from CMD 0x29 (6 bits)
    uint8_t manufacturer;   // Manufacturer ID
    uint8_t multiInfo;      // Multi-info byte (ATT, SyncCtrlGrp, etc.)
    uint16_t timestamp;     // Device timestamp
    String name;            // Device name from CMD 0x51
    
    // General Info 1 (14 bytes from CMD 0x55)
    uint8_t generalInfo1[14];
    bool hasGeneralInfo1;
    
    // General Info 2 (16 bytes from CMD 0x57)
    uint8_t generalInfo2[16];
    bool hasGeneralInfo2;
    
    DeviceCapabilities() : nodeType(0), nodeSubtype(0), manufacturer(0), 
                          multiInfo(0), timestamp(0), 
                          hasGeneralInfo1(false), hasGeneralInfo2(false) {}
};

// Single 2W device representation
class Device2W {
public:
    // Device identity
    address nodeAddress;        // 3-byte device address
    String addressStr;          // Hex string representation
    
    // Pairing state
    PairingState pairingState;
    uint32_t lastSeen;          // millis() timestamp of last communication
    uint32_t pairingStartTime;  // When pairing process started
    
    // Cryptographic state
    uint8_t systemKey[16];      // 2W system key
    bool hasSystemKey;
    uint8_t sessionKey[16];     // Current session key (derived from challenges)
    bool hasSessionKey;
    uint16_t sequenceNumber;    // For command sequencing
    u_int8_t stackKey[16];      // 2W stack key
    bool hasStackKey;
    
    // Last challenge data (for ongoing authentication)
    uint8_t lastChallenge[6];
    uint8_t lastResponse[6];
    bool hasPendingChallenge;
    
    // Last command sent (for MAC calculation in CMD 0x3D)
    uint8_t lastCommand[32];    // Store full command payload (without CMD byte)
    uint8_t lastCommandLen;
    uint8_t lastCommandByte;    // The CMD byte itself (e.g., 0x00 for on/off)
    
    // Device information
    DeviceCapabilities capabilities;
    String description;         // User-provided description
    
    // Constructor
    Device2W() : pairingState(PairingState::UNPAIRED), lastSeen(0), pairingStartTime(0),
                hasSystemKey(false), hasSessionKey(false), sequenceNumber(0),
                hasStackKey(false), hasPendingChallenge(false), lastCommandLen(0) {
        memset(nodeAddress, 0, 3);
        memset(systemKey, 0, 16);
        memset(sessionKey, 0, 16);
        memset(stackKey, 0, 16);
        memset(lastChallenge, 0, 6);
        memset(lastResponse, 0, 6);
        memset(lastCommand, 0, 32);
    }
    
    Device2W(const address& addr) : Device2W() {
        setAddress(addr);
    }
    
    // Set address and update string representation
    void setAddress(const address& addr) {
        memcpy(nodeAddress, addr, 3);
        char buf[7];
        snprintf(buf, sizeof(buf), "%02x%02x%02x", addr[0], addr[1], addr[2]);
        addressStr = String(buf);
    }
    
    // Update last seen timestamp
    void touch() {
        lastSeen = millis();
    }
    
    // Check if pairing is in progress
    bool isPairing() const {
        return pairingState == PairingState::DISCOVERING ||
               pairingState == PairingState::ALIVE_CHECK ||
               pairingState == PairingState::WAITING_BEFORE_LEARNING ||
               pairingState == PairingState::LEARNING_MODE ||
               pairingState == PairingState::CHALLENGE_RECEIVED ||
               pairingState == PairingState::PAIRING_CONFIRMED ||
               pairingState == PairingState::ASKING_CHALLENGE;
    }
    
    // Check if pairing timeout occurred (30 seconds)
    bool hasPairingTimedOut() const {
        if (!isPairing()) return false;
        return (millis() - pairingStartTime) > 30000;
    }
    
    // Get pairing state as string
    String getPairingStateStr() const {
        switch (pairingState) {
            case PairingState::UNPAIRED: return "UNPAIRED";
            case PairingState::DISCOVERING: return "DISCOVERING";
            case PairingState::ALIVE_CHECK: return "ALIVE_CHECK";
            case PairingState::WAITING_BEFORE_LEARNING: return "WAITING_BEFORE_LEARNING";
            case PairingState::LEARNING_MODE: return "LEARNING_MODE";
            case PairingState::CHALLENGE_RECEIVED: return "CHALLENGE_RECEIVED";
            case PairingState::PAIRING_CONFIRMED: return "PAIRING_CONFIRMED";
            case PairingState::ASKING_CHALLENGE: return "ASKING_CHALLENGE";
            case PairingState::KEY_EXCHANGED: return "KEY_EXCHANGED";
            case PairingState::PAIRED: return "PAIRED";
            case PairingState::PAIRING_FAILED: return "PAIRING_FAILED";
            default: return "UNKNOWN";
        }
    }
    
    // Serialize to JSON object (used by manager)
    String toJson() const;
    
    // Deserialize from JSON object (used by manager)
    bool fromJson(const String& addressKey, const String& jsonStr);
};

// Device manager singleton - handles all 2W devices
class Device2WManager {
private:
    static Device2WManager* instance;
    std::map<String, Device2W*> devices;  // Keyed by address hex string
    String jsonFilePath;
    
    Device2WManager() : jsonFilePath("/2W.json") {}
    
public:
    // Singleton access
    static Device2WManager* getInstance() {
        if (!instance) {
            instance = new Device2WManager();
        }
        return instance;
    }
    
    // Device CRUD operations
    Device2W* addDevice(const address& addr);
    Device2W* getDevice(const address& addr);
    Device2W* getDevice(const String& addrStr);
    bool removeDevice(const address& addr);
    bool removeDevice(const String& addrStr);
    std::vector<Device2W*> getAllDevices();
    
    // Find devices by state
    std::vector<Device2W*> getDevicesByState(PairingState state);
    Device2W* findDeviceInPairing();
    
    // Persistence
    bool loadFromFile();
    bool saveToFile();
    
    // Pairing workflow helpers
    bool startPairing(const address& addr);
    bool completePairing(const address& addr);
    bool failPairing(const address& addr);
    
    // Update device info from received packets
    bool updateFromDiscoveryAnswer(const address& addr, const uint8_t* data, size_t len);
    bool updateFromNameAnswer(const address& addr, const String& name);
    bool updateFromGeneralInfo1(const address& addr, const uint8_t* data, size_t len);
    bool updateFromGeneralInfo2(const address& addr, const uint8_t* data, size_t len);
    
    // Challenge/response tracking
    bool storeChallenge(const address& addr, const uint8_t* challenge, size_t len);
    bool storeResponse(const address& addr, const uint8_t* response, size_t len);
    
    // Session key management
    bool storeSystemKey(const address& addr, const uint8_t* key, size_t len);
    bool storeStackKey(const address &addr, const uint8_t *key, size_t len);
    bool storeSessionKey(const address &addr, const uint8_t *key, size_t len);

    // Cleanup
    void removeTimedOutDevices();
    void clear();
    
    ~Device2WManager() {
        clear();
    }
};

#endif // IOHCDEVICE2W_H
