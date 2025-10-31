# 2W Key Exchange Implementation Guide

## Overview

This document describes the implementation of the 2-Way (2W) key exchange for io-homecontrol devices in the ESP32-based controller.

**Status**: Implementation complete, ready for testing with real devices.

**Last Updated**: October 31, 2025

---

## Protocol Background

The io-homecontrol 2W protocol uses AES-128 encryption for secure key exchange during device pairing. The process follows the "PUSH" method where the controller sends its stack key to the device.

### Key Exchange Flow (PUSH Method)

```
Controller                              Device
    |                                      |
    |--- CMD 0x31 (Ask Challenge) -------->|
    |                                      |
    |<---- CMD 0x3C (6-byte challenge) ----|
    |                                      |
    |--- CMD 0x32 (Encrypted Key) -------->|
    |                                      |
    |<---- CMD 0x3C (Auth Challenge) ------|
    |                                      |
    |--- CMD 0x3D (Auth Response) -------->|
    |                                      |
    |<---- CMD 0x33 (Key Ack) -------------|
    |                                      |
```

---

## Implementation Details

### File: `src/iohcPairingController.cpp`

#### Function: `sendKeyTransfer(Device2W* device)`

This is the core function that implements CMD 0x32 (key transfer) encryption.

**Algorithm Steps:**

1. **Prepare Frame Data**
   - Use CMD byte (0x32) as frame data
   - Padding to 8 bytes with 0x55 is handled by `constructInitialValue()`

2. **Construct Initial Value (IV)**
   - Input: Frame data (CMD 0x32) + 6-byte challenge from device
   - Function: `constructInitialValue()` from `crypto2Wutils.h`
   - Output: 16-byte IV
   - Structure: `[frame_data (8 bytes), checksum (2 bytes), challenge (6 bytes)]`

3. **Encrypt IV with Transfer Key**
   - Algorithm: AES-128 ECB
   - Key: `transfert_key` (fixed, global constant)
   - Value: `{0x34, 0xc3, 0x46, 0x6e, 0xd8, 0x8f, 0x4e, 0x8e, 0x16, 0xaa, 0x47, 0x39, 0x49, 0x88, 0x43, 0x73}`

4. **XOR with System Key**
   - Take encrypted IV and XOR with system key (stack key)
   - Result: 16-byte encrypted key payload for CMD 0x32

### Key Variables

```cpp
// Global constants (from crypto2Wutils.h)
uint8_t transfert_key[16]  // Fixed transfer key for all devices
uint8_t system_key[16]     // Stack key (should be unique per installation)

// Per-device state (in PairingController)
uint8_t deviceChallenge[6]  // Challenge received from device (CMD 0x3C)
uint8_t systemKey2W[16]     // System key to send to device
```

---

## Cryptographic Verification

### Test Vector (from Protocol Documentation)

**Test Parameters:**
- Stack Key: `01020304050607080910111213141516`
- Challenge: `123456789ABC`
- CMD: `0x32`

**Expected Result:**
- Encrypted Key Payload: `102E49A16D3B69726F3192CF17534AD9`

### Verification Command

```bash
> verifyCrypto
```

This command runs the cryptographic algorithm with test vectors from the protocol documentation and verifies the output matches expected values.

**Expected Output:**
```
=== Verifying Crypto Implementation ===
Test IV: 32555555555555559A123456789ABC
Encrypted IV: [16 bytes after AES encryption]
Final key: 102E49A16D3B69726F3192CF17534AD9
✅ Crypto implementation VERIFIED - matches protocol docs!
=== Crypto Verification Complete ===
```

---

## Debug Logging

The implementation includes comprehensive debug logging at each step:

1. **Challenge Received**: Logs the 6-byte challenge from device
2. **System Key**: Logs the key being sent
3. **Generated IV**: Shows the 16-byte IV before encryption
4. **Encrypted IV**: Shows the IV after AES-128 ECB encryption
5. **Final Payload**: Shows the encrypted key that will be sent in CMD 0x32

**Example Log Output:**
```
Using challenge: 123456789ABC
System key: CE0D4B2F5C6824932DFFED7E7006D338
Generated IV: 32555555555555559A123456789ABC
Encrypted IV: [encrypted IV bytes]
Encrypted key payload: [final 16 bytes to send]
Sent key transfer (CMD 0x32)
```

---

## Usage

### Testing Crypto Implementation

Before attempting real device pairing, verify the crypto:

```bash
> verifyCrypto
```

Check the logs to ensure "✅ Crypto implementation VERIFIED" appears.

### Pairing a Device

1. **Discovery**
   ```bash
   > discovery
   ```
   Press and hold the pairing button on your 2W device.

2. **Start Pairing**
   ```bash
   > pair2W <address>
   ```
   Example: `pair2W fe90ee`

3. **Monitor Progress**
   The pairing controller will automatically:
   - Send CMD 0x28 (discovery broadcast)
   - Send CMD 0x2E (learning mode)
   - Send CMD 0x31 (ask challenge)
   - **Send CMD 0x32 (encrypted key transfer)** ← Key exchange happens here
   - Send CMD 0x3D (authentication)
   - Complete pairing

---

## Key Security Considerations

### Current Implementation

- **Transfer Key**: Fixed global constant (same for all installations)
  - This is correct per protocol specification
  - Used only for encrypting the initial value

- **System Key**: Currently using `transfert_key` as system key (line 156 in main.cpp)
  - ⚠️ **This should be changed!**
  - Each installation should have a unique system key
  - System key is what gets sent to devices and used for future communication

### Recommended Improvements

1. **Generate Unique System Key**
   ```cpp
   // On first boot, generate or load from NVS
   uint8_t unique_system_key[16];
   // Generate random or load from secure storage
   esp_fill_random(unique_system_key, 16);
   // Save to NVS for persistence
   ```

2. **Secure Storage**
   - Move keys from JSON files to ESP32 NVS (Non-Volatile Storage)
   - Use encrypted NVS partition
   - Never log full keys in production builds

3. **Per-Device Keys** (Future)
   - Support storing unique keys per device
   - Implement key rotation

---

## Troubleshooting

### Pairing Fails at Key Transfer

**Symptoms:**
- Device responds with error status (CMD 0xFE)
- Device doesn't send CMD 0x33 (key ack)

**Possible Causes:**
1. Challenge mismatch
   - Check that `deviceChallenge` contains correct 6 bytes from CMD 0x3C
   
2. Encryption error
   - Run `verifyCrypto` command to verify algorithm
   - Check debug logs for IV construction

3. Timing issues
   - Device may time out waiting for key
   - Check radio transmission completed successfully

### Debug Steps

1. Enable verbose logging:
   ```bash
   > verbose
   ```

2. Check each step in logs:
   - CMD 0x31 sent successfully
   - CMD 0x3C received with challenge
   - IV generated correctly
   - CMD 0x32 sent successfully

3. Compare with protocol examples:
   - Use test vectors from documentation
   - Verify each intermediate value

---

## Protocol References

- **Documentation**: `iown-homecontrol/docs/linklayer.md` (lines 520-650)
- **Commands Reference**: `iown-homecontrol/docs/commands.md`
- **Crypto Utilities**: `include/crypto2Wutils.h`
- **AES Implementation**: `include/Aes.h`

### Key Protocol Sections

**Initial Value Construction** (from `constructInitialValue`):
- Bytes 0-7: Frame data (CMD byte + padding 0x55)
- Bytes 8-9: Checksum (computed from frame data)
- Bytes 10-15: Challenge (6 bytes from device)

**Encryption Process**:
1. IV = constructInitialValue(frame_data, challenge)
2. encrypted_IV = AES_ECB_encrypt(IV, transfer_key)
3. encrypted_key = encrypted_IV XOR system_key

---

## Testing Checklist

Before deploying to production:

- [ ] Run `verifyCrypto` - verify passes
- [ ] Test pairing with known good device
- [ ] Verify CMD 0x32 payload matches expected format
- [ ] Confirm device sends CMD 0x33 (key ack)
- [ ] Test subsequent authenticated commands work
- [ ] Generate unique system key for installation
- [ ] Store keys securely in NVS

---

## Code Changes Summary

### Modified Files

1. **`src/iohcPairingController.cpp`** (lines 542-640)
   - Enhanced debug logging
   - Added step-by-step comments
   - Verified algorithm correctness

2. **`src/iohcPairingController.cpp`** (lines 750-850)
   - Added `verifyCryptoImplementation()` function
   - Test vectors from protocol docs

3. **`src/interact.cpp`**
   - Added `verifyCrypto` command

4. **`include/iohcPairingController.h`**
   - Added public method declaration

### No Changes Required

- `crypto2Wutils.h` - Working correctly as-is
- `constructInitialValue()` - Verified correct
- AES encryption - Using standard library correctly

---

## Next Steps

1. **Test with Real Device**
   - Monitor full pairing sequence
   - Capture logs of successful pairing
   - Verify device accepts key

2. **Implement Unique System Keys**
   - Generate on first boot
   - Store in NVS
   - Add UI for key management

3. **Add Challenge Validation**
   - Verify challenge response (CMD 0x3D)
   - Implement proper HMAC if needed

4. **Security Hardening**
   - Disable debug key logging in production
   - Use encrypted NVS
   - Implement key backup/restore

---

## Support

For issues or questions:
1. Check verbose logs (`verbose` command)
2. Run crypto verification (`verifyCrypto`)
3. Review protocol documentation in `iown-homecontrol/docs/`
4. Compare with reference implementation in `src/main.cpp` (legacy code around line 470)

**Author**: AI Assistant
**Based on work by**: cridp, Velocet
**Protocol Reverse Engineering**: io-homecontrol community
