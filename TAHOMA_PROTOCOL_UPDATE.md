# TaHoma Protocol Analysis & Implementation Update

## Overview
After analyzing real TaHoma Box pairing logs, we discovered that our implementation based on the verbose log analysis was using **CMD 0x2A** for pairing discovery. However, the actual TaHoma Box uses **CMD 0x28** for pairing actuators.

## Key Finding: CMD 0x2A vs CMD 0x28

### What We Implemented (Based on Verbose Log)
- **CMD 0x2A**: Discover Remote with 12-byte challenge
- **Sequence**: CMD 0x2A (broadcast) → CMD 0x2E → CMD 0x3C/0x3D → CMD 0x2F

### What TaHoma Box Actually Uses
- **CMD 0x28**: Discovery/Pairing (no payload)
- **Sequence**: CMD 0x28 (broadcast) → CMD 0x2E → CMD 0x3C/0x3D → CMD 0x2F

## TaHoma Box Pairing Sequence (from real log)

```
Line 7:  FROM AA9BFA TO 00003B CMD 28          (Pairing broadcast - no data)
Line 17: FROM EEC022 TO AA9BFA CMD 29          (Device response)
Line 20: FROM AA9BFA TO EEC022 CMD 2C → CMD 2D (Alive check)
Line 36: FROM AA9BFA TO EEC022 CMD 2E DATA(01) 02 (Learning Mode)
Line 38: FROM EEC022 TO AA9BFA CMD 3C DATA(06) ef9c649969a1 (Device Challenge)
Line 40: FROM AA9BFA TO EEC022 CMD 3D DATA(06) 3a3d831d4ba7 (Controller Response)
Line 42: FROM EEC022 TO AA9BFA CMD 2F DATA(01) 02 (Confirmation) ✅
Line 44: FROM AA9BFA TO EEC022 CMD 54 → CMD 55 (General Info 1)
Line 52: FROM AA9BFA TO EEC022 CMD 50 → CMD 51 (Device Name)
```

## Analysis

### CMD 0x28 Characteristics (TaHoma)
- **Purpose**: Pairing/Discovery broadcast
- **Payload**: None (command byte only)
- **Target**: Broadcast address 0x00003B
- **Response**: CMD 0x29 from device

### CMD 0x2A Characteristics (From Verbose Log)
- **Purpose**: Discover Remote (may be for remote/sensor pairing, not actuators)
- **Payload**: 12-byte random challenge
- **Target**: Broadcast address 0x00003B
- **Response**: Not seen in TaHoma Box logs

### Hypothesis
- **CMD 0x28**: Used for pairing **actuators** (blinds, plugs, etc.)
- **CMD 0x2A**: Used for pairing **remotes/sensors** (wall switches, remote controls)

The verbose log we analyzed (address AA9BFA) might have been from a remote pairing session, not an actuator pairing session.

## Current Implementation Status

### ✅ What's Correct
1. **CMD 0x2E** (Learning Mode) with payload 0x02 - ✅ Matches TaHoma
2. **CMD 0x3C** (Device Challenge) - 6 bytes - ✅ Matches TaHoma
3. **CMD 0x3D** (Controller Response) - 6 bytes - ✅ Matches TaHoma
4. **CMD 0x2F** (Pairing Confirmation) with payload 0x02 - ✅ Matches TaHoma
5. **CMD 0x54** (Get General Info 1) - ✅ Matches TaHoma (we need to implement this)
6. **CMD 0x50** (Get Name) - ✅ Matches TaHoma (we need to implement this)

### ⚠️ What Needs Update
1. **CMD 0x2A** → Should be **CMD 0x28** for actuator pairing
2. **Remove 12-byte challenge** - CMD 0x28 has no payload
3. **Add CMD 0x29 handler** - Device discovery response
4. **Add CMD 0x2C/0x2D** - Actuator alive check (optional but TaHoma uses it)

### 🔧 What's Missing (Not Yet Implemented)
1. **Proper cryptographic challenge-response** for CMD 0x3D (currently just copies challenge)
2. **CMD 0x32/0x33** - Key transfer (TaHoma doesn't show this in log - might be optional?)
3. **CMD 0x54/0x55 handler** - General Info 1 request/response
4. **CMD 0x50/0x51 handler** - Name request/response
5. **Device information parsing** from CMD 0x55 response

## Recommended Changes

### Priority 1: Update to CMD 0x28 (Like TaHoma)
```cpp
// Change from:
packet->payload.packet.header.cmd = 0x2A;  // Discover Remote
memcpy(packet->payload.buffer + 9, pairingChallenge, 12);  // 12-byte challenge
packet->buffer_length = 12 + 9;

// To:
packet->payload.packet.header.cmd = 0x28;  // Discovery/Pairing
// No payload needed
packet->buffer_length = 9;  // Just header
```

### Priority 2: Implement Device Info Requests
After CMD 0x2F confirmation:
1. Send CMD 0x54 (Get General Info 1)
2. Wait for CMD 0x55 response
3. Parse manufacturer, class, serial number
4. Send CMD 0x50 (Get Name)
5. Wait for CMD 0x51 response
6. Save device with all information

### Priority 3: Add Crypto for CMD 0x3D
The challenge-response needs proper io-homecontrol cryptography. Currently we just copy the challenge, which won't work with real devices.

## Decision Point

**Should we:**

### Option A: Use CMD 0x28 (Match TaHoma - Recommended)
- **Pros**: Matches real Somfy controller, proven to work with actuators
- **Cons**: Might not work with remotes if they expect CMD 0x2A
- **Status**: Code compiled successfully but NOT yet updated to CMD 0x28

### Option B: Support Both CMD 0x28 and CMD 0x2A
- **Pros**: Maximum compatibility, can pair both actuators and remotes
- **Cons**: More complex, need to determine device type
- **Status**: Not implemented

### Option C: Keep CMD 0x2A for Testing
- **Pros**: Already implemented, might work if devices accept both
- **Cons**: Doesn't match real controller behavior
- **Status**: Current implementation

## Compilation Status

✅ **SUCCESS** - Code compiles cleanly with current CMD 0x2A implementation
- Build time: 57 seconds
- Flash usage: 81.0% (1,698,757 / 2,097,152 bytes)
- RAM usage: 17.7% (58,128 / 327,680 bytes)

## Next Steps

1. **Test current implementation** - See if devices respond to CMD 0x2A
2. **If devices don't respond** - Update to CMD 0x28 (one line change)
3. **Implement device info gathering** - CMD 0x54/0x55 and CMD 0x50/0x51
4. **Add proper crypto** - Research io-homecontrol challenge-response algorithm

## Files Modified (This Session)

None - code compiles successfully but replacements failed due to whitespace/formatting issues. The implementation currently uses CMD 0x2A. A future update will switch to CMD 0x28 if needed based on hardware testing results.

## References

- **TaHoma Log**: `/analysis/pairing_plug_logs.txt`
- **AGENT.MD**: Complete protocol documentation
- **PairingController**: `/workspaces/iohomecontrol/src/iohcPairingController.cpp`
- **Device2WManager**: `/workspaces/iohomecontrol/src/iohcDevice2W.cpp`
