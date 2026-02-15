/*
 * ctop - A system resource monitor in C inspired by btop++
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>
#include <sys/time.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define TB_OPT_ATTR_W 32
#define TB_IMPL
#include "termbox2.h"

#define CTOP_VERSION "1.0.0"
#define REFRESH_RATE_MS 1000

/* Colors matching btop++ */
#define COLOR_BG 0x1a1a1a
#define COLOR_FG 0xcccccc
#define COLOR_CPU 0x88cc88
#define COLOR_CPU_GRAPH 0x44aa44
#define COLOR_MEM 0xccaa44
#define COLOR_NET_DOWN 0x44aaff
#define COLOR_NET_UP 0xff6666
#define COLOR_DISK 0xaa88cc
#define COLOR_PROC 0xcccccc
#define COLOR_HEADER 0x666666
#define COLOR_HIGH 0xff4444
#define COLOR_MED 0xffaa44
#define COLOR_LOW 0x44ff44
#define COLOR_BATTERY 0x88cc44
#define COLOR_TIME 0xffaa44

/* Maximum values */
#define MAX_PROCESSES 512
#define MAX_CPU_CORES 256
#define HISTORY_SIZE 120
#define MAX_DISKS 32

#define PROC_PID_WIDTH 8
#define PROC_CPU_WIDTH 6
#define PROC_MEM_WIDTH 8
#define PROC_PROG_MIN_WIDTH 8
#define PROC_CMD_MIN_WIDTH 8
#define PROC_USER_MIN_WIDTH 6
#define PROC_COLUMN_SPACING 4
#define PROC_NARROW_OFFSET 1

/* Superscript numbers */
static const char *SUPERSCRIPT[] = {"", "¹", "²", "³", "⁴", "⁵"};

/* Disk info structure */
typedef struct {
    char name[32];
    char device[32];
    char mount[256];
    unsigned long long total;
    unsigned long long used;
    unsigned long long free;
    unsigned long long read_sectors;
    unsigned long long write_sectors;
    unsigned long long prev_read;
    unsigned long long prev_write;
    float read_speed;
    float write_speed;
    int history_rx[HISTORY_SIZE];
    int history_tx[HISTORY_SIZE];
} DiskInfo;

/* Process information structure */
typedef struct {
    int pid;
    char name[256];
    char cmdline[512];
    char user[32];
    char state;
    long utime;
    long stime;
    long prev_utime;
    long prev_stime;
    long mem_rss;
    float cpu_percent;
    float cpu_percent_lazy;
    float mem_percent;
} ProcessInfo;

/* CPU core stats */
typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    unsigned long long prev_total, prev_idle;
    float percent;
    float history[HISTORY_SIZE];
    int history_idx;
} CoreStat;

/* System stats structure */
typedef struct {
    int num_cores;
    CoreStat cores[MAX_CPU_CORES];
    CoreStat overall;
    unsigned long total_mem;
    unsigned long free_mem;
    unsigned long available_mem;
    unsigned long buffers;
    unsigned long cached;
    unsigned long swap_total;
    unsigned long swap_free;
    float mem_percent;
    float swap_percent;
    int process_count;
    int running_count;
    ProcessInfo processes[MAX_PROCESSES];
    int cpu_history[HISTORY_SIZE];
    int mem_history[HISTORY_SIZE];
    int history_index;
    unsigned long long net_rx_bytes;
    unsigned long long net_tx_bytes;
    unsigned long long prev_net_rx;
    unsigned long long prev_net_tx;
    float net_rx_speed;
    float net_tx_speed;
    int net_history_rx[HISTORY_SIZE];
    int net_history_tx[HISTORY_SIZE];
    DiskInfo disks[MAX_DISKS];
    int num_disks;
    int battery_percent;
    int battery_present;
    char battery_status[16];
} SystemStats;

/* Pane visibility */
static int g_show_cpu = 1;
static int g_show_mem = 1;
static int g_show_disks = 1;
static int g_show_net = 1;
static int g_show_proc = 1;

static SystemStats g_stats = {0};
static int g_running = 1;
static int g_selected_process = 0;
static int g_scroll_offset = 0;

/* Sort modes */
#define SORT_CPU_LAZY 0
#define SORT_CPU_DIRECT 1
#define SORT_MEM 2
#define SORT_PID 3
#define SORT_NAME 4
#define SORT_MAX 5
static int g_sort_mode = SORT_CPU_LAZY;
static int g_refresh_rate_ms = REFRESH_RATE_MS;
static float g_elapsed_seconds = 1.0f;
static long g_clk_tck = 0;

const char *get_sort_name(void) {
    switch (g_sort_mode) {
        case SORT_CPU_LAZY: return "CPU-L";
        case SORT_CPU_DIRECT: return "CPU-D";
        case SORT_MEM: return "Mem";
        case SORT_PID: return "PID";
        case SORT_NAME: return "Name";
        default: return "CPU-L";
    }
}

int is_number(const char *str) {
    while (*str) {
        if (!isdigit(*str)) return 0;
        str++;
    }
    return 1;
}

void get_username(int uid, char *buf, size_t buflen) {
    struct passwd pwd;
    struct passwd *result;
    char *pwdbuf;
    size_t pwdbuflen = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (pwdbuflen == (size_t)-1) pwdbuflen = 16384;
    
    pwdbuf = malloc(pwdbuflen);
    if (!pwdbuf) {
        snprintf(buf, buflen, "%d", uid);
        return;
    }
    
    if (getpwuid_r(uid, &pwd, pwdbuf, pwdbuflen, &result) == 0 && result) {
        strncpy(buf, pwd.pw_name, buflen - 1);
        buf[buflen - 1] = '\0';
    } else {
        snprintf(buf, buflen, "%d", uid);
    }
    free(pwdbuf);
}

void format_bytes(unsigned long bytes, char *buf, size_t buflen) {
    const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int unit = 0;
    double val = bytes;
    while (val >= 1024 && unit < 4) {
        val /= 1024;
        unit++;
    }
    snprintf(buf, buflen, "%.2f %s", val, units[unit]);
}

void format_speed(float kbps, char *buf, size_t buflen) {
    if (kbps >= 1024 * 1024) {
        snprintf(buf, buflen, "%.2f GiB/s", kbps / (1024 * 1024));
    } else if (kbps >= 1024) {
        snprintf(buf, buflen, "%.2f MiB/s", kbps / 1024);
    } else {
        snprintf(buf, buflen, "%.2f KiB/s", kbps);
    }
}

void parse_cpu_stats(void) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return;
    
    char line[256];
    
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu", 3) != 0) continue;
        
        CoreStat *core = NULL;
        if (line[3] == ' ') {
            core = &g_stats.overall;
        } else {
            int n;
            if (sscanf(line, "cpu%d", &n) == 1 && n < MAX_CPU_CORES) {
                core = &g_stats.cores[n];
                if (n >= g_stats.num_cores) g_stats.num_cores = n + 1;
            } else {
                continue;
            }
        }
        
        if (!core) continue;
        
        unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
        sscanf(line, "%*s %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
        
        unsigned long long total = user + nice + system + idle + iowait + irq + softirq + steal;
        unsigned long long idle_time = idle + iowait;
        
        if (core->prev_total > 0) {
            unsigned long long total_diff = total - core->prev_total;
            unsigned long long idle_diff = idle_time - core->prev_idle;
            if (total_diff > 0) {
                core->percent = ((total_diff - idle_diff) * 100.0f) / total_diff;
            }
        }
        
        core->history[core->history_idx] = core->percent;
        core->history_idx = (core->history_idx + 1) % HISTORY_SIZE;
        
        core->prev_total = total;
        core->prev_idle = idle_time;
    }
    
    g_stats.cpu_history[g_stats.history_index] = (int)g_stats.overall.percent;
    fclose(fp);
}

void parse_meminfo(void) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return;
    
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        unsigned long val;
        if (sscanf(line, "MemTotal: %lu", &val) == 1) {
            g_stats.total_mem = val;
        } else if (sscanf(line, "MemFree: %lu", &val) == 1) {
            g_stats.free_mem = val;
        } else if (sscanf(line, "MemAvailable: %lu", &val) == 1) {
            g_stats.available_mem = val;
        } else if (sscanf(line, "Buffers: %lu", &val) == 1) {
            g_stats.buffers = val;
        } else if (sscanf(line, "Cached: %lu", &val) == 1) {
            g_stats.cached = val;
        } else if (sscanf(line, "SwapTotal: %lu", &val) == 1) {
            g_stats.swap_total = val;
        } else if (sscanf(line, "SwapFree: %lu", &val) == 1) {
            g_stats.swap_free = val;
        }
    }
    fclose(fp);
    
    if (g_stats.total_mem > 0) {
        unsigned long used = g_stats.total_mem - g_stats.available_mem;
        g_stats.mem_percent = (used * 100.0f) / g_stats.total_mem;
    }
    
    if (g_stats.swap_total > 0) {
        unsigned long used = g_stats.swap_total - g_stats.swap_free;
        g_stats.swap_percent = (used * 100.0f) / g_stats.swap_total;
    }
    
    g_stats.mem_history[g_stats.history_index] = (int)g_stats.mem_percent;
}

void parse_net_stats(void) {
    FILE *fp = fopen("/proc/net/dev", "r");
    if (!fp) return;
    
    char line[512];
    unsigned long long total_rx = 0, total_tx = 0;
    
    fgets(line, sizeof(line), fp);
    fgets(line, sizeof(line), fp);
    
    while (fgets(line, sizeof(line), fp)) {
        char iface[32];
        unsigned long long rx_bytes, tx_bytes;
        
        sscanf(line, "%s %llu %*u %*u %*u %*u %*u %*u %*u %llu",
               iface, &rx_bytes, &tx_bytes);
        
        if (strcmp(iface, "lo:") == 0) continue;
        
        total_rx += rx_bytes;
        total_tx += tx_bytes;
    }
    fclose(fp);
    
    if (g_stats.prev_net_rx > 0) {
        g_stats.net_rx_speed = (total_rx - g_stats.prev_net_rx) / 1024.0f;
        g_stats.net_tx_speed = (total_tx - g_stats.prev_net_tx) / 1024.0f;
    }
    
    g_stats.net_history_rx[g_stats.history_index] = (int)(g_stats.net_rx_speed / 100);
    g_stats.net_history_tx[g_stats.history_index] = (int)(g_stats.net_tx_speed / 100);
    
    g_stats.prev_net_rx = total_rx;
    g_stats.prev_net_tx = total_tx;
}

static unsigned int get_sector_size(const char *name) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/block/%s/queue/hw_sector_size", name);
    FILE *fp = fopen(path, "r");
    if (!fp) return 512;
    unsigned int size = 512;
    fscanf(fp, "%u", &size);
    fclose(fp);
    return size > 0 ? size : 512;
}

void parse_disk_stats(void) {
    FILE *fp = fopen("/proc/diskstats", "r");
    if (!fp) return;
    
    char line[256];
    DiskInfo new_disks[MAX_DISKS];
    int new_disk_count = 0;
    
    while (fgets(line, sizeof(line), fp) && new_disk_count < MAX_DISKS) {
        char name[32];
        unsigned long long read_sectors, write_sectors;
        
        if (sscanf(line, "%*d %*d %s %*u %*u %llu %*u %*u %*u %llu",
                   name, &read_sectors, &write_sectors) == 3) {
            
            if (strncmp(name, "loop", 4) == 0 || 
                strncmp(name, "ram", 3) == 0 ||
                strncmp(name, "dm-", 3) == 0) continue;
            
            unsigned int sector_size = get_sector_size(name);
            
            DiskInfo *disk = &new_disks[new_disk_count];
            memset(disk, 0, sizeof(DiskInfo));
            strncpy(disk->name, name, sizeof(disk->name) - 1);
            
            for (int i = 0; i < g_stats.num_disks; i++) {
                if (strcmp(g_stats.disks[i].name, name) == 0) {
                    unsigned long long read_diff = (read_sectors - g_stats.disks[i].read_sectors) * sector_size;
                    unsigned long long write_diff = (write_sectors - g_stats.disks[i].write_sectors) * sector_size;
                    disk->read_speed = read_diff / 1024.0f;
                    disk->write_speed = write_diff / 1024.0f;
                    memcpy(disk->history_rx, g_stats.disks[i].history_rx, sizeof(disk->history_rx));
                    memcpy(disk->history_tx, g_stats.disks[i].history_tx, sizeof(disk->history_tx));
                    break;
                }
            }
            
            disk->read_sectors = read_sectors;
            disk->write_sectors = write_sectors;
            disk->history_rx[g_stats.history_index] = (int)(disk->read_speed / 100);
            disk->history_tx[g_stats.history_index] = (int)(disk->write_speed / 100);
            
            new_disk_count++;
        }
    }
    fclose(fp);
    
    memcpy(g_stats.disks, new_disks, sizeof(new_disks[0]) * new_disk_count);
    g_stats.num_disks = new_disk_count;
}

void parse_battery(void) {
    DIR *dir = opendir("/sys/class/power_supply");
    if (!dir) {
        g_stats.battery_present = 0;
        return;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "BAT", 3) != 0) continue;
        
        char path[512];
        
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", entry->d_name);
        FILE *fp = fopen(path, "r");
        if (fp) {
            fscanf(fp, "%d", &g_stats.battery_percent);
            fclose(fp);
            g_stats.battery_present = 1;
        }
        
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status", entry->d_name);
        fp = fopen(path, "r");
        if (fp) {
            fgets(g_stats.battery_status, sizeof(g_stats.battery_status), fp);
            g_stats.battery_status[strcspn(g_stats.battery_status, "\n")] = '\0';
            fclose(fp);
        }
        break;
    }
    closedir(dir);
}

int compare_processes(const void *a, const void *b) {
    const ProcessInfo *pa = (const ProcessInfo *)a;
    const ProcessInfo *pb = (const ProcessInfo *)b;
    
    switch (g_sort_mode) {
        case SORT_CPU_DIRECT:
            if (pb->cpu_percent > pa->cpu_percent) return 1;
            if (pb->cpu_percent < pa->cpu_percent) return -1;
            break;
        case SORT_CPU_LAZY:
            if (pb->cpu_percent_lazy > pa->cpu_percent_lazy) return 1;
            if (pb->cpu_percent_lazy < pa->cpu_percent_lazy) return -1;
            break;
        case SORT_MEM:
            if (pb->mem_rss > pa->mem_rss) return 1;
            if (pb->mem_rss < pa->mem_rss) return -1;
            break;
        case SORT_PID:
            if (pa->pid > pb->pid) return 1;
            if (pa->pid < pb->pid) return -1;
            break;
        case SORT_NAME:
            return strcasecmp(pa->name, pb->name);
    }
    
    /* Secondary sort by PID for stable ordering */
    if (pa->pid > pb->pid) return 1;
    if (pa->pid < pb->pid) return -1;
    return 0;
}

void parse_processes(void) {
    DIR *dir = opendir("/proc");
    if (!dir) return;
    
    struct dirent *entry;
    
    /* Save previous process list for CPU delta calculation */
    ProcessInfo prev_procs[MAX_PROCESSES];
    int prev_count = g_stats.process_count;
    memcpy(prev_procs, g_stats.processes, sizeof(ProcessInfo) * prev_count);
    
    g_stats.process_count = 0;
    g_stats.running_count = 0;
    
    while ((entry = readdir(dir)) != NULL && g_stats.process_count < MAX_PROCESSES) {
        if (!is_number(entry->d_name)) continue;
        
        char path[512];
        snprintf(path, sizeof(path), "/proc/%s/stat", entry->d_name);
        
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        
        char line[1024];
        if (fgets(line, sizeof(line), fp)) {
            ProcessInfo *proc = &g_stats.processes[g_stats.process_count];
            memset(proc, 0, sizeof(ProcessInfo));
            
            char *p = strchr(line, '(');
            if (!p) { fclose(fp); continue; }
            
            sscanf(line, "%d", &proc->pid);
            
            char *end = strrchr(p, ')');
            if (!end) { fclose(fp); continue; }
            
            int comm_len = end - p - 1;
            if (comm_len < 0) comm_len = 0;
            if (comm_len >= 255) comm_len = 255;
            strncpy(proc->name, p + 1, comm_len);
            proc->name[comm_len] = '\0';
            
            int ppid, pgrp, session, tty_nr, tpgid, uid = 0;
            unsigned int flags;
            unsigned long minflt, cminflt, majflt, cmajflt, utime, stime;
            
            sscanf(end + 2, "%c %d %d %d %d %d %u %lu %lu %lu %lu %lu %lu",
                   &proc->state, &ppid, &pgrp, &session, &tty_nr, &tpgid,
                   &flags, &minflt, &cminflt, &majflt, &cmajflt, &utime, &stime);
            
            /* Read UID and memory from /proc/[pid]/status - more reliable */
            snprintf(path, sizeof(path), "/proc/%s/status", entry->d_name);
            FILE *status_fp = fopen(path, "r");
            if (status_fp) {
                char status_line[256];
                proc->mem_rss = 0;
                uid = 0;
                while (fgets(status_line, sizeof(status_line), status_fp)) {
                    unsigned long val;
                    if (sscanf(status_line, "VmRSS: %lu", &val) == 1) {
                        proc->mem_rss = val;
                    }
                    /* Parse Uid line: "Uid: 1000 1000 1000 1000" */
                    if (strncmp(status_line, "Uid:", 4) == 0) {
                        sscanf(status_line, "Uid: %d", &uid);
                    }
                }
                fclose(status_fp);
            }
            
            get_username(uid, proc->user, sizeof(proc->user));
            
            snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);
            FILE *cmd_fp = fopen(path, "r");
            if (cmd_fp) {
                size_t n = fread(proc->cmdline, 1, sizeof(proc->cmdline) - 1, cmd_fp);
                proc->cmdline[n] = '\0';
                for (size_t i = 0; i < n; i++) {
                    if (proc->cmdline[i] == '\0') proc->cmdline[i] = ' ';
                }
                fclose(cmd_fp);
            }
            
            if (strlen(proc->cmdline) == 0) {
                strncpy(proc->cmdline, proc->name, sizeof(proc->cmdline) - 1);
            }
            
            proc->mem_percent = g_stats.total_mem > 0 ? 
                (proc->mem_rss * 100.0f) / g_stats.total_mem : 0;
            
            /* Store current CPU times */
            long current_utime = utime;
            long current_stime = stime;
            
            /* Try to find previous values from saved processes */
            proc->cpu_percent = 0.0f;
            for (int j = 0; j < prev_count; j++) {
                if (prev_procs[j].pid == proc->pid) {
                    /* Found existing process - calculate CPU% from delta */
                    long delta_utime = current_utime - prev_procs[j].prev_utime;
                    long delta_stime = current_stime - prev_procs[j].prev_stime;
                    long delta_total = delta_utime + delta_stime;
                    
                    /* CPU% = (delta_ticks / clock_ticks_per_second) / elapsed_seconds * 100 / num_cores */
                    if (g_clk_tck <= 0) g_clk_tck = sysconf(_SC_CLK_TCK);
                    if (g_clk_tck <= 0) g_clk_tck = 100;
                    
                    float cpu_raw = 0.0f;
                    if (g_stats.num_cores > 0 && g_elapsed_seconds > 0) {
                        cpu_raw = (delta_total * 100.0f) / (g_clk_tck * g_elapsed_seconds * g_stats.num_cores);
                    }
                    proc->cpu_percent = cpu_raw;
                    
                    /* Lazy mode: exponential moving average (smoothing factor 0.3) */
                    if (proc->cpu_percent_lazy > 0 || cpu_raw > 0) {
                        proc->cpu_percent_lazy = proc->cpu_percent_lazy * 0.7f + cpu_raw * 0.3f;
                    } else {
                        proc->cpu_percent_lazy = cpu_raw;
                    }
                    break;
                }
            }
            
            /* Store current values for next time */
            proc->prev_utime = current_utime;
            proc->prev_stime = current_stime;
            
            if (proc->state == 'R') {
                g_stats.running_count++;
            }
            
            g_stats.process_count++;
        }
        
        fclose(fp);
    }
    
    closedir(dir);
    qsort(g_stats.processes, g_stats.process_count, sizeof(ProcessInfo), compare_processes);
}

void update_stats(void) {
    parse_cpu_stats();
    parse_meminfo();
    parse_net_stats();
    parse_disk_stats();
    parse_battery();
    parse_processes();
    g_stats.history_index = (g_stats.history_index + 1) % HISTORY_SIZE;
}

/* Draw functions */
void draw_section_header(int x, int y, int num, const char *title, uint32_t color) {
    tb_print(x, y, color, COLOR_BG, "[");
    tb_print(x + 1, y, color | TB_BOLD, COLOR_BG, SUPERSCRIPT[num]);
    tb_print(x + 1 + strlen(SUPERSCRIPT[num]), y, color, COLOR_BG, title);
    tb_print(x + 1 + strlen(SUPERSCRIPT[num]) + strlen(title), y, color, COLOR_BG, "]");
}

void draw_graph(int x, int y, int w, int h, float *data, int idx, uint32_t color) {
    const char *blocks[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    int idx_start = (idx - w + HISTORY_SIZE) % HISTORY_SIZE;
    
    for (int col = 0; col < w && col < HISTORY_SIZE; col++) {
        int data_idx = (idx_start + col) % HISTORY_SIZE;
        float val = data[data_idx];
        int height = (int)((val / 100.0f) * h);
        float frac = ((val / 100.0f) * h) - height;
        int block_idx = (int)(frac * 7);
        if (block_idx > 6) block_idx = 6;
        if (block_idx < 0) block_idx = 0;
        
        for (int row = 0; row < h; row++) {
            int cy = y + h - 1 - row;
            if (cy < y) continue;
            if (row < height) {
                tb_print(x + col, cy, color, COLOR_BG, "█");
            } else if (row == height && block_idx >= 0) {
                tb_print(x + col, cy, color, COLOR_BG, blocks[block_idx]);
            } else {
                tb_set_cell(x + col, cy, ' ', COLOR_FG, COLOR_BG);
            }
        }
    }
}

void draw_mini_bar(int x, int y, int w, float percent, uint32_t color) {
    const char *blocks[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    int filled = (int)((percent / 100.0f) * w);
    float frac = ((percent / 100.0f) * w) - filled;
    int frac_idx = (int)(frac * 7);
    
    for (int i = 0; i < w; i++) {
        if (i < filled) {
            tb_print(x + i, y, color, COLOR_BG, "█");
        } else if (i == filled && frac_idx >= 0) {
            tb_print(x + i, y, color, COLOR_BG, blocks[frac_idx]);
        } else {
            tb_set_cell(x + i, y, ' ', COLOR_FG, COLOR_BG);
        }
    }
}

void draw_sparkline_horizontal(int x, int y, int w, float *data, int idx, uint32_t color) {
    const char *blocks[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    int idx_start = (idx - w + HISTORY_SIZE) % HISTORY_SIZE;
    
    for (int col = 0; col < w && col < HISTORY_SIZE; col++) {
        int data_idx = (idx_start + col) % HISTORY_SIZE;
        float val = data[data_idx];
        int block_idx = (int)((val / 100.0f) * 7);
        if (block_idx > 7) block_idx = 7;
        if (block_idx < 0) block_idx = 0;
        tb_print(x + col, y, color, COLOR_BG, blocks[block_idx]);
    }
}

/* CPU section - full width at top */
void draw_cpu_section(int x, int y, int w, int h) {
    draw_section_header(x, y, 1, "cpu", COLOR_CPU);
    
    if (h < 3) return;
    
    uint32_t color = g_stats.overall.percent > 80 ? COLOR_HIGH :
                     g_stats.overall.percent > 50 ? COLOR_MED : COLOR_LOW;
    
    /* Overall CPU - compact inline with header */
    tb_printf(x + 6, y, COLOR_FG, COLOR_BG, "CPU ");
    int header_bar_w = (w > 40) ? 12 : (w > 30 ? 8 : 5);
    draw_mini_bar(x + 10, y, header_bar_w, g_stats.overall.percent, color);
    tb_printf(x + 10 + header_bar_w + 1, y, color, COLOR_BG, "%3.0f%%", g_stats.overall.percent);
    
    if (h < 4) return;
    
    /* Compact history graph - 1-2 rows */
    int graph_w = w - 2;
    int graph_h = (h > 8) ? 2 : 1;
    if (graph_w > 60) graph_w = 60;
    draw_graph(x, y + 1, graph_w, graph_h, g_stats.overall.history, g_stats.overall.history_idx, COLOR_CPU);
    
    /* Per-core CPUs */
    int core_start_y = y + 1 + graph_h;
    if (core_start_y >= y + h - 1) return;
    
    int core_label_width = (g_stats.num_cores >= 100) ? 4 : (g_stats.num_cores >= 10 ? 3 : 2);
    int available_core_rows = (y + h - 1) - core_start_y;
    
    /* Calculate for two-line display */
    int two_line_item_width = core_label_width + 12;
    int max_cores_per_row = (w - 2) / two_line_item_width;
    if (max_cores_per_row < 1) max_cores_per_row = 1;
    
    /* Check if we can fit all cores with 2-line display */
    int min_rows_needed = (g_stats.num_cores + max_cores_per_row - 1) / max_cores_per_row;
    int use_two_line = (available_core_rows >= min_rows_needed * 2);
    
    if (use_two_line) {
        /* Two-line per core: calculate optimal cores per row for even distribution */
        /* Find the number of rows that gives us the most even distribution */
        int best_cores_per_row = max_cores_per_row;
        int best_rows = min_rows_needed;
        
        /* Try to find a better distribution by using fewer cores per row */
        for (int test_cores_per_row = max_cores_per_row; test_cores_per_row >= 1; test_cores_per_row--) {
            int rows_needed = (g_stats.num_cores + test_cores_per_row - 1) / test_cores_per_row;
            if (rows_needed * 2 > available_core_rows) break;  /* Too many rows */
            
            /* Check if this gives a more even distribution */
            int cores_in_last_row = g_stats.num_cores - (rows_needed - 1) * test_cores_per_row;
            int cores_in_full_rows = test_cores_per_row;
            
            /* Prefer configurations where all rows have the same number of cores */
            if (cores_in_last_row == cores_in_full_rows || 
                (best_rows != rows_needed && rows_needed <= available_core_rows / 2)) {
                best_cores_per_row = test_cores_per_row;
                best_rows = rows_needed;
                /* Stop if we found a perfect distribution */
                if (cores_in_last_row == cores_in_full_rows) break;
            }
        }
        
        int cores_per_row = best_cores_per_row;
        
        /* Recalculate item width based on actual cores_per_row to fill available space */
        int usable_width = w - 2;
        int actual_item_width = usable_width / cores_per_row;
        if (actual_item_width < two_line_item_width) actual_item_width = two_line_item_width;
        
        for (int i = 0; i < g_stats.num_cores && i < MAX_CPU_CORES; i++) {
            int col = i % cores_per_row;
            int row_group = i / cores_per_row;
            int cx = x + col * actual_item_width;
            int cy1 = core_start_y + row_group * 2;
            int cy2 = cy1 + 1;
            
            if (cy2 >= y + h - 1) break;
            
            CoreStat *core = &g_stats.cores[i];
            uint32_t ccolor = core->percent > 80 ? COLOR_HIGH :
                              core->percent > 50 ? COLOR_MED : COLOR_LOW;
            
            /* Line 1: C0 [████████] 45% */
            tb_printf(cx, cy1, COLOR_FG, COLOR_BG, "C%-*d", core_label_width - 1, i);
            int bar_width = actual_item_width - core_label_width - 5;
            if (bar_width > 10) bar_width = 10;
            draw_mini_bar(cx + core_label_width, cy1, bar_width, core->percent, ccolor);
            tb_printf(cx + core_label_width + bar_width + 1, cy1, ccolor, COLOR_BG, "%3.0f%%", core->percent);
            
            /* Line 2: mini sparkline */
            int spark_width = actual_item_width - 2;
            draw_sparkline_horizontal(cx + 1, cy2, spark_width, core->history, core->history_idx, ccolor);
        }
    } else {
        /* Single-line per core: compact display */
        int single_line_item_width = core_label_width + 6;
        int cores_per_row = (w - 2) / single_line_item_width;
        if (cores_per_row < 1) cores_per_row = 1;
        
        int max_cores_display = cores_per_row * available_core_rows;
        if (max_cores_display > g_stats.num_cores) max_cores_display = g_stats.num_cores;
        
        for (int i = 0; i < max_cores_display && i < MAX_CPU_CORES; i++) {
            int cx = x + (i % cores_per_row) * single_line_item_width;
            int cy = core_start_y + (i / cores_per_row);
            
            if (cy >= y + h - 1) break;
            
            CoreStat *core = &g_stats.cores[i];
            uint32_t ccolor = core->percent > 80 ? COLOR_HIGH :
                              core->percent > 50 ? COLOR_MED : COLOR_LOW;
            
            /* Ultra-compact: C0 ██ 45% */
            tb_printf(cx, cy, COLOR_FG, COLOR_BG, "C%-*d", core_label_width - 1, i);
            draw_mini_bar(cx + core_label_width, cy, 2, core->percent, ccolor);
            tb_printf(cx + core_label_width + 3, cy, ccolor, COLOR_BG, "%2.0f%%", core->percent);
        }
    }
}

/* Memory section */
void draw_memory_section(int x, int y, int w, int h) {
    draw_section_header(x, y, 2, "mem", COLOR_MEM);
    
    if (h < 4) return;  /* Too small to render */
    
    unsigned long used = g_stats.total_mem - g_stats.available_mem;
    unsigned long cached = g_stats.cached + g_stats.buffers;
    char buf[64];
    
    uint32_t used_color = g_stats.mem_percent > 80 ? COLOR_HIGH :
                          g_stats.mem_percent > 50 ? COLOR_MED : COLOR_LOW;
    
    int line = y + 2;
    int max_line = y + h - 1;
    
    /* Used with bar - always show this */
    if (line < max_line) {
        tb_printf(x, line, COLOR_FG, COLOR_BG, "Used:");
        format_bytes(used * 1024, buf, sizeof(buf));
        if (w > 30) {
            tb_printf(x + 10, line, used_color | TB_BOLD, COLOR_BG, "%10s", buf);
            draw_mini_bar(x + 22, line, w - 26, g_stats.mem_percent, used_color);
        } else {
            tb_printf(x + 6, line, used_color | TB_BOLD, COLOR_BG, "%s", buf);
        }
        line++;
    }
    
    /* Total */
    if (line < max_line) {
        tb_printf(x, line, COLOR_FG, COLOR_BG, "Total:");
        format_bytes(g_stats.total_mem * 1024, buf, sizeof(buf));
        tb_printf(x + 10, line, COLOR_FG | TB_BOLD, COLOR_BG, "%10s", buf);
        line++;
    }
    
    /* Available */
    if (line < max_line) {
        tb_printf(x, line, COLOR_FG, COLOR_BG, "Free:");
        format_bytes(g_stats.available_mem * 1024, buf, sizeof(buf));
        tb_printf(x + 10, line, COLOR_LOW | TB_BOLD, COLOR_BG, "%10s", buf);
        line++;
    }
    
    /* Cached - only if space */
    if (line < max_line && h > 6) {
        tb_printf(x, line, COLOR_FG, COLOR_BG, "Cached:");
        format_bytes(cached * 1024, buf, sizeof(buf));
        tb_printf(x + 10, line, COLOR_FG | TB_BOLD, COLOR_BG, "%10s", buf);
        line++;
    }
    
    /* Memory graph - show if there's any space */
    if (line < max_line) {
        int graph_h = max_line - line;
        if (graph_h > 3) graph_h = 3;
        if (graph_h < 1) graph_h = 1;
        draw_graph(x, line, w - 2, graph_h, (float*)g_stats.mem_history, 
                   g_stats.history_index, COLOR_MEM);
    }
}

/* Disk section */
void draw_disk_section(int x, int y, int w, int h) {
    draw_section_header(x, y, 3, "disk", COLOR_DISK);
    
    if (h < 4) return;
    
    int line = y + 2;
    int max_line = y + h - 1;
    int disks_per_row = (w > 60) ? 2 : 1;  /* Show 2 disks side-by-side if wide enough */
    int disk_width = (w - 2) / disks_per_row;
    
    for (int i = 0; i < g_stats.num_disks && line < max_line - 1; i++) {
        DiskInfo *disk = &g_stats.disks[i];
        char buf[32];
        int disk_x = x + (i % disks_per_row) * disk_width;
        
        /* Disk name */
        tb_printf(disk_x, line, COLOR_DISK | TB_BOLD, COLOR_BG, "%-8s", disk->name);
        
        /* Read speed */
        if (line + 1 < max_line) {
            tb_printf(disk_x, line + 1, COLOR_NET_DOWN, COLOR_BG, "▼");
            format_speed(disk->read_speed, buf, sizeof(buf));
            tb_printf(disk_x + 2, line + 1, COLOR_FG, COLOR_BG, "%-10s", buf);
        }
        
        /* Write speed */
        if (line + 2 < max_line) {
            tb_printf(disk_x, line + 2, COLOR_NET_UP, COLOR_BG, "▲");
            format_speed(disk->write_speed, buf, sizeof(buf));
            tb_printf(disk_x + 2, line + 2, COLOR_FG, COLOR_BG, "%-10s", buf);
        }
        
        /* I/O graph */
        if (line + 3 < max_line && disk_width > 15) {
            int graph_h = 2;
            int graph_w = disk_width - 2;
            if (graph_w > 30) graph_w = 30;
            
            /* Combine read and write for total I/O graph */
            float combined_history[HISTORY_SIZE];
            for (int j = 0; j < HISTORY_SIZE; j++) {
                combined_history[j] = disk->history_rx[j] + disk->history_tx[j];
                if (combined_history[j] > 100) combined_history[j] = 100;
            }
            
            draw_graph(disk_x, line + 3, graph_w, graph_h, combined_history, 
                       g_stats.history_index, COLOR_DISK);
        }
        
        /* Move to next row of disks if we've filled this one */
        if ((i + 1) % disks_per_row == 0) {
            line += 6;  /* Name + read + write + graph + spacing */
        }
    }
}

/* Network section */
void draw_net_section(int x, int y, int w, int h) {
    draw_section_header(x, y, 4, "net", COLOR_NET_DOWN);
    
    if (h < 4) return;
    
    char buf[32];
    int line = y + 2;
    int max_line = y + h - 1;
    
    /* Download */
    if (line < max_line) {
        tb_printf(x, line, COLOR_NET_DOWN, COLOR_BG, "▼ down ");
        format_speed(g_stats.net_rx_speed, buf, sizeof(buf));
        tb_printf(x + 10, line++, COLOR_FG | TB_BOLD, COLOR_BG, "%s", buf);
    }
    
    /* Download graph */
    if (line + 1 < max_line && h > 5) {
        int graph_h = max_line - line - 2;
        if (graph_h > 2) graph_h = 2;
        if (graph_h > 0) {
            draw_graph(x, line, w - 2, graph_h, (float*)g_stats.net_history_rx, 
                       g_stats.history_index, COLOR_NET_DOWN);
            line += graph_h;
        }
    }
    
    /* Upload */
    if (line < max_line) {
        tb_printf(x, line, COLOR_NET_UP, COLOR_BG, "▲ up   ");
        format_speed(g_stats.net_tx_speed, buf, sizeof(buf));
        tb_printf(x + 10, line++, COLOR_FG | TB_BOLD, COLOR_BG, "%s", buf);
    }
    
    /* Upload graph */
    if (line < max_line && h > 6) {
        int graph_h = max_line - line;
        if (graph_h > 2) graph_h = 2;
        if (graph_h > 0) {
            draw_graph(x, line, w - 2, graph_h, (float*)g_stats.net_history_tx,
                       g_stats.history_index, COLOR_NET_UP);
        }
    }
}

/* Process list */
void draw_process_list(int x, int y, int w, int h) {
    draw_section_header(x, y, 5, "proc", COLOR_PROC);
    
    if (h < 5) return;
    
    int list_start = y + 2;
    int list_height = h - 3;
    int max_line = y + h - 1;
    
    /* Calculate column widths based on available width */
    int pid_width = PROC_PID_WIDTH;
    int cpu_width = PROC_CPU_WIDTH;
    int mem_width = PROC_MEM_WIDTH;
    int min_prog_width = PROC_PROG_MIN_WIDTH;
    int min_cmd_width = PROC_CMD_MIN_WIDTH;
    int min_user_width = PROC_USER_MIN_WIDTH;
    
    /* Available width for variable columns */
    int fixed_width = pid_width + mem_width + cpu_width + PROC_COLUMN_SPACING;
    int var_width = w - fixed_width;
    
    /* Determine what columns to show based on width */
    int show_user = 1;
    int show_cmd = 1;
    int prog_width, cmd_width, user_width;
    
    if (var_width < min_prog_width + min_user_width) {
        /* Very narrow - just show PID, Program, CPU% */
        show_user = 0;
        show_cmd = 0;
        prog_width = var_width - PROC_NARROW_OFFSET;
        if (prog_width < 6) prog_width = 6;
    } else if (var_width < min_prog_width + min_cmd_width + min_user_width) {
        /* Narrow - show PID, Program, User, CPU% */
        show_cmd = 0;
        prog_width = var_width * 60 / 100;
        if (prog_width < min_prog_width) prog_width = min_prog_width;
        user_width = var_width - prog_width - 1;
        if (user_width < min_user_width) user_width = min_user_width;
    } else {
        /* Normal width - show all columns */
        prog_width = var_width * 30 / 100;
        cmd_width = var_width * 40 / 100;
        user_width = var_width - prog_width - cmd_width - 2;
        if (prog_width < min_prog_width) prog_width = min_prog_width;
        if (cmd_width < min_cmd_width) cmd_width = min_cmd_width;
        if (user_width < min_user_width) user_width = min_user_width;
    }
    
    /* Column headers - only draw what fits */
    int cx = x;
    tb_printf(cx, list_start - 1, COLOR_HEADER | TB_BOLD, COLOR_BG, "%-*s", pid_width, "Pid:");
    cx += pid_width + 1;
    
    tb_printf(cx, list_start - 1, COLOR_HEADER | TB_BOLD, COLOR_BG, "%-*s", prog_width, "Program:");
    cx += prog_width + 1;
    
    if (show_cmd) {
        tb_printf(cx, list_start - 1, COLOR_HEADER | TB_BOLD, COLOR_BG, "%-*s", cmd_width, "Command:");
        cx += cmd_width + 1;
    }
    
    if (show_user) {
        tb_printf(cx, list_start - 1, COLOR_HEADER | TB_BOLD, COLOR_BG, "%-*s", user_width, "User:");
        cx += user_width + 1;
    }
    
    tb_printf(cx, list_start - 1, COLOR_HEADER | TB_BOLD, COLOR_BG, "%-*s", mem_width, "MemB");
    cx += mem_width + 1;
    
    tb_printf(cx, list_start - 1, COLOR_HEADER | TB_BOLD, COLOR_BG, "%-*s", cpu_width, "Cpu%");
    
    /* Scroll handling */
    if (g_selected_process < g_scroll_offset) {
        g_scroll_offset = g_selected_process;
    } else if (g_selected_process >= g_scroll_offset + list_height) {
        g_scroll_offset = g_selected_process - list_height + 1;
    }
    
    /* Process rows */
    for (int i = 0; i < list_height && (g_scroll_offset + i) < g_stats.process_count; i++) {
        int idx = g_scroll_offset + i;
        ProcessInfo *proc = &g_stats.processes[idx];
        int row = list_start + i;
        
        if (row >= max_line) break;
        
        uint32_t row_fg = COLOR_FG;
        uint32_t row_bg = COLOR_BG;
        
        if (idx == g_selected_process) {
            row_fg = TB_BLACK;
            row_bg = COLOR_HEADER;
        }
        
        /* Truncate strings to fit */
        char name[256], cmd[256], user[32], mem_buf[32];
        strncpy(name, proc->name, prog_width);
        name[prog_width] = '\0';
        
        if (show_cmd) {
            strncpy(cmd, proc->cmdline, cmd_width);
            cmd[cmd_width] = '\0';
        }
        
        if (show_user) {
            strncpy(user, proc->user, user_width);
            user[user_width] = '\0';
        }
        
        format_bytes(proc->mem_rss * 1024, mem_buf, sizeof(mem_buf));
        /* Truncate memory string if needed */
        if ((int)strlen(mem_buf) > mem_width) {
            mem_buf[mem_width] = '\0';
        }
        
        uint32_t cpu_color = proc->cpu_percent > 50 ? COLOR_HIGH :
                             proc->cpu_percent > 20 ? COLOR_MED : COLOR_LOW;
        
        /* Draw row with proper widths */
        cx = x;
        tb_printf(cx, row, row_fg, row_bg, "%-*d", pid_width, proc->pid);
        cx += pid_width + 1;
        
        tb_printf(cx, row, row_fg, row_bg, "%-*s", prog_width, name);
        cx += prog_width + 1;
        
        if (show_cmd) {
            tb_printf(cx, row, row_fg, row_bg, "%-*s", cmd_width, cmd);
            cx += cmd_width + 1;
        }
        
        if (show_user) {
            tb_printf(cx, row, row_fg, row_bg, "%-*s", user_width, user);
            cx += user_width + 1;
        }
        
        tb_printf(cx, row, row_fg, row_bg, "%-*s", mem_width, mem_buf);
        cx += mem_width + 1;
        
        tb_printf(cx, row, cpu_color | (idx == g_selected_process ? 0 : TB_BOLD), row_bg,
                  "%*.1f", cpu_width - 1, proc->cpu_percent);
    }
    
    /* Status bar at bottom - show sort mode */
    if (max_line > y + 2) {
        char status[128];
        snprintf(status, sizeof(status), "%d/%d | %d | Sort:%s",
                 g_stats.running_count, g_stats.process_count, g_selected_process + 1, get_sort_name());
        int status_len = strlen(status);
        if (status_len > w - 2) status_len = w - 2;
        tb_printf(x, max_line, COLOR_FG, COLOR_BG, "%s", status);
    }
}

void draw_top_bar(int w) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);
    
    tb_printf(w / 2 - 4, 0, COLOR_TIME | TB_BOLD, COLOR_BG, "%s", time_str);
    
    if (g_stats.battery_present) {
        int batt_x = w - 20;
        uint32_t batt_color = g_stats.battery_percent < 20 ? COLOR_HIGH :
                              g_stats.battery_percent < 50 ? COLOR_MED : COLOR_BATTERY;
        
        const char *icon = strstr(g_stats.battery_status, "Charging") ? "▲" :
                          strstr(g_stats.battery_status, "Discharging") ? "▼" : "●";
        
        tb_printf(batt_x, 0, batt_color, COLOR_BG, "BAT%s %d%%", icon, g_stats.battery_percent);
        draw_mini_bar(batt_x + 10, 0, 8, g_stats.battery_percent, batt_color);
    }
    
    tb_printf(2, 0, COLOR_HEADER | TB_BOLD, COLOR_BG, "ctop %s", CTOP_VERSION);
}

void draw_help_bar(int y, int w) {
    tb_printf(2, y, COLOR_FG, COLOR_BG, 
              "1-5:toggle | C-f/b:sort | C-n/p:nav | C-v/M-v:page | C-a/e:home/end | q:quit");
}

/* Calculate minimum required dimensions - optimized for density like btop++ */
void calculate_minimum_size(int *min_w, int *min_h) {
    *min_w = 80;  /* Absolute minimum width */
    *min_h = 24;  /* Absolute minimum height */
    
    int num_left_panes = g_show_mem + g_show_disks + g_show_net;
    
    /* Base height: top bar (1) + help bar (1) = 2, minimum usable area = 22 */
    int base_h = 2;
    int content_h = 0;
    
    /* CPU pane - can be as small as 5 rows */
    if (g_show_cpu) {
        content_h += 5;  /* Header (1) + graph (2) + cores (2+) */
    }
    
    /* Bottom section - left panes stacked + proc list */
    if (num_left_panes > 0 || g_show_proc) {
        /* Each left pane needs minimum 4 rows */
        int left_panes_h = num_left_panes * 4;
        
        /* Proc list needs at least 6 rows */
        int proc_h = g_show_proc ? 6 : 0;
        
        /* Bottom section height is max of left or right */
        int bottom_h = (left_panes_h > proc_h) ? left_panes_h : proc_h;
        if (bottom_h < 6) bottom_h = 6;  /* Absolute minimum for bottom */
        
        content_h += bottom_h;
        
        /* Width requirements - much more compact */
        int min_proc_width = g_show_proc ? 45 : 0;  /* Proc list can be narrow */
        int min_left_width = num_left_panes > 0 ? 20 : 0;  /* Left panes can be 20 chars */
        int required_width = min_left_width + min_proc_width + 4; /* +4 for margins and gap */
        
        if (required_width > *min_w) *min_w = required_width;
    }
    
    *min_h = base_h + content_h;
    if (*min_h < 10) *min_h = 10;  /* Absolute minimum */
}

/* Draw error screen when terminal is too small */
void draw_error_screen(int w, int h) {
    tb_clear();
    
    int min_w, min_h;
    calculate_minimum_size(&min_w, &min_h);
    
    int y = 2;
    int x = 2;
    
    /* Error message */
    tb_printf(x, y++, COLOR_HIGH | TB_BOLD, COLOR_BG, 
              "ERROR: Terminal too small!");
    y++;
    
    /* Current vs required size */
    tb_printf(x, y++, COLOR_FG, COLOR_BG, 
              "Current size: %dx%d", w, h);
    tb_printf(x, y++, COLOR_FG, COLOR_BG, 
              "Required size: %dx%d (for current layout)", min_w, min_h);
    y++;
    
    /* Pane status */
    tb_printf(x, y++, COLOR_HEADER | TB_BOLD, COLOR_BG, "Pane Status:");
    tb_printf(x, y++, g_show_cpu ? COLOR_LOW : COLOR_HIGH, COLOR_BG, 
              "  [1] CPU: %s", g_show_cpu ? "ON" : "OFF");
    tb_printf(x, y++, g_show_mem ? COLOR_LOW : COLOR_HIGH, COLOR_BG, 
              "  [2] Memory: %s", g_show_mem ? "ON" : "OFF");
    tb_printf(x, y++, g_show_disks ? COLOR_LOW : COLOR_HIGH, COLOR_BG, 
              "  [3] Disk: %s", g_show_disks ? "ON" : "OFF");
    tb_printf(x, y++, g_show_net ? COLOR_LOW : COLOR_HIGH, COLOR_BG, 
              "  [4] Network: %s", g_show_net ? "ON" : "OFF");
    tb_printf(x, y++, g_show_proc ? COLOR_LOW : COLOR_HIGH, COLOR_BG, 
              "  [5] Processes: %s", g_show_proc ? "ON" : "OFF");
    y++;
    
    /* Instructions */
    tb_printf(x, y++, COLOR_FG, COLOR_BG, 
              "Press 1-5 to toggle panes, or resize terminal.");
    tb_printf(x, y++, COLOR_FG, COLOR_BG, 
              "Press 'q' to quit.");
    
    tb_present();
}

/* Main layout matching btop++ exactly */
void draw_screen(void) {
    int w = tb_width();
    int h = tb_height();
    
    /* Check if terminal is large enough */
    int min_w, min_h;
    calculate_minimum_size(&min_w, &min_h);
    
    if (w < min_w || h < min_h) {
        draw_error_screen(w, h);
        return;
    }
    
    tb_clear();
    
    /* Top bar */
    draw_top_bar(w);
    
    /* Layout matching btop++ - compact:
     * Row 1: CPU (full width)
     * Row 2: Left (Mem/Disk/Net stacked) | Right (Process list)
     */
    
    int top_margin = 1;
    int bottom_margin = 1;
    int available_height = h - top_margin - bottom_margin;
    
    /* Smart height allocation based on available space */
    int cpu_height = 0;
    if (g_show_cpu) {
        /* CPU gets minimum 5 rows, but process list should get priority for extra space */
        cpu_height = 5;
        
        /* Only give CPU more space if terminal is very tall and we have room */
        int min_proc_rows = 10;  /* Minimum useful process list rows */
        int other_panes = (g_show_mem || g_show_disks || g_show_net) ? 3 : 0;  /* Rough estimate */
        int needed_for_bottom = min_proc_rows + other_panes;
        
        if (available_height > cpu_height + needed_for_bottom) {
            /* We have extra space - give most to bottom section */
            int extra = available_height - cpu_height - needed_for_bottom;
            cpu_height += extra / 4;  /* CPU gets 1/4 of extra, bottom gets 3/4 */
            if (cpu_height > available_height / 3) cpu_height = available_height / 3;  /* Max 33% */
        }
    }
    
    int bottom_height = available_height - cpu_height;
    if (bottom_height < 6 && (g_show_mem || g_show_disks || g_show_net || g_show_proc)) {
        if (g_show_cpu) {
            cpu_height = available_height - 6;
            if (cpu_height < 3) cpu_height = 0;
        }
        bottom_height = available_height - cpu_height;
    }
    
    /* Row 1: CPU section - full width */
    int current_y = top_margin;
    if (g_show_cpu && cpu_height > 0) {
        draw_cpu_section(1, current_y, w - 2, cpu_height);
        current_y += cpu_height;
    }
    
    /* Row 2: Split view - Left (Mem/Disk/Net) | Right (Process list) */
    int bottom_y = current_y;
    
    /* Calculate widths */
    int num_left_panes = g_show_mem + g_show_disks + g_show_net;
    int proc_width = 0;
    int left_width = 0;
    
    if (g_show_proc && num_left_panes == 0) {
        proc_width = w - 2;
    } else if (!g_show_proc && num_left_panes > 0) {
        left_width = w - 2;
    } else if (g_show_proc && num_left_panes > 0) {
        left_width = (w - 3) * 35 / 100;
        if (left_width < 18) left_width = 18;
        proc_width = (w - 3) - left_width;
        if (proc_width < 40) {
            proc_width = 40;
            left_width = (w - 3) - proc_width;
        }
    }
    
    /* Left side: Mem, Disk, Net stacked - use all available height */
    if (num_left_panes > 0 && left_width > 0) {
        /* Distribute bottom_height among left panes proportionally */
        int left_y = bottom_y;
        int remaining_height = bottom_height;
        
        /* Give each pane proportional height, with minimums */
        int base_pane_height = remaining_height / num_left_panes;
        
        if (g_show_mem && remaining_height > 0) {
            int pane_h = base_pane_height;
            if (pane_h < 4) pane_h = remaining_height;  /* Use all remaining if too small */
            if (pane_h > remaining_height) pane_h = remaining_height;
            draw_memory_section(1, left_y, left_width, pane_h);
            left_y += pane_h;
            remaining_height -= pane_h;
        }
        
        if (g_show_disks && remaining_height > 0) {
            int pane_h = base_pane_height;
            if (pane_h < 4) pane_h = remaining_height;
            if (pane_h > remaining_height) pane_h = remaining_height;
            draw_disk_section(1, left_y, left_width, pane_h);
            left_y += pane_h;
            remaining_height -= pane_h;
        }
        
        if (g_show_net && remaining_height > 0) {
            draw_net_section(1, left_y, left_width, remaining_height);
        }
    }
    
    /* Right side: Process list - use all available height */
    if (g_show_proc && proc_width > 0) {
        int proc_x = (num_left_panes > 0) ? left_width + 2 : 1;
        draw_process_list(proc_x, bottom_y, proc_width, bottom_height);
    }
    
    /* Help bar at bottom */
    draw_help_bar(h - 1, w);
    
    tb_present();
}

int64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Get config directory path */
void get_config_dir(char *buf, size_t buflen) {
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && strlen(xdg_config) > 0) {
        snprintf(buf, buflen, "%s/ctop", xdg_config);
    } else {
        const char *home = getenv("HOME");
        if (home && strlen(home) > 0) {
            snprintf(buf, buflen, "%s/.config/ctop", home);
        } else {
            snprintf(buf, buflen, "/tmp/ctop");
        }
    }
}

/* Save settings to config file */
void save_settings(void) {
    char config_dir[512];
    get_config_dir(config_dir, sizeof(config_dir));
    
    /* Create directory if it doesn't exist */
    struct stat st;
    if (stat(config_dir, &st) != 0) {
        mkdir(config_dir, 0755);
    }
    
    char config_file[1024];
    snprintf(config_file, sizeof(config_file), "%s/config", config_dir);
    
    FILE *fp = fopen(config_file, "w");
    if (!fp) return;
    
    fprintf(fp, "# ctop configuration file\n");
    fprintf(fp, "show_cpu=%d\n", g_show_cpu);
    fprintf(fp, "show_mem=%d\n", g_show_mem);
    fprintf(fp, "show_disks=%d\n", g_show_disks);
    fprintf(fp, "show_net=%d\n", g_show_net);
    fprintf(fp, "show_proc=%d\n", g_show_proc);
    fprintf(fp, "sort_mode=%d\n", g_sort_mode);
    fprintf(fp, "refresh_rate=%d\n", g_refresh_rate_ms);
    
    fclose(fp);
}

/* Load settings from config file */
void load_settings(void) {
    char config_dir[512];
    get_config_dir(config_dir, sizeof(config_dir));
    
    char config_file[1024];
    snprintf(config_file, sizeof(config_file), "%s/config", config_dir);
    
    FILE *fp = fopen(config_file, "r");
    if (!fp) return;
    
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        /* Skip comments and empty lines */
        if (line[0] == '#' || line[0] == '\n') continue;
        
        char key[64];
        int value;
        if (sscanf(line, "%63[^=]=%d", key, &value) == 2) {
            if (strcmp(key, "show_cpu") == 0) g_show_cpu = value;
            else if (strcmp(key, "show_mem") == 0) g_show_mem = value;
            else if (strcmp(key, "show_disks") == 0) g_show_disks = value;
            else if (strcmp(key, "show_net") == 0) g_show_net = value;
            else if (strcmp(key, "show_proc") == 0) g_show_proc = value;
            else if (strcmp(key, "sort_mode") == 0) {
                if (value >= 0 && value < SORT_MAX) g_sort_mode = value;
            }
            else if (strcmp(key, "refresh_rate") == 0) {
                if (value >= 100 && value <= 10000) g_refresh_rate_ms = value;
            }
        }
    }
    
    fclose(fp);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    int ret = tb_init();
    if (ret != TB_OK) {
        fprintf(stderr, "Failed to initialize termbox: %s\n", tb_strerror(ret));
        return 1;
    }
    
    tb_set_output_mode(TB_OUTPUT_TRUECOLOR);
    tb_hide_cursor();
    
    load_settings();
    
    update_stats();
    draw_screen();
    
    int64_t last_update = get_time_ms();
    int64_t prev_update = last_update;
    
    while (g_running) {
        int w = tb_width();
        int h = tb_height();
        int min_w, min_h;
        calculate_minimum_size(&min_w, &min_h);
        int in_error_mode = (w < min_w || h < min_h);
        
        int64_t now = get_time_ms();
        int64_t time_until_update = REFRESH_RATE_MS - (now - last_update);
        if (time_until_update < 0) time_until_update = 0;
        
        struct tb_event ev;
        ret = tb_peek_event(&ev, (int)time_until_update);
        
        int need_redraw = 0;
        int pane_toggled = 0;
        int sort_changed = 0;
        
        if (ret == TB_OK) {
            if (ev.type == TB_EVENT_KEY) {
                if (ev.ch == '1') {
                    g_show_cpu = !g_show_cpu;
                    need_redraw = 1;
                    pane_toggled = 1;
                } else if (ev.ch == '2') {
                    g_show_mem = !g_show_mem;
                    need_redraw = 1;
                    pane_toggled = 1;
                } else if (ev.ch == '3') {
                    g_show_disks = !g_show_disks;
                    need_redraw = 1;
                    pane_toggled = 1;
                } else if (ev.ch == '4') {
                    g_show_net = !g_show_net;
                    need_redraw = 1;
                    pane_toggled = 1;
                } else if (ev.ch == '5') {
                    g_show_proc = !g_show_proc;
                    need_redraw = 1;
                    pane_toggled = 1;
                } else if (ev.key == TB_KEY_CTRL_F) {
                    /* Cycle sort mode forward */
                    g_sort_mode = (g_sort_mode + 1) % SORT_MAX;
                    need_redraw = 1;
                    sort_changed = 1;
                } else if (ev.key == TB_KEY_CTRL_B) {
                    /* Cycle sort mode backward */
                    g_sort_mode = (g_sort_mode - 1 + SORT_MAX) % SORT_MAX;
                    need_redraw = 1;
                    sort_changed = 1;
                } else if (ev.ch == 'q' || ev.ch == 'Q' || ev.key == TB_KEY_ESC || 
                          ev.key == TB_KEY_CTRL_C) {
                    g_running = 0;
                } else if (!in_error_mode && g_show_proc && (ev.key == TB_KEY_CTRL_N || ev.key == TB_KEY_ARROW_DOWN)) {
                    if (g_selected_process < g_stats.process_count - 1) g_selected_process++;
                    need_redraw = 1;
                } else if (!in_error_mode && g_show_proc && (ev.key == TB_KEY_CTRL_P || ev.key == TB_KEY_ARROW_UP)) {
                    if (g_selected_process > 0) g_selected_process--;
                    need_redraw = 1;
                } else if (!in_error_mode && g_show_proc && (ev.key == TB_KEY_CTRL_V || ev.key == TB_KEY_PGDN)) {
                    g_selected_process += 10;
                    if (g_selected_process >= g_stats.process_count)
                        g_selected_process = g_stats.process_count - 1;
                    need_redraw = 1;
                } else if (!in_error_mode && g_show_proc && ((ev.key == 'v' && (ev.mod & TB_MOD_ALT)) || ev.key == TB_KEY_PGUP)) {
                    g_selected_process -= 10;
                    if (g_selected_process < 0) g_selected_process = 0;
                    need_redraw = 1;
                } else if (!in_error_mode && g_show_proc && (ev.key == TB_KEY_CTRL_A || ev.key == TB_KEY_HOME)) {
                    g_selected_process = 0;
                    need_redraw = 1;
                } else if (!in_error_mode && g_show_proc && (ev.key == TB_KEY_CTRL_E || ev.key == TB_KEY_END)) {
                    g_selected_process = g_stats.process_count - 1;
                    need_redraw = 1;
                }
            } else if (ev.type == TB_EVENT_RESIZE) {
                need_redraw = 1;
            }
        }
        
        if (need_redraw) {
            draw_screen();
        }
        
        /* Update stats and redraw periodically, or immediately after pane toggle or sort change */
        now = get_time_ms();
        if (pane_toggled || sort_changed || now - last_update >= g_refresh_rate_ms) {
            if (pane_toggled) {
                save_settings();
            }
            g_elapsed_seconds = (now - prev_update) / 1000.0f;
            if (g_elapsed_seconds <= 0) g_elapsed_seconds = 1.0f;
            if (!in_error_mode || pane_toggled || sort_changed) {
                update_stats();
            }
            prev_update = now;
            last_update = now;
            draw_screen();
        }
    }
    
    save_settings();
    tb_shutdown();
    return 0;
}
