#ifndef IOHC_PAIRING_CONTROLLER_H
#define IOHC_PAIRING_CONTROLLER_H

#include "iohcDevice2W.h"
#include "iohcPacket.h"
#include <functional>

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
    
    // Track which command is being authenticated (e.g., 0x32 for key transfer, 0x36 for address request)
    uint8_t commandBeingAuthenticated;
    
    // Pairing challenge (12 bytes for CMD 0x2A) - DEPRECATED, CMD 0x28 has no payload
    uint8_t pairingChallenge[12];
    
    // System key (should be loaded from secure storage)
    uint8_t systemKey2W[16];
    bool hasSystemKey;
    
    // Auto-pairing mode (automatically pair first device that responds to discovery)
    bool autoPairMode;
    
    // CMD 0x2A broadcast counter (send 4 times)
    uint8_t cmd2ABroadcastCount;
    
    // Auto-retry mechanism for failed sends
    std::function<bool()> pendingRetryFunc;  // Function to retry
    uint8_t retryCount;
    uint32_t lastRetryTime;
    static const uint8_t MAX_RETRIES = 5;
    static const uint32_t RETRY_DELAY_MS = 100;
    
    PairingController() : deviceMgr(nullptr), radio(nullptr), 
                         pairingActive(false), lastStepTime(0), hasSystemKey(false),
                         hasChallenge(false), commandBeingAuthenticated(0), lastSentPacket(nullptr), 
                         autoPairMode(false), cmd2ABroadcastCount(0), pendingRetryFunc(nullptr), 
                         retryCount(0), lastRetryTime(0) {
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
    
    // Enable auto-pairing mode (automatically pair first device that responds)
    void enableAutoPairMode();
    
    // Disable auto-pairing mode
    void disableAutoPairMode();
    
    // Process received packets during pairing
    bool handlePairingPacket(iohcPacket* packet);
    
    // Periodic processing (call from loop)
    void process();
    
    // Get current pairing status
    bool isPairingActive() const { return pairingActive; }
    bool isAutoPairMode() const { return autoPairMode; }
    Device2W* getCurrentPairingDevice();
    
    // Debug/testing: Verify crypto implementation with known test vectors
    void verifyCryptoImplementation();
    
private:
    // Auto-retry helper functions
    void scheduleRetry(std::function<bool()> retryFunc);
    void clearRetry();
    void processRetry();
    
    // Pairing workflow steps (New Protocol - matches log sequence)
    bool sendPairingBroadcast();        // CMD 0x28 - Discovery (broadcast, no payload)
    bool sendAliveCheck(Device2W* device);  // CMD 0x2C - Actuator alive check
    bool send2ABroadcast();             // CMD 0x2A - Pairing broadcast (12-byte payload, send 4x)
    bool sendPriorityAddressRequest(Device2W* device);  // CMD 0x36 - Priority Address Request
    bool sendChallengeToPair(Device2W* device);  // CMD 0x3C - Send Challenge to device (WE send challenge)
    // Device responds with CMD 0x3D, then we request info with CMD 0x54
    
    // Old/deprecated workflow steps
    bool sendLearningMode(Device2W* device);  // CMD 0x2E - 1W Learning mode (NOT USED)
    bool sendAskChallenge(Device2W* device);  // CMD 0x31 - Ask Challenge (Push key exchange method)
    bool sendForceKeyExchange(Device2W* device);  // CMD 0x38 - Force key exchange when device skips challenge (Pull method)
    bool handleDeviceChallenge(iohcPacket* packet);  // CMD 0x3C from device
    bool sendChallengeResponse(Device2W* device); // CMD 0x3D to device (using stored challenge)
    bool handlePairingConfirmation(iohcPacket* packet);  // CMD 0x2F from device
    
    // DEPRECATED: CMD 0x32/0x33 not used in TaHoma flow
    bool sendKeyTransfer(Device2W* device);  // CMD 0x32 - Encrypted key transfer (OLD)
    bool handleKeyTransferAck(iohcPacket* packet);
    
    // Info gathering steps
    bool requestName(Device2W* device);
    bool requestGeneralInfo1(Device2W* device);
    bool requestGeneralInfo2(Device2W* device);
    
    // Helper to send packet
    bool sendPacket(iohcPacket* packet);
};

#endif // IOHC_PAIRING_CONTROLLER_H
