/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2018 Google, Inc
 * Copyright (C) 2025 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef _LMKD_H_
#define _LMKD_H_

#include <psi/psi.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __unused
#define __unused __attribute__((__unused__))
#endif

/* Path definitions */
#define ZONEINFO_PATH "/proc/zoneinfo"
#define MEMINFO_PATH "/proc/meminfo"
#define VMSTAT_PATH "/proc/vmstat"
#define PROC_STATUS_TGID_FIELD "Tgid:"
#define PROC_STATUS_RSS_FIELD "VmRSS:"
#define PROC_STATUS_SWAP_FIELD "VmSwap:"

/* Constants */
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

/*
 * Max number of targets in LMK_TARGET command.
 */
#define MAX_TARGETS 6

/*
 * 3 memory pressure levels, 1 fd to wait for process death + 1 fd to receive kill failure notifications
 */
#define MAX_EPOLL_EVENTS (VMPRESS_LEVEL_COUNT + 1 + 1)

/* OOM score values used by both kernel and framework */
#define OOM_SCORE_ADJ_MIN (-1000)
#define OOM_SCORE_ADJ_MAX 1000

/* see __MAX_NR_ZONES definition in kernel mmzone.h */
#define MAX_NR_ZONES 6

/* for now two memory nodes is more than enough */
#define MAX_NR_NODES 2

#define PIDHASH_SZ 1024
#define pid_hashfn(x) ((((x) >> 8) ^ (x)) & (PIDHASH_SZ - 1))

#define ADJTOSLOT(adj) ((adj) + -OOM_SCORE_ADJ_MIN)
#define ADJTOSLOT_COUNT (ADJTOSLOT(OOM_SCORE_ADJ_MAX) + 1)

#define MAX_DISTINCT_OOM_ADJ 32
#define KILLCNT_INVALID_IDX 0xFF

/* memory pressure levels */
enum vmpressure_level {
    VMPRESS_LEVEL_LOW = 0,
    VMPRESS_LEVEL_MEDIUM,
    VMPRESS_LEVEL_CRITICAL,
    VMPRESS_LEVEL_COUNT
};

enum wakeup_reason {
    Event,
    Polling
};

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

enum polling_update {
    POLLING_DO_NOT_CHANGE,
    POLLING_START,
    POLLING_PAUSE,
    POLLING_RESUME,
};

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

/* zoneinfo per-zone special fields */
enum zoneinfo_zone_spec_field {
    ZI_ZONE_SPEC_PROTECTION = 0,
    ZI_ZONE_SPEC_PAGESETS,
    ZI_ZONE_SPEC_FIELD_COUNT,
};

/* zoneinfo per-node fields */
enum zoneinfo_node_field {
    ZI_NODE_NR_INACTIVE_FILE = 0,
    ZI_NODE_NR_ACTIVE_FILE,
    ZI_NODE_FIELD_COUNT
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

enum field_match_result {
    NO_MATCH,
    PARSE_FAIL,
    PARSE_SUCCESS
};

enum zone_watermark {
    WMARK_MIN = 0,
    WMARK_LOW,
    WMARK_HIGH,
    WMARK_NONE
};

struct low_pressure_mem {
    int64_t min_nr_free_pages; /* recorded but not used yet */
    int64_t max_nr_free_pages;
};

struct psi_threshold {
    enum psi_stall_type stall_type;
    int threshold_ms;
};

struct polling_params {
    struct event_handler_info *poll_handler;
    struct event_handler_info *paused_handler;
    struct timespec poll_start_tm;
    struct timespec last_poll_tm;
    int polling_interval_ms;
    enum polling_update update;
};

struct event_handler_info {
    int data;
    void (*handler)(int data, uint32_t events, struct polling_params *poll_params);
    bool bypass_call_handler;
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

struct zoneinfo {
    int node_count;
    struct zoneinfo_node nodes[MAX_NR_NODES];
    int64_t totalreserve_pages;
    int64_t total_inactive_file;
    int64_t total_active_file;
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
    } field;
    int64_t arr[MI_FIELD_COUNT];
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

struct zone_watermarks {
    long high_wmark;
    long low_wmark;
    long min_wmark;
};

struct wakeup_info {
    struct timespec wakeup_tm;
    struct timespec prev_wakeup_tm;
    struct timespec last_event_tm;
    int wakeups_since_event;
    int skipped_wakeups;
};

struct kill_info {
    enum kill_reasons kill_reason;
    const char *kill_desc;
    int thrashing;
    int max_thrashing;
};

/* Static arrays and field names */
extern const char *const level_name[VMPRESS_LEVEL_COUNT];
extern const char *const zoneinfo_zone_field_names[ZI_ZONE_FIELD_COUNT];
extern const char *const zoneinfo_zone_spec_field_names[ZI_ZONE_SPEC_FIELD_COUNT];
extern const char *const zoneinfo_node_field_names[ZI_NODE_FIELD_COUNT];
extern const char *const meminfo_field_names[MI_FIELD_COUNT];
extern const char *const vmstat_field_names[VS_FIELD_COUNT];

/* List of process names to skip during registration */
extern const char* const SKIP_PROCESS_NAMES[];
/* List of process prefixes to skip during registration */
extern const char* const SKIP_PROCESS_WITH_PREFIX[];

/* lmkd configurable parameters */
extern bool enable_pressure_upgrade;
extern int64_t upgrade_pressure;
extern int64_t downgrade_pressure;
extern bool low_ram_device;
extern bool kill_heaviest_task;
extern unsigned long kill_timeout_ms;
extern bool use_minfree_levels;
extern bool per_app_memcg;
extern int swap_free_low_percentage;
extern int psi_partial_stall_ms;
extern int psi_complete_stall_ms;
extern int thrashing_limit_pct;
extern int thrashing_limit_decay_pct;
extern int thrashing_critical_pct;
extern int swap_util_max;
extern int64_t filecache_min_kb;
extern int64_t stall_limit_critical;
extern bool use_psi_monitors;
extern struct psi_threshold psi_thresholds[VMPRESS_LEVEL_COUNT];

/* Global state variables */
extern struct low_pressure_mem low_pressure_mem;
extern int level_oomadj[VMPRESS_LEVEL_COUNT];
extern int mpevfd[VMPRESS_LEVEL_COUNT];
extern bool pidfd_supported;
extern int last_kill_pid_or_fd;
extern struct timespec last_kill_tm;

/* vmpressure event handler data */
extern struct event_handler_info vmpressure_hinfo[VMPRESS_LEVEL_COUNT];
extern int epollfd;
extern int maxevents;
extern int lowmem_adj[MAX_TARGETS];
extern int lowmem_minfree[MAX_TARGETS];
extern int lowmem_targets_size;

/* Process tracking structures */
extern struct proc *pidhash[PIDHASH_SZ];
extern struct adjslot_list procadjslot_list[ADJTOSLOT_COUNT];
extern uint8_t killcnt_idx[ADJTOSLOT_COUNT];
extern uint16_t killcnt[MAX_DISTINCT_OOM_ADJ];
extern int killcnt_free_idx;
extern uint32_t killcnt_total;

/* Reaper and D-Bus service */
extern int reaper_comm_fd[2];

/* Configuration system */
extern bool config_loaded;

/* PAGE_SIZE / 1024 */
extern long page_k;

#ifdef __cplusplus
}
#endif

#endif /* _LMKD_H_ */
