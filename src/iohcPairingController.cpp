#include "iohcPairingController.h"
#include "iohcPacket.h"
#include "iohcRadio.h"
#include "iohcCryptoHelpers.h"
#include "crypto2Wutils.h"
#include "log_buffer.h"

using namespace IOHC;

PairingController* PairingController::instance = nullptr;

void PairingController::begin(Device2WManager* devMgr, iohcRadio* radioInstance) {
    deviceMgr = devMgr;
    radio = radioInstance;
    
    // TODO: Load system key from NVS
    // For now, use a placeholder
    addLogMessage("PairingController initialized");
}

void PairingController::setSystemKey(const uint8_t* key) {
    memcpy(systemKey2W, key, 16);
    hasSystemKey = true;
    addLogMessage("2W system key configured");
}

bool PairingController::startPairing(const address& deviceAddr) {
    if (pairingActive) {
        addLogMessage("Pairing already in progress");
        return false;
    }
    
    if (!hasSystemKey) {
        addLogMessage("ERROR: No 2W system key configured!");
        return false;
    }
    
    // Store address but DON'T set pairingActive yet (to avoid race with process())
    memcpy(currentPairingAddr, deviceAddr, 3);
    
    // Start device in manager (adds device if not exists, sets state to DISCOVERING)
    deviceMgr->startPairing(deviceAddr);
    
    // Verify device was created
    Device2W* device = deviceMgr->getDevice(currentPairingAddr);
    if (!device) {
        addLogMessage("ERROR: Failed to create device!");
        memset(currentPairingAddr, 0, 3);
        return false;
    }
    
    // NOW set pairing active (after device is confirmed to exist)
    pairingActive = true;
    lastStepTime = millis() - 1000;  // Set to past time to trigger immediate first send
    
    // Note: No longer generating challenge - CMD 0x28 has no payload
    
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.println("⚠️  PRESS AND HOLD PAIRING BUTTON NOW!");
    Serial.println("   Keep holding until pairing completes");
    Serial.println("   (usually 10-20 seconds)");
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    addLogMessage("Pairing will start immediately...");
    
    // Don't send immediately - let process() handle it when radio is ready
    // This prevents "radio busy" errors
    return true;
}

void PairingController::cancelPairing() {
    if (!pairingActive) return;
    
    Device2W* device = deviceMgr->getDevice(currentPairingAddr);
    if (device) {
        device->pairingState = PairingState::UNPAIRED;
    }
    
    pairingActive = false;
    memset(currentPairingAddr, 0, 3);
    addLogMessage("Pairing cancelled");
}

Device2W* PairingController::getCurrentPairingDevice() {
    if (!pairingActive) return nullptr;
    return deviceMgr->getDevice(currentPairingAddr);
}

bool PairingController::handlePairingPacket(iohcPacket* packet) {
    if (!pairingActive) {
        return false;
    }
    
    // Convert to simple packet structure
    SimplePairingPacket simplePacket;
    simplePacket.fromIohcPacket(packet);
    
    ets_printf("[Pairing] Packet received: CMD 0x%02X from %02X%02X%02X (expecting %02X%02X%02X)\n",
               simplePacket.command,
               simplePacket.source[0], simplePacket.source[1], simplePacket.source[2],
               currentPairingAddr[0], currentPairingAddr[1], currentPairingAddr[2]);
    
    // Check if packet is from device we're pairing
    if (memcmp(simplePacket.source, currentPairingAddr, 3) != 0) {
        return false;
    }
    
    Device2W* device = getCurrentPairingDevice();
    if (!device) return false;
    
    char logMsg[128];
    snprintf(logMsg, sizeof(logMsg), 
             "📥 Pairing packet CMD 0x%02X from %02X%02X%02X (state=%d)",
             simplePacket.command,
             simplePacket.source[0], simplePacket.source[1], simplePacket.source[2],
             (int)device->pairingState);
    ets_printf("[Pairing] %s\n", logMsg);
    
    bool handled = false;
    
    switch (simplePacket.command) {
        case 0x29: // Discovery Response (device responds to CMD 0x28)
            if (device->pairingState == PairingState::DISCOVERING) {
                char logMsg[128];
                snprintf(logMsg, sizeof(logMsg), 
                         "✅ Received CMD 0x29 from %02X%02X%02X - Device is ready for pairing!",
                         simplePacket.source[0], simplePacket.source[1], simplePacket.source[2]);
                addLogMessage(logMsg);
                
                // Device responded to our CMD 0x28 broadcast
                // Now send CMD 0x2E (Learning Mode) to this specific device
                handled = sendLearningMode(device);
            }
            break;
            
        case 0xFE: // Error/Status Response
            {
                uint8_t statusCode = simplePacket.payload_len > 0 ? simplePacket.payload[0] : 0;
                char logMsg[128];
                
                if (statusCode == 0x08) {
                    // Status 0x08 typically means "not in pairing mode" or "operation not permitted"
                    snprintf(logMsg, sizeof(logMsg), "⚠️  Device not ready (status 0x08) - Is pairing button pressed?");
                    addLogMessage(logMsg);
                    // Cancel after too many error responses
                    static uint8_t errorCount = 0;
                    errorCount++;
                    if (errorCount > 6) {
                        addLogMessage("Too many device errors. Please:");
                        addLogMessage("1. Press and HOLD the pairing button");
                        addLogMessage("2. Run: pair2W <address> again");
                        cancelPairing();
                        errorCount = 0;
                    }
                } else {
                    snprintf(logMsg, sizeof(logMsg), "Device sent status 0x%02X", statusCode);
                    addLogMessage(logMsg);
                }
                // Don't mark as handled - let retry logic continue (for now)
            }
            break;
            
        case 0x2F: // Pairing Confirmation (NEW PROTOCOL)
            if (simplePacket.payload_len >= 1) {
                addLogMessage("Received pairing confirmation (CMD 0x2F)");
                device->pairingState = PairingState::PAIRING_CONFIRMED;
                
                // Now request challenge from device (CMD 0x31)
                addLogMessage("Requesting challenge from device (CMD 0x31)...");
                handled = sendAskChallenge(device);
            }
            break;
            
        case 0x3C: // Device Challenge (2W Protocol)
            if (simplePacket.payload_len >= 6) {
                // Store the 6-byte challenge from the device
                memcpy(deviceChallenge, simplePacket.payload, 6);
                hasChallenge = true;
                
                char logMsg[128];
                snprintf(logMsg, sizeof(logMsg), 
                         "Received device challenge: %02X%02X%02X%02X%02X%02X",
                         deviceChallenge[0], deviceChallenge[1], deviceChallenge[2],
                         deviceChallenge[3], deviceChallenge[4], deviceChallenge[5]);
                addLogMessage(logMsg);
                
                // Now send encrypted key transfer (CMD 0x32)
                addLogMessage("Sending encrypted key transfer (CMD 0x32)...");
                handled = sendKeyTransfer(device);
            }
            break;
            
        case 0x33: // Key Transfer Ack
            device->pairingState = PairingState::KEY_EXCHANGED;
            addLogMessage("Key transfer acknowledged, requesting device info...");
            
            // Start gathering device information
            lastStepTime = millis() - 2000; // Trigger immediate info request
            handled = true;
            break;
            
        case 0x51: // Name Answer
            if (simplePacket.payload_len >= 16) {
                String name = "";
                for (int i = 0; i < 16 && simplePacket.payload[i] != 0; i++) {
                    name += (char)simplePacket.payload[i];
                }
                deviceMgr->updateFromNameAnswer(currentPairingAddr, name);
                // Now request general info 1
                requestGeneralInfo1(device);
                handled = true;
            }
            break;
            
        case 0x55: // General Info 1 Answer
            if (simplePacket.payload_len >= 14) {
                deviceMgr->updateFromGeneralInfo1(currentPairingAddr, simplePacket.payload, simplePacket.payload_len);
                // Now request general info 2
                requestGeneralInfo2(device);
                handled = true;
            }
            break;
            
        case 0x57: // General Info 2 Answer
            if (simplePacket.payload_len >= 16) {
                deviceMgr->updateFromGeneralInfo2(currentPairingAddr, simplePacket.payload, simplePacket.payload_len);
                // Pairing complete!
                deviceMgr->completePairing(currentPairingAddr);
                pairingActive = false;
                addLogMessage("=== PAIRING COMPLETED SUCCESSFULLY ===");
                handled = true;
            }
            break;
            
        default:
            // During pairing, consume all packets from the device we're pairing
            // This prevents legacy pairing code from interfering
            char logMsg[128];
            snprintf(logMsg, sizeof(logMsg), 
                     "Ignoring unexpected CMD 0x%02X during pairing",
                     simplePacket.command);
            ets_printf("[Pairing] %s\n", logMsg);
            handled = true;  // Mark as handled to prevent legacy code from running
            break;
    }
    
    return handled;
}

void PairingController::process() {
    if (!pairingActive) return;
    
    Device2W* device = getCurrentPairingDevice();
    if (!device) {
        ets_printf("[Pairing] process(): No device found, cancelling\n");
        cancelPairing();
        return;
    }
    
    // Check for timeout
    if (device->hasPairingTimedOut()) {
        addLogMessage("Pairing timeout!");
        deviceMgr->failPairing(currentPairingAddr);
        pairingActive = false;
        return;
    }
    
    // Auto-progress through pairing states (with delays to allow responses)
    uint32_t now = millis();
    
    switch (device->pairingState) {
        case PairingState::DISCOVERING:
            // Send pairing broadcast and wait for CMD 0x29 response
            // Send immediately on first call, then retry every 500ms if no response
            if (now - lastStepTime > 500) {  // 500ms retry interval for faster pairing
                addLogMessage("Sending pairing broadcast (CMD 0x28), waiting for device response...");
                if (sendPairingBroadcast()) {
                    lastStepTime = now;
                }
                // Device should respond with CMD 0x29
                // handlePairingPacket will call sendLearningMode() when CMD 0x29 is received
            }
            break;
            
        case PairingState::CHALLENGE_SENT:
            // Wait for CMD 0x3C/0x2F exchange (handled in handlePairingPacket)
            if (now - lastStepTime > 5000) {
                addLogMessage("Waiting for device response...");
                lastStepTime = now;
            }
            break;
            
        case PairingState::KEY_EXCHANGED:
            // After key transfer, request device info
            if (now - lastStepTime > 1000) {  // 1 second delay before info request
                requestName(device);
                lastStepTime = now;
            }
            break;
            
        default:
            break;
    }
}

// Private workflow methods

bool PairingController::sendPairingBroadcast() {
    // CMD 0x28 - Discovery/Pairing (matches TaHoma Box, no payload)
    iohcPacket* packet = new iohcPacket();
    
    // Set up packet structure
    packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) - 1;
    packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
    packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
    packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 1;  // Both start and end frame!
    // No payload data - CMD 0x28 has no data (unlike CMD 0x2A)
    packet->buffer_length = 9;  // Just header, no payload
    
    // CtrlByte2: Set LPM and Prio flags like discover28
    packet->payload.packet.header.CtrlByte2.asByte = 0;
    packet->payload.packet.header.CtrlByte2.asStruct.LPM = 1;   // Low Power Mode flag
    packet->payload.packet.header.CtrlByte2.asStruct.Prio = 1;  // Priority flag
    
    // Source: controller address
    address myAddr = {0xBA, 0x11, 0xAD}; // TODO: Make configurable
    memcpy(packet->payload.packet.header.source, myAddr, 3);
    
    // Target: 2W broadcast address (CMD 0x28 MUST be broadcast, not targeted)
    address broadcast2W = {0x00, 0x00, 0x3B};
    memcpy(packet->payload.packet.header.target, broadcast2W, 3);
    
    packet->payload.packet.header.cmd = 0x28;  // CMD 0x28 like TaHoma Box
    packet->frequency = CHANNEL2;
    packet->repeatTime = 25;
    packet->repeat = 0;  // No hardware repeats - we'll resend in process()
    packet->lock = false;
    packet->shortPreamble = false;  // Use LONG preamble for initial discovery (device needs to wake up)
    packet->delayed = 250;  // Give enough time for device to respond (like discover28)
    
    // Debug log
    char logMsg[256];
    snprintf(logMsg, sizeof(logMsg),
             "Pairing Broadcast: CMD 0x28 to 0x%02X%02X%02X (long preamble, LPM+Prio, delayed 250ms)",
             broadcast2W[0], broadcast2W[1], broadcast2W[2]);
    addLogMessage(logMsg);
    
    bool sent = sendPacket(packet);
    
    if (sent) {
        lastStepTime = millis();
        // After broadcasting CMD 0x28, wait for device response (CMD 0x29)
        // Then we'll send CMD 0x2E (Learning Mode) to specific device
        // Note: TaHoma waits for CMD 0x29 response before sending CMD 0x2E
        // We'll handle this in handlePairingPacket when we receive CMD 0x29
    }
    return sent;
}

bool PairingController::sendLearningMode(Device2W* device) {
    // CMD 0x2E - 1W Learning mode (1-byte payload: 0x02)
    iohcPacket* packet = new iohcPacket();
    
    uint8_t payload = 0x02;
    
    // Set up packet structure
    packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) - 1;
    packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
    packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
    packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
    packet->payload.packet.header.CtrlByte1.asByte += 1;
    packet->payload.buffer[9] = payload;
    packet->buffer_length = 1 + 9;
    
    packet->payload.packet.header.CtrlByte2.asByte = 0;
    
    // Source: controller address
    address myAddr = {0xBA, 0x11, 0xAD};
    memcpy(packet->payload.packet.header.source, myAddr, 3);
    
    // Target: specific device address
    memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
    
    packet->payload.packet.header.cmd = 0x2E;
    packet->frequency = CHANNEL2;
    packet->repeatTime = 25;
    packet->repeat = 0;
    packet->lock = false;
    packet->shortPreamble = true;  // Use SHORT preamble for active pairing session
    
    // Update state
    device->pairingState = PairingState::CHALLENGE_SENT;
    
    char logMsg[128];
    snprintf(logMsg, sizeof(logMsg), 
             "Sent Learning Mode: CMD 0x2E to %02X%02X%02X",
             device->nodeAddress[0], device->nodeAddress[1], device->nodeAddress[2]);
    addLogMessage(logMsg);
    
    bool sent = sendPacket(packet);
    if (sent) {
        lastStepTime = millis();
    }
    return sent;
}

bool PairingController::sendChallengeResponse(Device2W* device, const uint8_t* deviceChallenge) {
    // CMD 0x3D - Challenge Response (6-byte response)
    iohcPacket* packet = new iohcPacket();
    
    // Store device's challenge
    deviceMgr->storeChallenge(device->nodeAddress, deviceChallenge, 6);
    
    // Generate response (for now, just copy - should be crypto operation)
    uint8_t response[6];
    memcpy(response, deviceChallenge, 6);
    // TODO: Proper crypto response generation
    
    // Set up packet structure
    packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) - 1;
    packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
    packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
    packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
    packet->payload.packet.header.CtrlByte1.asByte += 6;
    memcpy(packet->payload.buffer + 9, response, 6);
    packet->buffer_length = 6 + 9;
    
    packet->payload.packet.header.CtrlByte2.asByte = 0;
    
    // Source: controller address
    address myAddr = {0xBA, 0x11, 0xAD};
    memcpy(packet->payload.packet.header.source, myAddr, 3);
    
    // Target: specific device
    memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
    
    packet->payload.packet.header.cmd = 0x3D;
    packet->frequency = CHANNEL2;
    packet->repeatTime = 25;
    packet->repeat = 0;
    packet->lock = false;
    packet->shortPreamble = true;  // Use SHORT preamble for active pairing session
    
    char logMsg[128];
    snprintf(logMsg, sizeof(logMsg), 
             "Sent Challenge Response: CMD 0x3D to %02X%02X%02X",
             device->nodeAddress[0], device->nodeAddress[1], device->nodeAddress[2]);
    addLogMessage(logMsg);
    
    bool sent = sendPacket(packet);
    if (sent) {
        lastStepTime = millis();
    }
    return sent;
}

// OLD PROTOCOL - Not used anymore (kept for reference)
// Real pairing uses CMD 0x2A → 0x2E → 0x3C/0x3D → 0x2F, not CMD 0x28 → 0x38
/*
bool PairingController::sendChallenge(Device2W* device) {
    // CMD 0x38 - Launch Key Transfer with 6-byte challenge
    std::vector<uint8_t> challenge;
    
    // Generate random challenge
    for (int i = 0; i < 6; i++) {
        challenge.push_back(random(0, 256));
    }
    
    iohcPacket* packet = new iohcPacket();
    
    // Set up packet structure  
    packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) - 1;
    packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
    packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
    packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
    packet->payload.packet.header.CtrlByte1.asByte += challenge.size();
    memcpy(packet->payload.buffer + 9, challenge.data(), challenge.size());
    packet->buffer_length = challenge.size() + 9;
    
    packet->payload.packet.header.CtrlByte2.asByte = 0;
    
    address myAddr = {0xBA, 0x11, 0xAD};
    memcpy(packet->payload.packet.header.source, myAddr, 3);
    memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
    
    packet->payload.packet.header.cmd = 0x38;
    packet->frequency = CHANNEL2;
    packet->repeatTime = 25;
    packet->repeat = 0;
    packet->lock = false;
    
    // Store challenge in device
    deviceMgr->storeChallenge(device->nodeAddress, challenge.data(), 6);
    device->pairingState = PairingState::CHALLENGE_SENT;
    
    bool sent = sendPacket(packet);
    
    if (sent) {
        addLogMessage("Sent challenge (CMD 0x38)");
        lastStepTime = millis();
    }
    return sent;
}
*/

bool PairingController::sendAskChallenge(Device2W* device) {
    // CMD 0x31 - Ask Challenge (no payload)
    iohcPacket* packet = new iohcPacket();
    
    // Set up packet structure (no payload)
    packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) - 1;
    packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
    packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
    packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 1;  // Single frame
    packet->buffer_length = 9;  // Header only, no payload
    
    packet->payload.packet.header.CtrlByte2.asByte = 0;
    
    // Source: controller address
    address myAddr = {0xBA, 0x11, 0xAD};
    memcpy(packet->payload.packet.header.source, myAddr, 3);
    
    // Target: specific device address
    memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
    
    packet->payload.packet.header.cmd = 0x31;
    packet->frequency = CHANNEL2;
    packet->repeatTime = 25;
    packet->repeat = 0;
    packet->lock = false;
    packet->shortPreamble = true;  // Use SHORT preamble for active pairing session
    
    bool sent = sendPacket(packet);
    // DON'T delete - radio needs packet until transmission completes
    
    if (sent) {
        addLogMessage("Sent ask challenge (CMD 0x31)");
        lastStepTime = millis();
    }
    return sent;
}

bool PairingController::sendKeyTransfer(Device2W* device) {
    // CMD 0x32 - Key Transfer with 16-byte encrypted key
    
    if (!hasChallenge) {
        addLogMessage("ERROR: No challenge received! Cannot encrypt key.");
        return false;
    }
    
    // Prepare frame data for IV (first 8 bytes of payload = CMD byte only)
    std::vector<uint8_t> frame_data;
    frame_data.push_back(0x32);  // CMD 0x32
    // Pad to 8 bytes with 0x55
    for (int i = 1; i < 8; i++) {
        frame_data.push_back(0x55);
    }
    
    // Convert challenge to vector for constructInitialValue
    std::vector<uint8_t> challenge_vec(6);
    for (int i = 0; i < 6; i++) {
        challenge_vec[i] = deviceChallenge[i];
    }
    
    // Generate Initial Value (IV) according to 2W protocol
    uint8_t initial_value[16];
    constructInitialValue(frame_data, initial_value, frame_data.size(), challenge_vec, nullptr);
    
    // Print IV for debug
    char ivMsg[128];
    snprintf(ivMsg, sizeof(ivMsg), 
             "Generated IV: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
             initial_value[0], initial_value[1], initial_value[2], initial_value[3],
             initial_value[4], initial_value[5], initial_value[6], initial_value[7],
             initial_value[8], initial_value[9], initial_value[10], initial_value[11],
             initial_value[12], initial_value[13], initial_value[14], initial_value[15]);
    addLogMessage(ivMsg);
    
    // Encrypt IV with transfer key using AES-128 ECB
    AES_ctx ctx;
    AES_init_ctx(&ctx, transfert_key);
    uint8_t encrypted_iv[16];
    memcpy(encrypted_iv, initial_value, 16);
    AES_ECB_encrypt(&ctx, encrypted_iv);
    
    // XOR system key with encrypted IV to get encrypted key
    std::vector<uint8_t> keyData(16);
    for (int i = 0; i < 16; i++) {
        keyData[i] = systemKey2W[i] ^ encrypted_iv[i];
    }
    
    // Print encrypted key for debug
    char keyMsg[128];
    snprintf(keyMsg, sizeof(keyMsg), 
             "Encrypted key: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
             keyData[0], keyData[1], keyData[2], keyData[3],
             keyData[4], keyData[5], keyData[6], keyData[7],
             keyData[8], keyData[9], keyData[10], keyData[11],
             keyData[12], keyData[13], keyData[14], keyData[15]);
    addLogMessage(keyMsg);
    
    // Build packet with encrypted key
    iohcPacket* packet = new iohcPacket();
    
    // Set up packet structure
    packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) - 1;
    packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
    packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
    packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
    packet->payload.packet.header.CtrlByte1.asByte += keyData.size();
    memcpy(packet->payload.buffer + 9, keyData.data(), keyData.size());
    packet->buffer_length = keyData.size() + 9;
    
    packet->payload.packet.header.CtrlByte2.asByte = 0;
    
    address myAddr = {0xBA, 0x11, 0xAD};
    memcpy(packet->payload.packet.header.source, myAddr, 3);
    memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
    
    packet->payload.packet.header.cmd = 0x32;
    packet->frequency = CHANNEL2;
    packet->repeatTime = 25;
    packet->repeat = 0;
    packet->lock = false;
    packet->shortPreamble = true;  // Use SHORT preamble for active pairing session
    
    // Store system key in device
    deviceMgr->storeSystemKey(device->nodeAddress, systemKey2W, 16);
    
    bool sent = sendPacket(packet);
    // DON'T delete - radio needs packet until transmission completes
    
    if (sent) {
        addLogMessage("Sent key transfer (CMD 0x32)");
        lastStepTime = millis();
    }
    return sent;
}

bool PairingController::requestName(Device2W* device) {
    // CMD 0x50 - Get Name (no parameters)
    std::vector<uint8_t> toSend = {};
    
    iohcPacket* packet = new iohcPacket();
    
    packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) - 1;
    packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
    packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
    packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
    packet->buffer_length = 9;
    
    packet->payload.packet.header.CtrlByte2.asByte = 0;
    
    address myAddr = {0xBA, 0x11, 0xAD};
    memcpy(packet->payload.packet.header.source, myAddr, 3);
    memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
    
    packet->payload.packet.header.cmd = 0x50;
    packet->frequency = CHANNEL2;
    packet->repeatTime = 25;
    packet->repeat = 0;
    packet->lock = false;
    packet->shortPreamble = true;  // Use SHORT preamble for active pairing session
    
    bool sent = sendPacket(packet);
    // DON'T delete - radio needs packet until transmission completes
    
    if (sent) {
        addLogMessage("Requested name (CMD 0x50)");
        // Note: Info requests will be sent when responses are received
        // No blocking delays!
    }
    return sent;
}

bool PairingController::requestGeneralInfo1(Device2W* device) {
    // CMD 0x54 - Get General Info 1 (no parameters)
    std::vector<uint8_t> toSend = {};
    
    iohcPacket* packet = new iohcPacket();
    
    packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) - 1;
    packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
    packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
    packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
    packet->buffer_length = 9;
    
    packet->payload.packet.header.CtrlByte2.asByte = 0;
    
    address myAddr = {0xBA, 0x11, 0xAD};
    memcpy(packet->payload.packet.header.source, myAddr, 3);
    memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
    
    packet->payload.packet.header.cmd = 0x54;
    packet->frequency = CHANNEL2;
    packet->repeatTime = 25;
    packet->repeat = 0;
    packet->lock = false;
    packet->shortPreamble = true;  // Use SHORT preamble for active pairing session
    
    bool sent = sendPacket(packet);
    // DON'T delete - radio needs packet until transmission completes
    
    if (sent) {
        addLogMessage("Requested General Info 1 (CMD 0x54)");
        // Note: Info 2 request will be sent when response is received
    }
    return sent;
}

bool PairingController::requestGeneralInfo2(Device2W* device) {
    // CMD 0x56 - Get General Info 2 (no parameters)
    std::vector<uint8_t> toSend = {};
    
    iohcPacket* packet = new iohcPacket();
    
    packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) - 1;
    packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
    packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
    packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
    packet->buffer_length = 9;
    
    packet->payload.packet.header.CtrlByte2.asByte = 0;
    
    address myAddr = {0xBA, 0x11, 0xAD};
    memcpy(packet->payload.packet.header.source, myAddr, 3);
    memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
    
    packet->payload.packet.header.cmd = 0x56;
    packet->frequency = CHANNEL2;
    packet->repeatTime = 25;
    packet->repeat = 0;
    packet->lock = false;
    packet->shortPreamble = true;  // Use SHORT preamble for active pairing session
    
    bool sent = sendPacket(packet);
    // DON'T delete - radio needs packet until transmission completes
    
    if (sent) {
        addLogMessage("Requested General Info 2 (CMD 0x56)");
    }
    return sent;
}

bool PairingController::sendPacket(iohcPacket* packet) {
    if (!radio) {
        addLogMessage("ERROR: Radio not initialized!");
        delete packet;  // Clean up since we won't send it
        return false;
    }
    
    // Check if radio is busy transmitting
    // If so, don't send (prevents creating orphaned packets)
    if (radio->radioState == iohcRadio::RadioState::TX) {
        ets_printf("PairingController: Radio busy, deleting packet to prevent leak\n");
        delete packet;
        return false;  // Will retry later via process() timeout logic
    }
    
    // Delete previous packet if transmission completed
    // (radio state is RX, so previous transmission is done)
    if (lastSentPacket != nullptr) {
        delete lastSentPacket;
        lastSentPacket = nullptr;
    }
    
    // Radio->send expects a vector
    std::vector<iohcPacket*> packets;
    packets.push_back(packet);
    radio->send(packets);
    
    // Store pointer to track this packet
    lastSentPacket = packet;
    
    return true;
}

