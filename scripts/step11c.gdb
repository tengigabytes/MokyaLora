set pagination off
target remote :2331

echo \n=== Reset and halt ===\n
monitor reset halt

echo \n=== Set breakpoint at sram_test entry ===\n
break sram_test

echo \n=== Resume (board boots → waits at REPL) ===\n
continue

echo \n--- BP hit: sram_test entry ---\n
print total_errors
print sram_test_buf[0]

echo \n=== Set hardware watchpoint on sram_test_buf[0] ===\n
watch sram_test_buf[0]

echo \n=== Continue — watchpoint triggers on first pattern write ===\n
continue

echo \n--- Watchpoint hit: sram_test_buf[0] written ---\n
print sram_test_buf[0]
print total_errors

echo \n=== Resume board ===\n
delete breakpoints
continue
quit
