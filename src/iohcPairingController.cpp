#include "iohcPairingController.h"
#include "iohcPacket.h"
#include "iohcRadio.h"
#include "iohcCryptoHelpers.h"
#include "crypto2Wutils.h"
#include "log_buffer.h"
#include "user_config.h"
#include "iohcOtherDevice2W.h"

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

void PairingController::enableAutoPairMode() {
    autoPairMode = true;
    addLogMessage("✨ Auto-pairing mode ENABLED - will pair first device that responds");
}

void PairingController::disableAutoPairMode() {
    autoPairMode = false;
    addLogMessage("Auto-pairing mode disabled");
}

Device2W* PairingController::getCurrentPairingDevice() {
    if (!pairingActive) return nullptr;
    return deviceMgr->getDevice(currentPairingAddr);
}

bool PairingController::handlePairingPacket(iohcPacket* packet) {
    // Check for auto-pair mode - CMD 0x29 (Discovery Response)
    if (autoPairMode && !pairingActive && packet->payload.packet.header.cmd == 0x29) {
        // Device responded to discovery broadcast - automatically start pairing!
        address deviceAddr;
        memcpy(deviceAddr, packet->payload.packet.header.source, 3);
        
        // Stop discovery broadcasts since device was found
        iohcOtherDevice2W::getInstance()->notifyDeviceFound();
        
        Serial.println();
        Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        Serial.printf(" DEVICE DETECTED - Address: %02X%02X%02X\n", deviceAddr[0], deviceAddr[1], deviceAddr[2]);
        Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        Serial.println();
        
        // Disable auto-pair mode (we found our device)
        disableAutoPairMode();
        
        // Start pairing this device
        if (startPairing(deviceAddr)) {
            // Continue processing this CMD 0x29 packet below
            // Don't return yet - let it fall through to normal pairing flow
        } else {
            Serial.println("❌ Failed to start pairing!");
            return false;
        }
    }
    
    if (!pairingActive) {
        return false;
    }

    // Convert to simple packet structure
    SimplePairingPacket simplePacket;
    simplePacket.fromIohcPacket(packet);
    
    // Check if packet is from device we're pairing
    if (memcmp(simplePacket.source, currentPairingAddr, 3) != 0) {
        return false;
    }
    
    Device2W* device = getCurrentPairingDevice();
    if (!device) return false;
    
    bool handled = false;

    Serial.printf("[Pairing] Current state: %s, CMD: 0x%02X\n", device->getPairingStateStr().c_str(), simplePacket.command);
    switch (simplePacket.command) {
        case 0x29: // Discovery Response (device responds to CMD 0x28)
            if (device->pairingState == PairingState::DISCOVERING) {
                addLogMessage("✅ Device responded to our CMD 0x28 broadcast");
                // Send CMD 0x31 (Ask Challenge) to see what device responds with
                addLogMessage("Sending CMD 0x31 (Ask Challenge)...");
                device->pairingState = PairingState::ASKING_CHALLENGE;
                
                if (sendAskChallenge(device)) {
                    lastStepTime = millis();
                    handled = true;
                    clearRetry();  // Success, clear any pending retry
                } else {
                    addLogMessage("❌ Failed to send CMD 0x31 - scheduling auto-retry...");
                    // Schedule auto-retry
                    scheduleRetry([this, device]() {
                        return sendAskChallenge(device);
                    });
                    handled = true;  // Mark as handled to prevent other code from interfering
                }
            }
            break;
            
        case 0x2D: // Alive Check Response (device responds to CMD 0x2C)
            if (device->pairingState == PairingState::ALIVE_CHECK) {
                addLogMessage("✅ Alive check passed (CMD 0x2D)");
                // After alive check, broadcast CMD 0x2A 4 times
                device->pairingState = PairingState::BROADCASTING_2A;
                cmd2ABroadcastCount = 0;  // Reset counter
                lastStepTime = millis();
                handled = true;
            }
            break;
            
        case 0x37: // Priority Address Response (device responds to CMD 0x36)
            Serial.printf("[Pairing] CMD 0x37 received! State=%d, payload_len=%d\n", 
                         (int)device->pairingState, simplePacket.payload_len);
            // Accept CMD 0x37 in either WAITING_BEFORE_LEARNING or LEARNING_MODE state
            if (device->pairingState == PairingState::WAITING_BEFORE_LEARNING || 
                device->pairingState == PairingState::LEARNING_MODE) {
                if (simplePacket.payload_len >= 3) {
                    char logMsg[128];
                    snprintf(logMsg, sizeof(logMsg), 
                             "✅ Priority Address received (CMD 0x37): %02X%02X%02X",
                             simplePacket.payload[0], simplePacket.payload[1], simplePacket.payload[2]);
                    addLogMessage(logMsg);
                    // Immediately send challenge (CMD 0x3C) to device
                    addLogMessage("Sending challenge (CMD 0x3C) to device...");
                    if (sendChallengeToPair(device)) {
                        device->pairingState = PairingState::CHALLENGE_SENT;
                        lastStepTime = millis();
                    } else {
                        addLogMessage("❌ Failed to send CMD 0x3C");
                        cancelPairing();
                    }
                    handled = true;
                }
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
            
        case 0x2F: // Pairing Confirmation (device responds to CMD 0x3D)
            if (simplePacket.payload_len >= 1) {
                uint8_t confirmationStatus = simplePacket.payload[0];
                char logMsg[128];
                snprintf(logMsg, sizeof(logMsg), 
                         "✅ Received pairing confirmation (CMD 0x2F) status: 0x%02X",
                         confirmationStatus);
                addLogMessage(logMsg);
                
                if (confirmationStatus == 0x02) {
                    addLogMessage("🎉 Pairing authentication successful!");
                    // After CMD 0x3D response, move to device info gathering
                    device->pairingState = PairingState::KEY_EXCHANGED;
                    lastStepTime = millis();
                    handled = true;
                } else {
                    addLogMessage("❌ Pairing authentication failed");
                    cancelPairing();
                    handled = true;
                }
            }
            break;
            
        case 0x32: // Key Transfer from device (response to CMD 0x38) - NOT used in Tahoma flow
            if (simplePacket.payload_len >= 16) {
                addLogMessage("✅ Received CMD 0x32 (device key transfer)");
                // Device is sending us its key - this is the Pull method
                // Store the encrypted key and decrypt it
                // TODO: Process device key if needed
                // For now, we'll challenge the device to verify the key
                device->pairingState = PairingState::KEY_EXCHANGED;
                
                // Challenge the device to authenticate (send CMD 0x3C)
                addLogMessage("Challenging device to authenticate key...");
                // The challenge will be sent in the process() loop
                lastStepTime = millis();
                handled = true;
            }
            break;
            
        case 0x3C: // Challenge from device (response to CMD 0x31 or challenging a sent command)
            if (device->pairingState == PairingState::ASKING_CHALLENGE) {
                if (simplePacket.payload_len >= 6) {
                    // Store the 6-byte challenge from the device
                    memcpy(deviceChallenge, simplePacket.payload, 6);
                    hasChallenge = true;
                    
                    char logMsg[128];
                    snprintf(logMsg, sizeof(logMsg), 
                             "✅ Received device challenge (CMD 0x3C) after CMD 0x31: %02X%02X%02X%02X%02X%02X",
                             deviceChallenge[0], deviceChallenge[1], deviceChallenge[2],
                             deviceChallenge[3], deviceChallenge[4], deviceChallenge[5]);
                    addLogMessage(logMsg);
                    
                    // Device sent challenge - respond with CMD 0x32 (Key Transfer)
                    addLogMessage("Sending CMD 0x32 (Key Transfer) with encrypted stack key...");
                    if (sendKeyTransfer(device)) {
                        device->pairingState = PairingState::CHALLENGE_RECEIVED;
                        lastStepTime = millis();
                        handled = true;
                    } else {
                        addLogMessage("❌ Failed to send CMD 0x32");
                        // Schedule retry
                        scheduleRetry([this, device]() {
                            return sendKeyTransfer(device);
                        });
                        handled = true;
                    }
                } else {
                    addLogMessage("⚠️ CMD 0x3C received but payload too short");
                }
            }
            else if (device->pairingState == PairingState::CHALLENGE_RECEIVED || 
                     device->pairingState == PairingState::KEY_EXCHANGED) {
                // Device is challenging our CMD 0x32 (Key Transfer) or CMD 0x36 (Address Request)
                if (simplePacket.payload_len >= 6) {
                    memcpy(deviceChallenge, simplePacket.payload, 6);
                    hasChallenge = true;
                    
                    char logMsg[128];
                    snprintf(logMsg, sizeof(logMsg), 
                             "✅ Device challenging CMD 0x%02X: %02X%02X%02X%02X%02X%02X",
                             commandBeingAuthenticated,
                             deviceChallenge[0], deviceChallenge[1], deviceChallenge[2],
                             deviceChallenge[3], deviceChallenge[4], deviceChallenge[5]);
                    addLogMessage(logMsg);
                    
                    // Send CMD 0x3D challenge response
                    addLogMessage("Sending CMD 0x3D challenge response...");
                    if (sendChallengeResponse(device)) {
                        addLogMessage("✅ Sent CMD 0x3D authentication response");
                        lastStepTime = millis();
                        handled = true;
                    } else {
                        addLogMessage("❌ Failed to send CMD 0x3D");
                        handled = true;
                    }
                } else {
                    addLogMessage("⚠️ CMD 0x3C received but payload too short");
                }
            }
            break;
            
        case 0x3D: // Challenge Response from device (device responds to our CMD 0x3C)
            if (simplePacket.payload_len >= 6) {
                char logMsg[128];
                snprintf(logMsg, sizeof(logMsg), 
                         "✅ Received challenge response (CMD 0x3D): %02X%02X%02X%02X%02X%02X",
                         simplePacket.payload[0], simplePacket.payload[1], simplePacket.payload[2],
                         simplePacket.payload[3], simplePacket.payload[4], simplePacket.payload[5]);
                addLogMessage(logMsg);
                
                if (device->pairingState == PairingState::CHALLENGE_SENT) {
                    // Device responded to our challenge - now request device info
                    addLogMessage("Challenge authenticated! Requesting device info...");
                    device->pairingState = PairingState::KEY_EXCHANGED;
                    lastStepTime = millis();
                    handled = true;
                }
            }
            break;
            
        case 0x33: // Key Transfer Ack
            addLogMessage("✅ Key transfer acknowledged (CMD 0x33)!");
            
            // send device address request
            
            
            // Mark pairing as completed
            addLogMessage("🎉 Pairing completed successfully!");
            if (!sendPriorityAddressRequest(device)) {
                addLogMessage("❌ Failed to send CMD 0x36");
            }
            
            // Store system key in device
            if (hasSystemKey) {
                memcpy(device->systemKey, systemKey2W, 16);
                device->hasSystemKey = true;
                addLogMessage("✅ Stored system key in device");
            }
            
            deviceMgr->completePairing(currentPairingAddr);
            pairingActive = false;
            device->pairingState = PairingState::PAIRED;
            handled = true;
            break;
            
        case 0x51: // Name Answer
            if (simplePacket.payload_len >= 16) {
                String name = "";
                for (int i = 0; i < 16 && simplePacket.payload[i] != 0; i++) {
                    name += (char)simplePacket.payload[i];
                }
                addLogMessage("Name received: " + name);
                deviceMgr->updateFromNameAnswer(currentPairingAddr, name);
                // Now request general info 1
                requestGeneralInfo1(device);
                handled = true;
            } else {
                addLogMessage("❌ Name answer too short");
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

// Auto-retry helper functions
void PairingController::scheduleRetry(std::function<bool()> retryFunc) {
    pendingRetryFunc = retryFunc;
    retryCount = 0;
    lastRetryTime = millis();
    ets_printf("[Pairing] Scheduled auto-retry (will attempt up to %d times)\n", MAX_RETRIES);
}

void PairingController::clearRetry() {
    pendingRetryFunc = nullptr;
    retryCount = 0;
}

void PairingController::processRetry() {
    if (!pendingRetryFunc) return;
    
    uint32_t now = millis();
    
    // Check if enough time has passed since last retry
    if (now - lastRetryTime < RETRY_DELAY_MS) {
        return;
    }
    
    // Try to execute the retry function
    if (pendingRetryFunc()) {
        // Success! Clear the retry
        ets_printf("[Pairing] Auto-retry succeeded after %d attempts\n", retryCount + 1);
        clearRetry();
    } else {
        // Failed, increment counter
        retryCount++;
        lastRetryTime = now;
        
        if (retryCount >= MAX_RETRIES) {
            ets_printf("[Pairing] Auto-retry failed after %d attempts, giving up\n", MAX_RETRIES);
            addLogMessage("⚠️  Auto-retry exhausted, operation failed");
            clearRetry();
        } else {
            ets_printf("[Pairing] Auto-retry attempt %d/%d...\n", retryCount, MAX_RETRIES);
        }
    }
}

void PairingController::process() {
    if (!pairingActive) return;
    
    // Process any pending retries first
    processRetry();
    
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
           
        case PairingState::BROADCASTING_2A:
            // After alive check, broadcast CMD 0x2A 4 times
            if (now - lastStepTime > 200) {  // 200ms between broadcasts
                if (cmd2ABroadcastCount < 4) {
                    if (send2ABroadcast()) {
                        cmd2ABroadcastCount++;
                        char logMsg[64];
                        snprintf(logMsg, sizeof(logMsg), "CMD 0x2A broadcast %d/4 sent", cmd2ABroadcastCount);
                        addLogMessage(logMsg);
                        lastStepTime = now;
                    } else {
                        addLogMessage("❌ Failed to send CMD 0x2A broadcast");
                    }
                } else {
                    // All 4 broadcasts sent, move to next step
                    addLogMessage("✅ All CMD 0x2A broadcasts sent");
                    device->pairingState = PairingState::WAITING_BEFORE_LEARNING;
                    lastStepTime = now;
                }
            }
            break;
            
        case PairingState::WAITING_BEFORE_LEARNING:
            // After alive check response (CMD 0x2D), send Priority Address Request (CMD 0x36)
            // Only send once, then wait for CMD 0x37 response
            if (now - lastStepTime > 100) {  // Small delay to ensure CMD 0x2D is fully processed
                if (sendPriorityAddressRequest(device)) {
                    device->pairingState = PairingState::LEARNING_MODE;  // Transition to wait for CMD 0x37
                    lastStepTime = now;
                } else {
                    addLogMessage("❌ Failed to send CMD 0x36");
                    cancelPairing();
                }
            }
            break;
            
        case PairingState::LEARNING_MODE:
            // Wait for CMD 0x37 (priority address response)
            // handlePairingPacket will send CMD 0x3C when CMD 0x37 is received
            if (now - lastStepTime > 5000) {
                addLogMessage("Waiting for priority address response (CMD 0x37)...");
                lastStepTime = now;
            }
            break;
            
        case PairingState::CHALLENGE_SENT:
            // Wait for CMD 0x3D (challenge response from device)
            // handlePairingPacket will handle CMD 0x3D
            if (now - lastStepTime > 5000) {
                addLogMessage("Waiting for challenge response (CMD 0x3D)...");
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
            // After CMD 0x2F, we send CMD 0x31 (Ask Challenge) for key exchange
            // This is handled in the CMD 0x2F packet handler
            if (now - lastStepTime > 5000) {
                addLogMessage("Timeout in PAIRING_CONFIRMED state");
                cancelPairing();
            }
            break;
            
        case PairingState::KEY_EXCHANGED:
            // After CMD 0x32 key transfer, start gathering device info
            // CMD 0x50 (Name), CMD 0x54 (General Info 1), CMD 0x56 (General Info 2)
            if (now - lastStepTime > 500) {  // 500ms delay after key transfer
                ets_printf("[Pairing] KEY_EXCHANGED - requesting device info...\n");
                requestName(device);
                device->pairingState = PairingState::PAIRED;  // Move to final state
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
    address myAddr = CONTROLLER_ADDRESS;
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
    address myAddr = CONTROLLER_ADDRESS;
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

bool PairingController::send2ABroadcast() {
    // CMD 0x2A - Pairing Broadcast (12-byte payload)
    // From log: DATA(12) 01386e3c72c82ef848407773
    iohcPacket* packet = new iohcPacket();
    
    // Use the 12-byte payload from the log
    uint8_t payload[12] = {0x01, 0x38, 0x6E, 0x3C, 0x72, 0xC8, 
                           0x2E, 0xF8, 0x48, 0x40, 0x77, 0x73};
    
    // Set up packet structure
    packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) - 1;
    packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
    packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
    packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 1;  // Both start and end
    packet->payload.packet.header.CtrlByte1.asByte += 12;
    memcpy(packet->payload.buffer + 9, payload, 12);
    packet->buffer_length = 12 + 9;
    
    packet->payload.packet.header.CtrlByte2.asByte = 0;
    packet->payload.packet.header.CtrlByte2.asStruct.LPM = 1;  // Low Power Mode flag from log
    
    // Source: controller address
    address myAddr = CONTROLLER_ADDRESS;
    memcpy(packet->payload.packet.header.source, myAddr, 3);
    
    // Target: 2W broadcast address
    address broadcast2W = {0x00, 0x00, 0x3B};
    memcpy(packet->payload.packet.header.target, broadcast2W, 3);
    
    packet->payload.packet.header.cmd = 0x2A;
    packet->frequency = CHANNEL2;
    packet->repeatTime = 25;
    packet->repeat = 0;
    packet->lock = false;
    packet->shortPreamble = false;  // Use long preamble for broadcast
    packet->delayed = 250;
    
    bool sent = sendPacket(packet);
    if (sent) {
        lastStepTime = millis();
    }
    return sent;
}

bool PairingController::sendLearningMode(Device2W* device) {
    // CMD 0x2E - 1W Learning mode (1-byte payload: 0x02)
    // NOTE: This is NOT used in the new pairing sequence!
    // Kept for backward compatibility only.
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
    address myAddr = CONTROLLER_ADDRESS;
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

bool PairingController::sendPriorityAddressRequest(Device2W* device) {
    // CMD 0x36 - Priority Address Request (no payload)
    iohcPacket* packet = new iohcPacket();
    
    // Set up packet structure (no payload)
    packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) - 1;
    packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
    packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
    packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
    packet->buffer_length = 9;  // Header only, no payload
    
    packet->payload.packet.header.CtrlByte2.asByte = 0;
    packet->payload.packet.header.CtrlByte2.asStruct.Prio = 1;  // Set priority flag
    
    // Source: controller address
    address myAddr = CONTROLLER_ADDRESS;
    memcpy(packet->payload.packet.header.source, myAddr, 3);
    
    // Target: specific device address
    memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
    
    packet->payload.packet.header.cmd = 0x36;
    packet->frequency = CHANNEL2;
    packet->repeatTime = 25;
    packet->repeat = 0;
    packet->lock = false;
    packet->shortPreamble = true;
    
    bool sent = sendPacket(packet);
    if (sent) {
        char logMsg[128];
        snprintf(logMsg, sizeof(logMsg), 
                 "Sent Priority Address Request: CMD 0x36 to %02X%02X%02X",
                 device->nodeAddress[0], device->nodeAddress[1], device->nodeAddress[2]);
        addLogMessage(logMsg);
        commandBeingAuthenticated = 0x36;  // Track that CMD 0x36 needs authentication
        lastStepTime = millis();
    }
    return sent;
}

bool PairingController::sendChallengeToPair(Device2W* device) {
    // CMD 0x3C - Send Challenge Request to device (6-byte challenge)
    iohcPacket* packet = new iohcPacket();
    
    // Generate random 6-byte challenge
    for (int i = 0; i < 6; i++) {
        deviceChallenge[i] = random(0, 256);
    }
    hasChallenge = true;
    
    // Set up packet structure
    packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) - 1;
    packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
    packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
    packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
    packet->payload.packet.header.CtrlByte1.asByte += 6;
    memcpy(packet->payload.buffer + 9, deviceChallenge, 6);
    packet->buffer_length = 6 + 9;
    
    packet->payload.packet.header.CtrlByte2.asByte = 0;
    
    // Source: controller address
    address myAddr = CONTROLLER_ADDRESS;
    memcpy(packet->payload.packet.header.source, myAddr, 3);
    
    // Target: specific device address
    memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
    
    packet->payload.packet.header.cmd = 0x3C;
    packet->frequency = CHANNEL2;
    packet->repeatTime = 25;
    packet->repeat = 0;
    packet->lock = false;
    packet->shortPreamble = true;
    
    bool sent = sendPacket(packet);
    if (sent) {
        char logMsg[128];
        snprintf(logMsg, sizeof(logMsg), 
                 "Sent Challenge: CMD 0x3C to %02X%02X%02X (challenge: %02X%02X%02X%02X%02X%02X)",
                 device->nodeAddress[0], device->nodeAddress[1], device->nodeAddress[2],
                 deviceChallenge[0], deviceChallenge[1], deviceChallenge[2],
                 deviceChallenge[3], deviceChallenge[4], deviceChallenge[5]);
        addLogMessage(logMsg);
        lastStepTime = millis();
    }
    return sent;
}

bool PairingController::sendAskChallenge(Device2W* device) {
    // CMD 0x31 - Ask Challenge (Push key exchange method)
    // After CMD 0x2F, send CMD 0x31 to initiate Push key exchange
    // Device will respond with CMD 0x3C (challenge)
    // Then we send CMD 0x32 (encrypted stack key)
    // Device authenticates with CMD 0x3C
    // We respond with CMD 0x3D
    // Device confirms with CMD 0x33
    
    addLogMessage("🔑 Sending CMD 0x31 (Ask Challenge) to initiate Push key exchange");
    
    iohcPacket* packet = new iohcPacket();
    
    // Set up packet structure - CMD 0x31 has no payload
    packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) - 1;
    packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
    packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
    packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
    packet->buffer_length = 9;  // Header only, no payload
    
    packet->payload.packet.header.CtrlByte2.asByte = 0;
    
    address myAddr = CONTROLLER_ADDRESS;
    memcpy(packet->payload.packet.header.source, myAddr, 3);
    memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
    
    packet->payload.packet.header.cmd = 0x31;
    packet->frequency = CHANNEL2;
    packet->repeatTime = 25;
    packet->repeat = 0;
    packet->lock = false;
    
    bool sent = sendPacket(packet);
    
    if (sent) {
        addLogMessage("Sent CMD 0x31 - waiting for device to respond with CMD 0x3C (challenge)");
        lastStepTime = millis();
    }
    return sent;
}

bool PairingController::sendForceKeyExchange(Device2W* device) {
    // CMD 0x38 - Launch Key Transfer with 6-byte challenge
    // Used when device skips CMD 0x3C and goes straight to CMD 0x2F
    // This forces the device to exchange keys
    
    addLogMessage("🔑 Device skipped challenge - forcing key exchange with CMD 0x38");
    
    // Generate random challenge
    for (int i = 0; i < 6; i++) {
        deviceChallenge[i] = random(0, 256);
    }
    
    char challengeMsg[128];
    snprintf(challengeMsg, sizeof(challengeMsg), 
             "[Pairing] Generated challenge: %02X%02X%02X%02X%02X%02X",
             deviceChallenge[0], deviceChallenge[1], deviceChallenge[2],
             deviceChallenge[3], deviceChallenge[4], deviceChallenge[5]);
    addLogMessage(challengeMsg);
    
    iohcPacket* packet = new iohcPacket();
    
    // Set up packet structure  
    packet->payload.packet.header.CtrlByte1.asStruct.MsgLen = sizeof(_header) - 1;
    packet->payload.packet.header.CtrlByte1.asStruct.Protocol = 0;
    packet->payload.packet.header.CtrlByte1.asStruct.StartFrame = 1;
    packet->payload.packet.header.CtrlByte1.asStruct.EndFrame = 0;
    packet->payload.packet.header.CtrlByte1.asByte += 6;
    memcpy(packet->payload.buffer + 9, deviceChallenge, 6);
    packet->buffer_length = 6 + 9;
    
    packet->payload.packet.header.CtrlByte2.asByte = 0;
    
    address myAddr = CONTROLLER_ADDRESS;
    memcpy(packet->payload.packet.header.source, myAddr, 3);
    memcpy(packet->payload.packet.header.target, device->nodeAddress, 3);
    
    packet->payload.packet.header.cmd = 0x38;
    packet->frequency = CHANNEL2;
    packet->repeatTime = 25;
    packet->repeat = 0;
    packet->lock = false;
    
    hasChallenge = true;  // Mark that we have a challenge
    
    bool sent = sendPacket(packet);
    
    if (sent) {
        addLogMessage("Sent CMD 0x38 - waiting for device to respond with CMD 0x3C");
        lastStepTime = millis();
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
        // Create frame data for MAC calculation
        // According to linklayer.md: "The initial value is always created using data from the requesting command"
        // This means we use the command that triggered the challenge (e.g., 0x32 for key transfer, 0x36 for address request)
        std::vector<uint8_t> frame_data;
        frame_data.push_back(commandBeingAuthenticated);  // Command that is being authenticated
        
        char cmdMsg[64];
        snprintf(cmdMsg, sizeof(cmdMsg), "Authenticating CMD 0x%02X with challenge", commandBeingAuthenticated);
        addLogMessage(cmdMsg);
        
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
    address myAddr = CONTROLLER_ADDRESS;
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
    
    address myAddr = CONTROLLER_ADDRESS;
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
    // The "last frame" is the CMD 0x31 (Ask Challenge) that preceded receiving CMD 0x3C
    // NOT the current CMD 0x32 being sent!
    std::vector<uint8_t> frame_data;
    frame_data.push_back(0x31);  // CMD 0x31 (previous command sent before challenge)
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
    
    address myAddr = CONTROLLER_ADDRESS;
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
    deviceMgr->storeStackKey(device->nodeAddress, encrypted_iv, 16);
    
    ets_printf("[Pairing] Calling sendPacket() for CMD 0x32...\n");
    bool sent = sendPacket(packet);
    // DON'T delete - radio needs packet until transmission completes
    
    if (sent) {
        ets_printf("[Pairing] sendPacket() returned SUCCESS for CMD 0x32\n");
        addLogMessage("Sent key transfer (CMD 0x32) - key exchange complete!");
        device->pairingState = PairingState::KEY_EXCHANGED;
        commandBeingAuthenticated = 0x32;  // Track that CMD 0x32 needs authentication
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
    
    address myAddr = CONTROLLER_ADDRESS;
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
    
    address myAddr = CONTROLLER_ADDRESS;
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
    
    address myAddr = CONTROLLER_ADDRESS;
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

