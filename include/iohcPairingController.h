#ifndef IOHC_PAIRING_CONTROLLER_H
#define IOHC_PAIRING_CONTROLLER_H

#include "iohcDevice2W.h"
#include "iohcPacket.h"

// Forward declarations
namespace IOHC {
    class iohcRadio;
}

using namespace IOHC;

// Simple packet wrapper for pairing controller
struct SimplePairingPacket {
    address source;
    address target;
    uint8_t command;
    uint8_t payload[21];
    uint8_t payload_len;
    
    // Convert from old iohcPacket structure
    void fromIohcPacket(iohcPacket* oldPacket) {
        memcpy(source, oldPacket->payload.packet.header.source, 3);
        memcpy(target, oldPacket->payload.packet.header.target, 3);
        command = oldPacket->payload.packet.header.cmd;
        
        // Extract payload (skip header which is 9 bytes)
        if (oldPacket->buffer_length > 9) {
            payload_len = oldPacket->buffer_length - 9;
            if (payload_len > 21) payload_len = 21;
            memcpy(payload, oldPacket->payload.buffer + 9, payload_len);
        } else {
            payload_len = 0;
        }
    }
};

// Pairing workflow orchestrator
class PairingController {
private:
    static PairingController* instance;
    Device2WManager* deviceMgr;
    iohcRadio* radio;
    
    // Current pairing session
    address currentPairingAddr;
    bool pairingActive;
    uint32_t lastStepTime;
    iohcPacket* lastSentPacket;  // Track last packet to prevent memory corruption
    
    // Device challenge received from CMD 0x3C (6 bytes for 2W)
    uint8_t deviceChallenge[6];
    bool hasChallenge;
    
    // Pairing challenge (12 bytes for CMD 0x2A) - DEPRECATED, CMD 0x28 has no payload
    uint8_t pairingChallenge[12];
    
    // System key (should be loaded from secure storage)
    uint8_t systemKey2W[16];
    bool hasSystemKey;
    
    PairingController() : deviceMgr(nullptr), radio(nullptr), 
                         pairingActive(false), lastStepTime(0), hasSystemKey(false),
                         hasChallenge(false), lastSentPacket(nullptr) {
        memset(currentPairingAddr, 0, 3);
        memset(systemKey2W, 0, 16);
        memset(deviceChallenge, 0, 6);
    }
    
public:
    static PairingController* getInstance() {
        if (!instance) {
            instance = new PairingController();
        }
        return instance;
    }
    
    // Initialize with required dependencies
    void begin(Device2WManager* devMgr, iohcRadio* radioInstance);
    
    // Set the 2W system key (load from NVS/config)
    void setSystemKey(const uint8_t* key);
    
    // Start pairing a new device
    bool startPairing(const address& deviceAddr);
    
    // Cancel ongoing pairing
    void cancelPairing();
    
    // Process received packets during pairing
    bool handlePairingPacket(iohcPacket* packet);
    
    // Periodic processing (call from loop)
    void process();
    
    // Get current pairing status
    bool isPairingActive() const { return pairingActive; }
    Device2W* getCurrentPairingDevice();
    
private:
    // Pairing workflow steps (NEW PROTOCOL)
    bool sendPairingBroadcast();        // CMD 0x28 - Discover Remote (broadcast, no payload)
    bool sendLearningMode(Device2W* device);  // CMD 0x2E - 1W Learning mode
    bool sendAskChallenge(Device2W* device);  // CMD 0x31 - Ask for challenge
    bool handleDeviceChallenge(iohcPacket* packet);  // CMD 0x3C from device
    bool sendChallengeResponse(Device2W* device, const uint8_t* deviceChallenge); // CMD 0x3D to device
    bool handlePairingConfirmation(iohcPacket* packet);  // CMD 0x2F from device
    bool sendKeyTransfer(Device2W* device);  // CMD 0x32 - Encrypted key transfer
    bool handleKeyTransferAck(iohcPacket* packet);
    
    // Info gathering steps
    bool requestName(Device2W* device);
    bool requestGeneralInfo1(Device2W* device);
    bool requestGeneralInfo2(Device2W* device);
    
    // Helper to send packet
    bool sendPacket(iohcPacket* packet);
};

#endif // IOHC_PAIRING_CONTROLLER_H
