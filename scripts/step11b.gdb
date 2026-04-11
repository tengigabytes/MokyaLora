set pagination off
target remote :2331

echo \n=== 1. Reset and halt at first instruction ===\n
monitor reset halt
info registers

echo \n=== 2. Single-step 5 instructions (PC must advance) ===\n
stepi
stepi
stepi
stepi
stepi
info registers pc

echo \n=== 3. Memory read via symbol name (sram_test_buf) ===\n
x/8xw &sram_test_buf

echo \n=== 4. Set breakpoint at sram_test ===\n
break sram_test
info breakpoints

echo \n=== 5. Variable type info ===\n
ptype sram_test_buf

echo \n=== 6. Resume board ===\n
monitor go

quit
