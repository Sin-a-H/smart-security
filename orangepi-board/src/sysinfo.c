#include "sysinfo.h"
#include <stdio.h>
#include <string.h>

int sysinfo_read_cpu_temp(double *out_celsius)
{
    FILE *f = fopen(SYSINFO_THERMAL_ZONE_PATH, "r");
    if (!f) {
        return -1;
    }
    long millidegrees = 0;
    int matched = fscanf(f, "%ld", &millidegrees);
    fclose(f);
    if (matched != 1) {
        return -1;
    }
    *out_celsius = millidegrees / 1000.0;
    return 0;
}

int sysinfo_read_memory(double *out_free_mb, double *out_total_mb)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) {
        return -1;
    }

    char line[256];
    long mem_total_kb = -1, mem_free_kb = -1, buffers_kb = 0, cached_kb = 0;

    while (fgets(line, sizeof(line), f)) {
        long value;
        if (sscanf(line, "MemTotal: %ld kB", &value) == 1) {
            mem_total_kb = value;
        } else if (sscanf(line, "MemFree: %ld kB", &value) == 1) {
            mem_free_kb = value;
        } else if (sscanf(line, "Buffers: %ld kB", &value) == 1) {
            buffers_kb = value;
        } else if (sscanf(line, "Cached: %ld kB", &value) == 1) {
            cached_kb = value;
        }
    }
    fclose(f);

    if (mem_total_kb < 0 || mem_free_kb < 0) {
        return -1;
    }

    /* "Free" here counts reclaimable buffers/cache as effectively free,
     * which better reflects what's actually available (matches `free -m`
     * "available" more closely than raw MemFree would). */
    double free_kb = (double)mem_free_kb + (double)buffers_kb + (double)cached_kb;

    *out_free_mb = free_kb / 1024.0;
    *out_total_mb = mem_total_kb / 1024.0;
    return 0;
}

int sysinfo_cpu_stat_sample(cpu_stat_sample_t *out)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) {
        return -1;
    }

    char cpu_label[8];
    int matched = fscanf(f, "%7s %llu %llu %llu %llu %llu %llu %llu %llu",
                          cpu_label,
                          &out->user, &out->nice, &out->system_, &out->idle,
                          &out->iowait, &out->irq, &out->softirq, &out->steal);
    fclose(f);

    if (matched != 9 || strcmp(cpu_label, "cpu") != 0) {
        return -1;
    }
    return 0;
}

double sysinfo_cpu_load_from_samples(const cpu_stat_sample_t *prev, const cpu_stat_sample_t *cur)
{
    unsigned long long prev_idle = prev->idle + prev->iowait;
    unsigned long long cur_idle  = cur->idle + cur->iowait;

    unsigned long long prev_non_idle = prev->user + prev->nice + prev->system_ +
                                        prev->irq + prev->softirq + prev->steal;
    unsigned long long cur_non_idle  = cur->user + cur->nice + cur->system_ +
                                        cur->irq + cur->softirq + cur->steal;

    unsigned long long prev_total = prev_idle + prev_non_idle;
    unsigned long long cur_total  = cur_idle + cur_non_idle;

    double total_delta = (double)(cur_total - prev_total);
    double idle_delta   = (double)(cur_idle - prev_idle);

    if (total_delta <= 0.0) {
        return 0.0;
    }

    double load_pct = (total_delta - idle_delta) / total_delta * 100.0;
    if (load_pct < 0.0) load_pct = 0.0;
    if (load_pct > 100.0) load_pct = 100.0;
    return load_pct;
}
