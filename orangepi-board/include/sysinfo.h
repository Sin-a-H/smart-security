/*
 * sysinfo.h
 * -----------------------------------------------------------------------
 * Direct readers for CPU temperature, memory, and CPU load.
 *
 * Per the assignment: "Temperature and memory should be read from the
 * corresponding directory directly in C code and the use of Linux
 * commands is not graded." So this module opens /sys and /proc files
 * itself; it never calls system()/popen() for these values.
 */

#ifndef SYSINFO_H
#define SYSINFO_H

/*
 * Reads CPU temperature in.
 * Orange Pi Zero: thermal zone 0 is the SoC sensor, exposed in millidegrees C.
 */
#define SYSINFO_THERMAL_ZONE_PATH "/sys/class/thermal/thermal_zone0/temp"

/* Returns 0 on success and fills *out_celsius, -1 on read failure. */
int sysinfo_read_cpu_temp(double *out_celsius);

/* Returns 0 on success; fills free/total memory in MB from /proc/meminfo. */
int sysinfo_read_memory(double *out_free_mb, double *out_total_mb);

/*
 * CPU load percentage measured as a delta between two /proc/stat
 * snapshots. Call sysinfo_cpu_load_sample() once, wait some interval
 * (the caller's polling loop already waits ~2s per the spec), then call
 * sysinfo_cpu_load_percent() which internally takes a fresh sample and
 * diffs it against the previous one, returning 0-100.
 */
typedef struct {
    unsigned long long user, nice, system_, idle, iowait, irq, softirq, steal;
} cpu_stat_sample_t;

int sysinfo_cpu_stat_sample(cpu_stat_sample_t *out);
double sysinfo_cpu_load_from_samples(const cpu_stat_sample_t *prev, const cpu_stat_sample_t *cur);

#endif /* SYSINFO_H */
