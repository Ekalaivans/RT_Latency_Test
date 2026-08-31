#!/home/erl/cnc_latency/venv/bin/python3
import sys
import os

os.environ["MPLCONFIGDIR"] = "/tmp/mplconfig"

import csv
import numpy as np
import matplotlib.pyplot as plt

# Default configuration for 25% load (1 ms period, 250 us load)
TARGET_LOAD_US = 250.0
TARGET_PERIOD_US = 1000.0
LOAD_PERCENT = "25%"

# Log filename detection
log_file = "cnc_1.txt"
if len(sys.argv) > 1:
    log_file = sys.argv[1]
elif not os.path.exists(log_file):
    for candidate in ["isolate_25.txt", "busywait_iso_cpu.txt", "busywait_cpu.txt"]:
        if os.path.exists(candidate):
            log_file = candidate
            break

print(f"Reading log file: {log_file}")

job_load_us = []
cycle_period_us = []
wakeup_jitter_us = []

# Parse execution time and thread period
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

                # Filter out startup artifacts or invalid samples
                if load_ns < 10000 or period_ns < 100000 or period_ns > 10000000:
                    continue

                load_us = load_ns / 1000.0
                period_us = period_ns / 1000.0
                jitter_us = period_us - TARGET_PERIOD_US

                job_load_us.append(load_us)
                cycle_period_us.append(period_us)
                wakeup_jitter_us.append(jitter_us)

            elif "busywait_ns=" in line_str and "wakeup_jitter_ns=" in line_str:
                busy_str = line_str.split("busywait_ns=")[1].split()[0].strip()
                jitter_str = line_str.split("wakeup_jitter_ns=")[1].split()[0].strip()

                load_ns = float(busy_str)
                jitter_ns = float(jitter_str)
                period_ns = 1000000.0 + jitter_ns

                if load_ns >= 10000:
                    load_us = load_ns / 1000.0
                    period_us = period_ns / 1000.0

                    job_load_us.append(load_us)
                    cycle_period_us.append(period_us)
                    wakeup_jitter_us.append(jitter_ns / 1000.0)
        except Exception as e:
            pass

if not job_load_us:
    print(f"Error: No valid data points found in '{log_file}'!")
    sys.exit(1)

job_load_arr = np.array(job_load_us)
cycle_period_arr = np.array(cycle_period_us)
jitter_arr = np.array(wakeup_jitter_us)

# Auto-detect target workload duration (250us, 500us, 750us, 900us, etc.)
avg_load = np.mean(job_load_arr)
if abs(avg_load - 250.0) < 50.0:
    TARGET_LOAD_US = 250.0
elif abs(avg_load - 500.0) < 50.0:
    TARGET_LOAD_US = 500.0
elif abs(avg_load - 750.0) < 50.0:
    TARGET_LOAD_US = 750.0
elif abs(avg_load - 900.0) < 50.0:
    TARGET_LOAD_US = 900.0
else:
    TARGET_LOAD_US = round(avg_load, 1)

load_pct_int = int(round((TARGET_LOAD_US / TARGET_PERIOD_US) * 100))
LOAD_PERCENT = f"{load_pct_int}%"

print("\n" + "="*65)
print(f"   PREEMPT_RT / LINUXCNC REAL-TIME CORE ISOLATION STATISTICS ({LOAD_PERCENT} LOAD)")
print("="*65)
print(f"Total Valid Samples Recorded : {len(job_load_us):,}")
print("-" * 65)
print(f"Job Load Time (Workload)     : Min = {np.min(job_load_arr):.3f} | Avg = {np.mean(job_load_arr):.3f} | Max = {np.max(job_load_arr):.3f} us")
print(f"Total 1 ms Cycle Period      : Min = {np.min(cycle_period_arr):.3f} | Avg = {np.mean(cycle_period_arr):.3f} | Max = {np.max(cycle_period_arr):.3f} us")
print(f"Max Workload Execution Jitter: {np.max(job_load_arr) - TARGET_LOAD_US:.3f} us")
print(f"Max Period Wakeup Jitter     : {np.max(cycle_period_arr) - TARGET_PERIOD_US:.3f} us")
print(f"Std Dev Jitter (Period)      : {np.std(cycle_period_arr):.3f} us")
print("="*65 + "\n")

# Save parsed data to CSV matching workload percentage
csv_filename = f"data_rt_iso_{load_pct_int}.csv"
with open(csv_filename, "w", newline="") as csvfile:
    writer = csv.writer(csvfile)
    writer.writerow(["Sample", "Job_Load_us", "Cycle_Period_us", "Wakeup_Jitter_us"])
    for i in range(len(job_load_us)):
        writer.writerow([i, f"{job_load_us[i]:.3f}", f"{cycle_period_us[i]:.3f}", f"{wakeup_jitter_us[i]:.3f}"])

print(f"Saved parsed sample data to '{csv_filename}'")

# Plot Comparison Graphs (Upper: Job Load | Lower: 1 ms Thread Period)
fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 8), sharex=True)

# Upper Subplot: Job Load Time
ax1.plot(job_load_us, label=f"Job Load Time ({LOAD_PERCENT} Workload on Isolated Core)", color="tab:blue", alpha=0.85)
ax1.axhline(TARGET_LOAD_US, color="red", linestyle="--", alpha=0.8, label=f"Target Workload ({int(TARGET_LOAD_US)} us)")
ax1.set_ylabel("Execution Time (us)")
ax1.set_title(f"PREEMPT_RT {LOAD_PERCENT} Real-Time Workload Execution Time (Isolated Core)")
ax1.legend(loc="upper right")
ax1.grid(True, linestyle=":", alpha=0.6)
if max(job_load_us) - min(job_load_us) > 0.1:
    ax1.set_ylim(min(job_load_us) - 2.0, max(job_load_us) + 4.0)

# Lower Subplot: Total 1 ms Thread Period
ax2.plot(cycle_period_us, label="Measured 1 ms Thread Cycle Period (PREEMPT_RT)", color="tab:green", alpha=0.85)
ax2.axhline(TARGET_PERIOD_US, color="darkred", linestyle="--", alpha=0.8, label=f"Target Thread Period ({int(TARGET_PERIOD_US)} us)")
ax2.set_xlabel("Sample Index (1 ms per sample)")
ax2.set_ylabel("Cycle Period (us)")
ax2.set_title("PREEMPT_RT Total 1 ms Real-Time Thread Period on Isolated Core")
ax2.legend(loc="upper right")
ax2.grid(True, linestyle=":", alpha=0.6)
if max(cycle_period_us) - min(cycle_period_us) > 0.1:
    ax2.set_ylim(min(cycle_period_us) - 3.0, max(cycle_period_us) + 5.0)

plt.tight_layout()
plt.show()
