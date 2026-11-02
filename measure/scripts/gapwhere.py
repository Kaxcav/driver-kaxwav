import subprocess, sys, time
sys.path.insert(0, r"D:\DEV\driver-kaxwav\measure\scripts")
import run_baseline as RB, um2_ipc
dll, load = sys.argv[1], int(sys.argv[2])
burn = RB.cpu_burners(12) if load else []
time.sleep(3 if load else 0)
reg = RB.reg_read()
try:
    p = subprocess.Popen([str(RB.PROBE), "--dll", dll, "--mode", "0", "--buffer", "256", "--seconds", "8"], stdout=subprocess.PIPE)
    last = None
    while p.poll() is None:
        t = um2_ipc.read_telemetry()
        if t is not None and t.ownerPid == p.pid:
            last = t
        time.sleep(0.2)
    p.communicate()
finally:
    for b in burn:
        b.kill()
    RB.reg_restore(reg)
n = min(int(last.writeIndex), um2_ipc.RING)
s = sorted(last.ring[i].qpcStart for i in range(n))
f = float(last.qpcFrequency)
g = [(s[i + 1] - s[i]) / f * 1e3 for i in range(len(s) - 1)]
top = sorted(range(len(g)), key=lambda i: -g[i])[:5]
print("blocks", len(s), "maxIntervalTicks_ms", round(last.maxIntervalTicks / f * 1e3, 2))
print("top gaps (block index, ms):", [(i, round(g[i], 2)) for i in top])
print("first 8 gaps ms:", [round(x, 2) for x in g[:8]])
