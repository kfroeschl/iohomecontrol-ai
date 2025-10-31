# 2W Device Pairing Guide

## Quick Start

### Step 1: Discover Devices
Type the `discovery` command in the serial console:
```
> discovery
```

This will:
- Send a 2W discovery broadcast (CMD 0x28) on all configured frequencies
- Listen for device responses
- **Put your device in pairing mode now** (usually by pressing and holding a button)

### Step 2: View Discovered Devices
When a device responds, you'll see output like:
```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
📡 2W Device discovered!
   Address: fe90ee
   To pair this device, use:
   > pair2W fe90ee
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

### Step 3: Pair the Device
Copy the address from the discovery output and use it with the `pair2W` command:
```
> pair2W fe90ee
```

The pairing controller will automatically:
1. Send discovery (CMD 0x28)
2. Wait for device answer (CMD 0x29)
3. Send challenge (CMD 0x38)
4. Wait for challenge response (CMD 0x3D)
5. Send key transfer (CMD 0x32)
6. Wait for key ack (CMD 0x33)
7. Request device name (CMD 0x50)
8. Request general info (CMD 0x54, 0x56)
9. Save device to `/2W.json`

## Available Commands

### Discovery & Pairing
- `discovery` - Broadcast discovery request to find 2W devices
- `pair2W <address>` - Start automatic pairing with a device
  - Example: `pair2W fe90ee`
- `cancelPair2W` - Cancel an ongoing pairing process

### Device Management
- `list2W` - List all paired 2W devices with status
- `info2W <address>` - Show detailed information for a device
  - Example: `info2W fe90ee`
- `del2W <address>` - Remove a device from the database
  - Example: `del2W fe90ee`

### Manual Operations
- `save2W` - Manually save device database to `/2W.json`
- `reload2W` - Reload devices from `/2W.json` file

## Understanding Device Addresses

### Address Format
2W device addresses are **3 bytes (6 hex characters)**:
- Example: `fe90ee` = bytes `[0xFE, 0x90, 0xEE]`
- Always displayed in lowercase hexadecimal
- No spaces, colons, or dashes

### Where Addresses Come From
- **Discovery Response (CMD 0x29)**: The device's address is in the source field
- **Device Label**: Some devices have their address printed on a label
- **Previous Pairings**: Check `/2W.json` or `list2W` output

## Troubleshooting

### No Devices Discovered
1. Make sure device is in pairing mode (hold pairing button)
2. Check that radio is configured (frequencies, SX1276 working)
3. Try discovery multiple times
4. Check antenna connection

### Pairing Fails
1. Use `cancelPair2W` to stop current attempt
2. Verify address is correct (6 hex characters)
3. Make sure device is still in pairing mode
4. Check logs for specific error messages

### Device Not Responding After Pairing
1. Check `list2W` to see device status
2. Use `info2W <address>` to see last communication time
3. Device may need to be woken up or re-paired

## Technical Details

### Pairing State Machine
```
UNPAIRED → DISCOVERING → CHALLENGE_SENT → KEY_EXCHANGED → PAIRED
           ↓                                             ↑
         PAIRING_FAILED ←─────────────────────────────┘
         (timeout: 30s)
```

### Storage
- File: `/2W.json` (LittleFS filesystem)
- Format: JSON array of devices with:
  - Address (3 bytes)
  - Pairing state
  - Crypto keys (system key, session key, challenge)
  - Device capabilities (type, subtype, manufacturer)
  - Device info (name, general info)
  - Timestamps (last seen, pairing time)

### Security
- System key: 16 bytes, unique per device
- Session key: 16 bytes, derived from challenge/response
- Challenge: 6 bytes, random for each pairing
- **TODO**: Move keys to NVS (Non-Volatile Storage) for better security

## Example Session

```
> discovery
Sending 2W discovery broadcast...
Listening for devices (press device pairing button now)...
Device addresses will be shown when they respond.
Use 'pair2W <address>' to pair a discovered device.

[... press device button ...]

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
📡 2W Device discovered!
   Address: fe90ee
   To pair this device, use:
   > pair2W fe90ee
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

> pair2W fe90ee
Started pairing device fe90ee
Pairing process will complete automatically.

[Pairing Controller] Started pairing with device fe90ee
[Pairing Controller] Sending discovery request...
[Pairing Controller] Received discovery answer
[Pairing Controller] Sending challenge...
[Pairing Controller] Received challenge response
[Pairing Controller] Sending key transfer...
[Pairing Controller] Received key ack
[Pairing Controller] Requesting device name...
[Pairing Controller] Requesting general info 1...
[Pairing Controller] Requesting general info 2...
[Pairing Controller] Pairing completed!
Device fe90ee successfully paired and saved

> list2W
Address   State    Type  Name           Last Seen
fe90ee    PAIRED   0x02  My Blind       2024-10-30 15:23:45

> info2W fe90ee
Device: fe90ee
State: PAIRED
Node Type: 0x02
Node Subtype: 0x01
Manufacturer: 0x0005
Name: My Blind
General Info 1: 0x1234
General Info 2: 0x5678
System Key: [16 bytes]
Last Seen: 2024-10-30 15:23:45
Paired At: 2024-10-30 15:23:30
```

## Next Steps

1. **Test Pairing**: Try discovering and pairing a real 2W device
2. **Security Enhancement**: Implement NVS storage for sensitive keys
3. **Key Encryption**: Implement proper key encryption in `sendKeyTransfer()`
4. **MQTT Integration**: Add MQTT support for 2W devices (similar to 1W)
5. **Device Control**: Implement command sending to paired devices

## References

- See `AGENT.MD` for complete protocol documentation
- See `COMMANDS.MD` for full command reference
- See `docs/devices.md` (iown-homecontrol folder) for device types
