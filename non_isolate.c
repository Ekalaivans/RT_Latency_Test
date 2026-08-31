#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <x86intrin.h>
#include <regex>
#include <fstream>
#include <iostream>
#include <string>
#include <cstdint>
#include <cstring>
#include <cerrno>

#define BUSY_WAIT_NS  250000ULL   // 250 us (25% load)
#define PERIOD_NS    1000000ULL   // 1 ms period
#define RT_PRIORITY          90   // SCHED_FIFO priority

/* NOTE: No CPU isolation / pinning in this version.
 * The motion thread runs SCHED_FIFO at RT_PRIORITY but is left free
 * to run on whatever CPU the Linux scheduler picks, and can migrate
 * between cores. Useful as a baseline to compare against the
 * isolated/pinned variant and quantify what isolation buys you. */

/* ---------------------------------------------------------------------
 * Time-arithmetic helpers (struct timespec add/diff)
 * ------------------------------------------------------------------- */
static inline void ts_add_ns(struct timespec *ts, uint64_t ns)
{
    ts->tv_nsec += ns;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
}

static inline int64_t ts_diff_ns(const struct timespec *a, const struct timespec *b)
{
    /* a - b, in nanoseconds */
    return (int64_t)(a->tv_sec - b->tv_sec) * 1000000000LL +
           (int64_t)(a->tv_nsec - b->tv_nsec);
}

/* 1. Detect CPU base frequency in GHz from /proc/cpuinfo */
double cpu_base_frequency()
{
    std::regex re("model name\\s*:[^@]+@\\s*([0-9.]+)\\s*GHz");
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::smatch m;
    for (std::string line; getline(cpuinfo, line);) {
        if (std::regex_search(line, m, re)) {
            if (m.size() == 2)
                return std::stod(m[1]);
        }
    }
    return 1.0; // Fallback: couldn't determine CPU frequency, count raw TSC ticks
}

// 2. Global inverse frequency constant (ns per clock cycle)
double CPU_GHZ_INV = 1.0;

/* Busy-wait for `wait_ns` using CLOCK_MONOTONIC */
static inline void busy_wait_ns(uint64_t wait_ns)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (ts_diff_ns(&now, &start) < (int64_t)wait_ns);
}

void *motion_rt_task(void *arg)
{
    (void)arg;
    struct timespec next_wakeup;
    struct timespec cycle_start, prev_cycle_start;
    struct timespec busywait_tm, busywait_end;
    uint64_t start_tsc, end_tsc, diff_ticks;
    double tsc_time_ns;
    bool have_prev_cycle = false;

    /* Get initial monotonic time */
    clock_gettime(CLOCK_MONOTONIC, &next_wakeup);

    printf("Motion RT task started (no CPU isolation/pinning)\n");
    printf("Busy wait : 250 us\n");
    printf("Period    : 1 ms\n");

    while (1)
    {
        /* 1. Schedule next 1 ms deadline and sleep */
        ts_add_ns(&next_wakeup, PERIOD_NS);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_wakeup, NULL);

        /* 2. Capture cycle period the INSTANT the thread wakes up */
        clock_gettime(CLOCK_MONOTONIC, &cycle_start);
        int64_t period_ns = !have_prev_cycle
                               ? (int64_t)PERIOD_NS
                               : ts_diff_ns(&cycle_start, &prev_cycle_start);
        prev_cycle_start = cycle_start;
        have_prev_cycle = true;

        /* 3. Perform busy wait workload */
        clock_gettime(CLOCK_MONOTONIC, &busywait_tm);
        start_tsc = __rdtsc();

        busy_wait_ns(BUSY_WAIT_NS);

        /* 4. Record end time */
        end_tsc = __rdtsc();
        clock_gettime(CLOCK_MONOTONIC, &busywait_end);
        int64_t xeno_elapsed_ns = ts_diff_ns(&busywait_end, &busywait_tm);

        /* Calculate elapsed CPU clock cycles and convert to nanoseconds */
        diff_ticks = end_tsc - start_tsc;
        tsc_time_ns = (double)diff_ticks * CPU_GHZ_INV;
        (void)tsc_time_ns; /* kept for parity; not logged below */

        /* 5. Log clean metrics (plain printf; see notes in isolated variant
         *    about page-fault/lock risk if this needs to be latency-free) */
        printf("Job Load :: %lld ||  Job Time :: %lld\n",
               (long long)xeno_elapsed_ns, (long long)period_ns);
    }

    return NULL;
}

int main(void)
{
    int ret;

    /* 1. Disable CPU C-States via /dev/cpu_dma_latency to eliminate sleep-wakeup latency */
    int dma_fd = open("/dev/cpu_dma_latency", O_RDWR);
    if (dma_fd >= 0) {
        int32_t latency_target = 0; // Request maximum responsiveness (no deep C-states)
        if (write(dma_fd, &latency_target, sizeof(latency_target)) == sizeof(latency_target)) {
            printf("Power Management: Deep C-States disabled via /dev/cpu_dma_latency\n");
        }
    }

    /* 2. Detect CPU base frequency and calculate inverse constant */
    double freq_ghz = cpu_base_frequency();
    CPU_GHZ_INV = 1.0 / freq_ghz;
    printf("Detected CPU Base Frequency : %.3f GHz\n", freq_ghz);
    printf("CPU_GHZ_INV (ns/cycle)      : %.6f\n", CPU_GHZ_INV);
    printf("CPU isolation/pinning       : DISABLED (thread is free to migrate)\n");

    /* 3. Lock memory to prevent page faults in real-time context */
    mlockall(MCL_CURRENT | MCL_FUTURE);

    /* 4. Create the real-time thread: SCHED_FIFO, no affinity set.
     *    Leaving pthread_attr's affinity unset means the thread inherits
     *    the process's default affinity mask (normally all online CPUs),
     *    so the scheduler is free to run and migrate it anywhere. */
    pthread_t motion_thread;
    pthread_attr_t attr;
    struct sched_param sched_param;

    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    sched_param.sched_priority = RT_PRIORITY;
    pthread_attr_setschedparam(&attr, &sched_param);
    pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + 1024 * 1024);

    ret = pthread_create(&motion_thread, &attr, motion_rt_task, NULL);
    if (ret) {
        fprintf(stderr, "Failed to create RT thread: %s\n", strerror(ret));
        return 1;
    }

    pthread_attr_destroy(&attr);

    /* Keep the main thread alive */
    while (1)
    {
        pause();
    }

    if (dma_fd >= 0) {
        close(dma_fd);
    }

    return 0;
}
