/*
 * Copyright (C) 2013 The Android Open Source Project
 * Copyright (C) 2025 Furi Labs
 * Copyright (C) 2025 Bardia Moshiri <bardia@furilabs.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <inttypes.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>

#include <array>
#include <shared_mutex>
#include <sstream>

#include <glib.h>
#include <lmkd.h>
#include <psi/psi.h>

#include "pidfd-compat.h"
#include "processwatcher.h"
#include "reaper.h"
#include "watchdog.h"

#ifndef __unused
#define __unused __attribute__((__unused__))
#endif

#define ZONEINFO_PATH "/proc/zoneinfo"
#define MEMINFO_PATH "/proc/meminfo"
#define VMSTAT_PATH "/proc/vmstat"
#define PROC_STATUS_TGID_FIELD "Tgid:"
#define PROC_STATUS_RSS_FIELD "VmRSS:"
#define PROC_STATUS_SWAP_FIELD "VmSwap:"

#define PERCEPTIBLE_APP_ADJ 200

#define EIGHT_MEGA (1 << 23)

#define TARGET_UPDATE_MIN_INTERVAL_MS 1000
#define THRASHING_RESET_INTERVAL_MS 1000

#define NS_PER_SEC 1000000000LL
#define MS_PER_SEC 1000LL
#define US_PER_SEC 1000000LL
#define NS_PER_MS (NS_PER_SEC / MS_PER_SEC)
#define US_PER_MS (US_PER_SEC / MS_PER_SEC)

#define SYSTEM_ADJ (-900)

#define STRINGIFY(x) STRINGIFY_INTERNAL(x)
#define STRINGIFY_INTERNAL(x) #x

/* Get PAGE_SIZE */
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/*
 * PSI monitor tracking window size.
 * PSI monitor generates events at most once per window,
 * therefore we poll memory state for the duration of
 * PSI_WINDOW_SIZE_MS after the event happens.
 */
#define PSI_WINDOW_SIZE_MS 1000
/* Polling period after PSI signal when pressure is high */
#define PSI_POLL_PERIOD_SHORT_MS 10
/* Polling period after PSI signal when pressure is low */
#define PSI_POLL_PERIOD_LONG_MS 100

#define FAIL_REPORT_RLIMIT_MS 1000

/* swap_free_low_percentage default value */
#define DEF_LOW_SWAP 10
/* thrashing_limit default value */
#define DEF_THRASHING_LOWRAM 30
#define DEF_THRASHING 100
/* thrashing_limit_decay default value */
#define DEF_THRASHING_DECAY_LOWRAM 50
#define DEF_THRASHING_DECAY 10
/* psi_partial_stall_ms default value */
#define DEF_PARTIAL_STALL_LOWRAM 200
#define DEF_PARTIAL_STALL 70
/* psi_complete_stall_ms default value */
#define DEF_COMPLETE_STALL 700

#define WATCHDOG_TIMEOUT_SEC 2

/* Configuration system */
static GKeyFile *config = NULL;
static bool config_loaded = false;

static void mp_event_common(int data, uint32_t events, struct polling_params *poll_params);
static void proc_slot(struct proc *procp);

static void load_config(void) {
    if (config_loaded)
        return;

    config = g_key_file_new();
    GError *error = NULL;

    /* Try /usr/share/lmkd/lmkd.conf first, then /etc/lmkd.conf */
    const char *config_paths[] = {
        "/usr/share/lmkd/lmkd.conf",
        "/etc/lmkd.conf",
        NULL
    };

    for (int i = 0; config_paths[i]; i++) {
        if (g_file_test(config_paths[i], G_FILE_TEST_EXISTS)) {
            if (g_key_file_load_from_file(config, config_paths[i], G_KEY_FILE_NONE, &error)) {
                g_debug("Loaded configuration from %s", config_paths[i]);
                break;
            } else {
                g_printerr("Failed to load config from %s: %s",
                           config_paths[i],
                           error->message);
                g_error_free(error);
                error = NULL;
            }
        }
    }
    config_loaded = true;
}

static bool get_config_bool(const char *key, bool default_value) {
    load_config();
    GError *error = NULL;

    if (!config)
        return default_value;

    gboolean value = g_key_file_get_boolean(config, "lmkd", key, &error);
    if (error) {
        g_error_free(error);
        return default_value;
    }
    return value;
}

static int32_t get_config_int32(const char *key, int32_t default_value) {
    load_config();
    GError *error = NULL;

    if (!config)
        return default_value;

    gint value = g_key_file_get_integer(config, "lmkd", key, &error);
    if (error) {
        g_error_free(error);
        return default_value;
    }
    return (int32_t)value;
}

static int64_t get_config_int64(const char *key, int64_t default_value) {
    load_config();
    GError *error = NULL;

    if (!config)
        return default_value;

    gint64 value = g_key_file_get_int64(config, "lmkd", key, &error);
    if (error) {
        g_error_free(error);
        return default_value;
    }
    return value;
}

static bool find_cgroup_mount(const char *controller, char *mount_path, size_t path_len) {
    FILE *mounts = fopen("/proc/mounts", "r");
    if (!mounts) {
        g_printerr("Failed to open /proc/mounts: %s", strerror(errno));
        return false;
    }

    char line[1024];
    bool found = false;

    while (fgets(line, sizeof(line), mounts)) {
        char device[256], path[256], fstype[64], options[512];
        if (sscanf(line, "%255s %255s %63s %511s", device, path, fstype, options) == 4) {
            if (strcmp(fstype, "cgroup") == 0 || strcmp(fstype, "cgroup2") == 0) {
                if (strstr(options, controller) || strcmp(controller, "") == 0) {
                    snprintf(mount_path, path_len, "%s", path);
                    found = true;
                    break;
                }
            }
        }
    }

    fclose(mounts);
    return found;
}

static bool get_cgroup_attribute_path(const char *attr, std::string &path) {
    char mount_path[PATH_MAX];

    /* Try to find memory controller mount */
    if (!find_cgroup_mount("memory", mount_path, sizeof(mount_path))) {
        g_printerr("Memory cgroup not found");
        return false;
    }

    path = std::string(mount_path) + "/" + attr;
    return true;
}

/* memory pressure levels */
enum vmpressure_level {
    VMPRESS_LEVEL_LOW = 0,
    VMPRESS_LEVEL_MEDIUM,
    VMPRESS_LEVEL_CRITICAL,
    VMPRESS_LEVEL_COUNT
};

static const char *level_name[] = {
    "low",
    "medium",
    "critical"};

struct {
    int64_t min_nr_free_pages; /* recorded but not used yet */
    int64_t max_nr_free_pages;
} low_pressure_mem = {-1, -1};

struct psi_threshold {
    enum psi_stall_type stall_type;
    int threshold_ms;
};

static int level_oomadj[VMPRESS_LEVEL_COUNT];
static int mpevfd[VMPRESS_LEVEL_COUNT] = {-1, -1, -1};
static bool pidfd_supported;
static int last_kill_pid_or_fd = -1;
static struct timespec last_kill_tm;

/* lmkd configurable parameters */
static bool debug_process_killing;
static bool enable_pressure_upgrade;
static int64_t upgrade_pressure;
static int64_t downgrade_pressure;
static bool low_ram_device;
static bool kill_heaviest_task;
static unsigned long kill_timeout_ms;
static bool use_minfree_levels;
static bool per_app_memcg;
static int swap_free_low_percentage;
static int psi_partial_stall_ms;
static int psi_complete_stall_ms;
static int thrashing_limit_pct;
static int thrashing_limit_decay_pct;
static int thrashing_critical_pct;
static int swap_util_max;
static int64_t filecache_min_kb;
static int64_t stall_limit_critical;
static bool use_psi_monitors = false;
static struct psi_threshold psi_thresholds[VMPRESS_LEVEL_COUNT] = {
    {PSI_SOME, 70},  /* 70ms out of 1sec for partial stall */
    {PSI_SOME, 100}, /* 100ms out of 1sec for partial stall */
    {PSI_FULL, 70},  /* 70ms out of 1sec for complete stall */
};

static Reaper reaper;
static int reaper_comm_fd[2];

enum polling_update {
    POLLING_DO_NOT_CHANGE,
    POLLING_START,
    POLLING_PAUSE,
    POLLING_RESUME,
};

/*
 * Data used for periodic polling for the memory state of the device.
 * Note that when system is not polling poll_handler is set to NULL,
 * when polling starts poll_handler gets set and is reset back to
 * NULL when polling stops.
 */
struct polling_params {
    struct event_handler_info *poll_handler;
    struct event_handler_info *paused_handler;
    struct timespec poll_start_tm;
    struct timespec last_poll_tm;
    int polling_interval_ms;
    enum polling_update update;
};

/* data required to handle events */
struct event_handler_info {
    int data;
    void (*handler)(int data, uint32_t events, struct polling_params *poll_params);
};

/* vmpressure event handler data */
static struct event_handler_info vmpressure_hinfo[VMPRESS_LEVEL_COUNT];

/*
 * 3 memory pressure levels, 1 fd to wait for process death + 1 fd to receive kill failure notifications
 */
#define MAX_EPOLL_EVENTS (VMPRESS_LEVEL_COUNT + 1 + 1)
static int epollfd;
static int maxevents;

/* OOM score values used by both kernel and framework */
#define OOM_SCORE_ADJ_MIN (-1000)
#define OOM_SCORE_ADJ_MAX 1000

static std::array<int, MAX_TARGETS> lowmem_adj;
static std::array<int, MAX_TARGETS> lowmem_minfree;
static int lowmem_targets_size;

/* Fields to parse in /proc/zoneinfo */
/* zoneinfo per-zone fields */
enum zoneinfo_zone_field {
    ZI_ZONE_NR_FREE_PAGES = 0,
    ZI_ZONE_MIN,
    ZI_ZONE_LOW,
    ZI_ZONE_HIGH,
    ZI_ZONE_PRESENT,
    ZI_ZONE_NR_FREE_CMA,
    ZI_ZONE_FIELD_COUNT
};

static const char *const zoneinfo_zone_field_names[ZI_ZONE_FIELD_COUNT] = {
    "nr_free_pages",
    "min",
    "low",
    "high",
    "present",
    "nr_free_cma",
};

/* zoneinfo per-zone special fields */
enum zoneinfo_zone_spec_field {
    ZI_ZONE_SPEC_PROTECTION = 0,
    ZI_ZONE_SPEC_PAGESETS,
    ZI_ZONE_SPEC_FIELD_COUNT,
};

static const char *const zoneinfo_zone_spec_field_names[ZI_ZONE_SPEC_FIELD_COUNT] = {
    "protection:",
    "pagesets",
};

/* see __MAX_NR_ZONES definition in kernel mmzone.h */
#define MAX_NR_ZONES 6

union zoneinfo_zone_fields {
    struct {
        int64_t nr_free_pages;
        int64_t min;
        int64_t low;
        int64_t high;
        int64_t present;
        int64_t nr_free_cma;
    } field;
    int64_t arr[ZI_ZONE_FIELD_COUNT];
};

struct zoneinfo_zone {
    union zoneinfo_zone_fields fields;
    int64_t protection[MAX_NR_ZONES];
    int64_t max_protection;
};

/* zoneinfo per-node fields */
enum zoneinfo_node_field {
    ZI_NODE_NR_INACTIVE_FILE = 0,
    ZI_NODE_NR_ACTIVE_FILE,
    ZI_NODE_FIELD_COUNT
};

static const char *const zoneinfo_node_field_names[ZI_NODE_FIELD_COUNT] = {
    "nr_inactive_file",
    "nr_active_file",
};

union zoneinfo_node_fields {
    struct {
        int64_t nr_inactive_file;
        int64_t nr_active_file;
    } field;
    int64_t arr[ZI_NODE_FIELD_COUNT];
};

struct zoneinfo_node {
    int id;
    int zone_count;
    struct zoneinfo_zone zones[MAX_NR_ZONES];
    union zoneinfo_node_fields fields;
};

/* for now two memory nodes is more than enough */
#define MAX_NR_NODES 2

struct zoneinfo {
    int node_count;
    struct zoneinfo_node nodes[MAX_NR_NODES];
    int64_t totalreserve_pages;
    int64_t total_inactive_file;
    int64_t total_active_file;
};

/* Fields to parse in /proc/meminfo */
enum meminfo_field {
    MI_NR_FREE_PAGES = 0,
    MI_CACHED,
    MI_SWAP_CACHED,
    MI_BUFFERS,
    MI_SHMEM,
    MI_UNEVICTABLE,
    MI_TOTAL_SWAP,
    MI_FREE_SWAP,
    MI_ACTIVE_ANON,
    MI_INACTIVE_ANON,
    MI_ACTIVE_FILE,
    MI_INACTIVE_FILE,
    MI_SRECLAIMABLE,
    MI_SUNRECLAIM,
    MI_KERNEL_STACK,
    MI_PAGE_TABLES,
    MI_ION_HELP,
    MI_ION_HELP_POOL,
    MI_CMA_FREE,
    MI_FIELD_COUNT
};

static const char *const meminfo_field_names[MI_FIELD_COUNT] = {
    "MemFree:",
    "Cached:",
    "SwapCached:",
    "Buffers:",
    "Shmem:",
    "Unevictable:",
    "SwapTotal:",
    "SwapFree:",
    "Active(anon):",
    "Inactive(anon):",
    "Active(file):",
    "Inactive(file):",
    "SReclaimable:",
    "SUnreclaim:",
    "KernelStack:",
    "PageTables:",
    "ION_heap:",
    "ION_heap_pool:",
    "CmaFree:",
};

union meminfo {
    struct {
        int64_t nr_free_pages;
        int64_t cached;
        int64_t swap_cached;
        int64_t buffers;
        int64_t shmem;
        int64_t unevictable;
        int64_t total_swap;
        int64_t free_swap;
        int64_t active_anon;
        int64_t inactive_anon;
        int64_t active_file;
        int64_t inactive_file;
        int64_t sreclaimable;
        int64_t sunreclaimable;
        int64_t kernel_stack;
        int64_t page_tables;
        int64_t ion_heap;
        int64_t ion_heap_pool;
        int64_t cma_free;
        /* fields below are calculated rather than read from the file */
        int64_t nr_file_pages;
        int64_t total_gpu_kb;
    } field;
    int64_t arr[MI_FIELD_COUNT];
};

/* Fields to parse in /proc/vmstat */
enum vmstat_field {
    VS_FREE_PAGES,
    VS_INACTIVE_FILE,
    VS_ACTIVE_FILE,
    VS_WORKINGSET_REFAULT,
    VS_WORKINGSET_REFAULT_FILE,
    VS_PGSCAN_KSWAPD,
    VS_PGSCAN_DIRECT,
    VS_PGSCAN_DIRECT_THROTTLE,
    VS_FIELD_COUNT
};

static const char *const vmstat_field_names[VS_FIELD_COUNT] = {
    "nr_free_pages",
    "nr_inactive_file",
    "nr_active_file",
    "workingset_refault",
    "workingset_refault_file",
    "pgscan_kswapd",
    "pgscan_direct",
    "pgscan_direct_throttle",
};

union vmstat {
    struct {
        int64_t nr_free_pages;
        int64_t nr_inactive_file;
        int64_t nr_active_file;
        int64_t workingset_refault;
        int64_t workingset_refault_file;
        int64_t pgscan_kswapd;
        int64_t pgscan_direct;
        int64_t pgscan_direct_throttle;
    } field;
    int64_t arr[VS_FIELD_COUNT];
};

enum field_match_result {
    NO_MATCH,
    PARSE_FAIL,
    PARSE_SUCCESS
};

struct adjslot_list {
    struct adjslot_list *next;
    struct adjslot_list *prev;
};

struct proc {
    struct adjslot_list asl;
    int pid;
    int pidfd;
    uid_t uid;
    int oomadj;
    pid_t reg_pid; /* PID of the process that registered this record */
    bool valid;
    struct proc *pidhash_next;
};

struct reread_data {
    const char *filename;
    int fd;
};

#define PIDHASH_SZ 1024
static struct proc *pidhash[PIDHASH_SZ];
#define pid_hashfn(x) ((((x) >> 8) ^ (x)) & (PIDHASH_SZ - 1))

#define ADJTOSLOT(adj) ((adj) + -OOM_SCORE_ADJ_MIN)
#define ADJTOSLOT_COUNT (ADJTOSLOT(OOM_SCORE_ADJ_MAX) + 1)

/* protects procadjslot_list from concurrent access */
static std::shared_mutex adjslot_list_lock;
/* procadjslot_list should be modified only from the main thread while exclusively holding
 * adjslot_list_lock. Readers from non-main threads should hold adjslot_list_lock shared lock. */
static struct adjslot_list procadjslot_list[ADJTOSLOT_COUNT];

#define MAX_DISTINCT_OOM_ADJ 32
#define KILLCNT_INVALID_IDX 0xFF
/*
 * Because killcnt array is sparse a two-level indirection is used
 * to keep the size small. killcnt_idx stores index of the element in
 * killcnt array. Index KILLCNT_INVALID_IDX indicates an unused slot.
 */
static uint8_t killcnt_idx[ADJTOSLOT_COUNT];
static uint16_t killcnt[MAX_DISTINCT_OOM_ADJ];
static int killcnt_free_idx = 0;
static uint32_t killcnt_total = 0;

/* PAGE_SIZE / 1024 */
static long page_k;

static void update_props();
static bool init_monitors();
static void destroy_monitors();

static int clamp(int low, int high, int value) {
    return std::max(std::min(value, high), low);
}

static bool parse_int64(const char *str, int64_t *ret) {
    char *endptr;
    long long val = strtoll(str, &endptr, 10);
    if (str == endptr || val > INT64_MAX)
        return false;
    *ret = (int64_t)val;
    return true;
}

static int find_field(const char *name, const char *const field_names[], int field_count) {
    for (int i = 0; i < field_count; i++) {
        if (!strcmp(name, field_names[i]))
            return i;
    }
    return -1;
}

static enum field_match_result match_field(const char *cp, const char *ap, const char *const field_names[], int field_count, int64_t *field, int *field_idx) {
    int i = find_field(cp, field_names, field_count);
    if (i < 0)
        return NO_MATCH;
    *field_idx = i;
    return parse_int64(ap, field) ? PARSE_SUCCESS : PARSE_FAIL;
}

static ssize_t read_all(int fd, char *buf, size_t max_len) {
    ssize_t ret = 0;
    off_t offset = 0;

    while (max_len > 0) {
        ssize_t r = TEMP_FAILURE_RETRY(pread(fd, buf, max_len, offset));
        if (r == 0)
            break;
        if (r == -1)
            return -1;
        ret += r;
        buf += r;
        offset += r;
        max_len -= r;
    }

    return ret;
}

/*
 * Read a new or already opened file from the beginning.
 * If the file has not been opened yet data->fd should be set to -1.
 * To be used with files which are read often and possibly during high
 * memory pressure to minimize file opening which by itself requires kernel
 * memory allocation and might result in a stall on memory stressed system.
 */
static char *reread_file(struct reread_data *data) {
    /* start with page-size buffer and increase if needed */
    static ssize_t buf_size = PAGE_SIZE;
    static char *new_buf, *buf = NULL;
    ssize_t size;

    if (data->fd == -1) {
        /* First-time buffer initialization */
        if (!buf && (buf = static_cast<char *>(malloc(buf_size))) == nullptr)
            return NULL;

        data->fd = TEMP_FAILURE_RETRY(open(data->filename, O_RDONLY | O_CLOEXEC));
        if (data->fd < 0) {
            g_printerr("%s open: %s", data->filename, strerror(errno));
            return NULL;
        }
    }

    while (true) {
        size = read_all(data->fd, buf, buf_size - 1);
        if (size < 0) {
            g_printerr("%s read: %s", data->filename, strerror(errno));
            close(data->fd);
            data->fd = -1;
            return NULL;
        }
        if (size < buf_size - 1)
            break;

        /*
         * Since we are reading /proc files we can't use fstat to find out
         * the real size of the file. Double the buffer size and keep retrying.
         */
        if ((new_buf = static_cast<char *>(realloc(buf, buf_size * 2))) == nullptr) {
            errno = ENOMEM;
            return NULL;
        }
        buf = new_buf;
        buf_size *= 2;
    }
    buf[size] = 0;

    return buf;
}

static inline long get_time_diff_ms(struct timespec *from,
                                    struct timespec *to) {
    return (to->tv_sec - from->tv_sec) * (long)MS_PER_SEC +
           (to->tv_nsec - from->tv_nsec) / (long)NS_PER_MS;
}

/* Reads /proc/pid/status into buf. */
static bool read_proc_status(int pid, char *buf, size_t buf_sz) {
    char path[PATH_MAX];
    int fd;
    ssize_t size;

    snprintf(path, PATH_MAX, "/proc/%d/status", pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;

    size = read_all(fd, buf, buf_sz - 1);
    close(fd);
    if (size < 0)
        return false;
    buf[size] = 0;
    return true;
}

/* Looks for tag in buf and parses the first integer */
static bool parse_status_tag(char *buf, const char *tag, int64_t *out) {
    char *pos = buf;
    while (true) {
        pos = strstr(pos, tag);
        /* Stop if tag not found or found at the line beginning */
        if (pos == NULL || pos == buf || pos[-1] == '\n')
            break;
        pos++;
    }

    if (pos == NULL)
        return false;

    pos += strlen(tag);
    while (*pos == ' ') ++pos;
    return parse_int64(pos, out);
}

static int proc_get_size(int pid) {
    char path[PATH_MAX];
    char line[LINE_MAX];
    int fd;
    int rss = 0;
    int total;
    ssize_t ret;

    /* gid containing AID_READPROC required */
    snprintf(path, PATH_MAX, "/proc/%d/statm", pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd == -1)
        return -1;

    ret = read_all(fd, line, sizeof(line) - 1);
    if (ret < 0) {
        close(fd);
        return -1;
    }
    line[ret] = '\0';

    sscanf(line, "%d %d ", &total, &rss);
    close(fd);
    return rss;
}

static char *proc_get_name(int pid, char *buf, size_t buf_size) {
    char path[PATH_MAX];
    int fd;
    char *cp;
    ssize_t ret;

    /* gid containing AID_READPROC required */
    snprintf(path, PATH_MAX, "/proc/%d/cmdline", pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        return NULL;
    }
    ret = read_all(fd, buf, buf_size - 1);
    close(fd);
    if (ret < 0)
        return NULL;
    buf[ret] = '\0';

    cp = strchr(buf, ' ');
    if (cp)
        *cp = '\0';

    return buf;
}

static struct proc *pid_lookup(int pid) {
    struct proc *procp;

    for (procp = pidhash[pid_hashfn(pid)]; procp && procp->pid != pid;
         procp = procp->pidhash_next);

    return procp;
}

static void adjslot_insert(struct adjslot_list *head, struct adjslot_list *new_element) {
    struct adjslot_list *next = head->next;
    new_element->prev = head;
    new_element->next = next;
    next->prev = new_element;
    head->next = new_element;
}

static void adjslot_remove(struct adjslot_list *old) {
    struct adjslot_list *prev = old->prev;
    struct adjslot_list *next = old->next;
    next->prev = prev;
    prev->next = next;
}

static struct adjslot_list *adjslot_tail(struct adjslot_list *head) {
    struct adjslot_list *asl = head->prev;

    return asl == head ? NULL : asl;
}

/* Should be modified only from the main thread. */
static void proc_slot(struct proc *procp) {
    int adjslot = ADJTOSLOT(procp->oomadj);
    std::scoped_lock lock(adjslot_list_lock);

    adjslot_insert(&procadjslot_list[adjslot], &procp->asl);
}

/* Should be modified only from the main thread. */
static void proc_unslot(struct proc *procp) {
    std::scoped_lock lock(adjslot_list_lock);

    adjslot_remove(&procp->asl);
}

/* Can be called only from the main thread. */
static int pid_remove(int pid) {
    int hval = pid_hashfn(pid);
    struct proc *procp;
    struct proc *prevp;

    for (procp = pidhash[hval], prevp = NULL; procp && procp->pid != pid;
         procp = procp->pidhash_next)
        prevp = procp;

    if (!procp)
        return -1;

    if (!prevp)
        pidhash[hval] = procp->pidhash_next;
    else
        prevp->pidhash_next = procp->pidhash_next;

    proc_unslot(procp);
    /*
     * Close pidfd here if we are not waiting for corresponding process to die,
     * in which case stop_wait_for_proc_kill() will close the pidfd later
     */
    if (procp->pidfd >= 0 && procp->pidfd != last_kill_pid_or_fd) {
        close(procp->pidfd);
    }
    free(procp);
    return 0;
}

static void pid_invalidate(int pid) {
    std::shared_lock lock(adjslot_list_lock);
    struct proc *procp = pid_lookup(pid);

    if (procp) {
        procp->valid = false;
    }
}

static void inc_killcnt(int oomadj) {
    int slot = ADJTOSLOT(oomadj);
    uint8_t idx = killcnt_idx[slot];

    if (idx == KILLCNT_INVALID_IDX) {
        /* index is not assigned for this oomadj */
        if (killcnt_free_idx < MAX_DISTINCT_OOM_ADJ) {
            killcnt_idx[slot] = killcnt_free_idx;
            killcnt[killcnt_free_idx] = 1;
            killcnt_free_idx++;
        } else {
            g_warning("Number of distinct oomadj levels exceeds %d",
                      MAX_DISTINCT_OOM_ADJ);
        }
    } else {
        /*
         * wraparound is highly unlikely and is detectable using total
         * counter because it has to be equal to the sum of all counters
         */
        killcnt[idx]++;
    }
    /* increment total kill counter */
    killcnt_total++;
}

/*
 * /proc/zoneinfo parsing routines
 * Expected file format is:
 *
 *   Node <node_id>, zone   <zone_name>
 *   (
 *    per-node stats
 *       (<per-node field name> <value>)+
 *   )?
 *   (pages free     <value>
 *       (<per-zone field name> <value>)+
 *    pagesets
 *       (<unused fields>)*
 *   )+
 *   ...
 */
static void zoneinfo_parse_protection(char *buf, struct zoneinfo_zone *zone) {
    int zone_idx;
    int64_t max = 0;
    char *save_ptr;

    for (buf = strtok_r(buf, "(), ", &save_ptr), zone_idx = 0;
         buf && zone_idx < MAX_NR_ZONES;
         buf = strtok_r(NULL, "), ", &save_ptr), zone_idx++) {
        long long zoneval = strtoll(buf, &buf, 0);
        if (zoneval > max)
            max = (zoneval > INT64_MAX) ? INT64_MAX : zoneval;
        zone->protection[zone_idx] = zoneval;
    }
    zone->max_protection = max;
}

static int zoneinfo_parse_zone(char **buf, struct zoneinfo_zone *zone) {
    for (char *line = strtok_r(NULL, "\n", buf); line;
         line = strtok_r(NULL, "\n", buf)) {
        char *cp;
        char *ap;
        char *save_ptr;
        int64_t val;
        int field_idx;
        enum field_match_result match_res;

        cp = strtok_r(line, " ", &save_ptr);
        if (!cp)
            return false;

        field_idx = find_field(cp, zoneinfo_zone_spec_field_names, ZI_ZONE_SPEC_FIELD_COUNT);
        if (field_idx >= 0) {
            /* special field */
            if (field_idx == ZI_ZONE_SPEC_PAGESETS)
                /* no mode fields we are interested in */
                return true;

            /* protection field */
            ap = strtok_r(NULL, ")", &save_ptr);
            if (ap)
                zoneinfo_parse_protection(ap, zone);
            continue;
        }

        ap = strtok_r(NULL, " ", &save_ptr);
        if (!ap)
            continue;

        match_res = match_field(cp, ap, zoneinfo_zone_field_names, ZI_ZONE_FIELD_COUNT, &val, &field_idx);
        if (match_res == PARSE_FAIL)
            return false;
        if (match_res == PARSE_SUCCESS)
            zone->fields.arr[field_idx] = val;
        if (field_idx == ZI_ZONE_PRESENT && val == 0)
            /* zone is not populated, stop parsing it */
            return true;
    }
    return false;
}

static int zoneinfo_parse_node(char **buf, struct zoneinfo_node *node) {
    int fields_to_match = ZI_NODE_FIELD_COUNT;

    for (char *line = strtok_r(NULL, "\n", buf); line;
         line = strtok_r(NULL, "\n", buf)) {
        char *cp;
        char *ap;
        char *save_ptr;
        int64_t val;
        int field_idx;
        enum field_match_result match_res;

        cp = strtok_r(line, " ", &save_ptr);
        if (!cp)
            return false;

        ap = strtok_r(NULL, " ", &save_ptr);
        if (!ap)
            return false;

        match_res = match_field(cp, ap, zoneinfo_node_field_names, ZI_NODE_FIELD_COUNT, &val, &field_idx);
        if (match_res == PARSE_FAIL)
            return false;

        if (match_res == PARSE_SUCCESS) {
            node->fields.arr[field_idx] = val;
            fields_to_match--;
            if (!fields_to_match)
                return true;
        }
    }
    return false;
}

static int zoneinfo_parse(struct zoneinfo *zi) {
    static struct reread_data file_data = {
        .filename = ZONEINFO_PATH,
        .fd = -1,
    };
    char *buf;
    char *save_ptr;
    char *line;
    char zone_name[LINE_MAX + 1];
    struct zoneinfo_node *node = NULL;
    int node_idx = 0;
    int zone_idx = 0;

    memset(zi, 0, sizeof(struct zoneinfo));

    if ((buf = reread_file(&file_data)) == NULL)
        return -1;

    for (line = strtok_r(buf, "\n", &save_ptr); line;
         line = strtok_r(NULL, "\n", &save_ptr)) {
        int node_id;
        if (sscanf(line, "Node %d, zone %" STRINGIFY(LINE_MAX) "s", &node_id, zone_name) == 2) {
            if (!node || node->id != node_id) {
                /* new node is found */
                if (node) {
                    node->zone_count = zone_idx + 1;
                    node_idx++;
                    if (node_idx == MAX_NR_NODES) {
                        /* max node count exceeded */
                        g_printerr("%s parse error", file_data.filename);
                        return -1;
                    }
                }
                node = &zi->nodes[node_idx];
                node->id = node_id;
                zone_idx = 0;
                if (!zoneinfo_parse_node(&save_ptr, node)) {
                    g_printerr("%s parse error", file_data.filename);
                    return -1;
                }
            } else {
                /* new zone is found */
                zone_idx++;
            }
            if (!zoneinfo_parse_zone(&save_ptr, &node->zones[zone_idx])) {
                g_printerr("%s parse error", file_data.filename);
                return -1;
            }
        }
    }
    if (!node) {
        g_printerr("%s parse error", file_data.filename);
        return -1;
    }
    node->zone_count = zone_idx + 1;
    zi->node_count = node_idx + 1;

    /* calculate totals fields */
    for (node_idx = 0; node_idx < zi->node_count; node_idx++) {
        node = &zi->nodes[node_idx];
        for (zone_idx = 0; zone_idx < node->zone_count; zone_idx++) {
            struct zoneinfo_zone *zone = &zi->nodes[node_idx].zones[zone_idx];
            zi->totalreserve_pages += zone->max_protection + zone->fields.field.high;
        }
        zi->total_inactive_file += node->fields.field.nr_inactive_file;
        zi->total_active_file += node->fields.field.nr_active_file;
    }
    return 0;
}

/* /proc/meminfo parsing routines */
static bool meminfo_parse_line(char *line, union meminfo *mi) {
    char *cp = line;
    char *ap;
    char *save_ptr;
    int64_t val;
    int field_idx;
    enum field_match_result match_res;

    cp = strtok_r(line, " ", &save_ptr);
    if (!cp)
        return false;

    ap = strtok_r(NULL, " ", &save_ptr);
    if (!ap)
        return false;

    match_res = match_field(cp, ap, meminfo_field_names, MI_FIELD_COUNT, &val, &field_idx);
    if (match_res == PARSE_SUCCESS)
        mi->arr[field_idx] = val / page_k;

    return (match_res != PARSE_FAIL);
}

static int64_t read_gpu_total_kb() {
    return 0; /* GPU memory tracking not supported on Linux port */
}

static int meminfo_parse(union meminfo *mi) {
    static struct reread_data file_data = {
        .filename = MEMINFO_PATH,
        .fd = -1,
    };
    char *buf;
    char *save_ptr;
    char *line;

    memset(mi, 0, sizeof(union meminfo));

    if ((buf = reread_file(&file_data)) == NULL)
        return -1;

    for (line = strtok_r(buf, "\n", &save_ptr); line;
         line = strtok_r(NULL, "\n", &save_ptr)) {
        if (!meminfo_parse_line(line, mi)) {
            g_printerr("%s parse error", file_data.filename);
            return -1;
        }
    }
    mi->field.nr_file_pages = mi->field.cached + mi->field.swap_cached +
                              mi->field.buffers;
    mi->field.total_gpu_kb = read_gpu_total_kb();

    return 0;
}

/* /proc/vmstat parsing routines */
static bool vmstat_parse_line(char *line, union vmstat *vs) {
    char *cp;
    char *ap;
    char *save_ptr;
    int64_t val;
    int field_idx;
    enum field_match_result match_res;

    cp = strtok_r(line, " ", &save_ptr);
    if (!cp)
        return false;

    ap = strtok_r(NULL, " ", &save_ptr);
    if (!ap)
        return false;

    match_res = match_field(cp, ap, vmstat_field_names, VS_FIELD_COUNT, &val, &field_idx);
    if (match_res == PARSE_SUCCESS)
        vs->arr[field_idx] = val;

    return (match_res != PARSE_FAIL);
}

static int vmstat_parse(union vmstat *vs) {
    static struct reread_data file_data = {
        .filename = VMSTAT_PATH,
        .fd = -1,
    };
    char *buf;
    char *save_ptr;
    char *line;

    memset(vs, 0, sizeof(union vmstat));

    if ((buf = reread_file(&file_data)) == NULL) {
        return -1;
    }

    for (line = strtok_r(buf, "\n", &save_ptr); line;
         line = strtok_r(NULL, "\n", &save_ptr)) {
        if (!vmstat_parse_line(line, vs)) {
            g_printerr("%s parse error", file_data.filename);
            return -1;
        }
    }

    return 0;
}

static int psi_parse(struct reread_data *file_data, struct psi_stats stats[], bool full) {
    char *buf;
    char *save_ptr;
    char *line;

    if ((buf = reread_file(file_data)) == NULL)
        return -1;

    line = strtok_r(buf, "\n", &save_ptr);
    if (parse_psi_line(line, PSI_SOME, stats))
        return -1;

    if (full) {
        line = strtok_r(NULL, "\n", &save_ptr);
        if (parse_psi_line(line, PSI_FULL, stats))
            return -1;
    }

    return 0;
}

static int psi_parse_mem(struct psi_data *psi_data) {
    static struct reread_data file_data = {
        .filename = PSI_PATH_MEMORY,
        .fd = -1,
    };
    return psi_parse(&file_data, psi_data->mem_stats, true);
}

static int psi_parse_io(struct psi_data *psi_data) {
    static struct reread_data file_data = {
        .filename = PSI_PATH_IO,
        .fd = -1,
    };
    return psi_parse(&file_data, psi_data->io_stats, true);
}

static int psi_parse_cpu(struct psi_data *psi_data) {
    static struct reread_data file_data = {
        .filename = PSI_PATH_CPU,
        .fd = -1,
    };
    return psi_parse(&file_data, psi_data->cpu_stats, false);
}

enum wakeup_reason {
    Event,
    Polling
};

struct wakeup_info {
    struct timespec wakeup_tm;
    struct timespec prev_wakeup_tm;
    struct timespec last_event_tm;
    int wakeups_since_event;
    int skipped_wakeups;
};

/*
 * After the initial memory pressure event is received lmkd schedules periodic wakeups to check
 * the memory conditions and kill if needed (polling). This is done because pressure events are
 * rate-limited and memory conditions can change in between events. Therefore after the initial
 * event there might be multiple wakeups. This function records the wakeup information such as the
 * timestamps of the last event and the last wakeup, the number of wakeups since the last event
 * and how many of those wakeups were skipped (some wakeups are skipped if previously killed
 * process is still freeing its memory).
 */
static void record_wakeup_time(struct timespec *tm, enum wakeup_reason reason, struct wakeup_info *wi) {
    wi->prev_wakeup_tm = wi->wakeup_tm;
    wi->wakeup_tm = *tm;
    if (reason == Event) {
        wi->last_event_tm = *tm;
        wi->wakeups_since_event = 0;
        wi->skipped_wakeups = 0;
    } else {
        wi->wakeups_since_event++;
    }
}

enum kill_reasons {
    NONE = -1, /* To denote no kill condition */
    PRESSURE_AFTER_KILL = 0,
    NOT_RESPONDING,
    LOW_SWAP_AND_THRASHING,
    LOW_MEM_AND_SWAP,
    LOW_MEM_AND_THRASHING,
    DIRECT_RECL_AND_THRASHING,
    LOW_MEM_AND_SWAP_UTIL,
    LOW_FILECACHE_AFTER_THRASHING,
    KILL_REASON_COUNT
};

struct kill_info {
    enum kill_reasons kill_reason;
    const char *kill_desc;
    int thrashing;
    int max_thrashing;
};

/* Note: returned entry is only an anchor and does not hold a valid process info.
 * When called from a non-main thread, adjslot_list_lock read lock should be taken. */
static struct proc *proc_adj_head(int oomadj) {
    return (struct proc *)&procadjslot_list[ADJTOSLOT(oomadj)];
}

/* When called from a non-main thread, adjslot_list_lock read lock should be taken. */
static struct proc *proc_adj_tail(int oomadj) {
    return (struct proc *)adjslot_tail(&procadjslot_list[ADJTOSLOT(oomadj)]);
}

/* When called from a non-main thread, adjslot_list_lock read lock should be taken. */
static struct proc *proc_adj_prev(int oomadj, int pid) {
    struct adjslot_list *head = &procadjslot_list[ADJTOSLOT(oomadj)];
    struct adjslot_list *curr = adjslot_tail(&procadjslot_list[ADJTOSLOT(oomadj)]);

    while (curr != head) {
        if (((struct proc *)curr)->pid == pid)
            return (struct proc *)curr->prev;
        curr = curr->prev;
    }

    return NULL;
}

/* Can be called only from the main thread. */
static struct proc *proc_get_heaviest(int oomadj) {
    struct adjslot_list *head = &procadjslot_list[ADJTOSLOT(oomadj)];
    struct adjslot_list *curr = head->next;
    struct proc *maxprocp = NULL;
    int maxsize = 0;
    while (curr != head) {
        int pid = ((struct proc *)curr)->pid;
        int tasksize = proc_get_size(pid);
        if (tasksize < 0) {
            struct adjslot_list *next = curr->next;
            pid_remove(pid);
            curr = next;
        } else {
            if (tasksize > maxsize) {
                maxsize = tasksize;
                maxprocp = (struct proc *)curr;
            }
            curr = curr->next;
        }
    }
    return maxprocp;
}

static bool find_victim(int oom_score, int prev_pid, struct proc &target_proc) {
    struct proc *procp;
    std::shared_lock lock(adjslot_list_lock);

    if (!prev_pid) {
        procp = proc_adj_tail(oom_score);
    } else {
        procp = proc_adj_prev(oom_score, prev_pid);
        if (!procp)
            /* pid was removed, restart at the tail */
            procp = proc_adj_tail(oom_score);
    }

    /* the list is empty at this oom_score or we looped through it */
    if (!procp || procp == proc_adj_head(oom_score))
        return false;

    /* make a copy because original might be destroyed after adjslot_list_lock is released */
    target_proc = *procp;

    return true;
}

static void watchdog_callback() {
    int prev_pid = 0;

    g_warning("lmkd watchdog timed out!");
    for (int oom_score = OOM_SCORE_ADJ_MAX; oom_score >= 0;) {
        struct proc target;

        if (!find_victim(oom_score, prev_pid, target)) {
            oom_score--;
            prev_pid = 0;
            continue;
        }

        if (target.valid && reaper.kill({target.pidfd, target.pid, target.uid}, true) == 0) {
            g_warning("lmkd watchdog killed process %d, oom_score_adj %d", target.pid, oom_score);
            /* Simple logging instead of killinfo_log */
            g_debug("WATCHDOG_KILL: pid=%d uid=%d oom_score=%d",
                    target.pid,
                    target.uid,
                    oom_score);
            /* Can't call pid_remove() from non-main thread, therefore just invalidate the record */
            pid_invalidate(target.pid);
            break;
        }
        prev_pid = target.pid;
    }
}

static Watchdog watchdog(WATCHDOG_TIMEOUT_SEC, watchdog_callback);

static bool is_kill_pending(void) {
    char buf[24];

    if (last_kill_pid_or_fd < 0)
        return false;
    if (pidfd_supported)
        return true;

    /* when pidfd is not supported base the decision on /proc/<pid> existence */
    snprintf(buf, sizeof(buf), "/proc/%d/", last_kill_pid_or_fd);
    if (access(buf, F_OK) == 0)
        return true;

    return false;
}

static bool is_waiting_for_kill(void) {
    return pidfd_supported && last_kill_pid_or_fd >= 0;
}

static void stop_wait_for_proc_kill(bool finished) {
    struct epoll_event epev;

    if (last_kill_pid_or_fd < 0)
        return;

    if (debug_process_killing) {
        struct timespec curr_tm;

        if (clock_gettime(CLOCK_MONOTONIC_COARSE, &curr_tm) != 0)
            /*
             * curr_tm is used here merely to report kill duration, so this failure is not fatal.
             * Log an error and continue.
             */
            g_printerr("Failed to get current time");

        if (finished)
            g_debug("Process got killed in %ldms",
                    get_time_diff_ms(&last_kill_tm, &curr_tm));
        else
            g_debug("Stop waiting for process kill after %ldms",
                    get_time_diff_ms(&last_kill_tm, &curr_tm));
    }

    if (pidfd_supported) {
        /* unregister fd */
        if (epoll_ctl(epollfd, EPOLL_CTL_DEL, last_kill_pid_or_fd, &epev))
            g_printerr("epoll_ctl for last killed process failed; errno=%d", errno);
        maxevents--;
        close(last_kill_pid_or_fd);
    }

    last_kill_pid_or_fd = -1;
}

static void kill_done_handler(int data __unused, uint32_t events __unused, struct polling_params *poll_params) {
    stop_wait_for_proc_kill(true);
    poll_params->update = POLLING_RESUME;
}

static void kill_fail_handler(int data __unused, uint32_t events __unused, struct polling_params *poll_params) {
    int pid;

    /* Extract pid from the communication pipe. Clearing the pipe this way allows further
     * epoll_wait calls to sleep until the next event. */
    if (TEMP_FAILURE_RETRY(read(reaper_comm_fd[0], &pid, sizeof(pid))) != sizeof(pid))
        g_printerr("thread communication read failed: %s", strerror(errno));
    stop_wait_for_proc_kill(false);
    poll_params->update = POLLING_RESUME;
}

static void start_wait_for_proc_kill(int pid_or_fd) {
    static struct event_handler_info kill_done_hinfo = {0, kill_done_handler};
    struct epoll_event epev;

    if (last_kill_pid_or_fd >= 0) {
        /* Should not happen but if it does we should stop previous wait */
        g_printerr("Attempt to wait for a kill while another wait is in progress");
        stop_wait_for_proc_kill(false);
    }

    last_kill_pid_or_fd = pid_or_fd;

    if (!pidfd_supported)
        /* If pidfd is not supported just store PID and exit */
        return;

    epev.events = EPOLLIN;
    epev.data.ptr = (void *)&kill_done_hinfo;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, last_kill_pid_or_fd, &epev) != 0) {
        g_printerr("epoll_ctl for last kill failed; errno=%d", errno);
        close(last_kill_pid_or_fd);
        last_kill_pid_or_fd = -1;
        return;
    }
    maxevents++;
}

/* Kill one process specified by procp.  Returns the size (in pages) of the process killed */
static int kill_one_process(struct proc *procp, int min_oom_score, struct kill_info *ki, union meminfo *mi, struct wakeup_info *wi __unused, struct timespec *tm, struct psi_data *pd __unused) {
    int pid = procp->pid;
    int pidfd = procp->pidfd;
    uid_t uid = procp->uid;
    char *taskname;
    int kill_result;
    int result = -1;
    int64_t tgid;
    int64_t rss_kb;
    int64_t swap_kb;
    char buf[PAGE_SIZE];
    char desc[LINE_MAX];

    if (!procp->valid || !read_proc_status(pid, buf, sizeof(buf)))
        goto out;
    if (!parse_status_tag(buf, PROC_STATUS_TGID_FIELD, &tgid)) {
        g_printerr("Unable to parse tgid from /proc/%d/status", pid);
        goto out;
    }
    if (tgid != pid) {
        g_debug("Possible pid reuse detected (pid %d, tgid %" PRId64 ")!", pid, tgid);
        goto out;
    }

    /* Zombie processes will not have RSS / Swap fields. */
    if (!parse_status_tag(buf, PROC_STATUS_RSS_FIELD, &rss_kb))
        goto out;
    if (!parse_status_tag(buf, PROC_STATUS_SWAP_FIELD, &swap_kb))
        goto out;

    taskname = proc_get_name(pid, buf, sizeof(buf));
    /* taskname will point inside buf, do not reuse buf onwards. */
    if (!taskname)
        goto out;

    snprintf(desc, sizeof(desc), "lmk,%d,%d,%d,%d,%d", pid, ki ? (int)ki->kill_reason : -1, procp->oomadj, min_oom_score, ki ? ki->max_thrashing : -1);

    start_wait_for_proc_kill(pidfd < 0 ? pid : pidfd);
    kill_result = reaper.kill({pidfd, pid, uid}, false);

    if (kill_result) {
        stop_wait_for_proc_kill(false);
        g_printerr("kill(%d): errno=%d", pid, errno);
        /* Delete process record even when we fail to kill so that we don't get stuck on it */
        goto out;
    }

    last_kill_tm = *tm;

    inc_killcnt(procp->oomadj);

    if (ki) {
        g_print("Kill '%s' (pid=%d, uid=%d, oom_score_adj=%d) to free %" PRId64 "kB rss, %" PRId64
                "kB swap; reason: %s (thrashing=%d%%, max_thrashing=%d%%, free_mem=%" PRId64 "kB, free_swap=%" PRId64 "kB)\n",
                taskname,
                pid,
                uid,
                procp->oomadj,
                rss_kb,
                swap_kb,
                ki->kill_desc,
                ki->thrashing,
                ki->max_thrashing,
                mi->field.nr_free_pages * page_k,
                mi->field.free_swap * page_k);
    } else {
        g_print("Kill '%s' (pid=%d, uid=%d, oom_score_adj=%d) to free %" PRId64 "kB rss, %" PRId64
                "kB swap (free_mem=%" PRId64 "kB, free_swap=%" PRId64 "kB)\n",
                taskname,
                pid,
                uid,
                procp->oomadj,
                rss_kb,
                swap_kb,
                mi->field.nr_free_pages * page_k,
                mi->field.free_swap * page_k);
    }

    result = rss_kb / page_k;

out:
    /*
     * WARNING: After pid_remove() procp is freed and can't be used!
     * Therefore placed at the end of the function.
     */
    pid_remove(pid);
    return result;
}

/*
 * Find one process to kill at or above the given oom_score_adj level.
 * Returns size of the killed process.
 */
static int find_and_kill_process(int min_score_adj, struct kill_info *ki, union meminfo *mi, struct wakeup_info *wi, struct timespec *tm, struct psi_data *pd) {
    int i;
    int killed_size = 0;
    bool lmk_state_change_start = false;
    bool choose_heaviest_task = kill_heaviest_task;

    for (i = OOM_SCORE_ADJ_MAX; i >= min_score_adj; i--) {
        struct proc *procp;

        if (!choose_heaviest_task && i <= PERCEPTIBLE_APP_ADJ)
            /*
             * If we have to choose a perceptible process, choose the heaviest one to
             * hopefully minimize the number of victims.
             */
            choose_heaviest_task = true;

        while (true) {
            procp = choose_heaviest_task ? proc_get_heaviest(i) : proc_adj_tail(i);

            if (!procp)
                break;

            killed_size = kill_one_process(procp, min_score_adj, ki, mi, wi, tm, pd);
            if (killed_size >= 0) {
                if (!lmk_state_change_start)
                    lmk_state_change_start = true;
                break;
            }
        }
        if (killed_size)
            break;
    }

    return killed_size;
}

static int64_t get_memory_usage(struct reread_data *file_data) {
    int64_t mem_usage;
    char *buf;

    if ((buf = reread_file(file_data)) == NULL)
        return -1;

    if (!parse_int64(buf, &mem_usage)) {
        g_printerr("%s parse error", file_data->filename);
        return -1;
    }
    if (mem_usage == 0) {
        g_printerr("No memory!");
        return -1;
    }
    return mem_usage;
}

void record_low_pressure_levels(union meminfo *mi) {
    if (low_pressure_mem.min_nr_free_pages == -1 ||
        low_pressure_mem.min_nr_free_pages > mi->field.nr_free_pages) {
        if (debug_process_killing)
            g_debug("Low pressure min memory update from %" PRId64 " to %" PRId64,
                    low_pressure_mem.min_nr_free_pages,
                    mi->field.nr_free_pages);
        low_pressure_mem.min_nr_free_pages = mi->field.nr_free_pages;
    }

    /*
     * Free memory at low vmpressure events occasionally gets spikes,
     * possibly a stale low vmpressure event with memory already
     * freed up (no memory pressure should have been reported).
     * Ignore large jumps in max_nr_free_pages that would mess up our stats.
     */
    if (low_pressure_mem.max_nr_free_pages == -1 ||
        (low_pressure_mem.max_nr_free_pages < mi->field.nr_free_pages &&
         (mi->field.nr_free_pages - low_pressure_mem.max_nr_free_pages) <
             (low_pressure_mem.max_nr_free_pages * 0.1))) {
        if (debug_process_killing)
            g_debug("Low pressure max memory update from %" PRId64 " to %" PRId64,
                    low_pressure_mem.max_nr_free_pages,
                    mi->field.nr_free_pages);
        low_pressure_mem.max_nr_free_pages = mi->field.nr_free_pages;
    }
}

enum vmpressure_level upgrade_level(enum vmpressure_level level) {
    return (enum vmpressure_level)((level < VMPRESS_LEVEL_CRITICAL) ? level + 1 : level);
}

enum vmpressure_level downgrade_level(enum vmpressure_level level) {
    return (enum vmpressure_level)((level > VMPRESS_LEVEL_LOW) ? level - 1 : level);
}

enum zone_watermark {
    WMARK_MIN = 0,
    WMARK_LOW,
    WMARK_HIGH,
    WMARK_NONE
};

struct zone_watermarks {
    long high_wmark;
    long low_wmark;
    long min_wmark;
};

/*
 * Returns lowest breached watermark or WMARK_NONE.
 */
static enum zone_watermark get_lowest_watermark(union meminfo *mi,
                                                struct zone_watermarks *watermarks) {
    int64_t nr_free_pages = mi->field.nr_free_pages - mi->field.cma_free;

    if (nr_free_pages < watermarks->min_wmark)
        return WMARK_MIN;
    if (nr_free_pages < watermarks->low_wmark)
        return WMARK_LOW;
    if (nr_free_pages < watermarks->high_wmark)
        return WMARK_HIGH;
    return WMARK_NONE;
}

void calc_zone_watermarks(struct zoneinfo *zi, struct zone_watermarks *watermarks) {
    memset(watermarks, 0, sizeof(struct zone_watermarks));

    for (int node_idx = 0; node_idx < zi->node_count; node_idx++) {
        struct zoneinfo_node *node = &zi->nodes[node_idx];
        for (int zone_idx = 0; zone_idx < node->zone_count; zone_idx++) {
            struct zoneinfo_zone *zone = &node->zones[zone_idx];

            if (!zone->fields.field.present)
                continue;

            watermarks->high_wmark += zone->max_protection + zone->fields.field.high;
            watermarks->low_wmark += zone->max_protection + zone->fields.field.low;
            watermarks->min_wmark += zone->max_protection + zone->fields.field.min;
        }
    }
}

static int calc_swap_utilization(union meminfo *mi) {
    int64_t swap_used = mi->field.total_swap - mi->field.free_swap;
    int64_t total_swappable = mi->field.active_anon + mi->field.inactive_anon +
                              mi->field.shmem + swap_used;
    return total_swappable > 0 ? (swap_used * 100) / total_swappable : 0;
}

static void mp_event_psi(int data, uint32_t events, struct polling_params *poll_params) {
    enum reclaim_state {
        NO_RECLAIM = 0,
        KSWAPD_RECLAIM,
        DIRECT_RECLAIM,
    };
    static int64_t init_ws_refault;
    static int64_t prev_workingset_refault;
    static int64_t base_file_lru;
    static int64_t init_pgscan_kswapd;
    static int64_t init_pgscan_direct;
    static bool killing;
    static int thrashing_limit = thrashing_limit_pct;
    static struct zone_watermarks watermarks;
    static struct timespec wmark_update_tm;
    static struct wakeup_info wi;
    static struct timespec thrashing_reset_tm;
    static int64_t prev_thrash_growth = 0;
    static bool check_filecache = false;
    static int max_thrashing = 0;

    union meminfo mi;
    union vmstat vs;
    struct psi_data psi_data;
    struct timespec curr_tm;
    int64_t thrashing = 0;
    bool swap_is_low = false;
    enum vmpressure_level level = (enum vmpressure_level)data;
    enum kill_reasons kill_reason = NONE;
    bool cycle_after_kill = false;
    enum reclaim_state reclaim = NO_RECLAIM;
    enum zone_watermark wmark = WMARK_NONE;
    char kill_desc[LINE_MAX];
    bool cut_thrashing_limit = false;
    int min_score_adj = 0;
    int swap_util = 0;
    int64_t swap_low_threshold;
    long since_thrashing_reset_ms;
    int64_t workingset_refault_file;
    bool critical_stall = false;

    if (clock_gettime(CLOCK_MONOTONIC_COARSE, &curr_tm) != 0) {
        g_printerr("Failed to get current time");
        return;
    }

    record_wakeup_time(&curr_tm, events ? Event : Polling, &wi);

    bool kill_pending = is_kill_pending();
    if (kill_pending && (kill_timeout_ms == 0 ||
                         get_time_diff_ms(&last_kill_tm, &curr_tm) < static_cast<long>(kill_timeout_ms))) {
        /* Skip while still killing a process */
        wi.skipped_wakeups++;
        goto no_kill;
    }
    /*
     * Process is dead or kill timeout is over, stop waiting. This has no effect if pidfds are
     * supported and death notification already caused waiting to stop.
     */
    stop_wait_for_proc_kill(!kill_pending);

    if (vmstat_parse(&vs) < 0) {
        g_printerr("Failed to parse vmstat!");
        return;
    }
    /* Starting 5.9 kernel workingset_refault vmstat field was renamed workingset_refault_file */
    workingset_refault_file = vs.field.workingset_refault ?: vs.field.workingset_refault_file;

    if (meminfo_parse(&mi) < 0) {
        g_printerr("Failed to parse meminfo!");
        return;
    }

    /* Reset states after process got killed */
    if (killing) {
        killing = false;
        cycle_after_kill = true;
        /* Reset file-backed pagecache size and refault amounts after a kill */
        base_file_lru = vs.field.nr_inactive_file + vs.field.nr_active_file;
        init_ws_refault = workingset_refault_file;
        thrashing_reset_tm = curr_tm;
        prev_thrash_growth = 0;
    }

    /* Check free swap levels */
    if (swap_free_low_percentage) {
        swap_low_threshold = mi.field.total_swap * swap_free_low_percentage / 100;
        swap_is_low = mi.field.free_swap < swap_low_threshold;
    } else {
        swap_low_threshold = 0;
    }

    /* Identify reclaim state */
    if (vs.field.pgscan_direct > init_pgscan_direct) {
        init_pgscan_direct = vs.field.pgscan_direct;
        init_pgscan_kswapd = vs.field.pgscan_kswapd;
        reclaim = DIRECT_RECLAIM;
    } else if (vs.field.pgscan_kswapd > init_pgscan_kswapd) {
        init_pgscan_kswapd = vs.field.pgscan_kswapd;
        reclaim = KSWAPD_RECLAIM;
    } else if (workingset_refault_file == prev_workingset_refault) {
        /*
         * Device is not thrashing and not reclaiming, bail out early until we see these stats
         * changing
         */
        goto no_kill;
    }

    prev_workingset_refault = workingset_refault_file;

    /*
     * It's possible we fail to find an eligible process to kill (ex. no process is
     * above oom_adj_min). When this happens, we should retry to find a new process
     * for a kill whenever a new eligible process is available. This is especially
     * important for a slow growing refault case. While retrying, we should keep
     * monitoring new thrashing counter as someone could release the memory to mitigate
     * the thrashing. Thus, when thrashing reset window comes, we decay the prev thrashing
     * counter by window counts. If the counter is still greater than thrashing limit,
     * we preserve the current prev_thrash counter so we will retry kill again. Otherwise,
     * we reset the prev_thrash counter so we will stop retrying.
     */
    since_thrashing_reset_ms = get_time_diff_ms(&thrashing_reset_tm, &curr_tm);
    if (since_thrashing_reset_ms > THRASHING_RESET_INTERVAL_MS) {
        long windows_passed;
        /* Calculate prev_thrash_growth if we crossed THRASHING_RESET_INTERVAL_MS */
        prev_thrash_growth = (workingset_refault_file - init_ws_refault) * 100 / (base_file_lru + 1);
        windows_passed = (since_thrashing_reset_ms / THRASHING_RESET_INTERVAL_MS);
        /*
         * Decay prev_thrashing unless over-the-limit thrashing was registered in the window we
         * just crossed, which means there were no eligible processes to kill. We preserve the
         * counter in that case to ensure a kill if a new eligible process appears.
         */
        if (windows_passed > 1 || prev_thrash_growth < thrashing_limit)
            prev_thrash_growth >>= windows_passed;

        /* Record file-backed pagecache size when crossing THRASHING_RESET_INTERVAL_MS */
        base_file_lru = vs.field.nr_inactive_file + vs.field.nr_active_file;
        init_ws_refault = workingset_refault_file;
        thrashing_reset_tm = curr_tm;
        thrashing_limit = thrashing_limit_pct;
    } else {
        /* Calculate what % of the file-backed pagecache refaulted so far */
        thrashing = (workingset_refault_file - init_ws_refault) * 100 / (base_file_lru + 1);
    }

    /* Add previous cycle's decayed thrashing amount */
    thrashing += prev_thrash_growth;
    if (max_thrashing < thrashing)
        max_thrashing = thrashing;

    /*
     * Refresh watermarks once per min in case user updated one of the margins.
     * TODO: b/140521024 replace this periodic update with an API for AMS to notify LMKD
     * that zone watermarks were changed by the system software.
     */
    if (watermarks.high_wmark == 0 || get_time_diff_ms(&wmark_update_tm, &curr_tm) > 60000) {
        struct zoneinfo zi;

        if (zoneinfo_parse(&zi) < 0) {
            g_printerr("Failed to parse zoneinfo!");
            return;
        }

        calc_zone_watermarks(&zi, &watermarks);
        wmark_update_tm = curr_tm;
    }

    /* Find out which watermark is breached if any */
    wmark = get_lowest_watermark(&mi, &watermarks);

    if (!psi_parse_mem(&psi_data))
        critical_stall = psi_data.mem_stats[PSI_FULL].avg10 > (float)stall_limit_critical;

    /*
     * TODO: move this logic into a separate function
     * Decide if killing a process is necessary and record the reason
     */
    if (cycle_after_kill && wmark < WMARK_LOW) {
        /*
         * Prevent kills not freeing enough memory which might lead to OOM kill.
         * This might happen when a process is consuming memory faster than reclaim can
         * free even after a kill. Mostly happens when running memory stress tests.
         */
        kill_reason = PRESSURE_AFTER_KILL;
        strncpy(kill_desc, "min watermark is breached even after kill", sizeof(kill_desc));
    } else if (level == VMPRESS_LEVEL_CRITICAL && events != 0) {
        /*
         * Device is too busy reclaiming memory which might lead to ANR.
         * Critical level is triggered when PSI complete stall (all tasks are blocked because
         * of the memory congestion) breaches the configured threshold.
         */
        kill_reason = NOT_RESPONDING;
        strncpy(kill_desc, "device is not responding", sizeof(kill_desc));
    } else if (swap_is_low && thrashing > thrashing_limit_pct) {
        /* Page cache is thrashing while swap is low */
        kill_reason = LOW_SWAP_AND_THRASHING;
        snprintf(kill_desc, sizeof(kill_desc), "device is low on swap (%" PRId64 "kB < %" PRId64 "kB) and thrashing (%" PRId64 "%%)", mi.field.free_swap * page_k, swap_low_threshold * page_k, thrashing);
        /* Do not kill perceptible apps unless below min watermark or heavily thrashing */
        if (wmark > WMARK_MIN && thrashing < thrashing_critical_pct)
            min_score_adj = PERCEPTIBLE_APP_ADJ + 1;
        check_filecache = true;
    } else if (swap_is_low && wmark < WMARK_HIGH) {
        /* Both free memory and swap are low */
        kill_reason = LOW_MEM_AND_SWAP;
        snprintf(kill_desc, sizeof(kill_desc), "%s watermark is breached and swap is low (%" PRId64 "kB < %" PRId64 "kB)", wmark < WMARK_LOW ? "min" : "low", mi.field.free_swap * page_k, swap_low_threshold * page_k);
        /* Do not kill perceptible apps unless below min watermark or heavily thrashing */
        if (wmark > WMARK_MIN && thrashing < thrashing_critical_pct)
            min_score_adj = PERCEPTIBLE_APP_ADJ + 1;
    } else if (wmark < WMARK_HIGH && swap_util_max < 100 &&
               (swap_util = calc_swap_utilization(&mi)) > swap_util_max) {
        /*
         * Too much anon memory is swapped out but swap is not low.
         * Non-swappable allocations created memory pressure.
         */
        kill_reason = LOW_MEM_AND_SWAP_UTIL;
        snprintf(kill_desc, sizeof(kill_desc), "%s watermark is breached and swap utilization"
                                               " is high (%d%% > %d%%)",
                 wmark < WMARK_LOW ? "min" : "low",
                 swap_util,
                 swap_util_max);
    } else if (wmark < WMARK_HIGH && thrashing > thrashing_limit) {
        /* Page cache is thrashing while memory is low */
        kill_reason = LOW_MEM_AND_THRASHING;
        snprintf(kill_desc, sizeof(kill_desc), "%s watermark is breached and thrashing (%" PRId64 "%%)", wmark < WMARK_LOW ? "min" : "low", thrashing);
        cut_thrashing_limit = true;
        /* Do not kill perceptible apps unless thrashing at critical levels */
        if (thrashing < thrashing_critical_pct)
            min_score_adj = PERCEPTIBLE_APP_ADJ + 1;
        check_filecache = true;
    } else if (reclaim == DIRECT_RECLAIM && thrashing > thrashing_limit) {
        /* Page cache is thrashing while in direct reclaim (mostly happens on lowram devices) */
        kill_reason = DIRECT_RECL_AND_THRASHING;
        snprintf(kill_desc, sizeof(kill_desc), "device is in direct reclaim and thrashing (%" PRId64 "%%)", thrashing);
        cut_thrashing_limit = true;
        /* Do not kill perceptible apps unless thrashing at critical levels */
        if (thrashing < thrashing_critical_pct)
            min_score_adj = PERCEPTIBLE_APP_ADJ + 1;
        check_filecache = true;
    } else if (check_filecache) {
        int64_t file_lru_kb = (vs.field.nr_inactive_file + vs.field.nr_active_file) * page_k;

        if (file_lru_kb < filecache_min_kb) {
            /* File cache is too low after thrashing, keep killing background processes */
            kill_reason = LOW_FILECACHE_AFTER_THRASHING;
            snprintf(kill_desc, sizeof(kill_desc), "filecache is low (%" PRId64 "kB < %" PRId64 "kB) after thrashing", file_lru_kb, filecache_min_kb);
            min_score_adj = PERCEPTIBLE_APP_ADJ + 1;
        } else {
            /* File cache is big enough, stop checking */
            check_filecache = false;
        }
    }

    /* Kill a process if necessary */
    if (kill_reason != NONE) {
        struct kill_info ki = {
            .kill_reason = kill_reason,
            .kill_desc = kill_desc,
            .thrashing = (int)thrashing,
            .max_thrashing = max_thrashing,
        };

        /* Allow killing perceptible apps if the system is stalled */
        if (critical_stall)
            min_score_adj = 0;

        psi_parse_io(&psi_data);
        psi_parse_cpu(&psi_data);
        int pages_freed = find_and_kill_process(min_score_adj, &ki, &mi, &wi, &curr_tm, &psi_data);
        if (pages_freed > 0) {
            killing = true;
            max_thrashing = 0;
            if (cut_thrashing_limit)
                /*
                 * Cut thrasing limit by thrashing_limit_decay_pct percentage of the current
                 * thrashing limit until the system stops thrashing.
                 */
                thrashing_limit = (thrashing_limit * (100 - thrashing_limit_decay_pct)) / 100;
        }
    }

no_kill:
    /* Do not poll if kernel supports pidfd waiting */
    if (is_waiting_for_kill()) {
        /* Pause polling if we are waiting for process death notification */
        poll_params->update = POLLING_PAUSE;
        return;
    }

    /*
     * Start polling after initial PSI event;
     * extend polling while device is in direct reclaim or process is being killed;
     * do not extend when kswapd reclaims because that might go on for a long time
     * without causing memory pressure
     */
    if (events || killing || reclaim == DIRECT_RECLAIM)
        poll_params->update = POLLING_START;

    /* Decide the polling interval */
    if (swap_is_low || killing)
        /* Fast polling during and after a kill or when swap is low */
        poll_params->polling_interval_ms = PSI_POLL_PERIOD_SHORT_MS;
    else
        /* By default use long intervals */
        poll_params->polling_interval_ms = PSI_POLL_PERIOD_LONG_MS;
}

static bool init_mp_psi(enum vmpressure_level level, bool use_new_strategy) {
    int fd;

    /* Do not register a handler if threshold_ms is not set */
    if (!psi_thresholds[level].threshold_ms)
        return true;

    fd = init_psi_monitor(psi_thresholds[level].stall_type,
                          psi_thresholds[level].threshold_ms * US_PER_MS,
                          PSI_WINDOW_SIZE_MS * US_PER_MS);

    if (fd < 0)
        return false;

    vmpressure_hinfo[level].handler = use_new_strategy ? mp_event_psi : mp_event_common;
    vmpressure_hinfo[level].data = level;
    if (register_psi_monitor(epollfd, fd, &vmpressure_hinfo[level]) < 0) {
        destroy_psi_monitor(fd);
        return false;
    }
    maxevents++;
    mpevfd[level] = fd;

    return true;
}

static void destroy_mp_psi(enum vmpressure_level level) {
    int fd = mpevfd[level];

    if (fd < 0)
        return;

    if (unregister_psi_monitor(epollfd, fd) < 0) {
        g_printerr("Failed to unregister psi monitor for %s memory pressure; errno=%d",
                   level_name[level],
                   errno);
    }
    maxevents--;
    destroy_psi_monitor(fd);
    mpevfd[level] = -1;
}

enum class MemcgVersion {
    kNotFound,
    kV1,
    kV2,
};

static MemcgVersion __memcg_version() {
    std::string cgroupv2_path, memcg_path;
    char mount_path[PATH_MAX];

    if (!find_cgroup_mount("memory", mount_path, sizeof(mount_path)))
        return MemcgVersion::kNotFound;

    memcg_path = mount_path;

    if (find_cgroup_mount("", mount_path, sizeof(mount_path))) {
        cgroupv2_path = mount_path;
        return cgroupv2_path == memcg_path ? MemcgVersion::kV2 : MemcgVersion::kV1;
    }

    return MemcgVersion::kV1;
}

static MemcgVersion memcg_version() {
    static MemcgVersion version = __memcg_version();

    return version;
}

static bool init_psi_monitors() {
    /*
     * When PSI is used on low-ram devices or on high-end devices without memfree levels
     * use new kill strategy based on zone watermarks, free swap and thrashing stats.
     * Also use the new strategy if memcg has not been mounted in the v1 cgroups hiearchy since
     * the old strategy relies on memcg attributes that are available only in the v1 cgroups
     * hiearchy.
     */
    bool use_new_strategy =
        get_config_bool("use_new_strategy", low_ram_device || !use_minfree_levels);
    if (!use_new_strategy && memcg_version() != MemcgVersion::kV1) {
        g_printerr("Old kill strategy can only be used with v1 cgroup hierarchy");
        return false;
    }
    /* In default PSI mode override stall amounts using system properties */
    if (use_new_strategy) {
        /* Do not use low pressure level */
        psi_thresholds[VMPRESS_LEVEL_LOW].threshold_ms = 0;
        psi_thresholds[VMPRESS_LEVEL_MEDIUM].threshold_ms = psi_partial_stall_ms;
        psi_thresholds[VMPRESS_LEVEL_CRITICAL].threshold_ms = psi_complete_stall_ms;
    }

    if (!init_mp_psi(VMPRESS_LEVEL_LOW, use_new_strategy))
        return false;

    if (!init_mp_psi(VMPRESS_LEVEL_MEDIUM, use_new_strategy)) {
        destroy_mp_psi(VMPRESS_LEVEL_LOW);
        return false;
    }
    if (!init_mp_psi(VMPRESS_LEVEL_CRITICAL, use_new_strategy)) {
        destroy_mp_psi(VMPRESS_LEVEL_MEDIUM);
        destroy_mp_psi(VMPRESS_LEVEL_LOW);
        return false;
    }
    return true;
}

static bool init_mp_common(enum vmpressure_level level) {
    /* The implementation of this function relies on memcg statistics that are only available in the
     * v1 cgroup hierarchy. */
    if (memcg_version() != MemcgVersion::kV1) {
        g_printerr("%s: global monitoring is only available for the v1 cgroup hierarchy", __func__);
        return false;
    }

    int mpfd;
    int evfd;
    int evctlfd;
    char buf[256];
    struct epoll_event epev;
    int ret;
    int level_idx = (int)level;
    const char *levelstr = level_name[level_idx];

    std::string mempress_path, evcontrol_path;

    if (!get_cgroup_attribute_path("memory.pressure_level", mempress_path)) {
        g_debug("No kernel memory.pressure_level support");
        return false;
    }

    if (!get_cgroup_attribute_path("cgroup.event_control", evcontrol_path)) {
        g_debug("No kernel memory cgroup event control");
        return false;
    }

    /* gid containing AID_SYSTEM required */
    mpfd = open(mempress_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (mpfd < 0) {
        g_debug("No kernel memory.pressure_level support (errno=%d)", errno);
        return false;
    }

    evctlfd = open(evcontrol_path.c_str(), O_WRONLY | O_CLOEXEC);
    if (evctlfd < 0) {
        g_debug("No kernel memory cgroup event control (errno=%d)", errno);
        close(mpfd);
        return false;
    }

    evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evfd < 0) {
        g_printerr("eventfd failed for level %s; errno=%d", levelstr, errno);
        close(evctlfd);
        close(mpfd);
        return false;
    }

    ret = snprintf(buf, sizeof(buf), "%d %d %s", evfd, mpfd, levelstr);
    if (ret >= (ssize_t)sizeof(buf)) {
        g_printerr("cgroup.event_control line overflow for level %s", levelstr);
        close(evfd);
        close(evctlfd);
        close(mpfd);
        return false;
    }

    ret = TEMP_FAILURE_RETRY(write(evctlfd, buf, strlen(buf) + 1));
    if (ret == -1) {
        g_printerr("cgroup.event_control write failed for level %s; errno=%d",
                   levelstr,
                   errno);
        close(evfd);
        close(evctlfd);
        close(mpfd);
        return false;
    }

    epev.events = EPOLLIN;
    /* use data to store event level */
    vmpressure_hinfo[level_idx].data = level_idx;
    vmpressure_hinfo[level_idx].handler = mp_event_common;
    epev.data.ptr = (void *)&vmpressure_hinfo[level_idx];
    ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, evfd, &epev);
    if (ret == -1) {
        g_printerr("epoll_ctl for level %s failed; errno=%d", levelstr, errno);
        close(evfd);
        close(evctlfd);
        close(mpfd);
        return false;
    }
    maxevents++;
    mpevfd[level] = evfd;
    close(evctlfd);
    close(mpfd);
    return true;
}

static void destroy_mp_common(enum vmpressure_level level) {
    struct epoll_event epev;
    int fd = mpevfd[level];

    if (fd < 0)
        return;

    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, &epev))
        g_printerr("epoll_ctl for level %s failed; errno=%d", level_name[level], errno);

    maxevents--;
    close(fd);
    mpevfd[level] = -1;
}

static void mp_event_common(int data, uint32_t events, struct polling_params *poll_params) {
    unsigned long long evcount;
    int64_t mem_usage, memsw_usage;
    int64_t mem_pressure;
    union meminfo mi;
    struct zoneinfo zi;
    struct timespec curr_tm;
    static unsigned long kill_skip_count = 0;
    enum vmpressure_level level = (enum vmpressure_level)data;
    long other_free = 0, other_file = 0;
    int min_score_adj = 0;
    int minfree = 0;
    static struct wakeup_info wi;

    /* Variable declarations at the top to avoid goto issues */
    std::string mem_usage_path, memsw_usage_path;
    static struct reread_data mem_usage_file_data = {nullptr, -1};
    static struct reread_data memsw_usage_file_data = {nullptr, -1};
    static bool paths_initialized = false;

    if (debug_process_killing)
        g_debug("%s memory pressure event is triggered", level_name[level]);

    if (!use_psi_monitors) {
        /*
         * Check all event counters from low to critical
         * and upgrade to the highest priority one. By reading
         * eventfd we also reset the event counters.
         */
        for (int lvl = VMPRESS_LEVEL_LOW; lvl < VMPRESS_LEVEL_COUNT; lvl++) {
            if (mpevfd[lvl] != -1 &&
                TEMP_FAILURE_RETRY(read(mpevfd[lvl],
                                        &evcount,
                                        sizeof(evcount))) > 0 &&
                evcount > 0 && lvl > level) {
                level = static_cast<vmpressure_level>(lvl);
            }
        }
    }

    /* Start polling after initial PSI event */
    if (use_psi_monitors && events) {
        /* Override polling params only if current event is more critical */
        if (!poll_params->poll_handler || data > poll_params->poll_handler->data) {
            poll_params->polling_interval_ms = PSI_POLL_PERIOD_SHORT_MS;
            poll_params->update = POLLING_START;
        }
    }

    if (clock_gettime(CLOCK_MONOTONIC_COARSE, &curr_tm) != 0) {
        g_printerr("Failed to get current time");
        return;
    }

    record_wakeup_time(&curr_tm, events ? Event : Polling, &wi);

    if (kill_timeout_ms &&
        get_time_diff_ms(&last_kill_tm, &curr_tm) < static_cast<long>(kill_timeout_ms)) {
        /*
         * If we're within the no-kill timeout, see if there's pending reclaim work
         * from the last killed process. If so, skip killing for now.
         */
        if (is_kill_pending()) {
            kill_skip_count++;
            wi.skipped_wakeups++;
            return;
        }
        /*
         * Process is dead, stop waiting. This has no effect if pidfds are supported and
         * death notification already caused waiting to stop.
         */
        stop_wait_for_proc_kill(true);
    } else {
        /*
         * Killing took longer than no-kill timeout. Stop waiting for the last process
         * to die because we are ready to kill again.
         */
        stop_wait_for_proc_kill(false);
    }

    if (kill_skip_count > 0) {
        g_debug("%lu memory pressure events were skipped after a kill!",
                kill_skip_count);
        kill_skip_count = 0;
    }

    if (meminfo_parse(&mi) < 0 || zoneinfo_parse(&zi) < 0) {
        g_printerr("Failed to get free memory!");
        return;
    }

    if (use_minfree_levels) {
        int i;

        other_free = mi.field.nr_free_pages - zi.totalreserve_pages;
        if (mi.field.nr_file_pages > (mi.field.shmem + mi.field.unevictable + mi.field.swap_cached))
            other_file = (mi.field.nr_file_pages - mi.field.shmem -
                          mi.field.unevictable - mi.field.swap_cached);
        else
            other_file = 0;

        min_score_adj = OOM_SCORE_ADJ_MAX + 1;
        for (i = 0; i < lowmem_targets_size; i++) {
            minfree = lowmem_minfree[i];
            if (other_free < minfree && other_file < minfree) {
                min_score_adj = lowmem_adj[i];
                break;
            }
        }

        if (min_score_adj == OOM_SCORE_ADJ_MAX + 1) {
            if (debug_process_killing && lowmem_targets_size)
                g_debug("Ignore %s memory pressure event "
                        "(free memory=%ldkB, cache=%ldkB, limit=%ldkB)",
                        level_name[level],
                        other_free * page_k,
                        other_file * page_k,
                        (long)lowmem_minfree[lowmem_targets_size - 1] * page_k);
            return;
        }

        goto do_kill;
    }

    if (level == VMPRESS_LEVEL_LOW)
        record_low_pressure_levels(&mi);

    if (level_oomadj[level] > OOM_SCORE_ADJ_MAX)
        /* Do not monitor this pressure level */
        return;

    if (!paths_initialized) {
        if (get_cgroup_attribute_path("memory.usage_in_bytes", mem_usage_path)) {
            /* Cast away const for assignment - this is safe since we own the string */
            *(const char **)&mem_usage_file_data.filename = strdup(mem_usage_path.c_str());
            mem_usage_file_data.fd = -1;
        }
        if (get_cgroup_attribute_path("memory.memsw.usage_in_bytes", memsw_usage_path)) {
            *(const char **)&memsw_usage_file_data.filename = strdup(memsw_usage_path.c_str());
            memsw_usage_file_data.fd = -1;
        }
        paths_initialized = true;
    }

    if ((mem_usage = get_memory_usage(&mem_usage_file_data)) < 0)
        goto do_kill;
    if ((memsw_usage = get_memory_usage(&memsw_usage_file_data)) < 0)
        goto do_kill;

    /* Calculate percent for swappinness. */
    mem_pressure = (mem_usage * 100) / memsw_usage;

    if (enable_pressure_upgrade && level != VMPRESS_LEVEL_CRITICAL) {
        /* We are swapping too much. */
        if (mem_pressure < upgrade_pressure) {
            level = upgrade_level(level);
            if (debug_process_killing)
                g_debug("Event upgraded to %s", level_name[level]);
        }
    }

    /* If we still have enough swap space available, check if we want to
     * ignore/downgrade pressure events. */
    if (mi.field.free_swap >=
        mi.field.total_swap * swap_free_low_percentage / 100) {
        /* If the pressure is larger than downgrade_pressure lmk will not
         * kill any process, since enough memory is available. */
        if (mem_pressure > downgrade_pressure) {
            if (debug_process_killing)
                g_debug("Ignore %s memory pressure", level_name[level]);
            return;
        } else if (level == VMPRESS_LEVEL_CRITICAL && mem_pressure > upgrade_pressure) {
            if (debug_process_killing)
                g_debug("Downgrade critical memory pressure");
            /* Downgrade event, since enough memory available. */
            level = downgrade_level(level);
        }
    }

do_kill:
    if (low_ram_device) {
        /* For low memory devices kill only one task */
        if (find_and_kill_process(level_oomadj[level], NULL, &mi, &wi, &curr_tm, NULL) == 0) {
            if (debug_process_killing)
                g_debug("Nothing to kill");
        }
    } else {
        int pages_freed;
        static struct timespec last_report_tm;
        static unsigned long report_skip_count = 0;

        if (!use_minfree_levels) {
            /* Free up enough memory to downgrate the memory pressure to low level */
            if (mi.field.nr_free_pages >= low_pressure_mem.max_nr_free_pages) {
                if (debug_process_killing)
                    g_debug("Ignoring pressure since more memory is "
                            "available (%" PRId64 ") than watermark (%" PRId64 ")",
                            mi.field.nr_free_pages,
                            low_pressure_mem.max_nr_free_pages);
                return;
            }
            min_score_adj = level_oomadj[level];
        }

        pages_freed = find_and_kill_process(min_score_adj, NULL, &mi, &wi, &curr_tm, NULL);

        if (pages_freed == 0) {
            /* Rate limit kill reports when nothing was reclaimed */
            if (get_time_diff_ms(&last_report_tm, &curr_tm) < FAIL_REPORT_RLIMIT_MS) {
                report_skip_count++;
                return;
            }
        }

        /* Log whenever we kill or when report rate limit allows */
        if (use_minfree_levels)
            g_debug("Reclaimed %ldkB, cache(%ldkB) and free(%" PRId64 "kB)-reserved(%" PRId64 "kB) "
                    "below min(%ldkB) for oom_score_adj %d",
                    pages_freed * page_k,
                    other_file * page_k,
                    mi.field.nr_free_pages * page_k,
                    zi.totalreserve_pages * page_k,
                    minfree * page_k,
                    min_score_adj);
        else
            g_debug("Reclaimed %ldkB at oom_score_adj %d", pages_freed * page_k, min_score_adj);

        if (report_skip_count > 0) {
            g_debug("Suppressed %lu failed kill reports", report_skip_count);
            report_skip_count = 0;
        }

        last_report_tm = curr_tm;
    }
    if (is_waiting_for_kill())
        /* pause polling if we are waiting for process death notification */
        poll_params->update = POLLING_PAUSE;
}

static bool init_monitors() {
    g_debug("Attempting to initialize PSI monitors...");
    /* Try to use psi monitor first if kernel has it */
    use_psi_monitors = get_config_bool("use_psi", true) &&
                       init_psi_monitors();

    if (use_psi_monitors) {
        g_debug("PSI monitors initialized successfully");
    } else {
        g_debug("PSI monitors failed, trying vmpressure...");
        /* Fall back to vmpressure */
        if (!init_mp_common(VMPRESS_LEVEL_LOW) ||
            !init_mp_common(VMPRESS_LEVEL_MEDIUM) ||
            !init_mp_common(VMPRESS_LEVEL_CRITICAL)) {
            g_printerr("Kernel does not support memory pressure events");
            return false;
        }
        g_debug("VmPressure monitors initialized successfully");
    }

    if (use_psi_monitors)
        g_debug("Using psi monitors for memory pressure detection");
    else
        g_debug("Using vmpressure for memory pressure detection");

    return true;
}

static void destroy_monitors() {
    if (use_psi_monitors) {
        destroy_mp_psi(VMPRESS_LEVEL_CRITICAL);
        destroy_mp_psi(VMPRESS_LEVEL_MEDIUM);
        destroy_mp_psi(VMPRESS_LEVEL_LOW);
    } else {
        destroy_mp_common(VMPRESS_LEVEL_CRITICAL);
        destroy_mp_common(VMPRESS_LEVEL_MEDIUM);
        destroy_mp_common(VMPRESS_LEVEL_LOW);
    }
}

static void drop_reaper_comm() {
    close(reaper_comm_fd[0]);
    close(reaper_comm_fd[1]);
}

static bool setup_reaper_comm() {
    if (pipe(reaper_comm_fd)) {
        g_printerr("pipe failed: %s", strerror(errno));
        return false;
    }

    /* Ensure main thread never blocks on read */
    int flags = fcntl(reaper_comm_fd[0], F_GETFL);
    if (fcntl(reaper_comm_fd[0], F_SETFL, flags | O_NONBLOCK)) {
        g_printerr("fcntl failed: %s", strerror(errno));
        drop_reaper_comm();
        return false;
    }

    return true;
}

static bool init_reaper() {
    if (!reaper.is_reaping_supported()) {
        g_debug("Process reaping is not supported");
        return false;
    }

    if (!setup_reaper_comm()) {
        g_printerr("Failed to create thread communication channel");
        return false;
    }

    /* Setup epoll handler */
    struct epoll_event epev;
    static struct event_handler_info kill_failed_hinfo = {0, kill_fail_handler};
    epev.events = EPOLLIN;
    epev.data.ptr = (void *)&kill_failed_hinfo;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, reaper_comm_fd[0], &epev)) {
        g_printerr("epoll_ctl failed: %s", strerror(errno));
        drop_reaper_comm();
        return false;
    }

    if (!reaper.init(reaper_comm_fd[1])) {
        g_printerr("Failed to initialize reaper object");
        if (epoll_ctl(epollfd, EPOLL_CTL_DEL, reaper_comm_fd[0], &epev))
            g_printerr("epoll_ctl failed: %s", strerror(errno));
        drop_reaper_comm();
        return false;
    }
    maxevents++;

    return true;
}

static void on_process_register(pid_t pid, uid_t uid, int oomadj, int pidfd) {
    struct proc *procp;

    g_debug("Attempting to register process: pid=%d, uid=%d, oomadj=%d, pidfd=%d",
            pid,
            uid,
            oomadj,
            pidfd);

    /* Sanity check the oomadj value */
    if (oomadj < OOM_SCORE_ADJ_MIN || oomadj > OOM_SCORE_ADJ_MAX) {
        g_debug("Invalid oomadj %d for pid %d, skipping", oomadj, pid);
        if (pidfd >= 0)
            close(pidfd);
        return;
    }

    /* Check if already registered */
    struct proc *existing = pid_lookup(pid);
    if (existing) {
        g_debug("Process %d already registered, skipping", pid);
        if (pidfd >= 0)
            close(pidfd);
        return;
    }

    procp = (struct proc *)malloc(sizeof(struct proc));
    if (!procp) {
        g_printerr("Failed to allocate memory for process %d", pid);
        if (pidfd >= 0)
            close(pidfd);
        return;
    }

    memset(procp, 0, sizeof(struct proc));
    procp->pid = pid;
    procp->pidfd = pidfd;
    procp->uid = uid;
    procp->oomadj = oomadj;
    procp->reg_pid = getpid(); /* LMKD's PID */
    procp->valid = true;
    procp->asl.next = nullptr;
    procp->asl.prev = nullptr;
    procp->pidhash_next = nullptr;
    int hval = pid_hashfn(pid);
    procp->pidhash_next = pidhash[hval];
    pidhash[hval] = procp;

    proc_slot(procp);

    g_debug("Successfully registered process: pid=%d, uid=%d, oomadj=%d, pidfd=%d",
            pid,
            uid,
            oomadj,
            pidfd);
}

static void on_process_exit(pid_t pid) {
    if (debug_process_killing)
        g_debug("Process %d exited, removing from tracking", pid);

    /* Remove from tracking */
    pid_remove(pid);
}

static bool init_process_registration(void) {
    struct processwatcher_config config;

    g_debug("Initializing process registration...");

    memset(&config, 0, sizeof(config));
    config.epollfd = epollfd;
    config.maxevents = &maxevents;
    config.on_register = on_process_register;
    config.on_exit = on_process_exit;
    config.enable_debug = debug_process_killing;

    if (!processwatcher_init(&config)) {
        g_printerr("Failed to initialize process watcher");
        return false;
    }

    processwatcher_register_all_existing();

    g_debug("Process registration initialized successfully");
    return true;
}

static void cleanup_process_registration(void) {
    processwatcher_cleanup();
}

static int init(void) {
    struct reread_data file_data = {
        .filename = ZONEINFO_PATH,
        .fd = -1,
    };
    int pidfd;

    page_k = sysconf(_SC_PAGESIZE);
    if (page_k == -1)
        page_k = PAGE_SIZE;
    page_k /= 1024;

    g_debug("Page size: %ld KB", page_k);

    epollfd = epoll_create(MAX_EPOLL_EVENTS);
    if (epollfd == -1) {
        g_printerr("epoll_create failed (errno=%d)", errno);
        return -1;
    }
    g_debug("Epoll fd created: %d", epollfd);

    g_debug("Initializing memory pressure monitors...");
    if (!init_monitors()) {
        g_printerr("Failed to initialize monitors");
        return -1;
    }
    g_debug("Memory pressure monitors initialized, maxevents=%d", maxevents);

    g_debug("Initializing process tracking structures...");
    for (int i = 0; i <= ADJTOSLOT(OOM_SCORE_ADJ_MAX); i++) {
        procadjslot_list[i].next = &procadjslot_list[i];
        procadjslot_list[i].prev = &procadjslot_list[i];
    }
    memset(killcnt_idx, KILLCNT_INVALID_IDX, sizeof(killcnt_idx));

    g_debug("Initializing process registration netlink");
    if (!init_process_registration())
        g_warning("Failed to initialize process registration, kill functionality may be limited");

    g_debug("Initializing process tracking structures...");
    for (int i = 0; i <= ADJTOSLOT(OOM_SCORE_ADJ_MAX); i++) {
        procadjslot_list[i].next = &procadjslot_list[i];
        procadjslot_list[i].prev = &procadjslot_list[i];
    }

    memset(killcnt_idx, KILLCNT_INVALID_IDX, sizeof(killcnt_idx));
    g_debug("Process tracking structures initialized");

    /*
     * Read zoneinfo as the biggest file we read to create and size the initial
     * read buffer and avoid memory re-allocations during memory pressure
     */
    g_debug("Pre-reading zoneinfo file...");
    if (reread_file(&file_data) == NULL)
        g_printerr("Failed to read %s: %s", file_data.filename, strerror(errno));
    else
        g_debug("Zoneinfo file pre-read successfully");

    /* check if kernel supports pidfd_open syscall */
    g_debug("Testing pidfd support...");
    pidfd = TEMP_FAILURE_RETRY(pidfd_open(getpid(), 0));
    if (pidfd < 0) {
        pidfd_supported = (errno != ENOSYS);
        g_debug("pidfd_open test failed: %s (supported=%s)",
                strerror(errno),
                pidfd_supported ? "yes" : "no");
    } else {
        pidfd_supported = true;
        close(pidfd);
        g_debug("pidfd_open test successful");
    }
    g_debug("Process polling is %s", pidfd_supported ? "supported" : "not supported");

    return 0;
}

static bool polling_paused(struct polling_params *poll_params) {
    return poll_params->paused_handler != NULL;
}

static void resume_polling(struct polling_params *poll_params, struct timespec curr_tm) {
    poll_params->poll_start_tm = curr_tm;
    poll_params->poll_handler = poll_params->paused_handler;
    poll_params->polling_interval_ms = PSI_POLL_PERIOD_SHORT_MS;
    poll_params->paused_handler = NULL;
}

static void call_handler(struct event_handler_info *handler_info,
                         struct polling_params *poll_params,
                         uint32_t events) {
    struct timespec curr_tm;
    struct timespec start_tm;

    clock_gettime(CLOCK_MONOTONIC_COARSE, &start_tm);
    g_debug("handler start: data=%d, events=0x%x, poll_handler=%p, paused_handler=%p",
            handler_info->data,
            events,
            poll_params->poll_handler,
            poll_params->paused_handler);

    watchdog.start();
    poll_params->update = POLLING_DO_NOT_CHANGE;

    handler_info->handler(handler_info->data, events, poll_params);

    clock_gettime(CLOCK_MONOTONIC_COARSE, &curr_tm);
    if (poll_params->poll_handler == handler_info)
        poll_params->last_poll_tm = curr_tm;

    const char *update_names[] = {
        "POLLING_DO_NOT_CHANGE",
        "POLLING_START",
        "POLLING_PAUSE",
        "POLLING_RESUME"};

    g_debug("handler update: %s", update_names[poll_params->update]);

    switch (poll_params->update) {
    case POLLING_START:
        /*
         * Poll for the duration of PSI_WINDOW_SIZE_MS after the
         * initial PSI event because psi events are rate-limited
         * at one per sec.
         */
        poll_params->poll_start_tm = curr_tm;
        poll_params->poll_handler = handler_info;
        g_debug("polling start: handler set to data=%d, interval=%dms",
                handler_info->data,
                poll_params->polling_interval_ms);
        break;
    case POLLING_PAUSE:
        poll_params->paused_handler = handler_info;
        poll_params->poll_handler = NULL;
        g_debug("polling pause: handler paused (data=%d)", handler_info->data);
        break;
    case POLLING_RESUME:
        resume_polling(poll_params, curr_tm);
        g_debug("polling resume: polling resumed");
        break;
    case POLLING_DO_NOT_CHANGE:
        if (poll_params->poll_handler) {
            long poll_duration = get_time_diff_ms(&poll_params->poll_start_tm, &curr_tm);
            if (poll_duration > PSI_WINDOW_SIZE_MS) {
                /* Polled for the duration of PSI window, time to stop */
                g_debug("polling stop: poll window expired (%ldms > %dms)",
                        poll_duration,
                        PSI_WINDOW_SIZE_MS);
                poll_params->poll_handler = NULL;
            } else {
                g_debug("polling continue: poll window active (%ldms / %dms)",
                        poll_duration,
                        PSI_WINDOW_SIZE_MS);
            }
        }
        break;
    }

    watchdog.stop();
}

static void mainloop(void) {
    struct event_handler_info *handler_info;
    struct polling_params poll_params;
    struct timespec curr_tm;
    struct epoll_event *evt;
    long delay = -1;
    static int loop_count = 0;

    poll_params.poll_handler = NULL;
    poll_params.paused_handler = NULL;

    g_debug("Max epoll events: %d", maxevents);

    while (1) {
        struct epoll_event events[MAX_EPOLL_EVENTS];
        int nevents;
        int i;

        loop_count++;

        if (poll_params.poll_handler) {
            bool poll_now;

            clock_gettime(CLOCK_MONOTONIC_COARSE, &curr_tm);
            if (poll_params.update == POLLING_RESUME) {
                /* Just transitioned into POLLING_RESUME, poll immediately. */
                g_debug("POLLING_RESUME - polling immediately");
                poll_now = true;
                nevents = 0;
            } else {
                /* Calculate next timeout */
                delay = get_time_diff_ms(&poll_params.last_poll_tm, &curr_tm);
                delay = (delay < poll_params.polling_interval_ms) ? poll_params.polling_interval_ms - delay : poll_params.polling_interval_ms;

                g_debug("Polling mode: waiting for events (timeout=%ldms, interval=%dms)",
                        delay,
                        poll_params.polling_interval_ms);

                /* Wait for events until the next polling timeout */
                nevents = epoll_wait(epollfd, events, maxevents, delay);

                /* Update current time after wait */
                clock_gettime(CLOCK_MONOTONIC_COARSE, &curr_tm);
                poll_now = (get_time_diff_ms(&poll_params.last_poll_tm, &curr_tm) >=
                            poll_params.polling_interval_ms);

                if (poll_now)
                    g_debug("Polling timeout reached, calling handler");
            }
            if (poll_now) {
                g_debug("Calling polling handler (data=%d)",
                        poll_params.poll_handler->data);
                call_handler(poll_params.poll_handler, &poll_params, 0);
            }
        } else {
            if (kill_timeout_ms && is_waiting_for_kill()) {
                clock_gettime(CLOCK_MONOTONIC_COARSE, &curr_tm);
                delay = kill_timeout_ms - get_time_diff_ms(&last_kill_tm, &curr_tm);

                g_debug("Waiting for kill completion (delay=%ldms, kill_timeout=%lums)",
                        delay,
                        kill_timeout_ms);

                /* Wait for pidfds notification or kill timeout to expire */
                nevents = (delay > 0) ? epoll_wait(epollfd, events, maxevents, delay) : 0;
                if (nevents == 0) {
                    /* Kill notification timed out */
                    g_debug("Kill timeout expired, stopping wait");
                    stop_wait_for_proc_kill(false);
                    if (polling_paused(&poll_params)) {
                        clock_gettime(CLOCK_MONOTONIC_COARSE, &curr_tm);
                        poll_params.update = POLLING_RESUME;
                        g_debug("Resuming polling after kill timeout");
                        resume_polling(&poll_params, curr_tm);
                    }
                }
            } else {
                /* Wait for events with no timeout */
                g_debug("Idle mode: waiting for events (no timeout)");
                nevents = epoll_wait(epollfd, events, maxevents, -1);
            }
        }

        if (nevents == -1) {
            if (errno == EINTR) {
                g_debug("epoll_wait interrupted (EINTR)");
                continue;
            }
            g_printerr("epoll_wait failed (errno=%d): %s",
                       errno,
                       strerror(errno));
            continue;
        }

        g_debug("Received %d events", nevents);

        /* Handle all events */
        for (i = 0, evt = &events[0]; i < nevents; ++i, evt++) {
            g_debug("Event %d: events=0x%x, data.ptr=%p",
                    i,
                    evt->events,
                    evt->data.ptr);

            if (evt->events & EPOLLERR)
                g_debug("Event %d: EPOLLERR detected", i);

            if (evt->events & EPOLLHUP) {
                g_debug("Event %d: EPOLLHUP detected, skipping", i);
                continue;
            }
            if (evt->events & EPOLLIN)
                g_debug("Event %d: EPOLLIN detected", i);

            if (evt->data.ptr) {
                handler_info = (struct event_handler_info *)evt->data.ptr;
                g_debug("Event %d: Calling handler (data=%d)",
                        i,
                        handler_info->data);
                call_handler(handler_info, &poll_params, evt->events);
            } else {
                g_debug("Event %d: No handler (data.ptr is NULL)", i);
            }
        }

        static enum polling_update prev_update = POLLING_DO_NOT_CHANGE;
        if (poll_params.update != prev_update) {
            const char *update_names[] = {
                "POLLING_DO_NOT_CHANGE",
                "POLLING_START",
                "POLLING_PAUSE",
                "POLLING_RESUME"};
            g_debug("Poll state change: %s -> %s",
                    update_names[prev_update],
                    update_names[poll_params.update]);
            prev_update = poll_params.update;
        }

        if (loop_count % 100 == 0 && !poll_params.poll_handler) {
            g_debug("Status: idle, kill_pending=%s, maxevents=%d",
                    is_kill_pending() ? "yes" : "no",
                    maxevents);
        }
    }
}

static void update_props() {
    /* By default disable low level vmpressure events */
    level_oomadj[VMPRESS_LEVEL_LOW] =
        get_config_int32("low", OOM_SCORE_ADJ_MAX + 1);
    level_oomadj[VMPRESS_LEVEL_MEDIUM] =
        get_config_int32("medium", 800);
    level_oomadj[VMPRESS_LEVEL_CRITICAL] =
        get_config_int32("critical", 0);
    debug_process_killing = get_config_bool("debug", false);

    /* By default disable upgrade/downgrade logic */
    enable_pressure_upgrade =
        get_config_bool("critical_upgrade", false);
    upgrade_pressure =
        (int64_t)get_config_int32("upgrade_pressure", 100);
    downgrade_pressure =
        (int64_t)get_config_int32("downgrade_pressure", 100);
    kill_heaviest_task =
        get_config_bool("kill_heaviest_task", false);

    low_ram_device = get_config_bool("low_ram", false);

    kill_timeout_ms =
        (unsigned long)get_config_int32("kill_timeout_ms", 100);
    use_minfree_levels =
        get_config_bool("use_minfree_levels", false);
    per_app_memcg =
        get_config_bool("per_app_memcg", low_ram_device);
    swap_free_low_percentage = clamp(0, 100, get_config_int32("swap_free_low_percentage", DEF_LOW_SWAP));
    psi_partial_stall_ms = get_config_int32("psi_partial_stall_ms",
                                            low_ram_device ? DEF_PARTIAL_STALL_LOWRAM : DEF_PARTIAL_STALL);
    psi_complete_stall_ms = get_config_int32("psi_complete_stall_ms",
                                             DEF_COMPLETE_STALL);
    thrashing_limit_pct =
        std::max(0, get_config_int32("thrashing_limit", low_ram_device ? DEF_THRASHING_LOWRAM : DEF_THRASHING));
    thrashing_limit_decay_pct = clamp(0, 100, get_config_int32("thrashing_limit_decay", low_ram_device ? DEF_THRASHING_DECAY_LOWRAM : DEF_THRASHING_DECAY));
    thrashing_critical_pct = std::max(
        0, get_config_int32("thrashing_limit_critical", thrashing_limit_pct * 2));
    swap_util_max = clamp(0, 100, get_config_int32("swap_util_max", 100));
    filecache_min_kb = get_config_int64("filecache_min_kb", 0);
    stall_limit_critical = get_config_int64("stall_limit_critical", 100);

    reaper.enable_debug(debug_process_killing);
}

int main(int argc __unused, char **argv __unused) {
    g_debug("lmkd starting");
    g_debug("Build: %s %s", __DATE__, __TIME__);

    update_props();
    g_debug("Configuration loaded");

    if (!init()) {
        g_debug("Setting process priority and memory locking...");
        /*
         * MCL_ONFAULT pins pages as they fault instead of loading
         * everything immediately all at once. (Which would be bad,
         * because as of this writing, we have a lot of mapped pages we
         * never use.) Old kernels will see MCL_ONFAULT and fail with
         * EINVAL; we ignore this failure.
         *
         * N.B. read the man page for mlockall. MCL_CURRENT | MCL_ONFAULT
         * pins ⊆ MCL_CURRENT, converging to just MCL_CURRENT as we fault
         * in pages.
         */
        /* CAP_IPC_LOCK required */
        if (mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT) && (errno != EINVAL))
            g_warning("mlockall failed %s", strerror(errno));
        else
            g_debug("Memory locked successfully");

        /* CAP_NICE required */
        struct sched_param param = {
            .sched_priority = 1,
        };
        if (sched_setscheduler(0, SCHED_FIFO, &param))
            g_warning("set SCHED_FIFO failed %s", strerror(errno));
        else
            g_debug("SCHED_FIFO priority set successfully");

        if (init_reaper())
            g_debug("Process reaper initialized with %d threads in the pool",
                    reaper.thread_cnt());
        else
            g_debug("Process reaper initialization failed or not supported");

        if (!watchdog.init())
            g_printerr("Failed to initialize the watchdog");
        else
            g_debug("Watchdog initialized successfully");

        g_debug("entering main loop");
        mainloop();
    }

    if (config)
        g_key_file_free(config);

    destroy_monitors();
    cleanup_process_registration();
    g_debug("lmkd exiting");
    return 0;
}
