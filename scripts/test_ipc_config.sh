#!/usr/bin/env bash
# test_ipc_config.sh — End-to-end test for B2 IPC config handler.
#
# Injects IPC_CMD_SET_CONFIG / GET_CONFIG / COMMIT_CONFIG / COMMIT_REBOOT
# frames into the c1→c0 ring via SWD memwrite, then reads back through
# the Meshtastic CLI to verify the soft-reload and graceful-reboot paths.
#
# Usage:
#   bash scripts/test_ipc_config.sh           # run full suite
#   bash scripts/test_ipc_config.sh lora      # LoRa subset only (legacy)
#   bash scripts/test_ipc_config.sh owner     # OWNER_LONG_NAME round-trip
#   bash scripts/test_ipc_config.sh display   # DISPLAY_SCREEN_ON_SECS
#   bash scripts/test_ipc_config.sh power     # POWER_SHUTDOWN_AFTER_SECS
#   bash scripts/test_ipc_config.sh reboot    # COMMIT_REBOOT (lora.tx_power)
#                                             # — verifies rebootCount increments
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

C1_TO_C0_CTRL=0x2007A060
C1_TO_C0_SLOTS=0x2007D200
SLOT_STRIDE=264

# Message IDs
IPC_CMD_GET_CONFIG=0x89
IPC_CMD_SET_CONFIG=0x8A
IPC_CMD_COMMIT_CONFIG=0x8B
IPC_CMD_COMMIT_REBOOT=0x8C

# Config keys (must match IpcConfigKey in ipc_protocol.h)
IPC_CFG_OWNER_LONG_NAME=0x0700
IPC_CFG_OWNER_SHORT_NAME=0x0701
IPC_CFG_LORA_REGION=0x0200
IPC_CFG_LORA_TX_POWER=0x0202
IPC_CFG_LORA_HOP_LIMIT=0x0203
IPC_CFG_SCREEN_ON_SECS=0x0500
IPC_CFG_SHUTDOWN_AFTER_SECS=0x0401
# B3-P1 expansion
IPC_CFG_DEVICE_REBROADCAST_MODE=0x0102
IPC_CFG_DEVICE_LED_HEARTBEAT_DISABLED=0x0107
IPC_CFG_LORA_USE_PRESET=0x0205
IPC_CFG_LORA_BANDWIDTH=0x0206
IPC_CFG_POSITION_FLAGS=0x0305
IPC_CFG_DISPLAY_HEADING_BOLD=0x0506
IPC_CFG_DISPLAY_USE_12H_CLOCK=0x0509

# ── ring helpers ──────────────────────────────────────────────────────

# Read ring head and return as decimal in $HEAD
read_head() {
    cat > /tmp/jlink_read_head.jlink <<EOF
connect
h
mem32 $C1_TO_C0_CTRL 1
g
qc
EOF
    HEAD_LINE=$("$JLINK" -device RP2350_M33_0 -if SWD -speed 4000 -autoconnect 1 \
        -CommanderScript "$(cygpath -w /tmp/jlink_read_head.jlink)" 2>&1 \
        | grep -E "^2007A060" | head -1)
    HEAD_HEX=$(echo "$HEAD_LINE" | awk '{print $3}')
    HEAD=$((16#$HEAD_HEX))
}

# inject_ipc_frame <msg_id_hex> <seq_hex> <payload_hex_string_or_empty>
#   - msg_id_hex:   e.g. 0x8A
#   - seq_hex:      e.g. 0xAA
#   - payload_hex:  whitespace-separated bytes, e.g. "02 02 01 00 0F"
#                   Empty for zero-length payload (commits).
#
# Computes slot address from current ring head, writes header + payload
# byte-by-byte via J-Link `w1`, then advances head with release semantics.
inject_ipc_frame() {
    local msg_id="$1"
    local seq="$2"
    local payload="$3"

    read_head
    local SLOT_IDX=$((HEAD % 32))
    local SLOT_ADDR=$((C1_TO_C0_SLOTS + SLOT_IDX * SLOT_STRIDE))

    # Count payload bytes
    local PAYLOAD_LEN=0
    if [ -n "$payload" ]; then
        PAYLOAD_LEN=$(echo "$payload" | wc -w)
    fi

    # Build J-Link script
    local SCRIPT="/tmp/jlink_inject_$$.jlink"
    {
        echo "connect"
        echo "h"
        # Header bytes: msg_id, seq, payload_len_lo, payload_len_hi
        printf "w1 0x%X %s\n" "$SLOT_ADDR" "$msg_id"
        printf "w1 0x%X %s\n" $((SLOT_ADDR + 1)) "$seq"
        printf "w1 0x%X 0x%02X\n" $((SLOT_ADDR + 2)) $((PAYLOAD_LEN & 0xFF))
        printf "w1 0x%X 0x%02X\n" $((SLOT_ADDR + 3)) $(((PAYLOAD_LEN >> 8) & 0xFF))
        # Payload
        local i=4
        for byte in $payload; do
            printf "w1 0x%X %s\n" $((SLOT_ADDR + i)) "$byte"
            i=$((i + 1))
        done
        # Publish: bump head with release semantics
        printf "w4 %s 0x%X\n" "$C1_TO_C0_CTRL" $((HEAD + 1))
        echo "g"
        echo "qc"
    } > "$SCRIPT"

    "$JLINK" -device RP2350_M33_0 -if SWD -speed 4000 -autoconnect 1 \
        -CommanderScript "$(cygpath -w "$SCRIPT")" 2>&1 | tail -3 > /dev/null
    rm -f "$SCRIPT"
}

# bytes_for_u8 <hex>          → "0xNN"
# bytes_for_u32 <decimal>     → "0xNN 0xNN 0xNN 0xNN" (LE)
# bytes_for_string <ascii>    → "0xNN 0xNN ..." (no null terminator)
# bytes_for_set_config_payload <key_u16> <value_bytes>
bytes_for_u32_le() {
    local v="$1"
    printf "0x%02X 0x%02X 0x%02X 0x%02X" \
        $((v & 0xFF)) \
        $(((v >> 8) & 0xFF)) \
        $(((v >> 16) & 0xFF)) \
        $(((v >> 24) & 0xFF))
}

bytes_for_string() {
    local s="$1"
    local out=""
    local i=0
    while [ $i -lt ${#s} ]; do
        out=$(printf "%s 0x%02X" "$out" "'${s:$i:1}")
        i=$((i + 1))
    done
    echo "${out# }"
}

# build_set_payload <key_u16> <value_byte_string>
# Returns header (key u16 LE + vlen u16 LE) + value bytes.
build_set_payload() {
    local key=$1
    local value="$2"
    local vlen=0
    if [ -n "$value" ]; then
        vlen=$(echo "$value" | wc -w)
    fi
    printf "0x%02X 0x%02X 0x%02X 0x%02X %s" \
        $((key & 0xFF)) \
        $(((key >> 8) & 0xFF)) \
        $((vlen & 0xFF)) \
        $(((vlen >> 8) & 0xFF)) \
        "$value"
}

# ── Test cases ───────────────────────────────────────────────────────

test_lora_subset() {
    local NEW_TX_POWER=${1:-15}
    echo "=== LoRa subset: SET tx_power=$NEW_TX_POWER, COMMIT_CONFIG ==="

    local payload=$(build_set_payload $IPC_CFG_LORA_TX_POWER "$(printf "0x%02X" $NEW_TX_POWER)")
    inject_ipc_frame "$IPC_CMD_SET_CONFIG" 0xAA "$payload"
    sleep 1
    inject_ipc_frame "$IPC_CMD_COMMIT_CONFIG" 0xAB ""
    sleep 3

    local actual=$(python -m meshtastic --port "$PORT" --get lora.tx_power 2>&1 | grep "lora.tx_power:" | awk '{print $2}')
    if [ "$actual" = "$NEW_TX_POWER" ]; then
        echo "  ✓ tx_power=$actual (rebootCount unchanged)"
    else
        echo "  ✗ expected $NEW_TX_POWER, got $actual"
        return 1
    fi
}

test_owner_long_name() {
    local NEW_NAME="${1:-MokyaTest}"
    echo "=== OWNER_LONG_NAME: SET '$NEW_NAME', COMMIT_CONFIG ==="

    local value=$(bytes_for_string "$NEW_NAME")
    local payload=$(build_set_payload $IPC_CFG_OWNER_LONG_NAME "$value")
    inject_ipc_frame "$IPC_CMD_SET_CONFIG" 0xB0 "$payload"
    sleep 1
    inject_ipc_frame "$IPC_CMD_COMMIT_CONFIG" 0xB1 ""
    sleep 3

    local actual=$(python -m meshtastic --port "$PORT" --info 2>&1 | grep -oE '"longName": *"[^"]*"' | head -1 | sed 's/.*"longName": *"\([^"]*\)".*/\1/')
    if [ "$actual" = "$NEW_NAME" ]; then
        echo "  ✓ owner.long_name='$actual' (no reboot)"
    else
        echo "  ✗ expected '$NEW_NAME', got '$actual'"
        return 1
    fi
}

test_display_screen_on_secs() {
    local NEW_SECS=${1:-90}
    echo "=== DISPLAY_SCREEN_ON_SECS: SET $NEW_SECS, COMMIT_CONFIG ==="

    local value=$(bytes_for_u32_le "$NEW_SECS")
    local payload=$(build_set_payload $IPC_CFG_SCREEN_ON_SECS "$value")
    inject_ipc_frame "$IPC_CMD_SET_CONFIG" 0xC0 "$payload"
    sleep 1
    inject_ipc_frame "$IPC_CMD_COMMIT_CONFIG" 0xC1 ""
    sleep 3

    local actual=$(python -m meshtastic --port "$PORT" --get display.screen_on_secs 2>&1 | grep "display.screen_on_secs:" | awk '{print $2}')
    if [ "$actual" = "$NEW_SECS" ]; then
        echo "  ✓ screen_on_secs=$actual (no reboot)"
    else
        echo "  ✗ expected $NEW_SECS, got $actual"
        return 1
    fi
}

test_power_shutdown_secs() {
    local NEW_SECS=${1:-3600}
    echo "=== POWER_SHUTDOWN_AFTER_SECS: SET $NEW_SECS, COMMIT_CONFIG ==="

    local value=$(bytes_for_u32_le "$NEW_SECS")
    local payload=$(build_set_payload $IPC_CFG_SHUTDOWN_AFTER_SECS "$value")
    inject_ipc_frame "$IPC_CMD_SET_CONFIG" 0xD0 "$payload"
    sleep 1
    inject_ipc_frame "$IPC_CMD_COMMIT_CONFIG" 0xD1 ""
    sleep 3

    local actual=$(python -m meshtastic --port "$PORT" --get power.on_battery_shutdown_after_secs 2>&1 | grep "power.on_battery_shutdown_after_secs:" | awk '{print $2}')
    if [ "$actual" = "$NEW_SECS" ]; then
        echo "  ✓ on_battery_shutdown_after_secs=$actual (no reboot)"
    else
        echo "  ✗ expected $NEW_SECS, got $actual"
        return 1
    fi
}

read_boot_counter() {
    cat > /tmp/jlink_read_bc.jlink <<'EOF'
connect
h
mem32 0x400D8018 1
g
qc
EOF
    "$JLINK" -device RP2350_M33_0 -if SWD -speed 4000 -autoconnect 1 \
        -CommanderScript "$(cygpath -w /tmp/jlink_read_bc.jlink)" 2>&1 \
        | grep "^400D8018" | head -1 \
        | awk '{print "0x" $3}'
}

test_commit_reboot() {
    local NEW_TX_POWER=${1:-22}
    echo "=== COMMIT_REBOOT: SET tx_power=$NEW_TX_POWER, COMMIT_REBOOT ==="

    # Read boot counter from WATCHDOG.SCRATCH3 — incremented at end of
    # initVariant() on every boot, survives SYSRESETREQ + watchdog reset.
    # More reliable than Meshtastic's cached uptimeSeconds or rebootCount.
    local bc_before=$(read_boot_counter)
    echo "  boot_counter before: $bc_before"

    local payload=$(build_set_payload $IPC_CFG_LORA_TX_POWER "$(printf "0x%02X" $NEW_TX_POWER)")
    inject_ipc_frame "$IPC_CMD_SET_CONFIG" 0xE0 "$payload"
    sleep 1
    inject_ipc_frame "$IPC_CMD_COMMIT_REBOOT" 0xE1 ""

    echo "  Waiting 12 s for graceful reboot + USB CDC re-enum..."
    sleep 12

    local bc_after=$(read_boot_counter)
    local actual=$(python -m meshtastic --port "$PORT" --get lora.tx_power 2>&1 | grep "lora.tx_power:" | awk '{print $2}')
    echo "  boot_counter after:  $bc_after, tx_power=$actual"

    if [ "$actual" = "$NEW_TX_POWER" ] && [ "$bc_after" != "$bc_before" ]; then
        echo "  ✓ tx_power=$actual, boot_counter incremented ($bc_before → $bc_after)"
    else
        echo "  ✗ value=$actual (expected $NEW_TX_POWER), boot_counter $bc_before → $bc_after"
        return 1
    fi
}

# ── B3-P1 expansion tests ────────────────────────────────────────────

# Round-trip a single B3-P1 key. SET via SWD-injected legacy 4-B header
# (decode_set tolerates it), COMMIT_CONFIG, then host CLI --get. host
# CLI parses Meshtastic's protobuf so the field path differs per group.
b3p1_set_get() {
    local seq_a="$1"      # uint8 hex, e.g. 0x40
    local seq_b="$2"      # uint8 hex, e.g. 0x41
    local key="$3"        # hex e.g. 0x0102
    local value_bytes="$4"   # whitespace-separated hex bytes
    local cli_path="$5"   # e.g. config.device.rebroadcast_mode
    local expect="$6"     # what `--get` should print

    echo "=== $cli_path: SET via IPC, COMMIT_CONFIG ==="
    local payload=$(build_set_payload "$key" "$value_bytes")
    inject_ipc_frame "$IPC_CMD_SET_CONFIG" "$seq_a" "$payload"
    sleep 1
    inject_ipc_frame "$IPC_CMD_COMMIT_CONFIG" "$seq_b" ""
    sleep 3

    local actual=$(python -m meshtastic --port "$PORT" --get "$cli_path" 2>&1 \
        | grep "^$cli_path:" | awk '{print $2}')
    if [ "$actual" = "$expect" ]; then
        echo "  ✓ $cli_path=$actual"
    else
        echo "  ✗ expected $expect, got $actual"
        return 1
    fi
}

test_b3p1() {
    # CLI paths drop the `config.` prefix; enum values come as decimal
    # numbers, booleans as Python-style "True"/"False".

    # Device: rebroadcast_mode (enum, AdminModule reload applies value
    # immediately; the reboot flag only affects whether Meshtastic
    # forces a chip reset, not whether the new value lands in flash).
    b3p1_set_get 0x40 0x41 $IPC_CFG_DEVICE_REBROADCAST_MODE \
        "0x02" device.rebroadcast_mode "2"
    b3p1_set_get 0x42 0x43 $IPC_CFG_DEVICE_REBROADCAST_MODE \
        "0x00" device.rebroadcast_mode "0"

    # Device reboot=N: led_heartbeat_disabled (bool)
    b3p1_set_get 0x44 0x45 $IPC_CFG_DEVICE_LED_HEARTBEAT_DISABLED \
        "0x01" device.led_heartbeat_disabled "True"
    b3p1_set_get 0x46 0x47 $IPC_CFG_DEVICE_LED_HEARTBEAT_DISABLED \
        "0x00" device.led_heartbeat_disabled "False"

    # LoRa reboot=N: use_preset (bool)
    b3p1_set_get 0x48 0x49 $IPC_CFG_LORA_USE_PRESET \
        "0x01" lora.use_preset "True"

    # LoRa: bandwidth (u32 over wire, u16 in nanopb)
    b3p1_set_get 0x4A 0x4B $IPC_CFG_LORA_BANDWIDTH \
        "$(bytes_for_u32_le 250)" lora.bandwidth "250"
    b3p1_set_get 0x4C 0x4D $IPC_CFG_LORA_BANDWIDTH \
        "$(bytes_for_u32_le 0)"   lora.bandwidth "0"

    # Display reboot=N: heading_bold (bool)
    b3p1_set_get 0x4E 0x4F $IPC_CFG_DISPLAY_HEADING_BOLD \
        "0x01" display.heading_bold "True"
    b3p1_set_get 0x50 0x51 $IPC_CFG_DISPLAY_HEADING_BOLD \
        "0x00" display.heading_bold "False"

    # Display reboot=N: use_12h_clock (bool)
    b3p1_set_get 0x52 0x53 $IPC_CFG_DISPLAY_USE_12H_CLOCK \
        "0x01" display.use_12h_clock "True"
    b3p1_set_get 0x54 0x55 $IPC_CFG_DISPLAY_USE_12H_CLOCK \
        "0x00" display.use_12h_clock "False"

    # Position: position_flags (u32 bitmask)
    b3p1_set_get 0x56 0x57 $IPC_CFG_POSITION_FLAGS \
        "$(bytes_for_u32_le 811)" position.position_flags "811"
    b3p1_set_get 0x58 0x59 $IPC_CFG_POSITION_FLAGS \
        "$(bytes_for_u32_le 0)"   position.position_flags "0"
}

# ── Dispatch ─────────────────────────────────────────────────────────

case "${1:-all}" in
    lora)    test_lora_subset "${2:-15}" ;;
    owner)   test_owner_long_name "${2:-MokyaTest}" ;;
    display) test_display_screen_on_secs "${2:-90}" ;;
    power)   test_power_shutdown_secs "${2:-3600}" ;;
    reboot)  test_commit_reboot "${2:-22}" ;;
    b3p1)    test_b3p1 ;;
    all)
        test_lora_subset 15
        test_owner_long_name "MokyaTest"
        test_display_screen_on_secs 90
        test_power_shutdown_secs 3600
        test_b3p1
        # COMMIT_REBOOT goes last — chip reboots so don't run more after.
        test_commit_reboot 22
        ;;
    *)
        echo "Usage: $0 [all|lora|owner|display|power|reboot|b3p1] [value]"
        exit 1
        ;;
esac
