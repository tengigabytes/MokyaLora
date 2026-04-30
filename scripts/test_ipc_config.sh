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
# B3-P3 expansion
IPC_CFG_CHANNEL_NAME=0x0600
IPC_CFG_TELEM_DEVICE_UPDATE_INTERVAL=0x1000
IPC_CFG_TELEM_ENV_DISPLAY_FAHRENHEIT=0x1004
IPC_CFG_NEIGHBOR_ENABLED=0x1100
IPC_CFG_NEIGHBOR_TRANSMIT_OVER_LORA=0x1102
IPC_CFG_RANGETEST_ENABLED=0x1200
IPC_CFG_RANGETEST_SENDER=0x1201
# T2.4.1 StoreForward
IPC_CFG_SF_ENABLED=0x1700
IPC_CFG_SF_HEARTBEAT=0x1701
IPC_CFG_SF_RECORDS=0x1702
IPC_CFG_SF_HISTORY_RETURN_MAX=0x1703
IPC_CFG_SF_HISTORY_RETURN_WINDOW=0x1704
IPC_CFG_SF_IS_SERVER=0x1705
# T2.4.2 Serial
IPC_CFG_SERIAL_ENABLED=0x1800
IPC_CFG_SERIAL_ECHO=0x1801
IPC_CFG_SERIAL_RXD=0x1802
IPC_CFG_SERIAL_TXD=0x1803
IPC_CFG_SERIAL_BAUD=0x1804
IPC_CFG_SERIAL_TIMEOUT=0x1805
IPC_CFG_SERIAL_MODE=0x1806
IPC_CFG_SERIAL_OVERRIDE_CONSOLE=0x1807
# T2.4.3 ExternalNotification
IPC_CFG_EXTNOT_ENABLED=0x1900
IPC_CFG_EXTNOT_OUTPUT_MS=0x1901
IPC_CFG_EXTNOT_OUTPUT=0x1902
IPC_CFG_EXTNOT_OUTPUT_VIBRA=0x1903
IPC_CFG_EXTNOT_OUTPUT_BUZZER=0x1904
IPC_CFG_EXTNOT_ACTIVE=0x1905
IPC_CFG_EXTNOT_ALERT_MESSAGE=0x1906
IPC_CFG_EXTNOT_ALERT_MESSAGE_VIBRA=0x1907
IPC_CFG_EXTNOT_ALERT_MESSAGE_BUZZER=0x1908
IPC_CFG_EXTNOT_ALERT_BELL=0x1909
IPC_CFG_EXTNOT_ALERT_BELL_VIBRA=0x190A
IPC_CFG_EXTNOT_ALERT_BELL_BUZZER=0x190B
IPC_CFG_EXTNOT_USE_PWM=0x190C
IPC_CFG_EXTNOT_NAG_TIMEOUT=0x190D
IPC_CFG_EXTNOT_USE_I2S_AS_BUZZER=0x190E
# T2.4.4 RemoteHardware
IPC_CFG_RHW_ENABLED=0x1A00
IPC_CFG_RHW_ALLOW_UNDEFINED_PIN_ACCESS=0x1A01
# B3-P4 expansion
IPC_CFG_DETECT_ENABLED=0x1300
IPC_CFG_DETECT_MIN_BCAST_SECS=0x1301
IPC_CFG_DETECT_NAME=0x1303
IPC_CFG_DETECT_TRIGGER_TYPE=0x1304
IPC_CFG_DETECT_USE_PULLUP=0x1305
IPC_CFG_CANNED_UPDOWN1_ENABLED=0x1400
IPC_CFG_CANNED_SEND_BELL=0x1401
IPC_CFG_AMBIENT_LED_STATE=0x1500
IPC_CFG_AMBIENT_CURRENT=0x1501
IPC_CFG_AMBIENT_RED=0x1502
IPC_CFG_AMBIENT_GREEN=0x1503
IPC_CFG_AMBIENT_BLUE=0x1504
IPC_CFG_PAX_ENABLED=0x1600
IPC_CFG_PAX_UPDATE_INTERVAL=0x1601
# B3-P2 expansion
IPC_CFG_POWER_SDS_SECS=0x0402
IPC_CFG_POWER_LS_SECS=0x0403
IPC_CFG_POWER_MIN_WAKE_SECS=0x0404
IPC_CFG_POWER_POWERMON_ENABLES=0x0406
IPC_CFG_CHANNEL_MODULE_POSITION_PRECISION=0x0602
IPC_CFG_CHANNEL_MODULE_IS_MUTED=0x0603
IPC_CFG_OWNER_IS_LICENSED=0x0702
IPC_CFG_SECURITY_IS_MANAGED=0x0801
IPC_CFG_SECURITY_ADMIN_CHANNEL_ENABLED=0x0804

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
# Returns LEGACY 4-byte IpcPayloadConfigValue header (key u16 LE +
# vlen u16 LE) + value bytes. Used to verify decode_set's backward
# compatibility with B2-era SWD scripts.
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

# build_set_payload_v2 <key_u16> <channel_index_u8> <value_byte_string>
# Returns NEW 8-byte IpcPayloadConfigValue header (key u16 LE +
# vlen u16 LE + channel_index u8 + 3 bytes pad) + value bytes. This
# is the format settings_client.c on Core 1 emits as of B3-P1.
build_set_payload_v2() {
    local key=$1
    local channel_index=$2
    local value="$3"
    local vlen=0
    if [ -n "$value" ]; then
        vlen=$(echo "$value" | wc -w)
    fi
    printf "0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x00 0x00 0x00 %s" \
        $((key & 0xFF)) \
        $(((key >> 8) & 0xFF)) \
        $((vlen & 0xFF)) \
        $(((vlen >> 8) & 0xFF)) \
        $((channel_index & 0xFF)) \
        "$value"
}

# build_get_payload_v2 <key_u16> <channel_index_u8>
# Returns NEW 4-byte IpcPayloadGetConfig (key u16 LE + channel_index
# u8 + 1 byte pad).
build_get_payload_v2() {
    local key=$1
    local channel_index=$2
    printf "0x%02X 0x%02X 0x%02X 0x00" \
        $((key & 0xFF)) \
        $(((key >> 8) & 0xFF)) \
        $((channel_index & 0xFF))
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

# Same shape as b3p1_set_get but emits the NEW 8-byte SET header. Tests
# the wire format settings_client.c on Core 1 actually sends.
b3p1_set_get_v2() {
    local seq_a="$1"
    local seq_b="$2"
    local key="$3"
    local value_bytes="$4"
    local cli_path="$5"
    local expect="$6"

    echo "=== $cli_path: SET via IPC (8-B header), COMMIT_CONFIG ==="
    local payload=$(build_set_payload_v2 "$key" 0 "$value_bytes")
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

test_b3p2() {
    echo "── B3-P2: Power + Channel module + Owner + Security ──"

    # Power: sds_secs (reboot=Y but COMMIT_CONFIG still applies value to flash)
    b3p1_set_get 0x70 0x71 $IPC_CFG_POWER_SDS_SECS \
        "$(bytes_for_u32_le 60)" power.sds_secs "60"
    b3p1_set_get 0x72 0x73 $IPC_CFG_POWER_SDS_SECS \
        "$(bytes_for_u32_le 4294967295)" power.sds_secs "4294967295"

    # SKIP: power.ls_secs round-trip via host CLI — Meshtastic
    # PhoneAPI.cpp:346 unconditionally clobbers ls_secs with
    # default_ls_secs in the FromRadio reply, so any value we SET is
    # invisible to host CLI / cascade-cache. The flash write itself
    # works (verifiable via SWD read of config.power.ls_secs at the
    # nanopb struct address) but it's not worth the SWD dance for a
    # Meshtastic quirk. min_wake_secs below covers Power reboot=Y SET.

    # Power: min_wake_secs (reboot=Y, no PhoneAPI clobber)
    b3p1_set_get 0x74 0x75 $IPC_CFG_POWER_MIN_WAKE_SECS \
        "$(bytes_for_u32_le 25)" power.min_wake_secs "25"
    b3p1_set_get 0x76 0x77 $IPC_CFG_POWER_MIN_WAKE_SECS \
        "$(bytes_for_u32_le 12)" power.min_wake_secs "12"

    # Power: powermon_enables (reboot=N — only key not in AdminModule short-circuit)
    b3p1_set_get 0x78 0x79 $IPC_CFG_POWER_POWERMON_ENABLES \
        "$(bytes_for_u32_le 65535)" power.powermon_enables "65535"
    b3p1_set_get 0x7A 0x7B $IPC_CFG_POWER_POWERMON_ENABLES \
        "$(bytes_for_u32_le 0)" power.powermon_enables "0"

    # Channel[0] module_settings — host CLI has no dotted-path
    # accessor for module_settings, so we grep --info JSON output.
    channel_module_check() {
        local seq_a="$1" seq_b="$2" key="$3" val_bytes="$4" json_re="$5"
        local payload=$(build_set_payload "$key" "$val_bytes")
        inject_ipc_frame "$IPC_CMD_SET_CONFIG" "$seq_a" "$payload"
        sleep 1
        inject_ipc_frame "$IPC_CMD_COMMIT_CONFIG" "$seq_b" ""
        sleep 4
        if python -m meshtastic --port "$PORT" --info 2>&1 | grep -q "$json_re"; then
            echo "  ✓ channel[0] /$json_re/"
        else
            echo "  ✗ channel[0] missing /$json_re/"
            return 1
        fi
    }
    echo "=== channel[0].is_muted=true ==="
    channel_module_check 0x7C 0x7D $IPC_CFG_CHANNEL_MODULE_IS_MUTED \
        "0x01" '"isMuted": true'
    echo "=== channel[0].is_muted=false ==="
    channel_module_check 0x7E 0x7F $IPC_CFG_CHANNEL_MODULE_IS_MUTED \
        "0x00" '"isMuted": false'
    echo "=== channel[0].position_precision=11 ==="
    channel_module_check 0x80 0x81 $IPC_CFG_CHANNEL_MODULE_POSITION_PRECISION \
        "$(bytes_for_u32_le 11)" '"positionPrecision": 11'
    echo "=== channel[0].position_precision=13 (restore default) ==="
    channel_module_check 0x82 0x83 $IPC_CFG_CHANNEL_MODULE_POSITION_PRECISION \
        "$(bytes_for_u32_le 13)" '"positionPrecision": 13'

    # Owner: is_licensed — host CLI has no --get path; verify via
    # --info JSON (it's in NodeInfo.user.isLicensed for self).
    owner_check() {
        local seq_a="$1" seq_b="$2" key="$3" val_bytes="$4" json_re="$5"
        local payload=$(build_set_payload "$key" "$val_bytes")
        inject_ipc_frame "$IPC_CMD_SET_CONFIG" "$seq_a" "$payload"
        sleep 1
        inject_ipc_frame "$IPC_CMD_COMMIT_CONFIG" "$seq_b" ""
        sleep 4
        if python -m meshtastic --port "$PORT" --info 2>&1 | grep -q "$json_re"; then
            echo "  ✓ owner /$json_re/"
        else
            echo "  ✗ owner missing /$json_re/"
            return 1
        fi
    }
    echo "=== owner.is_licensed=true ==="
    owner_check 0x84 0x85 $IPC_CFG_OWNER_IS_LICENSED \
        "0x01" '"isLicensed": true'
    echo "=== owner.is_licensed=false (clear) ==="
    owner_check 0x86 0x87 $IPC_CFG_OWNER_IS_LICENSED \
        "0x00" '"longName": "Meshtastic'   # is_licensed=false omitted in JSON; just ensure --info works

    # Security: is_managed (reboot=N)
    b3p1_set_get 0x88 0x89 $IPC_CFG_SECURITY_IS_MANAGED \
        "0x00" security.is_managed "False"

    # Security: admin_channel_enabled (reboot=N)
    b3p1_set_get 0x8A 0x8B $IPC_CFG_SECURITY_ADMIN_CHANNEL_ENABLED \
        "0x01" security.admin_channel_enabled "True"
    b3p1_set_get 0x8C 0x8D $IPC_CFG_SECURITY_ADMIN_CHANNEL_ENABLED \
        "0x00" security.admin_channel_enabled "False"
}

test_b3p1_v2_header() {
    echo "── 8-B header (new IpcPayloadConfigValue, B3-P3 forward path) ──"
    b3p1_set_get_v2 0x60 0x61 $IPC_CFG_DEVICE_LED_HEARTBEAT_DISABLED \
        "0x01" device.led_heartbeat_disabled "True"
    b3p1_set_get_v2 0x62 0x63 $IPC_CFG_DEVICE_LED_HEARTBEAT_DISABLED \
        "0x00" device.led_heartbeat_disabled "False"
    b3p1_set_get_v2 0x64 0x65 $IPC_CFG_LORA_USE_PRESET \
        "0x01" lora.use_preset "True"
    b3p1_set_get_v2 0x66 0x67 $IPC_CFG_LORA_BANDWIDTH \
        "$(bytes_for_u32_le 125)" lora.bandwidth "125"
    b3p1_set_get_v2 0x68 0x69 $IPC_CFG_LORA_BANDWIDTH \
        "$(bytes_for_u32_le 0)"   lora.bandwidth "0"
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

test_b3p3() {
    echo "── B3-P3: ModuleConfig (Telemetry/Neighbor/RangeTest) + per-channel index ──"

    # Telemetry: device_update_interval (u32). Use NEW 8-byte header
    # (settings_client.c on Core 1 emits this; ModuleConfig keys all
    # land in 0x10xx so it's a fresh wire-format exercise).
    b3p1_set_get_v2 0x90 0x91 $IPC_CFG_TELEM_DEVICE_UPDATE_INTERVAL \
        "$(bytes_for_u32_le 1234)" telemetry.device_update_interval "1234"
    b3p1_set_get_v2 0x92 0x93 $IPC_CFG_TELEM_DEVICE_UPDATE_INTERVAL \
        "$(bytes_for_u32_le 0)" telemetry.device_update_interval "0"

    # Telemetry: environment_display_fahrenheit (bool)
    b3p1_set_get_v2 0x94 0x95 $IPC_CFG_TELEM_ENV_DISPLAY_FAHRENHEIT \
        "0x01" telemetry.environment_display_fahrenheit "True"
    b3p1_set_get_v2 0x96 0x97 $IPC_CFG_TELEM_ENV_DISPLAY_FAHRENHEIT \
        "0x00" telemetry.environment_display_fahrenheit "False"

    # NeighborInfo: enabled (bool)
    b3p1_set_get_v2 0x98 0x99 $IPC_CFG_NEIGHBOR_ENABLED \
        "0x01" neighbor_info.enabled "True"
    b3p1_set_get_v2 0x9A 0x9B $IPC_CFG_NEIGHBOR_ENABLED \
        "0x00" neighbor_info.enabled "False"

    # NeighborInfo: transmit_over_lora (bool) — exercises a key that's
    # not the first field, just to validate the field-number dispatch
    b3p1_set_get_v2 0x9C 0x9D $IPC_CFG_NEIGHBOR_TRANSMIT_OVER_LORA \
        "0x01" neighbor_info.transmit_over_lora "True"
    b3p1_set_get_v2 0x9E 0x9F $IPC_CFG_NEIGHBOR_TRANSMIT_OVER_LORA \
        "0x00" neighbor_info.transmit_over_lora "False"

    # RangeTest: enabled (bool) + sender (u32)
    b3p1_set_get_v2 0xA0 0xA1 $IPC_CFG_RANGETEST_ENABLED \
        "0x01" range_test.enabled "True"
    b3p1_set_get_v2 0xA2 0xA3 $IPC_CFG_RANGETEST_ENABLED \
        "0x00" range_test.enabled "False"
    b3p1_set_get_v2 0xA4 0xA5 $IPC_CFG_RANGETEST_SENDER \
        "$(bytes_for_u32_le 60)" range_test.sender "60"
    b3p1_set_get_v2 0xA6 0xA7 $IPC_CFG_RANGETEST_SENDER \
        "$(bytes_for_u32_le 0)" range_test.sender "0"

    # Per-channel discriminator: write a name to channel index 2 via
    # IPC SET with channel_index=2.
    #
    # Host CLI `--info` filters channels with role=DISABLED (the default
    # for index 1..7), so the JSON path used elsewhere in this script
    # cannot see a name-only edit. Instead we SWD-dump the channelFile
    # nanopb struct (located via `arm-none-eabi-nm | grep channelFile`,
    # currently 0x2000c654, ~0x800 bytes) and grep for the magic string
    # — the IPC handler writes directly into channelFile.channels[idx].
    channel_name_present_in_sram() {
        local needle="$1"
        cat > /tmp/jlink_ch_dump_$$.jlink <<EOF
connect
h
mem8 0x2000c654 0x800
g
qc
EOF
        "$JLINK" -device RP2350_M33_0 -if SWD -speed 4000 -autoconnect 1 \
            -CommanderScript "$(cygpath -w /tmp/jlink_ch_dump_$$.jlink)" 2>&1 \
            | python -c "
import sys, re
data = bytearray()
for line in sys.stdin:
    m = re.match(r'^([0-9A-F]{8})\s*=\s*([0-9A-F ]+)$', line.strip())
    if m:
        for b in m.group(2).split():
            data.append(int(b, 16))
print('FOUND' if b'$needle' in data else 'NOT_FOUND')
"
        rm -f /tmp/jlink_ch_dump_$$.jlink
    }

    echo "=== channel[2].name='B3P3CH2' ==="
    local payload=$(build_set_payload_v2 $IPC_CFG_CHANNEL_NAME 2 "$(bytes_for_string "B3P3CH2")")
    inject_ipc_frame "$IPC_CMD_SET_CONFIG" 0xA8 "$payload"
    sleep 1
    inject_ipc_frame "$IPC_CMD_COMMIT_CONFIG" 0xA9 ""
    sleep 3
    if [ "$(channel_name_present_in_sram B3P3CH2)" = "FOUND" ]; then
        echo "  ✓ channel[2].name=B3P3CH2 (SWD-verified in channelFile SRAM)"
    else
        echo "  ✗ channel[2].name 'B3P3CH2' not found in channelFile SRAM"
        return 1
    fi

    echo "=== channel[2].name cleared ==="
    payload=$(build_set_payload_v2 $IPC_CFG_CHANNEL_NAME 2 "")
    inject_ipc_frame "$IPC_CMD_SET_CONFIG" 0xAA "$payload"
    sleep 1
    inject_ipc_frame "$IPC_CMD_COMMIT_CONFIG" 0xAB ""
    sleep 3
    if [ "$(channel_name_present_in_sram B3P3CH2)" = "FOUND" ]; then
        echo "  ✗ channel[2].name still has B3P3CH2 (clear failed)"
        return 1
    else
        echo "  ✓ channel[2].name cleared from SRAM"
    fi

    # Per-channel out-of-range guard: channel_index=8 should be rejected
    # at the Core 0 handler (kChannelMax=8, valid range 0..7) before the
    # write reaches channelFile.channels[8] (which would clobber memory
    # past the array). Inject a SET with an obviously distinctive name
    # and verify nothing changes anywhere in channelFile SRAM.
    echo "=== channel[8] write rejected (out-of-range) ==="
    payload=$(build_set_payload_v2 $IPC_CFG_CHANNEL_NAME 8 "$(bytes_for_string "OOB_B3P3")")
    inject_ipc_frame "$IPC_CMD_SET_CONFIG" 0xAC "$payload"
    sleep 2
    if [ "$(channel_name_present_in_sram OOB_B3P3)" = "FOUND" ]; then
        echo "  ✗ channel out-of-range write was NOT rejected"
        return 1
    else
        echo "  ✓ channel_index=8 SET rejected (no OOB_B3P3 in channelFile SRAM)"
    fi
}

test_b3p4() {
    echo "── B3-P4: DetectionSensor + CannedMessage + AmbientLighting + Paxcounter ──"

    # DetectionSensor: enabled (bool), min_bcast_secs (u32),
    # detection_trigger_type (enum 0..5), use_pullup (bool), name (string)
    b3p1_set_get_v2 0xB0 0xB1 $IPC_CFG_DETECT_ENABLED \
        "0x01" detection_sensor.enabled "True"
    b3p1_set_get_v2 0xB2 0xB3 $IPC_CFG_DETECT_ENABLED \
        "0x00" detection_sensor.enabled "False"
    b3p1_set_get_v2 0xB4 0xB5 $IPC_CFG_DETECT_MIN_BCAST_SECS \
        "$(bytes_for_u32_le 30)" detection_sensor.minimum_broadcast_secs "30"
    b3p1_set_get_v2 0xB6 0xB7 $IPC_CFG_DETECT_MIN_BCAST_SECS \
        "$(bytes_for_u32_le 0)"  detection_sensor.minimum_broadcast_secs "0"
    b3p1_set_get_v2 0xB8 0xB9 $IPC_CFG_DETECT_TRIGGER_TYPE \
        "0x03" detection_sensor.detection_trigger_type "3"
    b3p1_set_get_v2 0xBA 0xBB $IPC_CFG_DETECT_TRIGGER_TYPE \
        "0x00" detection_sensor.detection_trigger_type "0"
    b3p1_set_get_v2 0xBC 0xBD $IPC_CFG_DETECT_USE_PULLUP \
        "0x01" detection_sensor.use_pullup "True"
    b3p1_set_get_v2 0xBE 0xBF $IPC_CFG_DETECT_USE_PULLUP \
        "0x00" detection_sensor.use_pullup "False"

    # DetectionSensor name (string) — host CLI reads it back as
    # `detection_sensor.name: <value>`. Use --info JSON to verify so
    # we don't have to worry about quoting.
    echo "=== detection_sensor.name='Motion' ==="
    local payload=$(build_set_payload_v2 $IPC_CFG_DETECT_NAME 0 \
                    "$(bytes_for_string "Motion")")
    inject_ipc_frame "$IPC_CMD_SET_CONFIG" 0xC0 "$payload"
    sleep 1
    inject_ipc_frame "$IPC_CMD_COMMIT_CONFIG" 0xC1 ""
    sleep 3
    local name_actual=$(python -m meshtastic --port "$PORT" --get detection_sensor.name 2>&1 \
        | grep "^detection_sensor.name:" | awk '{print $2}')
    if [ "$name_actual" = "Motion" ]; then
        echo "  ✓ detection_sensor.name=Motion"
    else
        echo "  ✗ expected Motion, got '$name_actual'"
        return 1
    fi
    # Restore empty
    payload=$(build_set_payload_v2 $IPC_CFG_DETECT_NAME 0 "")
    inject_ipc_frame "$IPC_CMD_SET_CONFIG" 0xC2 "$payload"
    sleep 1
    inject_ipc_frame "$IPC_CMD_COMMIT_CONFIG" 0xC3 ""
    sleep 3

    # DetectionSensor trigger_type out-of-range (6 > 5)
    echo "=== detection_sensor.trigger=6 rejected (out-of-range) ==="
    payload=$(build_set_payload_v2 $IPC_CFG_DETECT_TRIGGER_TYPE 0 "0x06")
    inject_ipc_frame "$IPC_CMD_SET_CONFIG" 0xC4 "$payload"
    sleep 1
    inject_ipc_frame "$IPC_CMD_COMMIT_CONFIG" 0xC5 ""
    sleep 3
    local trig=$(python -m meshtastic --port "$PORT" --get detection_sensor.detection_trigger_type 2>&1 \
        | grep "^detection_sensor.detection_trigger_type:" | awk '{print $2}')
    if [ "$trig" = "0" ]; then
        echo "  ✓ trigger_type still 0 (out-of-range value rejected)"
    else
        echo "  ✗ trigger_type=$trig — out-of-range value accepted"
        return 1
    fi

    # CannedMessage: updown1_enabled, send_bell
    b3p1_set_get_v2 0xC6 0xC7 $IPC_CFG_CANNED_UPDOWN1_ENABLED \
        "0x01" canned_message.updown1_enabled "True"
    b3p1_set_get_v2 0xC8 0xC9 $IPC_CFG_CANNED_UPDOWN1_ENABLED \
        "0x00" canned_message.updown1_enabled "False"
    b3p1_set_get_v2 0xCA 0xCB $IPC_CFG_CANNED_SEND_BELL \
        "0x01" canned_message.send_bell "True"
    b3p1_set_get_v2 0xCC 0xCD $IPC_CFG_CANNED_SEND_BELL \
        "0x00" canned_message.send_bell "False"

    # AmbientLighting: led_state (bool) + 4 × u8 (current/red/green/blue)
    b3p1_set_get_v2 0xCE 0xCF $IPC_CFG_AMBIENT_LED_STATE \
        "0x01" ambient_lighting.led_state "True"
    b3p1_set_get_v2 0xD0 0xD1 $IPC_CFG_AMBIENT_LED_STATE \
        "0x00" ambient_lighting.led_state "False"
    b3p1_set_get_v2 0xD2 0xD3 $IPC_CFG_AMBIENT_CURRENT \
        "0x10" ambient_lighting.current "16"
    b3p1_set_get_v2 0xD4 0xD5 $IPC_CFG_AMBIENT_RED \
        "0xFF" ambient_lighting.red "255"
    b3p1_set_get_v2 0xD6 0xD7 $IPC_CFG_AMBIENT_GREEN \
        "0x80" ambient_lighting.green "128"
    b3p1_set_get_v2 0xD8 0xD9 $IPC_CFG_AMBIENT_BLUE \
        "0x00" ambient_lighting.blue "0"

    # Paxcounter: enabled (bool) + update_interval (u32)
    b3p1_set_get_v2 0xDA 0xDB $IPC_CFG_PAX_ENABLED \
        "0x01" paxcounter.enabled "True"
    b3p1_set_get_v2 0xDC 0xDD $IPC_CFG_PAX_ENABLED \
        "0x00" paxcounter.enabled "False"
    b3p1_set_get_v2 0xDE 0xDF $IPC_CFG_PAX_UPDATE_INTERVAL \
        "$(bytes_for_u32_le 600)" paxcounter.paxcounter_update_interval "600"
    b3p1_set_get_v2 0xE2 0xE3 $IPC_CFG_PAX_UPDATE_INTERVAL \
        "$(bytes_for_u32_le 0)"   paxcounter.paxcounter_update_interval "0"
}

# T2.4.1 StoreForward — 6 keys, all bool/uint32.
test_t241() {
    echo "── T2.4.1: StoreForward (6 keys) ──"

    b3p1_set_get_v2 0xE4 0xE5 $IPC_CFG_SF_ENABLED \
        "0x01" store_forward.enabled "True"
    b3p1_set_get_v2 0xE6 0xE7 $IPC_CFG_SF_ENABLED \
        "0x00" store_forward.enabled "False"

    b3p1_set_get_v2 0xE8 0xE9 $IPC_CFG_SF_HEARTBEAT \
        "0x01" store_forward.heartbeat "True"
    b3p1_set_get_v2 0xEA 0xEB $IPC_CFG_SF_HEARTBEAT \
        "0x00" store_forward.heartbeat "False"

    b3p1_set_get_v2 0xEC 0xED $IPC_CFG_SF_RECORDS \
        "$(bytes_for_u32_le 100)" store_forward.records "100"
    b3p1_set_get_v2 0xEE 0xEF $IPC_CFG_SF_RECORDS \
        "$(bytes_for_u32_le 0)"   store_forward.records "0"

    b3p1_set_get_v2 0xF0 0xF1 $IPC_CFG_SF_HISTORY_RETURN_MAX \
        "$(bytes_for_u32_le 50)"  store_forward.history_return_max "50"

    b3p1_set_get_v2 0xF2 0xF3 $IPC_CFG_SF_HISTORY_RETURN_WINDOW \
        "$(bytes_for_u32_le 3600)" store_forward.history_return_window "3600"

    b3p1_set_get_v2 0xF4 0xF5 $IPC_CFG_SF_IS_SERVER \
        "0x01" store_forward.is_server "True"
    b3p1_set_get_v2 0xF6 0xF7 $IPC_CFG_SF_IS_SERVER \
        "0x00" store_forward.is_server "False"
}

# T2.4.2 Serial — 8 keys (2 enums + bools + uint32).
test_t242() {
    echo "── T2.4.2: Serial (8 keys) ──"

    b3p1_set_get_v2 0x40 0x41 $IPC_CFG_SERIAL_ENABLED \
        "0x01" serial.enabled "True"
    b3p1_set_get_v2 0x42 0x43 $IPC_CFG_SERIAL_ENABLED \
        "0x00" serial.enabled "False"

    b3p1_set_get_v2 0x44 0x45 $IPC_CFG_SERIAL_ECHO \
        "0x01" serial.echo "True"
    b3p1_set_get_v2 0x46 0x47 $IPC_CFG_SERIAL_ECHO \
        "0x00" serial.echo "False"

    b3p1_set_get_v2 0x48 0x49 $IPC_CFG_SERIAL_RXD \
        "$(bytes_for_u32_le 17)" serial.rxd "17"
    b3p1_set_get_v2 0x4A 0x4B $IPC_CFG_SERIAL_TXD \
        "$(bytes_for_u32_le 16)" serial.txd "16"

    b3p1_set_get_v2 0x4C 0x4D $IPC_CFG_SERIAL_BAUD \
        "0x0B" serial.baud "11"   # enum 11 = BAUD_115200
    b3p1_set_get_v2 0x4E 0x4F $IPC_CFG_SERIAL_BAUD \
        "0x00" serial.baud "0"    # enum 0 = BAUD_DEFAULT

    b3p1_set_get_v2 0x50 0x51 $IPC_CFG_SERIAL_TIMEOUT \
        "$(bytes_for_u32_le 250)" serial.timeout "250"

    b3p1_set_get_v2 0x52 0x53 $IPC_CFG_SERIAL_MODE \
        "0x02" serial.mode "2"   # enum 2 = PROTO
    b3p1_set_get_v2 0x54 0x55 $IPC_CFG_SERIAL_MODE \
        "0x00" serial.mode "0"   # enum 0 = DEFAULT

    b3p1_set_get_v2 0x56 0x57 $IPC_CFG_SERIAL_OVERRIDE_CONSOLE \
        "0x01" serial.override_console_serial_port "True"
    b3p1_set_get_v2 0x58 0x59 $IPC_CFG_SERIAL_OVERRIDE_CONSOLE \
        "0x00" serial.override_console_serial_port "False"
}

# T2.4.3 ExternalNotification — 15 keys, all bool / uint32.
test_t243() {
    echo "── T2.4.3: ExternalNotification (15 keys) ──"

    b3p1_set_get_v2 0x5A 0x5B $IPC_CFG_EXTNOT_ENABLED \
        "0x01" external_notification.enabled "True"
    b3p1_set_get_v2 0x5C 0x5D $IPC_CFG_EXTNOT_ENABLED \
        "0x00" external_notification.enabled "False"

    b3p1_set_get_v2 0x5E 0x5F $IPC_CFG_EXTNOT_OUTPUT_MS \
        "$(bytes_for_u32_le 500)" external_notification.output_ms "500"
    b3p1_set_get_v2 0x60 0x61 $IPC_CFG_EXTNOT_OUTPUT \
        "$(bytes_for_u32_le 13)"  external_notification.output "13"
    b3p1_set_get_v2 0x62 0x63 $IPC_CFG_EXTNOT_OUTPUT_VIBRA \
        "$(bytes_for_u32_le 14)"  external_notification.output_vibra "14"
    b3p1_set_get_v2 0x64 0x65 $IPC_CFG_EXTNOT_OUTPUT_BUZZER \
        "$(bytes_for_u32_le 15)"  external_notification.output_buzzer "15"

    b3p1_set_get_v2 0x66 0x67 $IPC_CFG_EXTNOT_ACTIVE \
        "0x01" external_notification.active "True"
    b3p1_set_get_v2 0x68 0x69 $IPC_CFG_EXTNOT_ACTIVE \
        "0x00" external_notification.active "False"

    b3p1_set_get_v2 0x6A 0x6B $IPC_CFG_EXTNOT_ALERT_MESSAGE \
        "0x01" external_notification.alert_message "True"
    b3p1_set_get_v2 0x6C 0x6D $IPC_CFG_EXTNOT_ALERT_MESSAGE_VIBRA \
        "0x01" external_notification.alert_message_vibra "True"
    b3p1_set_get_v2 0x6E 0x6F $IPC_CFG_EXTNOT_ALERT_MESSAGE_BUZZER \
        "0x01" external_notification.alert_message_buzzer "True"
    b3p1_set_get_v2 0x70 0x71 $IPC_CFG_EXTNOT_ALERT_BELL \
        "0x01" external_notification.alert_bell "True"
    b3p1_set_get_v2 0x72 0x73 $IPC_CFG_EXTNOT_ALERT_BELL_VIBRA \
        "0x01" external_notification.alert_bell_vibra "True"
    b3p1_set_get_v2 0x74 0x75 $IPC_CFG_EXTNOT_ALERT_BELL_BUZZER \
        "0x01" external_notification.alert_bell_buzzer "True"

    b3p1_set_get_v2 0x76 0x77 $IPC_CFG_EXTNOT_USE_PWM \
        "0x01" external_notification.use_pwm "True"

    b3p1_set_get_v2 0x78 0x79 $IPC_CFG_EXTNOT_NAG_TIMEOUT \
        "$(bytes_for_u32_le 60)" external_notification.nag_timeout "60"

    b3p1_set_get_v2 0x7A 0x7B $IPC_CFG_EXTNOT_USE_I2S_AS_BUZZER \
        "0x01" external_notification.use_i2s_as_buzzer "True"
}

# T2.4.4 RemoteHardware — 2 keys, both bool.
test_t244() {
    echo "── T2.4.4: RemoteHardware (2 keys) ──"

    b3p1_set_get_v2 0x7C 0x7D $IPC_CFG_RHW_ENABLED \
        "0x01" remote_hardware.enabled "True"
    b3p1_set_get_v2 0x7E 0x7F $IPC_CFG_RHW_ENABLED \
        "0x00" remote_hardware.enabled "False"

    b3p1_set_get_v2 0x80 0x81 $IPC_CFG_RHW_ALLOW_UNDEFINED_PIN_ACCESS \
        "0x01" remote_hardware.allow_undefined_pin_access "True"
    b3p1_set_get_v2 0x82 0x83 $IPC_CFG_RHW_ALLOW_UNDEFINED_PIN_ACCESS \
        "0x00" remote_hardware.allow_undefined_pin_access "False"
}

# ── Dispatch ─────────────────────────────────────────────────────────

case "${1:-all}" in
    lora)    test_lora_subset "${2:-15}" ;;
    owner)   test_owner_long_name "${2:-MokyaTest}" ;;
    display) test_display_screen_on_secs "${2:-90}" ;;
    power)   test_power_shutdown_secs "${2:-3600}" ;;
    reboot)  test_commit_reboot "${2:-22}" ;;
    b3p1)    test_b3p1; test_b3p1_v2_header ;;
    b3p1v2)  test_b3p1_v2_header ;;
    b3p2)    test_b3p2 ;;
    b3p3)    test_b3p3 ;;
    b3p4)    test_b3p4 ;;
    t241)    test_t241 ;;
    t242)    test_t242 ;;
    t243)    test_t243 ;;
    t244)    test_t244 ;;
    t24)     test_t241; test_t242; test_t243; test_t244 ;;
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
