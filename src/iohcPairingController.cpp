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
    
    // Log raw packet details for debugging
    ets_printf("[Pairing] RAW packet: CMD=0x%02X SF=%d EF=%d from=%02X%02X%02X len=%d\n",
               packet->payload.packet.header.cmd,
               packet->payload.packet.header.CtrlByte1.asStruct.StartFrame,
               packet->payload.packet.header.CtrlByte1.asStruct.EndFrame,
               packet->payload.packet.header.source[0],
               packet->payload.packet.header.source[1],
               packet->payload.packet.header.source[2],
               packet->buffer_length);
    
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
                         "✅ Received CMD 0x29 from %02X%02X%02X - Device discovered!",
                         simplePacket.source[0], simplePacket.source[1], simplePacket.source[2]);
                addLogMessage(logMsg);
                
                // Device responded to our CMD 0x28 broadcast
                // Now send CMD 0x2C (Alive Check) per TaHoma protocol
                handled = sendAliveCheck(device);
            }
            break;
            
        case 0x2D: // Alive Check Response (device responds to CMD 0x2C)
            if (device->pairingState == PairingState::ALIVE_CHECK) {
                addLogMessage("✅ Alive check passed (CMD 0x2D)");
                
                // Device is alive and ready
                // Now send CMD 0x2E (Learning Mode)
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
                } else if (statusCode == 0x76) {
                    // Status 0x76 - Key transfer rejected (wrong key/authentication failed)
                    ets_printf("[Pairing] ❌ Device rejected key transfer (status 0x76)\n");
                    ets_printf("[Pairing] This may indicate:\n");
                    ets_printf("[Pairing]   - Wrong encryption key\n");
                    ets_printf("[Pairing]   - Missing challenge (CMD 0x3C)\n");
                    ets_printf("[Pairing]   - Device requires different pairing method\n");
                    addLogMessage("Key transfer rejected by device (0x76)");
                    // Cancel pairing - this won't succeed without correct key
                    cancelPairing();
                } else {
                    snprintf(logMsg, sizeof(logMsg), "Device sent error status 0x%02X", statusCode);
                    addLogMessage(logMsg);
                }
                handled = true;  // Mark as handled to prevent retry loop
            }
            break;
            
        case 0x2F: // Pairing Confirmation
            if (simplePacket.payload_len >= 1) {
                addLogMessage("Received pairing confirmation (CMD 0x2F)");
                
                // Check if we received challenge (CMD 0x3C) before confirmation
                if (hasChallenge) {
                    addLogMessage("Challenge received, ready for key exchange");
                } else {
                    addLogMessage("No challenge received - device may not require it");
                }
                
                // Move to PAIRING_CONFIRMED state
                // Key transfer will be sent by process() loop
                device->pairingState = PairingState::PAIRING_CONFIRMED;
                lastStepTime = millis();
                handled = true;
            }
            break;
            
        case 0x3C: // Device Challenge (2W Protocol)
            if (simplePacket.payload_len >= 6) {
                // Store the 6-byte challenge from the device
                memcpy(deviceChallenge, simplePacket.payload, 6);
                hasChallenge = true;
                
                char logMsg[128];
                snprintf(logMsg, sizeof(logMsg), 
                         "✅ Received device challenge (CMD 0x3C): %02X%02X%02X%02X%02X%02X",
                         deviceChallenge[0], deviceChallenge[1], deviceChallenge[2],
                         deviceChallenge[3], deviceChallenge[4], deviceChallenge[5]);
                addLogMessage(logMsg);
                
                // Update state to indicate challenge received
                device->pairingState = PairingState::CHALLENGE_RECEIVED;
                
                // Send CMD 0x3D (challenge response) - TaHoma protocol
                addLogMessage("Sending challenge response (CMD 0x3D)...");
                handled = sendChallengeResponse(device);
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
                
                // Store system key in device before completing pairing
                if (hasSystemKey) {
                    memcpy(device->systemKey, systemKey2W, 16);
                    device->hasSystemKey = true;
                    addLogMessage("✅ Stored system key in device");
                } else {
                    addLogMessage("⚠️  No system key to store (device may not require it)");
                }
                
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
            // Add minimum delay between attempts to prevent tight loops
            if (now - lastStepTime > 500) {  // 500ms retry interval
                // Only log every few seconds to avoid spam
                static uint32_t lastLogTime = 0;
                if (now - lastLogTime > 3000) {
                    addLogMessage("Sending pairing broadcast (CMD 0x28), waiting for device response...");
                    lastLogTime = now;
                }
                if (sendPairingBroadcast()) {
                    lastStepTime = now;
                }
                // Device should respond with CMD 0x29
                // handlePairingPacket will call sendAliveCheck() when CMD 0x29 is received
            }
            break;
            
        case PairingState::ALIVE_CHECK:
            // Wait for CMD 0x2D (alive check response)
            // handlePairingPacket will call sendLearningMode() when CMD 0x2D is received
            if (now - lastStepTime > 5000) {
                addLogMessage("Waiting for alive check response (CMD 0x2D)...");
                lastStepTime = now;
            }
            break;
            
        case PairingState::LEARNING_MODE:
            // Wait for CMD 0x3C (device challenge)
            // handlePairingPacket will call sendChallengeResponse() when CMD 0x3C is received
            if (now - lastStepTime > 5000) {
                addLogMessage("Waiting for device challenge (CMD 0x3C)...");
                lastStepTime = now;
            }
            break;
            
        case PairingState::CHALLENGE_RECEIVED:
            // Wait for CMD 0x2F (pairing confirmation) after sending CMD 0x3D
            // handlePairingPacket will handle CMD 0x2F
            if (now - lastStepTime > 5000) {
                addLogMessage("Waiting for pairing confirmation (CMD 0x2F)...");
                lastStepTime = now;
            }
            break;
            
        case PairingState::PAIRING_CONFIRMED:
            // TaHoma flow: After CMD 0x2F, start gathering device info
            // CMD 0x50 (Name), CMD 0x54 (General Info 1), CMD 0x56 (General Info 2)
            if (now - lastStepTime > 200) {  // 200ms delay after CMD 0x2F
                ets_printf("[Pairing] PAIRING_CONFIRMED - requesting device info...\n");
                requestName(device);
                // Move to KEY_EXCHANGED state immediately to prevent re-requesting
                // The handlers will process responses (CMD 0x51, 0x55, 0x57) and complete pairing
                device->pairingState = PairingState::KEY_EXCHANGED;
                lastStepTime = now;
            }
            break;
            
        case PairingState::KEY_EXCHANGED:
            // Wait for device info responses (CMD 0x51, 0x55, 0x57)
            // Handlers will complete pairing when CMD 0x57 is received
            if (now - lastStepTime > 10000) {
                addLogMessage("Timeout waiting for device info responses");
                cancelPairing();
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

bool PairingController::sendAliveCheck(Device2W* device) {
    // CMD 0x2C - Actuator Alive Check (no payload)
    iohcPacket* packet = new iohcPacket();
    
    // Set up packet structure (no payload)
    packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) - 1;
    packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
    packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
    packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
    packet->payload.packet.header.CtrlByte1.asByte += 0;  // No payload
    packet->buffer_length = 9;  // Just header
    
    packet->payload.packet.header.CtrlByte2.asByte = 0;
    
    // Source: controller address
    address myAddr = {0xBA, 0x11, 0xAD};
    memcpy(packet->payload.packet.header.source, myAddr, 3);
    
    // Target: specific device address
    memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
    
    packet->payload.packet.header.cmd = 0x2C;
    packet->frequency = CHANNEL2;
    packet->repeatTime = 25;
    packet->repeat = 0;
    packet->lock = false;
    packet->shortPreamble = true;
    
    bool sent = sendPacket(packet);
    if (sent) {
        device->pairingState = PairingState::ALIVE_CHECK;
        lastStepTime = millis();
        
        char logMsg[128];
        snprintf(logMsg, sizeof(logMsg), 
                 "Sent Alive Check: CMD 0x2C to %02X%02X%02X",
                 device->nodeAddress[0], device->nodeAddress[1], device->nodeAddress[2]);
        addLogMessage(logMsg);
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
    
    bool sent = sendPacket(packet);
    if (sent) {
        // Only update state if packet was successfully sent
        device->pairingState = PairingState::LEARNING_MODE;
        lastStepTime = millis();
        
        char logMsg[128];
        snprintf(logMsg, sizeof(logMsg), 
                 "Sent Learning Mode: CMD 0x2E to %02X%02X%02X (waiting for CMD 0x3C challenge)",
                 device->nodeAddress[0], device->nodeAddress[1], device->nodeAddress[2]);
        addLogMessage(logMsg);
    }
    return sent;
}

bool PairingController::sendChallengeResponse(Device2W* device) {
    // CMD 0x3D - Challenge Response (6-byte response)
    // Uses the stored deviceChallenge from CMD 0x3C
    
    if (!hasChallenge) {
        addLogMessage("ERROR: Cannot send CMD 0x3D without challenge!");
        return false;
    }
    
    if (!hasSystemKey) {
        addLogMessage("WARNING: No system key - using dummy response for pairing");
        // During initial pairing, we may not have system key yet
        // Continue with simple copy for pairing process
    }
    
    iohcPacket* packet = new iohcPacket();
    
    // Generate response using proper crypto if we have the system key
    uint8_t response[6];
    
    if (hasSystemKey) {
        // Create frame data for MAC calculation (just the command byte for CMD 0x3D)
        std::vector<uint8_t> frame_data;
        frame_data.push_back(0x3D);  // Command
        
        // Generate MAC using 2W HMAC algorithm
        iohcCrypto::create_2W_hmac(response, deviceChallenge, systemKey2W, frame_data);
        addLogMessage("✅ Generated proper CMD 0x3D MAC using system key");
    } else {
        // Fallback for pairing when we don't have key yet
        memcpy(response, deviceChallenge, 6);
        addLogMessage("⚠️  Using simple challenge copy (pairing mode)");
    }
    
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
             "Sent Challenge Response: CMD 0x3D to %02X%02X%02X (MAC: %02X%02X%02X%02X%02X%02X)",
             device->nodeAddress[0], device->nodeAddress[1], device->nodeAddress[2],
             response[0], response[1], response[2], response[3], response[4], response[5]);
    addLogMessage(logMsg);
    
    bool sent = sendPacket(packet);
    if (sent) {
        // After sending CMD 0x3D, wait for CMD 0x2F (pairing confirmation)
        // State stays as CHALLENGE_RECEIVED until we get CMD 0x2F
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

// DEPRECATED: CMD 0x31 not used in TaHoma protocol
// TaHoma flow: Device automatically sends CMD 0x3C after receiving CMD 0x2E
// No need to explicitly ask for challenge
/*
bool PairingController::sendAskChallenge(Device2W* device) {
    // CMD 0x31 - Ask Challenge (no payload) - NOT USED IN TAHOMA FLOW
    return false;
}
*/

bool PairingController::sendKeyTransfer(Device2W* device) {
    // CMD 0x32 - Key Transfer with 16-byte encrypted key
    
    // If no challenge received, use zero challenge (some devices skip CMD 0x3C)
    if (!hasChallenge) {
        ets_printf("[Pairing] No challenge received, using zero challenge\n");
        memset(deviceChallenge, 0x00, 6);
        hasChallenge = true;  // Proceed anyway
    }
    
    // Debug: Print challenge being used
    char challengeMsg[128];
    snprintf(challengeMsg, sizeof(challengeMsg), 
             "Using challenge: %02X%02X%02X%02X%02X%02X",
             deviceChallenge[0], deviceChallenge[1], deviceChallenge[2],
             deviceChallenge[3], deviceChallenge[4], deviceChallenge[5]);
    ets_printf("[Pairing] %s\n", challengeMsg);
    
    // Debug: Print system key being used
    char sysKeyMsg[128];
    snprintf(sysKeyMsg, sizeof(sysKeyMsg), 
             "System key: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
             systemKey2W[0], systemKey2W[1], systemKey2W[2], systemKey2W[3],
             systemKey2W[4], systemKey2W[5], systemKey2W[6], systemKey2W[7],
             systemKey2W[8], systemKey2W[9], systemKey2W[10], systemKey2W[11],
             systemKey2W[12], systemKey2W[13], systemKey2W[14], systemKey2W[15]);
    addLogMessage(sysKeyMsg);
    
    // Prepare frame data for IV construction
    // According to protocol: "Controller creates an initial value based on last frame and the specified challenge"
    // The "last frame" is the CMD 0x32 frame being sent (just the command byte)
    std::vector<uint8_t> frame_data;
    frame_data.push_back(0x32);  // CMD 0x32
    // Note: Padding to 8 bytes with 0x55 is handled inside constructInitialValue
    
    // Convert challenge to vector for constructInitialValue
    std::vector<uint8_t> challenge_vec(6);
    for (int i = 0; i < 6; i++) {
        challenge_vec[i] = deviceChallenge[i];
    }
    
    // Step 1: Generate Initial Value (IV) according to 2W protocol
    // IV structure: [frame_data (8 bytes), checksum (2 bytes), challenge (6 bytes)]
    uint8_t initial_value[16];
    constructInitialValue(frame_data, initial_value, frame_data.size(), challenge_vec, nullptr);
    
    // Debug: Print generated IV
    char ivMsg[128];
    snprintf(ivMsg, sizeof(ivMsg), 
             "Generated IV: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
             initial_value[0], initial_value[1], initial_value[2], initial_value[3],
             initial_value[4], initial_value[5], initial_value[6], initial_value[7],
             initial_value[8], initial_value[9], initial_value[10], initial_value[11],
             initial_value[12], initial_value[13], initial_value[14], initial_value[15]);
    addLogMessage(ivMsg);
    
    // Step 2: Encrypt IV with transfer key using AES-128 ECB
    AES_ctx ctx;
    AES_init_ctx(&ctx, transfert_key);
    uint8_t encrypted_iv[16];
    memcpy(encrypted_iv, initial_value, 16);
    AES_ECB_encrypt(&ctx, encrypted_iv);
    
    // Debug: Print encrypted IV
    char encIvMsg[128];
    snprintf(encIvMsg, sizeof(encIvMsg), 
             "Encrypted IV: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
             encrypted_iv[0], encrypted_iv[1], encrypted_iv[2], encrypted_iv[3],
             encrypted_iv[4], encrypted_iv[5], encrypted_iv[6], encrypted_iv[7],
             encrypted_iv[8], encrypted_iv[9], encrypted_iv[10], encrypted_iv[11],
             encrypted_iv[12], encrypted_iv[13], encrypted_iv[14], encrypted_iv[15]);
    addLogMessage(encIvMsg);
    
    // Step 3: XOR system key with encrypted IV to get encrypted key payload
    // This is the key that will be sent in CMD 0x32
    std::vector<uint8_t> keyData(16);
    for (int i = 0; i < 16; i++) {
        keyData[i] = systemKey2W[i] ^ encrypted_iv[i];
    }
    
    // Debug: Print final encrypted key to be sent
    char keyMsg[128];
    snprintf(keyMsg, sizeof(keyMsg), 
             "Encrypted key payload: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
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
    
    ets_printf("[Pairing] Calling sendPacket() for CMD 0x32...\n");
    bool sent = sendPacket(packet);
    // DON'T delete - radio needs packet until transmission completes
    
    if (sent) {
        ets_printf("[Pairing] sendPacket() returned SUCCESS for CMD 0x32\n");
        addLogMessage("Sent key transfer (CMD 0x32)");
        lastStepTime = millis();
    } else {
        ets_printf("[Pairing] sendPacket() returned FAILURE for CMD 0x32\n");
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
    // Allow sending during RX and PAYLOAD states (PAYLOAD transitions to RX quickly)
    if (radio->radioState == iohcRadio::RadioState::TX ||
        radio->radioState == iohcRadio::RadioState::PREAMBLE) {
        ets_printf("PairingController: Radio busy transmitting (state=%d), will retry.\n", 
                   (int)radio->radioState);
        delete packet;
        // Set lastStepTime to add delay before retry
        lastStepTime = millis();
        return false;  // Caller should not change pairing state
    }
    
    // IMPORTANT: Do NOT delete lastSentPacket here!
    // The radio may still be using it internally
    // Let it be cleaned up on next successful send or when pairing completes
    // This prevents memory corruption when radio is processing packets
    
    // Radio->send expects a vector
    std::vector<iohcPacket*> packets;
    packets.push_back(packet);
    
    // CRITICAL: Do NOT delete lastSentPacket even after sending!
    // The radio holds a pointer and may read from it asynchronously.
    // Memory will be cleaned up when pairing completes or on next boot.
    // Small memory leak but prevents corruption.
    
    // Store pointer to track this packet
    lastSentPacket = packet;
    
    // Send packet to radio
    radio->send(packets);
    
    return true;
}

void PairingController::verifyCryptoImplementation() {
    // Test vectors from protocol documentation (linklayer.md)
    // Example: Stack key push - CMD 0x32
    // Stack key: 01020304050607080910111213141516
    // Challenge: 123456789ABC
    // Expected encrypted output: 102E49A16D3B69726F3192CF17534AD9
    
    addLogMessage("=== Verifying Crypto Implementation ===");
    
    // Test parameters from documentation
    uint8_t test_stack_key[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                   0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
    uint8_t test_challenge[6] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    
    // Create IV for CMD 0x32
    std::vector<uint8_t> frame_data;
    frame_data.push_back(0x32);
    
    std::vector<uint8_t> challenge_vec(6);
    memcpy(challenge_vec.data(), test_challenge, 6);
    
    uint8_t initial_value[16];
    constructInitialValue(frame_data, initial_value, frame_data.size(), challenge_vec, nullptr);
    
    char msg[256];
    snprintf(msg, sizeof(msg), 
             "Test IV: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
             initial_value[0], initial_value[1], initial_value[2], initial_value[3],
             initial_value[4], initial_value[5], initial_value[6], initial_value[7],
             initial_value[8], initial_value[9], initial_value[10], initial_value[11],
             initial_value[12], initial_value[13], initial_value[14], initial_value[15]);
    addLogMessage(msg);
    
    // Encrypt with transfer key
    AES_ctx ctx;
    AES_init_ctx(&ctx, transfert_key);
    uint8_t encrypted_iv[16];
    memcpy(encrypted_iv, initial_value, 16);
    AES_ECB_encrypt(&ctx, encrypted_iv);
    
    snprintf(msg, sizeof(msg), 
             "Encrypted IV: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
             encrypted_iv[0], encrypted_iv[1], encrypted_iv[2], encrypted_iv[3],
             encrypted_iv[4], encrypted_iv[5], encrypted_iv[6], encrypted_iv[7],
             encrypted_iv[8], encrypted_iv[9], encrypted_iv[10], encrypted_iv[11],
             encrypted_iv[12], encrypted_iv[13], encrypted_iv[14], encrypted_iv[15]);
    addLogMessage(msg);
    
    // XOR with test stack key
    uint8_t result[16];
    for (int i = 0; i < 16; i++) {
        result[i] = test_stack_key[i] ^ encrypted_iv[i];
    }
    
    snprintf(msg, sizeof(msg), 
             "Final key: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
             result[0], result[1], result[2], result[3],
             result[4], result[5], result[6], result[7],
             result[8], result[9], result[10], result[11],
             result[12], result[13], result[14], result[15]);
    addLogMessage(msg);
    
    // Compare with expected value from docs
    uint8_t expected[16] = {0x10, 0x2E, 0x49, 0xA1, 0x6D, 0x3B, 0x69, 0x72,
                            0x6F, 0x31, 0x92, 0xCF, 0x17, 0x53, 0x4A, 0xD9};
    
    bool matches = memcmp(result, expected, 16) == 0;
    if (matches) {
        addLogMessage("✅ Crypto implementation VERIFIED - matches protocol docs!");
    } else {
        addLogMessage("❌ Crypto implementation MISMATCH - check algorithm!");
        snprintf(msg, sizeof(msg), 
                 "Expected: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                 expected[0], expected[1], expected[2], expected[3],
                 expected[4], expected[5], expected[6], expected[7],
                 expected[8], expected[9], expected[10], expected[11],
                 expected[12], expected[13], expected[14], expected[15]);
        addLogMessage(msg);
    }
    
    addLogMessage("=== Crypto Verification Complete ===");
}

