#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <signal.h>
#include <stdint.h>
#include <limits.h>

#define DEFAULT_BUSY_WAIT_NS  500000ULL   // 500 us (50% load for 1 ms period)
#define DEFAULT_PERIOD_NS    1000000ULL   // 1 ms (1,000,000 ns) period
#define DEFAULT_RT_PRIORITY           90   // Real-time FIFO priority
#define DEFAULT_FALLBACK_CPU           4
#define STACK_PREFAULT_SIZE  (64 * 1024)  // 64 KB stack pre-allocation

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static inline uint64_t timespec_to_ns(const struct timespec *ts) {
    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return timespec_to_ns(&ts);
}

static void prefault_stack(void) {
    unsigned char dummy[STACK_PREFAULT_SIZE];
    memset(dummy, 0, sizeof(dummy));
}

static inline void spin_workload_ns(uint64_t duration_ns) {
    uint64_t start = get_time_ns();
    while (get_time_ns() - start < duration_ns) {
        #if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
        #endif
    }
}

static int detect_isolated_cpu(void) {
    FILE *fp = fopen("/sys/devices/system/cpu/isolated", "r");
    if (fp) {
        char line[64];
        if (fgets(line, sizeof(line), fp)) {
            int core = -1;
            if (sscanf(line, "%d", &core) == 1 && core >= 0) {
                fclose(fp);
                return core;
            }
        }
        fclose(fp);
    }
    return DEFAULT_FALLBACK_CPU;
}

typedef struct {
    uint64_t busy_elapsed_ns;
    uint64_t measured_period_ns;
} LatencySample;

typedef struct {
    int target_cpu;
    uint64_t period_ns;
    uint64_t busy_wait_ns;
    int priority;
    long num_samples;
    LatencySample *buffer;
    long recorded_count;
} ThreadArgs;

static void* motion_rt_thread(void* arg) {
    ThreadArgs* targs = (ThreadArgs*)arg;
    uint64_t period_ns = targs->period_ns;
    uint64_t busy_wait_ns = targs->busy_wait_ns;
    long max_samples = targs->num_samples;

    prefault_stack();

    struct timespec next_wakeup;
    clock_gettime(CLOCK_MONOTONIC, &next_wakeup);

    uint64_t prev_cycle_start = 0;
    long sample_count = 0;

    while (g_running) {
        // 1. Advance next periodic deadline
        next_wakeup.tv_nsec += period_ns;
        while (next_wakeup.tv_nsec >= 1000000000L) {
            next_wakeup.tv_nsec -= 1000000000L;
            next_wakeup.tv_sec += 1;
        }

        // 2. High-precision absolute sleep
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_wakeup, NULL);

        // 3. Capture exact cycle start time
        uint64_t cycle_start = get_time_ns();
        uint64_t measured_period_ns = (prev_cycle_start == 0) ? period_ns : (cycle_start - prev_cycle_start);
        prev_cycle_start = cycle_start;

        // 4. Emulate compute payload
        uint64_t busy_start = get_time_ns();
        spin_workload_ns(busy_wait_ns);
        uint64_t busy_elapsed_ns = get_time_ns() - busy_start;

        // Store sample in RAM buffer (zero disk I/O / zero printf latency penalty during RT execution)
        if (targs->buffer && sample_count < max_samples) {
            targs->buffer[sample_count].busy_elapsed_ns = busy_elapsed_ns;
            targs->buffer[sample_count].measured_period_ns = measured_period_ns;
        }

        sample_count++;
        if (max_samples > 0 && sample_count >= max_samples) {
            break;
        }
    }

    targs->recorded_count = sample_count;
    return NULL;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int target_cpu = -1;
    uint64_t period_ns = DEFAULT_PERIOD_NS;
    uint64_t busy_wait_ns = DEFAULT_BUSY_WAIT_NS;
    int priority = DEFAULT_RT_PRIORITY;
    long num_samples = 600000;

    int opt;
    while ((opt = getopt(argc, argv, "c:p:l:r:n:h")) != -1) {
        switch (opt) {
            case 'c': target_cpu = atoi(optarg); break;
            case 'p': period_ns = strtoull(optarg, NULL, 10) * 1000ULL; break;
            case 'l': busy_wait_ns = strtoull(optarg, NULL, 10) * 1000ULL; break;
            case 'r': priority = atoi(optarg); break;
            case 'n': num_samples = atol(optarg); break;
            case 'h':
            default:
                printf("Usage: %s [-c core] [-p period_us] [-l load_us] [-r prio] [-n samples]\n", argv[0]);
                return 0;
        }
    }

    if (target_cpu < 0) {
        target_cpu = detect_isolated_cpu();
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        perror("Warning: mlockall failed");
    }

    int dma_fd = open("/dev/cpu_dma_latency", O_RDWR);
    if (dma_fd >= 0) {
        int32_t latency_target = 0;
        if (write(dma_fd, &latency_target, sizeof(latency_target)) != sizeof(latency_target)) {
            perror("Warning: failed setting /dev/cpu_dma_latency");
        }
    }

    LatencySample *buffer = NULL;
    if (num_samples > 0) {
        buffer = (LatencySample*)malloc(sizeof(LatencySample) * num_samples);
        if (!buffer) {
            fprintf(stderr, "Failed to allocate memory buffer for %ld samples\n", num_samples);
            if (dma_fd >= 0) close(dma_fd);
            return 1;
        }
    }

    ThreadArgs targs = {target_cpu, period_ns, busy_wait_ns, priority, num_samples, buffer, 0};

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);

    struct sched_param param;
    param.sched_priority = priority;
    pthread_attr_setschedparam(&attr, &param);

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(target_cpu, &cpuset);
    pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpuset);

    pthread_t rt_thread;
    if (pthread_create(&rt_thread, &attr, motion_rt_thread, &targs) != 0) {
        perror("Failed to create RT thread");
        if (buffer) free(buffer);
        if (dma_fd >= 0) close(dma_fd);
        return 1;
    }
    pthread_attr_destroy(&attr);

    pthread_join(rt_thread, NULL);

    if (buffer) {
        long count = targs.recorded_count;
        if (count > num_samples) count = num_samples;
        for (long i = 0; i < count; i++) {
            printf("Job Load :: %llu ||  Job Time :: %llu\n",
                   (unsigned long long)buffer[i].busy_elapsed_ns,
                   (unsigned long long)buffer[i].measured_period_ns);
        }
        free(buffer);
    }

    if (dma_fd >= 0) close(dma_fd);
    return 0;
}
