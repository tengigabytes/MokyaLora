#!/usr/bin/env bash
# test_ipc_config.sh — End-to-end test for B2 IPC config handler.
#
# Injects IPC_CMD_SET_CONFIG (key=LORA_TX_POWER, value=NEW) followed by
# IPC_CMD_COMMIT_CONFIG into the c1→c0 ring via SWD memwrite, then
# reads tx_power back through Meshtastic CLI to verify the soft-reset
# path applied the change without rebooting.
#
# Layout (from ipc_shared_layout.h):
#   IpcSharedSram @ 0x2007A000
#     +0x000  5×u32 (boot_magic, c0_ready, c1_ready, flash_lock, flash_lock_c0)
#     +0x014  12B pad
#     +0x020  c0_to_c1_ctrl       (32 B)
#     +0x040  c0_log_to_c1_ctrl   (32 B)
#     +0x060  c1_to_c0_ctrl       (32 B)        ← inject increments head here
#     +0x080  c0_to_c1_slots[32]  (32 × 264 B)
#     +0x2180 c0_log_to_c1_slots[16] (16 × 264 B)
#     +0x3200 c1_to_c0_slots[32]  (32 × 264 B)  ← inject writes payload here
#
# Per-slot: 4B IpcMsgHeader (msg_id, seq, payload_len LE u16) + 256B payload + 4B pad.

set -e

JLINK="C:/Program Files/SEGGER/JLink_V932/JLink.exe"
PORT="${PORT:-COM16}"
NEW_TX_POWER="${1:-15}"

C1_TO_C0_CTRL=0x2007A060
C1_TO_C0_SLOTS=0x2007D200
SLOT_STRIDE=264

IPC_CMD_SET_CONFIG=0x8A
IPC_CMD_COMMIT_CONFIG=0x8B
IPC_CFG_LORA_TX_POWER=0x0202

# Build a J-Link script that:
#   1. Reads c1_to_c0_ctrl.head (offset +0)
#   2. Writes a SET frame at slots[head%32]
#   3. Increments head
#   4. Reads new head
#   5. Writes a COMMIT frame at slots[(head+1)%32]
#   6. Increments head again
#
# Since J-Link Commander has no arithmetic, the script only knows how to
# write to slot[N] for some hardcoded N. We pick the next two head slots
# by reading head once and then doing two pushes.

# Read head + tail
cat > /tmp/jlink_read_head.jlink <<EOF
connect
h
mem32 $C1_TO_C0_CTRL 2
g
qc
EOF
HEAD_LINE=$("$JLINK" -device RP2350_M33_0 -if SWD -speed 4000 -autoconnect 1 \
  -CommanderScript "$(cygpath -w /tmp/jlink_read_head.jlink)" 2>&1 \
  | grep -E "^2007A060" | head -1)
echo "Ring state: $HEAD_LINE"
HEAD_HEX=$(echo "$HEAD_LINE" | awk '{print $3}')
TAIL_HEX=$(echo "$HEAD_LINE" | awk '{print $4}')
HEAD=$((16#$HEAD_HEX))
TAIL=$((16#$TAIL_HEX))
echo "head=$HEAD tail=$TAIL pending=$((HEAD - TAIL))"

SLOT0_IDX=$((HEAD % 32))
SLOT1_IDX=$(((HEAD + 1) % 32))
SLOT0_ADDR=$(printf "0x%X" $((C1_TO_C0_SLOTS + SLOT0_IDX * SLOT_STRIDE)))
SLOT1_ADDR=$(printf "0x%X" $((C1_TO_C0_SLOTS + SLOT1_IDX * SLOT_STRIDE)))
NEW_HEAD_AFTER_SET=$((HEAD + 1))
NEW_HEAD_AFTER_COMMIT=$((HEAD + 2))
NEW_HEAD_HEX_SET=$(printf "0x%X" $NEW_HEAD_AFTER_SET)
NEW_HEAD_HEX_COMMIT=$(printf "0x%X" $NEW_HEAD_AFTER_COMMIT)

echo "Inject SET at slot $SLOT0_IDX ($SLOT0_ADDR), COMMIT at slot $SLOT1_IDX ($SLOT1_ADDR)"
echo "Setting tx_power = $NEW_TX_POWER"

# SET frame: header (msg_id=0x8A, seq=0xAA, payload_len=0x0005) + payload (key=0x0202 LE, value_len=0x0001 LE, value=NEW_TX_POWER)
# Header packed as 4 bytes LE: msg_id, seq, payload_len_lo, payload_len_hi
# So bytes: 8A AA 05 00 → as u32 LE = 0x000505AA8A → wait let me redo
# Actually header is struct: u8 msg_id, u8 seq, u16 payload_len. Packed = 4B.
# As bytes: [msg_id][seq][len_lo][len_hi] = 8A AA 05 00.
# As u32 little-endian: 0x000505AA — no wait:
#   byte 0 = 8A → bits [7:0]
#   byte 1 = AA → bits [15:8]
#   byte 2 = 05 → bits [23:16]
#   byte 3 = 00 → bits [31:24]
# u32 = 0x0005AA8A
SET_HDR_W0=0x0005AA8A
# Payload: IpcPayloadConfigValue{u16 key=0x0202, u16 value_len=0x0001, u8 value=NEW}
# bytes: [02 02 01 00 NN]
# Word 1 (slot+4): bytes 4..7 = key_lo key_hi vlen_lo vlen_hi = 02 02 01 00 → u32 = 0x00010202
# Word 2 (slot+8): bytes 8..11 = value(1B) + 3 garbage bytes — write only value byte
SET_PAYLOAD_W0=0x00010202
NEW_TX_POWER_HEX=$(printf "0x%02X" "$NEW_TX_POWER")

# COMMIT frame: header msg_id=0x8B seq=0xAB payload_len=0 → 8B AB 00 00 → u32 = 0x0000AB8B
COMMIT_HDR_W0=0x0000AB8B

# Inject script — write payload words first, then publish head with release semantics
cat > /tmp/jlink_inject.jlink <<EOF
connect
h
w4 $SLOT0_ADDR $SET_HDR_W0
w4 $(printf "0x%X" $((SLOT0_ADDR + 4))) $SET_PAYLOAD_W0
w1 $(printf "0x%X" $((SLOT0_ADDR + 8))) $NEW_TX_POWER_HEX
w4 $C1_TO_C0_CTRL $NEW_HEAD_HEX_SET
g
qc
EOF
echo "=== Injecting SET ==="
"$JLINK" -device RP2350_M33_0 -if SWD -speed 4000 -autoconnect 1 \
  -CommanderScript "$(cygpath -w /tmp/jlink_inject.jlink)" 2>&1 | tail -3

# Tiny pause so Core 0 drains the SET before we publish COMMIT — both
# would work in a single batch but separate writes match how Core 1 will
# eventually emit them.
sleep 1

cat > /tmp/jlink_commit.jlink <<EOF
connect
h
w4 $SLOT1_ADDR $COMMIT_HDR_W0
w4 $C1_TO_C0_CTRL $NEW_HEAD_HEX_COMMIT
g
qc
EOF
echo "=== Injecting COMMIT ==="
"$JLINK" -device RP2350_M33_0 -if SWD -speed 4000 -autoconnect 1 \
  -CommanderScript "$(cygpath -w /tmp/jlink_commit.jlink)" 2>&1 | tail -3

sleep 3

echo "=== Verifying via meshtastic --get lora.tx_power ==="
python -m meshtastic --port "$PORT" --get lora.tx_power 2>&1 | tail -3
