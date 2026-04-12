"""Benchmark: meshtastic serial round-trip speed and data integrity.

Usage:
    python scripts/bench_serial.py COM16 [rounds]

Measures:
  - Connection time (until 'Connected to radio')
  - Total --info wall time
  - Output size (bytes)
  - Node count parsed
  - Repeated 3x (or user-specified) to get min/avg/max
"""

import subprocess, sys, time, re

port = sys.argv[1] if len(sys.argv) > 1 else "COM16"
rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 3

print(f"=== Meshtastic serial benchmark on {port}, {rounds} rounds ===\n")

times = []
sizes = []
nodes = []
errors = 0

for i in range(rounds):
    print(f"--- Round {i+1}/{rounds} ---")
    t0 = time.perf_counter()
    try:
        r = subprocess.run(
            [sys.executable, "-m", "meshtastic", "--port", port, "--info"],
            capture_output=True, text=True, timeout=120
        )
        t1 = time.perf_counter()
        elapsed = t1 - t0
        output = r.stdout + r.stderr
        out_bytes = len(output.encode("utf-8"))

        # Count nodes
        node_matches = re.findall(r'"![\da-f]+":\s*\{', output)
        n_nodes = len(node_matches)

        # Check for errors
        if "Timed out" in output or r.returncode != 0:
            print(f"  ERROR: returncode={r.returncode}")
            if "Timed out" in output:
                print("  Timed out!")
            errors += 1
            # Still record timing
            times.append(elapsed)
            sizes.append(out_bytes)
            nodes.append(0)
        else:
            connected = "Connected to radio" in output
            print(f"  Time: {elapsed:.2f}s  Size: {out_bytes} B  Nodes: {n_nodes}  Connected: {connected}")
            times.append(elapsed)
            sizes.append(out_bytes)
            nodes.append(n_nodes)

    except subprocess.TimeoutExpired:
        t1 = time.perf_counter()
        print(f"  TIMEOUT after {t1-t0:.1f}s")
        errors += 1
        times.append(t1 - t0)
        sizes.append(0)
        nodes.append(0)

    # Small gap between rounds to let USB settle
    if i < rounds - 1:
        time.sleep(2)

print(f"\n=== Summary ({port}) ===")
print(f"Rounds:  {rounds}  (errors: {errors})")
if times:
    print(f"Time:    min={min(times):.2f}s  avg={sum(times)/len(times):.2f}s  max={max(times):.2f}s")
if sizes:
    print(f"Size:    min={min(sizes)}B  avg={sum(sizes)//len(sizes)}B  max={max(sizes)}B")
if nodes:
    print(f"Nodes:   min={min(nodes)}  avg={sum(nodes)//len(nodes)}  max={max(nodes)}")
