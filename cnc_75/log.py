#!/home/erl/cnc_latency/venv/bin/python3
import sys
import os

os.environ["MPLCONFIGDIR"] = "/tmp/mplconfig"

import csv
import numpy as np
import matplotlib.pyplot as plt

TARGET_LOAD_US = 750.0
TARGET_PERIOD_US = 1000.0
LOAD_PERCENT = "75%"

log_file = "isolate_75.txt"
if len(sys.argv) > 1:
    log_file = sys.argv[1]
elif not os.path.exists(log_file):
    for candidate in ["cnc_75.txt", "cnc.txt", "rt_iso_75.txt"]:
        if os.path.exists(candidate):
            log_file = candidate
            break

print(f"Reading log file: {log_file}")

job_load_us = []
cycle_period_us = []
wakeup_jitter_us = []

with open(log_file, "r") as f:
    for line in f:
        try:
            line_str = line.strip()
            if "Job Load ::" in line_str and "Job Time ::" in line_str:
                parts = line_str.split("||")
                if len(parts) < 2:
                    continue

                load_str = parts[0].split("::")[1].strip()
                period_str = parts[1].split("::")[1].strip()

                load_ns = float(load_str)
                period_ns = float(period_str)

                if load_ns < 10000 or period_ns < 100000 or period_ns > 10000000:
                    continue

                load_us = load_ns / 1000.0
                period_us = period_ns / 1000.0
                jitter_us = period_us - TARGET_PERIOD_US

                job_load_us.append(load_us)
                cycle_period_us.append(period_us)
                wakeup_jitter_us.append(jitter_us)
        except Exception:
            pass

if not job_load_us:
    print(f"Error: No valid data points found in '{log_file}'!")
    sys.exit(1)

job_load_arr = np.array(job_load_us)
cycle_period_arr = np.array(cycle_period_us)

print("\n" + "="*65)
print(f"   PREEMPT_RT REAL-TIME CORE ISOLATION STATISTICS ({LOAD_PERCENT} LOAD)")
print("="*65)
print(f"Total Valid Samples Recorded : {len(job_load_us):,}")
print("-" * 65)
print(f"Job Load Time (Workload)     : Min = {np.min(job_load_arr):.3f} | Avg = {np.mean(job_load_arr):.3f} | Max = {np.max(job_load_arr):.3f} us")
print(f"Total 1 ms Cycle Period      : Min = {np.min(cycle_period_arr):.3f} | Avg = {np.mean(cycle_period_arr):.3f} | Max = {np.max(cycle_period_arr):.3f} us")
print(f"Max Workload Execution Jitter: {np.max(job_load_arr) - TARGET_LOAD_US:.3f} us")
print(f"Max Period Wakeup Jitter     : {np.max(cycle_period_arr) - TARGET_PERIOD_US:.3f} us")
print(f"Std Dev Jitter (Period)      : {np.std(cycle_period_arr):.3f} us")
print("="*65 + "\n")

csv_filename = "data_iso_75.csv"
with open(csv_filename, "w", newline="") as csvfile:
    writer = csv.writer(csvfile)
    writer.writerow(["Sample", "Job_Load_us", "Cycle_Period_us", "Wakeup_Jitter_us"])
    for i in range(len(job_load_us)):
        writer.writerow([i, f"{job_load_us[i]:.3f}", f"{cycle_period_us[i]:.3f}", f"{wakeup_jitter_us[i]:.3f}"])

print(f"Saved parsed sample data to '{csv_filename}'")

fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 8), sharex=True)

ax1.plot(job_load_us, label=f"Job Load Time ({LOAD_PERCENT} Workload on Isolated Core)", color="tab:blue", alpha=0.85)
ax1.axhline(TARGET_LOAD_US, color="red", linestyle="--", alpha=0.8, label=f"Target Workload ({int(TARGET_LOAD_US)} us)")
ax1.set_ylabel("Execution Time (us)")
ax1.set_title(f"PREEMPT_RT {LOAD_PERCENT} Real-Time Workload Execution Time (Isolated Core)")
ax1.legend(loc="upper right")
ax1.grid(True, linestyle=":", alpha=0.6)

ax2.plot(cycle_period_us, label="Measured 1 ms Thread Cycle Period (PREEMPT_RT)", color="tab:green", alpha=0.85)
ax2.axhline(TARGET_PERIOD_US, color="darkred", linestyle="--", alpha=0.8, label=f"Target Thread Period ({int(TARGET_PERIOD_US)} us)")
ax2.set_xlabel("Sample Index (1 ms per sample)")
ax2.set_ylabel("Cycle Period (us)")
ax2.set_title("PREEMPT_RT Total 1 ms Real-Time Thread Period on Isolated Core")
ax2.legend(loc="upper right")
ax2.grid(True, linestyle=":", alpha=0.6)

plt.tight_layout()
plt.show()
