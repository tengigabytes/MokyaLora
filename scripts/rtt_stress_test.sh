#!/usr/bin/env bash
# rtt_stress_test.sh — exercise the SEGGER RTT up-buffer (4 KB after
# commit bd1dfca) and quantify drop rate under various loads.
#
# Reads emit / drop counters from `g_trace_emit_count`,
# `g_trace_drop_events`, `g_trace_drop_bytes` (added in mokya_trace.c).
# Compares delta-emitted vs delta-captured (lines in JLinkRTTLogger
# output) to surface mismatches the on-target counters miss.
#
# Usage:
#   bash scripts/rtt_stress_test.sh           # full suite
#   bash scripts/rtt_stress_test.sh idle      # one section only
#   bash scripts/rtt_stress_test.sh info_logger
#   bash scripts/rtt_stress_test.sh info_no_logger
#   bash scripts/rtt_stress_test.sh sustained

set -e

JLINK="C:/Program Files/SEGGER/JLink_V932/JLink.exe"
JLINK_RTT="C:/Program Files/SEGGER/JLink_V932/JLinkRTTLogger.exe"
NM="C:/Program Files/Arm/GNU Toolchain mingw-w64-x86_64-arm-none-eabi/bin/arm-none-eabi-nm.exe"
ELF="build/core1_bridge/core1_bridge.elf"
PORT="${PORT:-COM16}"
LOG_DIR="/tmp/rtt_stress_$$"
mkdir -p "$LOG_DIR"

# Resolve symbol addresses once
EMIT_ADDR=$("$NM" "$ELF" | grep " g_trace_emit_count$" | awk '{print "0x" $1}')
DROP_E_ADDR=$("$NM" "$ELF" | grep " g_trace_drop_events$" | awk '{print "0x" $1}')
DROP_B_ADDR=$("$NM" "$ELF" | grep " g_trace_drop_bytes$" | awk '{print "0x" $1}')

if [ -z "$EMIT_ADDR" ]; then
    echo "ERROR: g_trace_emit_count not found in $ELF — rebuild after mokya_trace.c update"
    exit 1
fi

read_counters() {
    cat > "$LOG_DIR/jlink_read.jlink" <<EOF
connect
h
mem32 $EMIT_ADDR 1
mem32 $DROP_E_ADDR 1
mem32 $DROP_B_ADDR 1
g
qc
EOF
    "$JLINK" -device RP2350_M33_1 -if SWD -speed 4000 -autoconnect 1 \
        -CommanderScript "$(cygpath -w "$LOG_DIR/jlink_read.jlink")" 2>&1 \
        > "$LOG_DIR/cnt_raw.txt"
    EMIT=$(grep -i "^${EMIT_ADDR:2}" "$LOG_DIR/cnt_raw.txt" | head -1 | awk '{print "0x" $3}')
    DROP_E=$(grep -i "^${DROP_E_ADDR:2}" "$LOG_DIR/cnt_raw.txt" | head -1 | awk '{print "0x" $3}')
    DROP_B=$(grep -i "^${DROP_B_ADDR:2}" "$LOG_DIR/cnt_raw.txt" | head -1 | awk '{print "0x" $3}')
    if [ -z "$EMIT" ]; then EMIT="0x0"; fi
    if [ -z "$DROP_E" ]; then DROP_E="0x0"; fi
    if [ -z "$DROP_B" ]; then DROP_B="0x0"; fi
    EMIT=$((EMIT))
    DROP_E=$((DROP_E))
    DROP_B=$((DROP_B))
}

start_logger() {
    local out="$1"
    "$JLINK_RTT" -Device RP2350_M33_1 -If SWD -Speed 4000 \
        -RTTSearchRanges "0x20000000 0x80000" -RTTChannel 0 "$out" \
        > "$LOG_DIR/logger_stdout.txt" 2>&1 &
    LOGGER_PID=$!
    sleep 1.5  # let logger find RTT control block
}

stop_logger() {
    taskkill //IM JLinkRTTLogger.exe //F > /dev/null 2>&1 || true
    sleep 1
}

# Drain whatever's in the up-buffer from prior tests by attaching the
# logger briefly. JLinkRTTLogger reads continuously at ~2.5 KB/s; 3 sec
# is enough for a full 4 KB buffer plus ~3 sec of idle traffic.
drain_buffer() {
    start_logger "$LOG_DIR/drain_$$.log"
    sleep 3
    stop_logger
    rm -f "$LOG_DIR/drain_$$.log"
    # Logger output may also live under AppData on Windows
    rm -f "/c/Users/user/AppData/Local/Temp/drain_$$.log" 2>/dev/null || true
}

# Resolve actual log path (logger may redirect /tmp → AppData on Windows)
resolve_log() {
    local hint="$1"
    if [ -s "$hint" ]; then
        echo "$hint"
    elif [ -s "/c/Users/user/AppData/Local/Temp/$(basename "$hint")" ]; then
        echo "/c/Users/user/AppData/Local/Temp/$(basename "$hint")"
    else
        echo "$hint"
    fi
}

print_table_row() {
    printf "  %-22s emit=%-7d drop_evt=%-6d drop_b=%-7d captured=%-6d  loss=%5.2f%%\n" \
        "$1" "$2" "$3" "$4" "$5" "$6"
}

calc_loss_pct() {
    local emit="$1" captured="$2"
    if [ "$emit" -eq 0 ]; then echo "0.00"; return; fi
    awk -v e="$emit" -v c="$captured" 'BEGIN{printf "%.2f", (e-c)*100.0/e}'
}

# ── Test 1: idle, NO logger attached ─────────────────────────────────
test_idle_no_logger() {
    echo "=== T1: idle 5s, NO logger (proves counters increment) ==="
    read_counters; local e0=$EMIT de0=$DROP_E db0=$DROP_B
    sleep 5
    read_counters; local e1=$EMIT de1=$DROP_E db1=$DROP_B
    local emit_d=$((e1 - e0))
    local drop_e_d=$((de1 - de0))
    local drop_b_d=$((db1 - db0))
    print_table_row "idle 5s (no logger)" "$emit_d" "$drop_e_d" "$drop_b_d" 0 \
        "$(calc_loss_pct $emit_d 0)"
}

# ── Test 2: idle, logger attached ───────────────────────────────────
test_idle_with_logger() {
    echo "=== T2: idle 5s, logger drains (steady-state baseline) ==="
    drain_buffer  # clear T1 backlog so the steady-state measurement is clean
    read_counters; local e0=$EMIT de0=$DROP_E
    start_logger "$LOG_DIR/idle.log"
    sleep 5
    stop_logger
    read_counters; local e1=$EMIT de1=$DROP_E db1=$DROP_B
    local emit_d=$((e1 - e0))
    local drop_e_d=$((de1 - de0))
    local actual_log=$(resolve_log "$LOG_DIR/idle.log")
    local captured=$(wc -l < "$actual_log" 2>/dev/null || echo 0)
    print_table_row "idle 5s (logger)" "$emit_d" "$drop_e_d" "0" "$captured" \
        "$(calc_loss_pct $emit_d $captured)"
}

# ── Test 3: --info × 3, logger attached ─────────────────────────────
test_info_with_logger() {
    echo "=== T3: --info × 3 with logger (cascade burst stress) ==="
    drain_buffer
    read_counters; local e0=$EMIT de0=$DROP_E
    start_logger "$LOG_DIR/info_logger.log"
    for i in 1 2 3; do
        python -m meshtastic --port "$PORT" --info > /dev/null 2>&1
        sleep 1
    done
    sleep 1
    stop_logger
    read_counters; local e1=$EMIT de1=$DROP_E
    local emit_d=$((e1 - e0))
    local drop_e_d=$((de1 - de0))
    local actual_log=$(resolve_log "$LOG_DIR/info_logger.log")
    local captured=$(grep -c "^[0-9]" "$actual_log" 2>/dev/null || echo 0)
    local cfg_count=$(grep -cE ",cfg_(device|lora|position|display|power|security)" "$actual_log" 2>/dev/null || echo 0)
    local mc_count=$(grep -cE ",mc_(telem|range|canned|neighbor|ambient|detect|pax)" "$actual_log" 2>/dev/null || echo 0)
    print_table_row "--info × 3 (logger)" "$emit_d" "$drop_e_d" "0" "$captured" \
        "$(calc_loss_pct $emit_d $captured)"
    echo "    cascade events captured: cfg_*=$cfg_count, mc_*=$mc_count (per-info: 6/7)"
}

# ── Test 4: --info × 3, logger NOT attached (worst case) ────────────
test_info_no_logger() {
    echo "=== T4: --info × 3, NO logger (worst case fill+overflow) ==="
    read_counters; local e0=$EMIT de0=$DROP_E
    for i in 1 2 3; do
        python -m meshtastic --port "$PORT" --info > /dev/null 2>&1
        sleep 1
    done
    read_counters; local e1=$EMIT de1=$DROP_E
    local emit_d=$((e1 - e0))
    local drop_e_d=$((de1 - de0))
    local drop_pct=$(awk -v d=$drop_e_d -v e=$emit_d 'BEGIN{if(e>0)printf "%.1f", d*100.0/e; else print "0"}')
    print_table_row "--info × 3 (no logger)" "$emit_d" "$drop_e_d" "0" 0 "0.00"
    echo "    on-target drop rate: $drop_pct%  (logger attached → would drain)"
}

# ── Test 5: sustained --info × 10 with logger (stability) ───────────
test_sustained() {
    echo "=== T5: sustained --info × 10 with logger (~50 cascade replays) ==="
    drain_buffer
    read_counters; local e0=$EMIT de0=$DROP_E
    start_logger "$LOG_DIR/sustained.log"
    for i in 1 2 3 4 5 6 7 8 9 10; do
        python -m meshtastic --port "$PORT" --info > /dev/null 2>&1
    done
    sleep 2
    stop_logger
    read_counters; local e1=$EMIT de1=$DROP_E
    local emit_d=$((e1 - e0))
    local drop_e_d=$((de1 - de0))
    local actual_log=$(resolve_log "$LOG_DIR/sustained.log")
    local captured=$(grep -c "^[0-9]" "$actual_log" 2>/dev/null || echo 0)
    local cfg_count=$(grep -cE ",cfg_" "$actual_log" 2>/dev/null || echo 0)
    local mc_count=$(grep -cE ",mc_" "$actual_log" 2>/dev/null || echo 0)
    print_table_row "--info × 10 (logger)" "$emit_d" "$drop_e_d" "0" "$captured" \
        "$(calc_loss_pct $emit_d $captured)"
    echo "    cascade events captured: cfg_*=$cfg_count (expect 60), mc_*=$mc_count (expect 70)"
}

case "${1:-all}" in
    idle)            test_idle_no_logger; test_idle_with_logger ;;
    info_logger)     test_info_with_logger ;;
    info_no_logger)  test_info_no_logger ;;
    sustained)       test_sustained ;;
    all)
        test_idle_no_logger
        test_idle_with_logger
        test_info_with_logger
        test_info_no_logger
        test_sustained
        ;;
    *) echo "usage: $0 [idle|info_logger|info_no_logger|sustained|all]"; exit 1 ;;
esac

echo
echo "Logs saved under $LOG_DIR"
