/*

numad - NUMA Daemon to automatically bind processes to NUMA nodes
Copyright (C) 2012 Bill Gray (bgray@redhat.com), Red Hat Inc

numad is free software; you can redistribute it and/or modify it under the
terms of the GNU Lesser General Public License as published by the Free
Software Foundation; version 2.1.

numad is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
details.

You should find a copy of v2.1 of the GNU Lesser General Public License
somewhere on your Linux system; if not, write to the Free Software Foundation,
Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

*/ 


// Compile with: gcc -std=gnu99 -g -Wall -pthread -o numad numad.c -lrt -lm


#define _GNU_SOURCE

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <values.h>

#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/syslog.h>
#include <sys/time.h>
#include <sys/types.h>

#include <asm/unistd.h>

#define VERSION_STRING "numad 20251104"

#define VAR_RUN_DIR_ADMIN "/var/run"
#define VAR_LOG_DIR_ADMIN "/var/log"
#define VAR_RUN_FILE "numad.pid"
#define VAR_LOG_FILE "numad.log"

#define KILOBYTE (1024)
#define MEGABYTE (1024 * 1024)

#define FNAME_SIZE 192
#define BUF_SIZE 1024
#define BIG_BUF_SIZE 4096

// The ONE_HUNDRED factor is used to scale time and CPU usage units.
// Several CPU quantities are measured in percents of a CPU; and
// several time values are counted in hundreths of a second.
#define ONE_HUNDRED 100


#define MIN_INTERVAL  5
#define MAX_INTERVAL 15
#define CPU_THRESHOLD     50
#define MEMORY_THRESHOLD 300
#define DEFAULT_HTT_PERCENT 20
#define DEFAULT_THP_SCAN_SLEEP_MS 1000
#define DEFAULT_UTILIZATION_PERCENT 90
#define DEFAULT_MEMLOCALITY_PERCENT 90
#define MIN_DELAY_FOR_REEVALUATION (300 * ONE_HUNDRED)

#define DEFAULT_GPU_AWARE 1
#define DEFAULT_GPU_MIN_BUSY_PCT 10
#define DEFAULT_GPU_MIN_VRAM_MB 256
#define DEFAULT_GPU_FDINFO_DISCOVERY_INTERVAL 15
#define DEFAULT_GPU_MIGRATE_BUSY_MAX 20
#define DEFAULT_GPU_GRAPHICS_PLACEMENT 0
#define DEFAULT_BIND_COOLDOWN_SEC 300

#define PROC_COMM_SIZE 256
#define GPU_BDF_SIZE 32
#define GPU_DRM_NAME_SIZE 32

#define MIN(a, b)   ((a) < (b) ? (a) : (b))
#define MAX(a, b)   ((a) > (b) ? (a) : (b))

#define CONVERT_DIGITS_TO_NUM(p, n) \
    n = *p++ - '0'; \
    while (isdigit(*p)) { \
        n *= 10; \
        n += (*p++ - '0'); \
    }

char var_run_file[BUFSIZ], var_log_file[BUFSIZ];
int num_cpus = 0;
int num_nodes = 0;
int threads_per_core = 0;
uint64_t page_size_in_bytes = 0;
uint64_t huge_page_size_in_bytes = 0;

int min_interval = MIN_INTERVAL;
int max_interval = MAX_INTERVAL;
int htt_percent = DEFAULT_HTT_PERCENT;
int thp_scan_sleep_ms = DEFAULT_THP_SCAN_SLEEP_MS;
int target_utilization  = DEFAULT_UTILIZATION_PERCENT;
int target_memlocality  = DEFAULT_MEMLOCALITY_PERCENT;
int scan_all_processes = 1;
int keep_interleaved_memory = 0;
int use_inactive_file_cache = 1;
int gpu_aware = DEFAULT_GPU_AWARE;
int gpu_min_busy_pct = DEFAULT_GPU_MIN_BUSY_PCT;
int gpu_min_vram_mb = DEFAULT_GPU_MIN_VRAM_MB;
int gpu_fdinfo_discovery_interval = DEFAULT_GPU_FDINFO_DISCOVERY_INTERVAL;
int gpu_migrate_busy_max = DEFAULT_GPU_MIGRATE_BUSY_MAX;
int bind_cooldown_sec = DEFAULT_BIND_COOLDOWN_SEC;
static int startup_only_longopt_seen = 0;

pthread_mutex_t pid_list_mutex;
pthread_mutex_t node_info_mutex;
long sum_MBs_total = 0;
long sum_CPUs_total = 0;
int requested_mbs = 0;
int requested_cpus = 0;
int got_sighup = 0;
int got_sigterm = 0;
int got_sigquit = 0;

int get_daemon_pid(int inited);

void sig_handler(int signum) { 
    switch (signum) {
        case SIGHUP:  got_sighup  = 1; break;
        case SIGTERM: got_sigterm = 1; break;
        case SIGQUIT: got_sigquit = 1; break;
    }
}



FILE *log_fs = NULL;
int log_level = LOG_NOTICE;

void numad_log(int level, const char *fmt, ...) {
    if (level > log_level) {
        return;
        // Logging levels (from sys/syslog.h)
        //     #define LOG_EMERG       0       /* system is unusable */
        //     #define LOG_ALERT       1       /* action must be taken immediately */
        //     #define LOG_CRIT        2       /* critical conditions */
        //     #define LOG_ERR         3       /* error conditions */
        //     #define LOG_WARNING     4       /* warning conditions */
        //     #define LOG_NOTICE      5       /* normal but significant condition */
        //     #define LOG_INFO        6       /* informational */
        //     #define LOG_DEBUG       7       /* debug-level messages */
    }
    char buf[BIG_BUF_SIZE];
    time_t ts = time(NULL);
    strncpy(buf, ctime(&ts), sizeof(buf));
    char *p = &buf[strlen(buf) - 1];
    *p++ = ':';
    *p++ = ' ';
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p, BIG_BUF_SIZE - strlen(buf) , fmt, ap);
    va_end(ap);
    fprintf(log_fs, "%s", buf);
    fflush(log_fs);
}

void init_run_file() {
    uid_t uid = getuid();
    gid_t gid = getgid();
    snprintf(var_run_file, BUFSIZ-1, "%s/%s", VAR_RUN_DIR_ADMIN, VAR_RUN_FILE);
    snprintf(var_log_file, BUFSIZ-1, "%s/%s", VAR_LOG_DIR_ADMIN, VAR_LOG_FILE);
    if (get_daemon_pid(0) == 0 && uid != 0 && gid != 0) {
        //char *homedir = getenv("HOME");
        //snprintf(var_run_file, BUFSIZ-1, "%s/%s", (homedir ? homedir : "/tmp"), VAR_RUN_FILE);
        //snprintf(var_log_file, BUFSIZ-1, "%s/%s", (homedir ? homedir : "/tmp"), VAR_LOG_FILE);
        snprintf(var_run_file, BUFSIZ-1, "%s/%s", "/tmp", VAR_RUN_FILE);
        snprintf(var_log_file, BUFSIZ-1, "%s/%s", "/tmp", VAR_LOG_FILE);
    }
}

void open_log_file() {
    log_fs = fopen(var_log_file, "a");
    if (log_fs == NULL) {
        log_fs = stderr;
        numad_log(LOG_ERR, "Cannot open numad log file (errno: %d) -- using stderr\n", errno);
    } else {
        fchmod(fileno(log_fs), S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP );
    }
}


void close_log_file() {
    if (log_fs != NULL) {
        if (log_fs != stderr) {
            fclose(log_fs);
        }
        log_fs = NULL;
    }
}



#define MSG_BODY_TEXT_SIZE 96

typedef struct msg_body {
    long src_pid;
    long cmd;
    long arg1;
    long arg2;
    char text[MSG_BODY_TEXT_SIZE];
} msg_body_t, *msg_body_p;

typedef struct msg {
    long dst_pid;  // msg mtype is dest PID
    msg_body_t body;
} msg_t, *msg_p;

int msg_qid;

void flush_msg_queue() {
    msg_t msg;
    errno = 0;
    while (msgrcv(msg_qid, &msg, sizeof(msg_body_t), 0, IPC_NOWAIT) >= 0) {
    }
    if ((errno != 0) && (errno != ENOMSG)) {
        numad_log(LOG_WARNING, "msgrcv during flush failed (errno: %d)\n", errno);
    }
}

void init_msg_queue() {
    //key_t msg_key = 0xdeadbeef;
    key_t msg_key = 0xaddaadda;
    int msg_flg = 0660 | IPC_CREAT;
    msg_qid = msgget(msg_key, msg_flg);
    if (msg_qid < 0) {
	numad_log(LOG_CRIT, "msgget failed (errno: %d)\n", errno);
	exit(EXIT_FAILURE);
    }
    flush_msg_queue();
}

void recv_msg(msg_p m) {
    if (msgrcv(msg_qid, m, sizeof(msg_body_t), getpid(), 0) < 0) {
        numad_log(LOG_CRIT, "msgrcv failed\n");
        exit(EXIT_FAILURE);
    }
    // printf("Received: >>%s<< from process %d\n", m->body.text, m->body.src_pid);
}

void send_msg(long dst_pid, long cmd, long arg1, long arg2, char *s) {
    msg_t msg;
    msg.dst_pid = dst_pid;
    msg.body.src_pid = getpid();
    msg.body.cmd = cmd;
    msg.body.arg1 = arg1;
    msg.body.arg2 = arg2;
    int s_len = strlen(s);
    if (s_len >= MSG_BODY_TEXT_SIZE) {
        numad_log(LOG_CRIT, "msgsnd text too big\n");
        exit(EXIT_FAILURE);
    }
    strcpy(msg.body.text, s);
    size_t m_len = sizeof(msg_body_t) - MSG_BODY_TEXT_SIZE + s_len + 1;
    if (msgsnd(msg_qid, &msg, m_len, IPC_NOWAIT) < 0) {
        numad_log(LOG_CRIT, "msgsnd failed\n");
        exit(EXIT_FAILURE);
    }
    // printf("Sent: >>%s<< to process %d\n", msg.body.text, msg.dst_pid);
}



typedef struct id_list {
    // Use CPU_SET(3) <sched.h> bitmasks,
    // but bundle size and pointer together
    // and genericize for both CPU and Node IDs
    cpu_set_t *set_p; 
    size_t bytes;
} id_list_t, *id_list_p;

typedef struct node_data node_data_t, *node_data_p;
typedef struct pid_list pid_list_t, *pid_list_p;
typedef struct process_data process_data_t, *process_data_p;

int all_digits(const struct dirent *dptr);

struct node_data {
    uint64_t node_id;
    uint64_t MBs_total;
    uint64_t MBs_free;
    uint64_t active_threads;
    uint64_t CPUs_total; // scaled * ONE_HUNDRED
    uint64_t CPUs_free;  // scaled * ONE_HUNDRED
    uint64_t magnitude;  // Free MBs * Free CPUs
    uint8_t *distance;
    id_list_p cpu_list_p;
};

struct pid_list {
    long pid;
    struct pid_list* next;
};

extern node_data_p node;
extern int *node_ix_by_id;
extern int max_node_id_seen;
extern int *cpu_to_node_ix;
extern id_list_p reserved_cpu_mask_list_p;
extern pid_list_p include_pid_list;
extern pid_list_p exclude_pid_list;

#define ID_LIST_SET_P(list_p) (list_p->set_p)
#define ID_LIST_BYTES(list_p) (list_p->bytes)

#define INIT_ID_LIST(list_p, num_elements) \
    list_p = malloc(sizeof(id_list_t)); \
    if (list_p == NULL) { numad_log(LOG_CRIT, "INIT_ID_LIST malloc failed\n"); exit(EXIT_FAILURE); } \
    list_p->set_p = CPU_ALLOC(num_elements); \
    if (list_p->set_p == NULL) { numad_log(LOG_CRIT, "CPU_ALLOC failed\n"); exit(EXIT_FAILURE); } \
    list_p->bytes = CPU_ALLOC_SIZE(num_elements);

#define CLEAR_CPU_LIST(list_p) \
    if (list_p == NULL) { \
        INIT_ID_LIST(list_p, num_cpus); \
    } \
    CPU_ZERO_S(list_p->bytes, list_p->set_p)

#define CLEAR_NODE_LIST(list_p) \
    if (list_p == NULL) { \
        INIT_ID_LIST(list_p, num_nodes); \
    } \
    CPU_ZERO_S(list_p->bytes, list_p->set_p)

#define FREE_LIST(list_p) \
    if (list_p != NULL) { \
        if (list_p->set_p != NULL) { CPU_FREE(list_p->set_p); } \
        free(list_p); \
        list_p = NULL; \
    }

#define COPY_LIST(orig_list_p, copy_list_p) \
    memcpy(copy_list_p->set_p, orig_list_p->set_p, orig_list_p->bytes)

#define NUM_IDS_IN_LIST(list_p)     CPU_COUNT_S(list_p->bytes, list_p->set_p)
#define ADD_ID_TO_LIST(k, list_p)  CPU_SET_S(k, list_p->bytes, list_p->set_p)
#define CLR_ID_IN_LIST(k, list_p)  CPU_CLR_S(k, list_p->bytes, list_p->set_p)
#define ID_IS_IN_LIST(k, list_p) CPU_ISSET_S(k, list_p->bytes, list_p->set_p)

#define           EQUAL_LISTS(list_1_p, list_2_p) CPU_EQUAL_S(list_1_p->bytes,                    list_1_p->set_p, list_2_p->set_p)
#define AND_LISTS(and_list_p, list_1_p, list_2_p) CPU_AND_S(and_list_p->bytes, and_list_p->set_p, list_1_p->set_p, list_2_p->set_p)
#define  OR_LISTS( or_list_p, list_1_p, list_2_p)  CPU_OR_S( or_list_p->bytes,  or_list_p->set_p, list_1_p->set_p, list_2_p->set_p)
#define XOR_LISTS(xor_list_p, list_1_p, list_2_p) CPU_XOR_S(xor_list_p->bytes, xor_list_p->set_p, list_1_p->set_p, list_2_p->set_p)

int negate_cpu_list(id_list_p list_p) {
    if (list_p == NULL) {
        numad_log(LOG_CRIT, "Cannot negate a NULL list\n");
        exit(EXIT_FAILURE);
    }
    if (num_cpus < 1) {
        numad_log(LOG_CRIT, "No CPUs to negate in list!\n");
        exit(EXIT_FAILURE);
    }
    for (int ix = 0;  (ix < num_cpus);  ix++) {
        if (ID_IS_IN_LIST(ix, list_p)) {
            CLR_ID_IN_LIST(ix, list_p);
        } else {
            ADD_ID_TO_LIST(ix, list_p);
        }
    }
    return NUM_IDS_IN_LIST(list_p);
}

int add_ids_to_list_from_str(id_list_p list_p, char *s) {
    if (list_p == NULL) {
        numad_log(LOG_CRIT, "Cannot add to NULL list\n");
        exit(EXIT_FAILURE);
    }
    if ((s == NULL) || (strlen(s) == 0)) {
        goto return_list;
    }
    int in_range = 0;
    int next_id = 0;
    for (;;) {
        // skip over non-digits
        while (!isdigit(*s)) {
            if ((*s == '\n') || (*s == '\0')) {
                goto return_list;
            }
            if (*s++ == '-') {
                in_range = 1;
            }
        }
        int id;
        CONVERT_DIGITS_TO_NUM(s, id);
        if (!in_range) {
            next_id = id;
        }
        for (; (next_id <= id); next_id++) {
            ADD_ID_TO_LIST(next_id, list_p);
        }
        in_range = 0;
    }
return_list:
    return NUM_IDS_IN_LIST(list_p);
}

int str_from_id_list(char *str_p, int str_size, id_list_p list_p) {
    char *p = str_p;
    if ((p == NULL) || (str_size < 3)) {
        numad_log(LOG_CRIT, "Bad string for ID listing\n");
        exit(EXIT_FAILURE);
    }
    int n;
    if ((list_p == NULL) || ((n = NUM_IDS_IN_LIST(list_p)) == 0)) {
        goto terminate_string;
    }
    int id_range_start = -1;
    for (int id = 0;  ;  id++) {
        int id_in_list = (ID_IS_IN_LIST(id, list_p) != 0);
        if ((id_in_list) && (id_range_start < 0)) {
            id_range_start = id; // beginning an ID range
        } else if ((!id_in_list) && (id_range_start >= 0)) {
            // convert the range that just ended...
            p += snprintf(p, (str_p + str_size - p - 1), "%d", id_range_start);
            if (id - id_range_start > 1) {
                *p++ = '-';
                p += snprintf(p, (str_p + str_size - p - 1), "%d", (id - 1));
            } 
            *p++ = ',';
            id_range_start = -1; // no longer in a range
            if (n <= 0) { break; } // exit only after finishing a range
        }
        n -= id_in_list;
    }
    p -= 1; // eliminate trailing ','
terminate_string:
    *p = '\0';
    return (p - str_p);
}


static ssize_t read_text_file(const char *path, char *buf, size_t buf_size) {
    if ((buf == NULL) || (buf_size < 2)) {
        errno = EINVAL;
        return -1;
    }
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        return -1;
    }
    ssize_t bytes = read(fd, buf, buf_size - 1);
    close(fd);
    if (bytes < 0) {
        return -1;
    }
    buf[bytes] = '\0';
    return bytes;
}


static int read_first_line(const char *path, char *buf, size_t buf_size) {
    ssize_t bytes = read_text_file(path, buf, buf_size);
    if (bytes < 0) {
        return -1;
    }
    char *p = strchr(buf, '\n');
    if (p != NULL) {
        *p = '\0';
    }
    return 0;
}


static const char *skip_spaces_const(const char *p) {
    while ((p != NULL) && (*p == ' ')) {
        p++;
    }
    return p;
}


static const char *skip_token_const(const char *p) {
    while ((p != NULL) && (*p != '\0') && (*p != ' ')) {
        p++;
    }
    return skip_spaces_const(p);
}


static int parse_u64_token(const char **pp, uint64_t *out) {
    if ((pp == NULL) || (*pp == NULL) || (out == NULL)) {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(*pp, &end, 10);
    if ((errno != 0) || (end == *pp)) {
        return -1;
    }
    *out = (uint64_t)v;
    *pp = skip_spaces_const(end);
    return 0;
}


static int parse_i64_token(const char **pp, int64_t *out) {
    if ((pp == NULL) || (*pp == NULL) || (out == NULL)) {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    long long v = strtoll(*pp, &end, 10);
    if ((errno != 0) || (end == *pp)) {
        return -1;
    }
    *out = (int64_t)v;
    *pp = skip_spaces_const(end);
    return 0;
}


static int parse_proc_stat_header(const char *buf, int *pid,
                                  char *comm, size_t comm_size,
                                  const char **tail) {
    if ((buf == NULL) || (pid == NULL) || (comm == NULL) || (comm_size < 2) || (tail == NULL)) {
        return -1;
    }
    const char *lp = strchr(buf, '(');
    const char *rp = strrchr(buf, ')');
    if ((lp == NULL) || (rp == NULL) || (rp <= lp)) {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    long parsed_pid = strtol(buf, &end, 10);
    if ((errno != 0) || (end == buf) || (parsed_pid < 0)) {
        return -1;
    }
    size_t len = (size_t)(rp - lp - 1);
    if (len >= comm_size) {
        len = comm_size - 1;
    }
    memcpy(comm, lp + 1, len);
    comm[len] = '\0';
    if (rp[1] != ' ') {
        return -1;
    }
    *pid = (int)parsed_pid;
    *tail = rp + 2;
    return 0;
}


static int pid_is_in_list(pid_list_p list_ptr, long pid) {
    while (list_ptr != NULL) {
        if (list_ptr->pid == pid) {
            return 1;
        }
        list_ptr = list_ptr->next;
    }
    return 0;
}


static void rebuild_node_id_index(void) {
    int new_max = -1;
    for (int ix = 0; (ix < num_nodes); ix++) {
        if ((int)node[ix].node_id > new_max) {
            new_max = (int)node[ix].node_id;
        }
    }

    if (new_max != max_node_id_seen) {
        free(node_ix_by_id);
        node_ix_by_id = NULL;
        if (new_max >= 0) {
            node_ix_by_id = malloc((new_max + 1) * sizeof(*node_ix_by_id));
            if (node_ix_by_id == NULL) {
                numad_log(LOG_CRIT, "node_ix_by_id malloc failed\n");
                exit(EXIT_FAILURE);
            }
        }
        max_node_id_seen = new_max;
    }

    if ((node_ix_by_id != NULL) && (max_node_id_seen >= 0)) {
        for (int ix = 0; (ix <= max_node_id_seen); ix++) {
            node_ix_by_id[ix] = -1;
        }
        for (int ix = 0; (ix < num_nodes); ix++) {
            node_ix_by_id[node[ix].node_id] = ix;
        }
    }

    cpu_to_node_ix = realloc(cpu_to_node_ix, num_cpus * sizeof(*cpu_to_node_ix));
    if ((num_cpus > 0) && (cpu_to_node_ix == NULL)) {
        numad_log(LOG_CRIT, "cpu_to_node_ix realloc failed\n");
        exit(EXIT_FAILURE);
    }
    for (int cpu = 0; (cpu < num_cpus); cpu++) {
        cpu_to_node_ix[cpu] = -1;
        for (int node_ix = 0; (node_ix < num_nodes); node_ix++) {
            if (ID_IS_IN_LIST(cpu, node[node_ix].cpu_list_p)) {
                cpu_to_node_ix[cpu] = node_ix;
                break;
            }
        }
    }
}


static inline int node_ix_from_id(int node_id) {
    if ((node_ix_by_id == NULL) || (node_id < 0) || (node_id > max_node_id_seen)) {
        return -1;
    }
    return node_ix_by_id[node_id];
}


static inline int node_id_from_ix(int node_ix) {
    if ((node_ix < 0) || (node_ix >= num_nodes)) {
        return -1;
    }
    return (int)node[node_ix].node_id;
}


static int str_from_node_ix_list_as_node_ids(char *str_p, int str_size, id_list_p list_p) {
    char *p = str_p;
    if ((str_p == NULL) || (str_size < 2)) {
        numad_log(LOG_CRIT, "Bad string for node ID listing\n");
        exit(EXIT_FAILURE);
    }
    *p = '\0';
    if ((list_p == NULL) || (NUM_IDS_IN_LIST(list_p) == 0)) {
        return 0;
    }

    int range_start_id = -1;
    int prev_id = -1;
    int wrote_any = 0;
    for (int node_ix = 0; (node_ix <= num_nodes); node_ix++) {
        int in_list = ((node_ix < num_nodes) && ID_IS_IN_LIST(node_ix, list_p));
        int cur_id = in_list ? node_id_from_ix(node_ix) : -1;

        if (in_list && (range_start_id < 0)) {
            range_start_id = cur_id;
            prev_id = cur_id;
            continue;
        }
        if (in_list && (cur_id == prev_id + 1)) {
            prev_id = cur_id;
            continue;
        }
        if (range_start_id >= 0) {
            if (wrote_any) {
                if (str_size - (p - str_p) <= 1) {
                    break;
                }
                *p++ = ',';
                *p = '\0';
            }
            int written = snprintf(p, str_size - (p - str_p), "%d", range_start_id);
            if ((written < 0) || (written >= str_size - (p - str_p))) {
                break;
            }
            p += written;
            if (prev_id > range_start_id) {
                written = snprintf(p, str_size - (p - str_p), "-%d", prev_id);
                if ((written < 0) || (written >= str_size - (p - str_p))) {
                    break;
                }
                p += written;
            }
            wrote_any = 1;
            range_start_id = -1;
            prev_id = -1;
        }
        if (in_list) {
            range_start_id = cur_id;
            prev_id = cur_id;
        }
    }
    return (int)(p - str_p);
}


static int collect_active_threads_for_pid(int pid, uint64_t num_threads, uint64_t *active_threads_out) {
    if (active_threads_out == NULL) {
        return -1;
    }
    if (num_threads <= 1) {
        *active_threads_out = (num_threads > 0) ? 1 : 0;
        return 0;
    }

    char task_dir[FNAME_SIZE];
    snprintf(task_dir, FNAME_SIZE, "/proc/%d/task", pid);
    struct dirent **namelist = NULL;
    int tasks = scandir(task_dir, &namelist, all_digits, NULL);
    if (tasks < 1) {
        *active_threads_out = 1;
        return -1;
    }

    uint64_t active = 0;
    char stat_buf[BUF_SIZE];
    char fname[FNAME_SIZE];
    for (int ix = 0; (ix < tasks); ix++) {
        snprintf(fname, FNAME_SIZE, "%s/%s/stat", task_dir, namelist[ix]->d_name);
        free(namelist[ix]);
        ssize_t bytes = read_text_file(fname, stat_buf, sizeof(stat_buf));
        if (bytes < 0) {
            continue;
        }
        int tid = 0;
        char comm[PROC_COMM_SIZE];
        const char *tail = NULL;
        if (parse_proc_stat_header(stat_buf, &tid, comm, sizeof(comm), &tail) == 0) {
            if ((tail != NULL) && (*tail == 'R')) {
                active += 1;
            }
        }
    }
    free(namelist);
    *active_threads_out = (active > 0) ? active : 1;
    return 0;
}


node_data_p node = NULL;


int min_node_CPUs_free_id = -1;
int min_node_MBs_free_id = -1;
long min_node_CPUs_free = MAXINT;
long min_node_MBs_free = MAXINT;
long max_node_CPUs_free = 0;
long max_node_MBs_free = 0;
long avg_node_CPUs_free = 0;
long avg_node_MBs_free = 0;
double stddev_node_CPUs_free = 0.0;
double stddev_node_MBs_free = 0.0;

int *node_ix_by_id = NULL;
int max_node_id_seen = -1;
int *cpu_to_node_ix = NULL;



// RING_BUF_SIZE must be a power of two
#define RING_BUF_SIZE 4

#define PROCESS_FLAG_INTERLEAVED   (1 << 0)
#define PROCESS_FLAG_CHECK_THREADS (1 << 1)
#define PROCESS_FLAG_EXPLICIT_PID  (1 << 2)
#define PROCESS_FLAG_EXCLUDED_PID  (1 << 3)
#define PROCESS_FLAG_GPU_ACTIVE    (1 << 4)
#define PROCESS_FLAG_GPU_COMPUTE   (1 << 5)
#define PROCESS_FLAG_GPU_GRAPHICS  (1 << 6)

typedef enum {
    GPU_KIND_NONE = 0,
    GPU_KIND_COMPUTE,
    GPU_KIND_GRAPHICS,
    GPU_KIND_MIXED,
} gpu_kind_t;

typedef enum {
    GPU_BACKEND_AUTO = 0,
    GPU_BACKEND_AMDSMI,
    GPU_BACKEND_FDINFO,
} gpu_backend_t;

typedef enum {
    MIGRATE_AUTO = 0,
    MIGRATE_ALWAYS,
    MIGRATE_NEVER,
} migrate_policy_t;

typedef enum {
    GPU_GRAPHICS_PLACEMENT_AUTO = 0,
    GPU_GRAPHICS_PLACEMENT_PREFER,
    GPU_GRAPHICS_PLACEMENT_STRICT,
} gpu_graphics_placement_t;

typedef enum {
    SCX_MODE_LEGACY = 0,
    SCX_MODE_COOPERATE,
    SCX_MODE_OBSERVE,
} scx_mode_t;

typedef enum {
    SCX_SCHED_NONE = 0,
    SCX_SCHED_BEERLAND,
    SCX_SCHED_P2DQ,
    SCX_SCHED_OTHER,
} scx_sched_t;

typedef struct gpu_device {
    int dev_ix;
    char drm_name[GPU_DRM_NAME_SIZE];
    char pci_bdf[GPU_BDF_SIZE];
    int numa_node_id;
    int numa_node_ix;
    id_list_p local_cpu_list_p;
    int valid;
} gpu_device_t, *gpu_device_p;

gpu_device_p gpu = NULL;
int gpu_count = 0;
uint64_t gpu_topology_time_stamp = 0;
uint64_t gpu_fdinfo_last_full_scan = 0;
gpu_backend_t gpu_backend = GPU_BACKEND_AUTO;
migrate_policy_t gpu_migrate_policy = MIGRATE_AUTO;
gpu_graphics_placement_t gpu_graphics_placement = DEFAULT_GPU_GRAPHICS_PLACEMENT;
scx_mode_t scx_mode = SCX_MODE_COOPERATE;
scx_sched_t scx_sched = SCX_SCHED_NONE;
int scx_enabled = 0;

struct process_data {
    int pid;
    unsigned int flags;
    uint64_t data_time_stamp; // hundredths of seconds
    uint64_t bind_time_stamp;
    uint64_t start_time_ticks;
    uint64_t num_threads;
    uint64_t num_active_threads;
    uint64_t MBs_size;
    uint64_t MBs_used;
    uint64_t cpu_util;
    uint64_t CPUs_used;  // scaled * ONE_HUNDRED
    uint64_t CPUs_used_ring_buf[RING_BUF_SIZE];
    char comm[PROC_COMM_SIZE];
    id_list_p node_list_p;
    uint64_t *process_MBs;
    uint64_t gpu_vram_mb;
    uint32_t gpu_busy_pct;
    uint32_t gpu_sdma_pct;
    uint64_t gpu_engine_busy_ns;
    uint64_t gpu_engine_busy_ns_prev;
    uint64_t last_gpu_ts;
    gpu_kind_t gpu_kind;
    id_list_p gpu_list_p;
    int ring_buf_ix;
};

static int name_starts_with_digit(const struct dirent *dptr);
int get_stat_data_for_pid(int pid, process_data_p out);
int process_hash_lookup(int pid);
int process_hash_update(process_data_p newp);
extern process_data_p process_hash_table;

static int drm_card_and_digits(const struct dirent *dptr) {
    const char *p = dptr->d_name;
    if (strncmp(p, "card", 4) != 0) {
        return 0;
    }
    p += 4;
    if (!isdigit(*p)) {
        return 0;
    }
    while (*p != '\0') {
        if (!isdigit(*p++)) {
            return 0;
        }
    }
    return 1;
}

static int read_int_file(const char *path, int *value) {
    char buf[64];
    if ((value == NULL) || (read_first_line(path, buf, sizeof(buf)) < 0)) {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if ((errno != 0) || (end == buf)) {
        return -1;
    }
    *value = (int)v;
    return 0;
}

static int read_link_basename(const char *path, char *buf, size_t buf_size) {
    if ((buf == NULL) || (buf_size < 2)) {
        return -1;
    }
    char link_buf[PATH_MAX];
    ssize_t n = readlink(path, link_buf, sizeof(link_buf) - 1);
    if (n < 0) {
        return -1;
    }
    link_buf[n] = '\0';
    char *base = strrchr(link_buf, '/');
    base = (base != NULL) ? (base + 1) : link_buf;
    strncpy(buf, base, buf_size - 1);
    buf[buf_size - 1] = '\0';
    return 0;
}

static int gpu_index_from_pci_bdf(const char *bdf) {
    if ((bdf == NULL) || (*bdf == '\0')) {
        return -1;
    }
    for (int ix = 0; ix < gpu_count; ix++) {
        if (gpu[ix].valid && (strcmp(gpu[ix].pci_bdf, bdf) == 0)) {
            return ix;
        }
    }
    return -1;
}

static uint64_t fdinfo_mem_to_mb(const char *line) {
    const char *p = strchr(line, ':');
    if (p == NULL) {
        return 0;
    }
    p = skip_spaces_const(p + 1);
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(p, &end, 10);
    if ((errno != 0) || (end == p)) {
        return 0;
    }
    end = (char *)skip_spaces_const(end);
    if ((strncmp(end, "MiB", 3) == 0) || (strncmp(end, "MB", 2) == 0)) {
        return (uint64_t)value;
    }
    if ((strncmp(end, "KiB", 3) == 0) || (strncmp(end, "kB", 2) == 0)) {
        return ((uint64_t)value + 1023) / 1024;
    }
    if ((strncmp(end, "bytes", 5) == 0) || (strncmp(end, "B", 1) == 0)) {
        return (uint64_t)value / MEGABYTE;
    }
    return (uint64_t)value;
}

static uint64_t fdinfo_engine_ns(const char *line) {
    const char *p = strchr(line, ':');
    if (p == NULL) {
        return 0;
    }
    p = skip_spaces_const(p + 1);
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(p, &end, 10);
    if ((errno != 0) || (end == p)) {
        return 0;
    }
    return (uint64_t)value;
}

typedef struct fdinfo_client_acc {
    int used;
    char pdev[GPU_BDF_SIZE];
    uint64_t client_id;
    uint64_t engine_ns;
    uint64_t vram_mb;
    int saw_compute;
    int saw_graphics;
} fdinfo_client_acc_t;

static int find_or_add_fdinfo_client(fdinfo_client_acc_t *acc, int cap,
                                     const char *pdev, uint64_t client_id) {
    if ((acc == NULL) || (cap <= 0) || (pdev == NULL) || (*pdev == '\0')) {
        return -1;
    }
    for (int ix = 0; ix < cap; ix++) {
        if (acc[ix].used
            && (acc[ix].client_id == client_id)
            && (strcmp(acc[ix].pdev, pdev) == 0)) {
            return ix;
        }
    }
    for (int ix = 0; ix < cap; ix++) {
        if (!acc[ix].used) {
            acc[ix].used = 1;
            acc[ix].client_id = client_id;
            strncpy(acc[ix].pdev, pdev, sizeof(acc[ix].pdev) - 1);
            acc[ix].pdev[sizeof(acc[ix].pdev) - 1] = '\0';
            return ix;
        }
    }
    return -1;
}

static int discover_amdgpu_devices_from_sysfs(void) {
    for (int ix = 0; ix < gpu_count; ix++) {
        FREE_LIST(gpu[ix].local_cpu_list_p);
    }
    free(gpu);
    gpu = NULL;
    gpu_count = 0;

    struct dirent **namelist = NULL;
    int cards = scandir("/sys/class/drm", &namelist, drm_card_and_digits, versionsort);
    if (cards < 0) {
        return 0;
    }

    gpu = calloc(cards, sizeof(*gpu));
    if ((cards > 0) && (gpu == NULL)) {
        numad_log(LOG_CRIT, "gpu topology calloc failed\n");
        exit(EXIT_FAILURE);
    }

    char path[FNAME_SIZE];
    char buf[BUF_SIZE];
    for (int ix = 0; ix < cards; ix++) {
        char *card = namelist[ix]->d_name;
        snprintf(path, sizeof(path), "/sys/class/drm/%s/device/vendor", card);
        if ((read_first_line(path, buf, sizeof(buf)) < 0) || (strcmp(buf, "0x1002") != 0)) {
            free(namelist[ix]);
            continue;
        }

        gpu_device_p g = &gpu[gpu_count];
        memset(g, 0, sizeof(*g));
        g->dev_ix = gpu_count;
        strncpy(g->drm_name, card, sizeof(g->drm_name) - 1);
        snprintf(path, sizeof(path), "/sys/class/drm/%s/device", card);
        read_link_basename(path, g->pci_bdf, sizeof(g->pci_bdf));

        snprintf(path, sizeof(path), "/sys/class/drm/%s/device/numa_node", card);
        if (read_int_file(path, &g->numa_node_id) == 0) {
            g->numa_node_ix = node_ix_from_id(g->numa_node_id);
        } else {
            g->numa_node_id = -1;
            g->numa_node_ix = -1;
        }

        snprintf(path, sizeof(path), "/sys/class/drm/%s/device/local_cpulist", card);
        if (read_first_line(path, buf, sizeof(buf)) == 0) {
            CLEAR_CPU_LIST(g->local_cpu_list_p);
            add_ids_to_list_from_str(g->local_cpu_list_p, buf);
        }

        g->valid = 1;
        gpu_count += 1;
        free(namelist[ix]);
    }
    free(namelist);
    return gpu_count;
}

static void update_gpu_topology_if_needed(uint64_t now) {
    if (!gpu_aware) {
        return;
    }
    if ((gpu_topology_time_stamp != 0) && (now < gpu_topology_time_stamp + (60 * ONE_HUNDRED))) {
        return;
    }
    gpu_topology_time_stamp = now;
    discover_amdgpu_devices_from_sysfs();
}

static int collect_amdgpu_fdinfo_for_pid(int pid, process_data_p sample, uint64_t now) {
    char path[FNAME_SIZE];
    snprintf(path, sizeof(path), "/proc/%d/fdinfo", pid);
    struct dirent **namelist = NULL;
    int files = scandir(path, &namelist, all_digits, NULL);
    if (files < 1) {
        return -1;
    }

    int found = 0;
    int saw_compute = 0;
    int saw_graphics = 0;
    uint64_t total_engine_ns = 0;
    uint64_t total_vram_mb = 0;
    char file_buf[BIG_BUF_SIZE];
    fdinfo_client_acc_t *clients = calloc(files, sizeof(*clients));
    if ((files > 0) && (clients == NULL)) {
        numad_log(LOG_CRIT, "fdinfo client accumulator calloc failed\n");
        exit(EXIT_FAILURE);
    }

    for (int ix = 0; ix < files; ix++) {
        snprintf(path, sizeof(path), "/proc/%d/fdinfo/%s", pid, namelist[ix]->d_name);
        free(namelist[ix]);
        if (read_text_file(path, file_buf, sizeof(file_buf)) < 0) {
            continue;
        }

        int is_amdgpu = 0;
        int fd_saw_compute = 0;
        int fd_saw_graphics = 0;
        char pdev[GPU_BDF_SIZE] = {0};
        uint64_t engine_ns = 0;
        uint64_t vram_mb = 0;
        uint64_t client_id = 0;
        int client_id_valid = 0;

        char *save = NULL;
        for (char *line = strtok_r(file_buf, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
            if (strncmp(line, "drm-driver:", 11) == 0) {
                if (strstr(line, "amdgpu") != NULL) {
                    is_amdgpu = 1;
                }
            } else if (strncmp(line, "drm-pdev:", 9) == 0) {
                const char *v = skip_spaces_const(line + 9);
                strncpy(pdev, v, sizeof(pdev) - 1);
            } else if (strncmp(line, "drm-client-id:", 14) == 0) {
                const char *v = skip_spaces_const(line + 14);
                errno = 0;
                char *end = NULL;
                unsigned long long parsed = strtoull(v, &end, 10);
                if ((errno == 0) && (end != v)) {
                    client_id = (uint64_t)parsed;
                    client_id_valid = 1;
                }
            } else if (strncmp(line, "drm-memory-vram:", 16) == 0) {
                vram_mb += fdinfo_mem_to_mb(line);
            } else if (strncmp(line, "drm-engine-", 11) == 0) {
                engine_ns += fdinfo_engine_ns(line);
                if (strstr(line, "compute") != NULL) {
                    fd_saw_compute = 1;
                } else {
                    fd_saw_graphics = 1;
                }
            }
        }

        if (!is_amdgpu) {
            continue;
        }
        found = 1;
        if ((pdev[0] != '\0') && (gpu_count > 0)) {
            int gpu_ix = gpu_index_from_pci_bdf(pdev);
            if (gpu_ix >= 0) {
                if (sample->gpu_list_p == NULL) {
                    INIT_ID_LIST(sample->gpu_list_p, MAX(gpu_count, 1));
                    CPU_ZERO_S(sample->gpu_list_p->bytes, sample->gpu_list_p->set_p);
                }
                ADD_ID_TO_LIST(gpu_ix, sample->gpu_list_p);
            }
        }

        if (client_id_valid && (pdev[0] != '\0')) {
            int client_ix = find_or_add_fdinfo_client(clients, files, pdev, client_id);
            if (client_ix >= 0) {
                clients[client_ix].engine_ns = MAX(clients[client_ix].engine_ns, engine_ns);
                clients[client_ix].vram_mb = MAX(clients[client_ix].vram_mb, vram_mb);
                clients[client_ix].saw_compute |= fd_saw_compute;
                clients[client_ix].saw_graphics |= fd_saw_graphics;
                continue;
            }
        }

        total_engine_ns += engine_ns;
        total_vram_mb += vram_mb;
        saw_compute |= fd_saw_compute;
        saw_graphics |= fd_saw_graphics;
    }
    free(namelist);

    for (int ix = 0; ix < files; ix++) {
        if (!clients[ix].used) {
            continue;
        }
        total_engine_ns += clients[ix].engine_ns;
        total_vram_mb += clients[ix].vram_mb;
        saw_compute |= clients[ix].saw_compute;
        saw_graphics |= clients[ix].saw_graphics;
    }
    free(clients);

    if (!found) {
        return -1;
    }

    sample->flags |= PROCESS_FLAG_GPU_ACTIVE;
    if (saw_compute) {
        sample->flags |= PROCESS_FLAG_GPU_COMPUTE;
    }
    if (saw_graphics) {
        sample->flags |= PROCESS_FLAG_GPU_GRAPHICS;
    }
    sample->gpu_kind = saw_compute && saw_graphics ? GPU_KIND_MIXED
                     : saw_compute ? GPU_KIND_COMPUTE
                     : GPU_KIND_GRAPHICS;
    sample->gpu_engine_busy_ns = total_engine_ns;
    sample->gpu_vram_mb = total_vram_mb;
    sample->last_gpu_ts = now;

    int hash_ix = process_hash_lookup(pid);
    if (hash_ix >= 0) {
        process_data_p prev = &process_hash_table[hash_ix];
        if ((prev->last_gpu_ts > 0) && (now > prev->last_gpu_ts)
            && (total_engine_ns >= prev->gpu_engine_busy_ns)) {
            uint64_t delta_engine = total_engine_ns - prev->gpu_engine_busy_ns;
            uint64_t delta_ns = (now - prev->last_gpu_ts) * 10000000ULL;
            if (delta_ns > 0) {
                uint64_t pct = (delta_engine * 100) / delta_ns;
                sample->gpu_busy_pct = (pct > 100) ? 100 : (uint32_t)pct;
            }
        }
    }
    if ((sample->gpu_busy_pct == 0) && (sample->gpu_engine_busy_ns > 0)) {
        sample->gpu_busy_pct = 1;
    }
    return 0;
}

static void update_gpu_processes_fdinfo(uint64_t now) {
    if (!gpu_aware) {
        return;
    }
    if ((gpu_fdinfo_last_full_scan != 0)
        && (now < gpu_fdinfo_last_full_scan + (gpu_fdinfo_discovery_interval * ONE_HUNDRED))) {
        return;
    }
    gpu_fdinfo_last_full_scan = now;

    struct dirent **namelist = NULL;
    int files = scandir("/proc", &namelist, name_starts_with_digit, NULL);
    if (files < 0) {
        return;
    }

    for (int ix = 0; ix < files; ix++) {
        int pid = atoi(namelist[ix]->d_name);
        free(namelist[ix]);

        pthread_mutex_lock(&pid_list_mutex);
        int excluded = pid_is_in_list(exclude_pid_list, pid);
        int explicit_pid = pid_is_in_list(include_pid_list, pid);
        pthread_mutex_unlock(&pid_list_mutex);
        if (excluded) {
            continue;
        }

        process_data_t sample = {0};
        if (get_stat_data_for_pid(pid, &sample) < 0) {
            continue;
        }
        if (collect_amdgpu_fdinfo_for_pid(pid, &sample, now) == 0) {
            if (explicit_pid) {
                sample.flags |= PROCESS_FLAG_EXPLICIT_PID;
            }
            sample.data_time_stamp = now;
            process_hash_update(&sample);
        }
        FREE_LIST(sample.node_list_p);
        FREE_LIST(sample.gpu_list_p);
    }
    free(namelist);
}

static int gpu_backend_wants_fdinfo(void) {
    return ((gpu_backend == GPU_BACKEND_AUTO) || (gpu_backend == GPU_BACKEND_FDINFO));
}

static void warn_gpu_backend_amdsmi_fallback(void) {
    static int warned = 0;

    if (!warned) {
        numad_log(LOG_WARNING,
                  "--gpu-backend=amdsmi requested, but the AMD SMI process collector is not implemented yet; falling back to fdinfo\n");
        warned = 1;
    }
}

static void update_gpu_processes(uint64_t now) {
    if (!gpu_aware) {
        return;
    }
    update_gpu_topology_if_needed(now);
    if (gpu_backend == GPU_BACKEND_AMDSMI) {
        warn_gpu_backend_amdsmi_fallback();
        update_gpu_processes_fdinfo(now);
        return;
    }
    if (gpu_backend_wants_fdinfo()) {
        update_gpu_processes_fdinfo(now);
    }
}

static int gpu_sample_is_fresh(const process_data_p p, uint64_t now) {
    uint64_t ttl = (uint64_t)gpu_fdinfo_discovery_interval * ONE_HUNDRED * 2;

    if ((p == NULL) || (p->last_gpu_ts == 0) || (now < p->last_gpu_ts)) {
        return 0;
    }
    if (ttl == 0) {
        ttl = ONE_HUNDRED;
    }
    return ((now - p->last_gpu_ts) <= ttl);
}

static void clear_stale_gpu_state(process_data_p p) {
    if (p == NULL) {
        return;
    }

    p->flags &= ~(PROCESS_FLAG_GPU_ACTIVE |
                  PROCESS_FLAG_GPU_COMPUTE |
                  PROCESS_FLAG_GPU_GRAPHICS);
    p->gpu_vram_mb = 0;
    p->gpu_busy_pct = 0;
    p->gpu_sdma_pct = 0;
    p->gpu_engine_busy_ns_prev = p->gpu_engine_busy_ns;
    p->gpu_engine_busy_ns = 0;
    p->last_gpu_ts = 0;
    p->gpu_kind = GPU_KIND_NONE;
    if (p->gpu_list_p != NULL) {
        CPU_ZERO_S(p->gpu_list_p->bytes, p->gpu_list_p->set_p);
    }
}

static const char *process_comm_name(const process_data_p p) {
    return ((p != NULL) && (p->comm[0] != '\0')) ? p->comm : "(unknown)";
}

static const char *gpu_kind_str(gpu_kind_t kind) {
    switch (kind) {
    case GPU_KIND_COMPUTE:
        return "compute";
    case GPU_KIND_GRAPHICS:
        return "graphics";
    case GPU_KIND_MIXED:
        return "mixed";
    case GPU_KIND_NONE:
    default:
        return "none";
    }
}

static const char *gpu_graphics_placement_str(gpu_graphics_placement_t placement) {
    switch (placement) {
    case GPU_GRAPHICS_PLACEMENT_PREFER:
        return "prefer";
    case GPU_GRAPHICS_PLACEMENT_STRICT:
        return "strict";
    case GPU_GRAPHICS_PLACEMENT_AUTO:
    default:
        return "auto";
    }
}

static int process_has_graphics_gpu_context(const process_data_p p) {
    return ((p != NULL)
            && (p->flags & PROCESS_FLAG_GPU_ACTIVE)
            && (((p->flags & PROCESS_FLAG_GPU_GRAPHICS) != 0)
                || (p->gpu_kind == GPU_KIND_GRAPHICS)
                || (p->gpu_kind == GPU_KIND_MIXED)));
}

static int should_migrate_process_memory(const process_data_p p, char *reason_buf, size_t reason_buf_size) {
#define SET_MIGRATE_REASON(fmt, ...) \
    do { \
        if ((reason_buf != NULL) && (reason_buf_size > 0)) { \
            snprintf(reason_buf, reason_buf_size, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

    if (gpu_migrate_policy == MIGRATE_NEVER) {
        SET_MIGRATE_REASON("policy=never");
        return 0;
    }
    if (gpu_migrate_policy == MIGRATE_ALWAYS) {
        SET_MIGRATE_REASON("policy=always");
        return 1;
    }
    if ((p == NULL) || !(p->flags & PROCESS_FLAG_GPU_ACTIVE)) {
        SET_MIGRATE_REASON("CPU-only or non-GPU workload");
        return 1;
    }
    if (p->gpu_kind == GPU_KIND_GRAPHICS) {
        SET_MIGRATE_REASON("graphics GPU workload under --gpu-migrate=auto");
        return 0;
    }
    if (p->gpu_busy_pct > (uint32_t)gpu_migrate_busy_max) {
        SET_MIGRATE_REASON("GPU busy %u%% exceeds --gpu-migrate-busy-max=%d", p->gpu_busy_pct, gpu_migrate_busy_max);
        return 0;
    }
    if ((p->gpu_list_p != NULL) && (NUM_IDS_IN_LIST(p->gpu_list_p) > 1)) {
        SET_MIGRATE_REASON("multiple active GPU NUMA domains under --gpu-migrate=auto");
        return 0;
    }
    SET_MIGRATE_REASON("eligible GPU workload under --gpu-migrate=auto");
    return 1;

#undef SET_MIGRATE_REASON
}

static void refresh_scx_state(void) {
    char buf[128];

    scx_enabled = 0;
    scx_sched = SCX_SCHED_NONE;

    if (read_first_line("/sys/kernel/sched_ext/state", buf, sizeof(buf)) < 0) {
        return;
    }
    if (strncmp(buf, "enabled", 7) != 0) {
        return;
    }

    scx_enabled = 1;
    if (read_first_line("/sys/kernel/sched_ext/root/ops", buf, sizeof(buf)) < 0) {
        scx_sched = SCX_SCHED_OTHER;
        return;
    }
    if (strstr(buf, "beerland") != NULL) {
        scx_sched = SCX_SCHED_BEERLAND;
    } else if (strstr(buf, "p2dq") != NULL) {
        scx_sched = SCX_SCHED_P2DQ;
    } else {
        scx_sched = SCX_SCHED_OTHER;
    }
}

static inline uint64_t abs_u64_diff(uint64_t a, uint64_t b) {
    return (a > b) ? (a - b) : (b - a);
}

static int process_is_manageable_now(const process_data_p p, uint64_t now) {
    if ((p == NULL) || (p->pid <= 0)) {
        return 0;
    }
    if (p->flags & PROCESS_FLAG_EXCLUDED_PID) {
        return 0;
    }
    if (p->flags & PROCESS_FLAG_EXPLICIT_PID) {
        return 1;
    }
    if (gpu_sample_is_fresh(p, now)
        && (p->flags & PROCESS_FLAG_GPU_ACTIVE)
        && ((p->gpu_busy_pct >= (uint32_t)gpu_min_busy_pct)
            || (p->gpu_vram_mb >= (uint64_t)gpu_min_vram_mb)
            || ((p->gpu_list_p != NULL) && (NUM_IDS_IN_LIST(p->gpu_list_p) > 0)))) {
        return 1;
    }
    return ((p->CPUs_used > CPU_THRESHOLD) && (p->MBs_used > MEMORY_THRESHOLD));
}

static uint64_t process_effective_magnitude_now(const process_data_p p, uint64_t now) {
    uint64_t cpu = MAX(p->CPUs_used, 1);
    uint64_t mem = MAX(p->MBs_used, 1);
    if ((p != NULL) && gpu_sample_is_fresh(p, now) && (p->flags & PROCESS_FLAG_GPU_ACTIVE)) {
        mem = MAX(mem, p->gpu_vram_mb);
        cpu = MAX(cpu, (uint64_t)p->gpu_busy_pct);
    }
    return cpu * mem;
}



// Hash table size must always be a power of two
#define MIN_PROCESS_HASH_TABLE_SIZE 32
#define PROCESS_HASH_INSERT_RETRY_ATTEMPTS 3
int process_hash_table_size = 0;
int process_hash_table_used = 0;
int process_hash_collisions = 0;
process_data_p process_hash_table = NULL;

int process_hash_ix(int pid) {
    unsigned ix = pid;
    ix *= 717;
    ix >>= 8;
    ix &= (process_hash_table_size - 1);
    return ix;
}

int process_hash_remove(int pid);
void process_hash_table_expand();

int process_hash_lookup(int pid) {
    int ix = process_hash_ix(pid);
    int starting_ix = ix;
    while (process_hash_table[ix].pid) {
        // Assumes table with some blank entries...
        if (pid == process_hash_table[ix].pid) {
            return ix;  // found it
        }
        ix += 1;
        ix &= (process_hash_table_size - 1);
        if (ix == starting_ix) {
            // Table full and pid not found.
            // This "should never happen"...
            break;
        }
    }
    return -1;
}

static int process_hash_insert_nolog(int pid) {
    // This reserves the hash table slot, but initializes only the pid field
    int ix = process_hash_ix(pid);
    int starting_ix = ix;
    while (process_hash_table[ix].pid) {
        if (pid == process_hash_table[ix].pid) {
            return ix;  // found it
        }
        process_hash_collisions += 1;
        ix += 1;
        ix &= (process_hash_table_size - 1);
        if (ix == starting_ix) {
            return -1;
        }
    }
    process_hash_table[ix].pid = pid;
    process_hash_table_used += 1;
    return ix;
}


int process_hash_insert(int pid) {
    int ix = process_hash_insert_nolog(pid);
    if (ix < 0) {
        numad_log(LOG_ERR, "Process hash table is full\n");
    }
    return ix;
}


static int process_hash_insert_with_retry(int pid) {
    if ((process_hash_table_size <= 0) || (process_hash_table == NULL)) {
        process_hash_table_expand();
    }

    for (int attempt = 0;  (attempt < PROCESS_HASH_INSERT_RETRY_ATTEMPTS);  attempt++) {
        int ix = process_hash_insert_nolog(pid);
        if (ix >= 0) {
            return ix;
        }
        if (attempt + 1 < PROCESS_HASH_INSERT_RETRY_ATTEMPTS) {
            process_hash_table_expand();
        }
    }

    numad_log(LOG_ERR,
              "Process hash table insert failed for PID %d even after expand\n",
              pid);
    return -1;
}

int process_hash_update(process_data_p newp) {
    // This updates hash table stats for processes we are monitoring. Only the
    // scalar resource consumption stats need to be updated here.
    int new_hash_table_entry = 1;
    int ix = process_hash_insert_with_retry(newp->pid);
    if (ix >= 0) {
        process_data_p p = &process_hash_table[ix];
        if ((p->data_time_stamp > 0) && (p->start_time_ticks > 0)
            && (newp->start_time_ticks > 0)
            && (p->start_time_ticks != newp->start_time_ticks)) {
            process_hash_remove(p->pid);
            ix = process_hash_insert_with_retry(newp->pid);
            if (ix < 0) {
                return 0;
            }
            p = &process_hash_table[ix];
            memset(p, 0, sizeof(*p));
            p->pid = newp->pid;
        }
        if (p->data_time_stamp && (newp->data_time_stamp > p->data_time_stamp)) {
            new_hash_table_entry = 0;
            p->ring_buf_ix += 1;
            p->ring_buf_ix &= (RING_BUF_SIZE - 1);
            uint64_t cpu_util_diff = newp->cpu_util  - p->cpu_util;
            uint64_t  time_diff = newp->data_time_stamp - p->data_time_stamp;
            p->CPUs_used_ring_buf[p->ring_buf_ix] = 100 * (cpu_util_diff) / time_diff;
            // Use largest CPU utilization currently in ring buffer
            uint64_t max_CPUs_used = p->CPUs_used_ring_buf[0];
            for (int ix = 1;  (ix < RING_BUF_SIZE);  ix++) {
                if (max_CPUs_used < p->CPUs_used_ring_buf[ix]) {
                    max_CPUs_used = p->CPUs_used_ring_buf[ix];
                }
            }
            p->CPUs_used = max_CPUs_used;
        } else if (p->data_time_stamp) {
            new_hash_table_entry = 0;
            if (newp->data_time_stamp < p->data_time_stamp) {
                numad_log(LOG_WARNING,
                          "Ignoring non-monotonic process timestamp for PID %d (%ld -> %ld)\n",
                          newp->pid, p->data_time_stamp, newp->data_time_stamp);
            }
        } else {
            memset(p->CPUs_used_ring_buf, 0, sizeof(p->CPUs_used_ring_buf));
            p->CPUs_used = 0;
        }
        if (strcmp(p->comm, newp->comm) != 0) {
            strncpy(p->comm, newp->comm, sizeof(p->comm) - 1);
            p->comm[sizeof(p->comm) - 1] = '\0';
        }
        int keep_gpu_state = ((newp->flags & PROCESS_FLAG_GPU_ACTIVE) == 0)
                           && gpu_sample_is_fresh(p, MAX(newp->data_time_stamp, p->data_time_stamp));
        unsigned int preserved_flags = p->flags & (PROCESS_FLAG_INTERLEAVED |
                                                   PROCESS_FLAG_EXPLICIT_PID |
                                                   PROCESS_FLAG_EXCLUDED_PID);
        if (keep_gpu_state) {
            preserved_flags |= p->flags & (PROCESS_FLAG_GPU_ACTIVE |
                                           PROCESS_FLAG_GPU_COMPUTE |
                                           PROCESS_FLAG_GPU_GRAPHICS);
        }
        p->flags = preserved_flags | (newp->flags & ~PROCESS_FLAG_INTERLEAVED);
        p->start_time_ticks = newp->start_time_ticks;
        p->MBs_size = newp->MBs_size;
        p->MBs_used = newp->MBs_used;
        p->cpu_util = newp->cpu_util;
        p->num_threads = newp->num_threads;
        p->num_active_threads = newp->num_active_threads;
        if (p->data_time_stamp == 0) {
            p->data_time_stamp = newp->data_time_stamp;
        } else if (newp->data_time_stamp > p->data_time_stamp) {
            p->data_time_stamp = newp->data_time_stamp;
        }
        if (newp->flags & PROCESS_FLAG_GPU_ACTIVE) {
            p->gpu_vram_mb = newp->gpu_vram_mb;
            p->gpu_busy_pct = newp->gpu_busy_pct;
            p->gpu_sdma_pct = newp->gpu_sdma_pct;
            p->gpu_engine_busy_ns = newp->gpu_engine_busy_ns;
            p->gpu_engine_busy_ns_prev = newp->gpu_engine_busy_ns_prev;
            p->last_gpu_ts = newp->last_gpu_ts;
            p->gpu_kind = newp->gpu_kind;
        } else if (!keep_gpu_state) {
            clear_stale_gpu_state(p);
        }
        CLEAR_NODE_LIST(p->node_list_p);
        COPY_LIST(newp->node_list_p, p->node_list_p);
        if ((newp->flags & PROCESS_FLAG_GPU_ACTIVE) && (newp->gpu_list_p != NULL)) {
            if (p->gpu_list_p == NULL) {
                INIT_ID_LIST(p->gpu_list_p, MAX(gpu_count, 1));
            }
            CPU_ZERO_S(p->gpu_list_p->bytes, p->gpu_list_p->set_p);
            memcpy(p->gpu_list_p->set_p, newp->gpu_list_p->set_p,
                   MIN(p->gpu_list_p->bytes, newp->gpu_list_p->bytes));
        } else if ((newp->flags & PROCESS_FLAG_GPU_ACTIVE) && (p->gpu_list_p != NULL)) {
            CPU_ZERO_S(p->gpu_list_p->bytes, p->gpu_list_p->set_p);
        }
    }
    return new_hash_table_entry;
}

static void apply_reserved_cpu_mask(id_list_p mask) {
    if ((mask != NULL) && (reserved_cpu_mask_list_p != NULL)) {
        AND_LISTS(mask, mask, reserved_cpu_mask_list_p);
    }
}

static id_list_p build_target_cpu_mask(const process_data_p p, id_list_p out) {
    if ((p == NULL) || (p->node_list_p == NULL)) {
        numad_log(LOG_CRIT, "Cannot build CPU mask for invalid process data\n");
        exit(EXIT_FAILURE);
    }

    CLEAR_CPU_LIST(out);
    for (int node_ix = 0; node_ix < num_nodes; node_ix++) {
        if (ID_IS_IN_LIST(node_ix, p->node_list_p)) {
            OR_LISTS(out, out, node[node_ix].cpu_list_p);
        }
    }

    if ((p->flags & PROCESS_FLAG_GPU_ACTIVE) && (p->gpu_list_p != NULL) && (gpu_count > 0)) {
        static id_list_p gpu_local_cpu_union_p = NULL;
        static id_list_p narrowed_p = NULL;

        CLEAR_CPU_LIST(gpu_local_cpu_union_p);
        for (int g = 0; g < gpu_count; g++) {
            if (ID_IS_IN_LIST(g, p->gpu_list_p) && (gpu[g].local_cpu_list_p != NULL)) {
                OR_LISTS(gpu_local_cpu_union_p, gpu_local_cpu_union_p, gpu[g].local_cpu_list_p);
            }
        }
        if (NUM_IDS_IN_LIST(gpu_local_cpu_union_p) > 0) {
            CLEAR_CPU_LIST(narrowed_p);
            AND_LISTS(narrowed_p, out, gpu_local_cpu_union_p);
            if (NUM_IDS_IN_LIST(narrowed_p) > 0) {
                COPY_LIST(narrowed_p, out);
            }
        }
    }

    apply_reserved_cpu_mask(out);

    if (NUM_IDS_IN_LIST(out) == 0) {
        CLEAR_CPU_LIST(out);
        for (int node_ix = 0; node_ix < num_nodes; node_ix++) {
            if (ID_IS_IN_LIST(node_ix, p->node_list_p)) {
                OR_LISTS(out, out, node[node_ix].cpu_list_p);
            }
        }
        apply_reserved_cpu_mask(out);
    }

    return out;
}

void process_hash_clear_all_bind_time_stamps() {
    for (int ix = 0;  (ix < process_hash_table_size);  ix++) {
        process_hash_table[ix].bind_time_stamp = 0;
    }
}

int process_hash_rehash(int old_ix) {
    // Given the index of a table entry that would otherwise be orphaned by
    // process_hash_remove(), reinsert into table using PID from existing record.
    process_data_p op = &process_hash_table[old_ix];
    int new_ix = process_hash_insert_nolog(op->pid);
    if (new_ix >= 0) {
        // Copy old slot to new slot, and zero old slot
        process_data_p np = &process_hash_table[new_ix];
        memcpy(np, op, sizeof(process_data_t));
        memset(op,  0, sizeof(process_data_t));
        process_hash_table_used -= 1;
    }
    return new_ix;
}

int process_hash_remove(int pid) {
    int ix = process_hash_lookup(pid);
    if (ix >= 0) {
        // remove the target
        process_data_p dp = &process_hash_table[ix];
        if (dp->process_MBs) { free(dp->process_MBs); }
        FREE_LIST(dp->node_list_p);
        FREE_LIST(dp->gpu_list_p);
        memset(dp, 0, sizeof(process_data_t));
        process_hash_table_used -= 1;
        // bubble up the collision chain and rehash if neeeded
        for (;;) {
            ix += 1;
            ix &= (process_hash_table_size - 1);
            if ((pid = process_hash_table[ix].pid) <= 0) {
                break;
            }
            if (process_hash_lookup(pid) < 0) {
                if (process_hash_rehash(ix) < 0) {
                    numad_log(LOG_ERR, "rehash fail\n");
                }
            }
        }
    }
    return ix;
}

void process_hash_table_expand() {
    // Save old table size and address
    int old_size = process_hash_table_size;
    process_data_p old_table = process_hash_table;
    // Double size of table and allocate new space
    if (old_size > 0) {
        process_hash_table_size *= 2;
    } else {
        process_hash_table_size = MIN_PROCESS_HASH_TABLE_SIZE;
    }
    numad_log(LOG_DEBUG, "Expanding hash table size: %d\n", process_hash_table_size);
    process_hash_table = malloc(process_hash_table_size * sizeof(process_data_t));
    if (process_hash_table == NULL) {
        numad_log(LOG_CRIT, "hash table malloc failed\n");
        exit(EXIT_FAILURE);
    }
    // Clear the new table, and copy valid entries from old table
    memset(process_hash_table, 0, process_hash_table_size * sizeof(process_data_t));
    process_hash_table_used = 0;
    for (int ix = 0;  (ix < old_size);  ix++) {
        process_data_p p = &old_table[ix];
        if (p->pid) {
            int new_table_ix = process_hash_insert_nolog(p->pid);
            if (new_table_ix < 0) {
                numad_log(LOG_CRIT,
                          "Process hash table reinsert failed during expand for PID %d\n",
                          p->pid);
                exit(EXIT_FAILURE);
            }
            memcpy(&process_hash_table[new_table_ix], p, sizeof(process_data_t));
        }
    }
    if (old_table != NULL) {
        free(old_table);
    }
}


static void process_hash_table_ensure_free_slots(int min_free_slots) {
    if (min_free_slots < 1) {
        min_free_slots = 1;
    }
    while ((process_hash_table_size <= 0)
           || ((process_hash_table_size - process_hash_table_used) < min_free_slots)) {
        process_hash_table_expand();
    }
}


void process_hash_table_cleanup(uint64_t update_time) {
    for (int ix = 0;  (ix < process_hash_table_size);  ix++) {
        process_data_p p = &process_hash_table[ix];
        if (p->pid) {
            if (p->data_time_stamp < update_time) {
                // Mark as old, and zero CPU utilization
                p->data_time_stamp = 0;
                p->CPUs_used = 0;
                if (!gpu_sample_is_fresh(p, update_time)) {
                    clear_stale_gpu_state(p);
                }
                // Check for dead pids and remove them...
                if ((kill(p->pid, 0) == -1) && (errno == ESRCH)) {
                    // Seems dead.  Forget this pid
                    process_hash_remove(p->pid);
                }
            }
        }
    }
    // Keep hash table approximately half empty
    if ((process_hash_table_used * 7) / 4 > process_hash_table_size) {
        process_hash_table_expand();
    }
}


void show_processes(int processes) {
    fprintf(log_fs, "\n");
    fprintf(log_fs, "Processes: looked at %d processes.\n", processes);
    for (int ix = 0;  (ix < process_hash_table_size);  ix++) {
        process_data_p p = &process_hash_table[ix];
        if (p->pid) {
            fprintf(log_fs,
                "ix: %d PID: %d %s Threads: %ld/%ld CPU: %ld MBs: %ld/%ld DataTS: %ld BindTS: %ld ",
                ix, p->pid, ((p->comm[0] != '\0') ? p->comm : "(Null)"), p->num_active_threads, p->num_threads,
                p->CPUs_used, p->MBs_used, p->MBs_size, p->data_time_stamp, p->bind_time_stamp);
            char buf[BUF_SIZE];
            str_from_node_ix_list_as_node_ids(buf, BUF_SIZE, p->node_list_p);
            fprintf(log_fs, " Node(s) %s\n", buf);
            fflush(log_fs);
        }
    }
}


pid_list_p include_pid_list = NULL;
pid_list_p exclude_pid_list = NULL;

pid_list_p insert_pid_into_pid_list(pid_list_p list_ptr, long pid) {
    if (process_hash_table != NULL) {
        int hash_ix = process_hash_lookup(pid);
        if ((hash_ix >= 0) && (list_ptr == include_pid_list)) {
            // Clear interleaved flag, in case user wants it to be re-evaluated
            process_hash_table[hash_ix].flags &= ~PROCESS_FLAG_INTERLEAVED;
        }
    }
    // Check for duplicate pid first
    pid_list_p pid_ptr = list_ptr;
    while (pid_ptr != NULL) {
        if (pid_ptr->pid == pid) {
            // pid already in list
            return list_ptr;
        }
        pid_ptr = pid_ptr->next;
    }
    // pid not yet in list -- insert new node
    pid_ptr = malloc(sizeof(pid_list_t));
    if (pid_ptr == NULL) {
        numad_log(LOG_CRIT, "pid_list malloc failed\n");
        exit(EXIT_FAILURE);
    }
    pid_ptr->pid = pid;
    pid_ptr->next = list_ptr;
    list_ptr = pid_ptr;
    return list_ptr;
}

pid_list_p remove_pid_from_pid_list(pid_list_p list_ptr, long pid) {
    pid_list_p last_pid_ptr = NULL;
    pid_list_p pid_ptr = list_ptr;
    while (pid_ptr != NULL) {
        if (pid_ptr->pid == pid) {
            if (pid_ptr == list_ptr) {
                list_ptr = list_ptr->next;
                free(pid_ptr);
                pid_ptr = list_ptr;
                continue;
            } else {
                last_pid_ptr->next = pid_ptr->next;
                free(pid_ptr);
                pid_ptr = last_pid_ptr;
            }
        }
        last_pid_ptr = pid_ptr;
        pid_ptr = pid_ptr->next;
    }
    return list_ptr;
}



void shut_down_numad() {
    numad_log(LOG_NOTICE, "Shutting down numad\n");
    flush_msg_queue();
    unlink(var_run_file);
    close_log_file();
    exit(EXIT_SUCCESS);
}


void print_version_and_exit(char *prog_name) {
    fprintf(stdout, "%s version: %s: compiled %s\n", prog_name, VERSION_STRING, __DATE__);
    exit(EXIT_SUCCESS);
}


void print_usage_and_exit(char *prog_name) {
    fprintf(stderr, "Usage: %s <options> ...\n", prog_name);
    fprintf(stderr, "-C 1  to count inactive file cache as available memory (default 1)\n");
    fprintf(stderr, "-C 0  to count inactive file cache memory as unavailable (default 1)\n");
    fprintf(stderr, "-d for debug logging (same effect as '-l 7')\n");
    fprintf(stderr, "-h to print this usage info\n");
    fprintf(stderr, "-H <N> to set THP scan_sleep_ms (default %d)\n", DEFAULT_THP_SCAN_SLEEP_MS);
    fprintf(stderr, "-i [<MIN>:]<MAX> to specify interval seconds\n");
    fprintf(stderr, "-K 1  to keep interleaved memory spread across nodes (default 0)\n");
    fprintf(stderr, "-K 0  to merge interleaved memory to local NUMA nodes (default 0)\n");
    fprintf(stderr, "-l <N> to specify logging level (usually 5, 6, or 7 -- default 5)\n");
    fprintf(stderr, "-m <N> to specify memory locality target percent (default %d)\n", DEFAULT_MEMLOCALITY_PERCENT);
    fprintf(stderr, "-p <PID> to add PID to inclusion pid list\n");
    fprintf(stderr, "-r <PID> to remove PID from explicit pid lists\n");
    fprintf(stderr, "-R <CPU_LIST> to reserve some CPUs for non-numad use\n");
    fprintf(stderr, "-S 1  to scan all processes (default 1)\n");
    fprintf(stderr, "-S 0  to scan only explicit PID list processes (default 1)\n");
    fprintf(stderr, "-t <N> to specify thread / logical CPU valuation percent (default %d)\n", DEFAULT_HTT_PERCENT);
    fprintf(stderr, "-u <N> to specify utilization target percent (default %d)\n", DEFAULT_UTILIZATION_PERCENT);
    fprintf(stderr, "-v for verbose  (same effect as '-l 6')\n");
    fprintf(stderr, "-V to show version info\n");
    fprintf(stderr, "-w <CPUs>[:<MBs>] for NUMA node suggestions\n");
    fprintf(stderr, "-x <PID> to add PID to exclusion pid list\n");
    fprintf(stderr, "--gpu-aware=0|1\n");
    fprintf(stderr, "--gpu-backend=auto|fdinfo|amdsmi\n");
    fprintf(stderr, "--gpu-min-busy=<N>\n");
    fprintf(stderr, "--gpu-min-vram=<MB>\n");
    fprintf(stderr, "--gpu-fdinfo-discovery=<sec>\n");
    fprintf(stderr, "--gpu-migrate=auto|always|never\n");
    fprintf(stderr, "--gpu-migrate-busy-max=<N>\n");
    fprintf(stderr, "--gpu-graphics-placement=auto|prefer|strict\n");
    fprintf(stderr, "--scx-mode=legacy|cooperate|observe\n");
    fprintf(stderr, "--scx-sched=auto|beerland|p2dq\n");
    fprintf(stderr, "--bind-cooldown=<sec>\n");
    fprintf(stderr, "Note: GPU/SCX long options (including --gpu-graphics-placement) and --bind-cooldown are startup-only.\n");
    exit(EXIT_FAILURE);
}


void set_thp_scan_sleep_ms(int new_ms) {
    if (new_ms < 1) {
        // 0 means do not change the system default
        return;
    }
    char *thp_scan_fname = "/sys/kernel/mm/transparent_hugepage/khugepaged/scan_sleep_millisecs";
    int fd = open(thp_scan_fname, O_RDWR, 0);
    if (fd >= 0) {
        char buf[BUF_SIZE];
        int bytes = read(fd, buf, BUF_SIZE);
        if (bytes > 0) {
            buf[bytes] = '\0';
            int cur_ms;
            char *p = buf;
            CONVERT_DIGITS_TO_NUM(p, cur_ms);
            if (cur_ms != new_ms) {
                lseek(fd, 0, SEEK_SET);
                numad_log(LOG_NOTICE, "Changing THP scan time in %s from %d to %d ms.\n", thp_scan_fname, cur_ms, new_ms);
                snprintf(buf, BUF_SIZE, "%d\n", new_ms);
                write(fd, buf, strlen(buf));
            }
        }
        close(fd);
    }
}

void check_prereqs(char *prog_name) {
    // Adjust kernel tunable to scan for THP more frequently...
    set_thp_scan_sleep_ms(thp_scan_sleep_ms);
}


int get_daemon_pid(int inited) {
    int fd = open(var_run_file, O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    char buf[BUF_SIZE];
    int bytes = read(fd, buf, BUF_SIZE);
    close(fd);
    if (bytes <= 0) {
        return 0;
    }
    int pid;
    char *p = buf;
    CONVERT_DIGITS_TO_NUM(p, pid);
    // Check run file pid still active
    char fname[FNAME_SIZE];
    snprintf(fname, FNAME_SIZE, "/proc/%d", pid);
    if (access(fname, F_OK) < 0) {
        if (inited && errno == ENOENT) {
            numad_log(LOG_NOTICE, "Removing out-of-date numad run file because %s doesn't exist\n", fname);
            unlink(var_run_file);
        }
        return 0;
    }
    // Daemon must be running already.
    return pid; 
}

int register_numad_pid() {
    int pid;
    char buf[BUF_SIZE];
    int fd;
create_run_file:
    fd = open(var_run_file, O_RDWR|O_CREAT|O_EXCL, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
    if (fd >= 0) {
        pid = getpid();
        snprintf(buf, BUF_SIZE, "%d\n", pid);
        write(fd, buf, strlen(buf));
        close(fd);
        numad_log(LOG_NOTICE, "Registering numad version %s PID %d\n", VERSION_STRING, pid);
        return pid;
    }
    if (errno == EEXIST) {
        fd = open(var_run_file, O_RDWR|O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
        if (fd < 0) {
            goto fail_numad_run_file;
        }
        int bytes = read(fd, buf, BUF_SIZE);
        close(fd);
        if (bytes > 0) {
            char *p = buf;
            CONVERT_DIGITS_TO_NUM(p, pid);
            // Check pid in run file still active
            char fname[FNAME_SIZE];
            snprintf(fname, FNAME_SIZE, "/proc/%d", pid);
            if (access(fname, F_OK) < 0) {
                if (errno == ENOENT) {
                    // Assume run file is out-of-date...
                    numad_log(LOG_NOTICE, "Removing out-of-date numad run file because %s doesn't exist\n", fname);
                    unlink(var_run_file);
                    goto create_run_file;
                }
            }
            // Daemon must be running already.
            return pid; 
        }
    }
fail_numad_run_file:
    numad_log(LOG_CRIT, "Cannot open numad.pid file\n");
    exit(EXIT_FAILURE);
}


int count_set_bits_in_hex_list_file(char *fname) {
    int sum = 0;
    int fd = open(fname, O_RDONLY, 0);
    if (fd >= 0) {
        char buf[BUF_SIZE];
        int bytes = read(fd, buf, BUF_SIZE);
        close(fd);
        for (int ix = 0;  (ix < bytes);  ix++) {
            char c = tolower(buf[ix]);
            switch (c) {
                case '0'  : sum += 0; break;
                case '1'  : sum += 1; break;
                case '2'  : sum += 1; break;
                case '3'  : sum += 2; break;
                case '4'  : sum += 1; break;
                case '5'  : sum += 2; break;
                case '6'  : sum += 2; break;
                case '7'  : sum += 3; break;
                case '8'  : sum += 1; break;
                case '9'  : sum += 2; break;
                case 'a'  : sum += 2; break;
                case 'b'  : sum += 3; break;
                case 'c'  : sum += 2; break;
                case 'd'  : sum += 3; break;
                case 'e'  : sum += 3; break;
                case 'f'  : sum += 4; break;
                case ' '  : sum += 0; break;
                case ','  : sum += 0; break;
                case '\n' : sum += 0; break;
                default : numad_log(LOG_CRIT, "Unexpected character in list\n"); exit(EXIT_FAILURE);
            }
        }
    }
    return sum;
}


int get_num_cpus() {
    int n1 = sysconf(_SC_NPROCESSORS_ONLN);
    // int n2 = sysconf(_SC_NPROCESSORS_CONF);
    // if (n1 < n2) {
    //     n1 = n2;
    // }
    if (n1 < 0) {
        numad_log(LOG_CRIT, "Cannot count number of processors\n");
        exit(EXIT_FAILURE);
    }
    return n1;
}


int get_num_kvm_vcpu_threads(int pid) {
    // Try to return the number of vCPU threads for this VM guest,
    // excluding the IO threads.  All failures return MAXINT.
    // FIXME: someday figure out some better way to do this...
    char fname[FNAME_SIZE];
    snprintf(fname, FNAME_SIZE, "/proc/%d/cmdline", pid);
    int fd = open(fname, O_RDONLY, 0);
    if (fd >= 0) {
        char buf[BUF_SIZE];
        int bytes = read(fd, buf, BUF_SIZE);
        close(fd);
        if (bytes > 0) {
            char *p = memmem(buf, bytes, "smp", 3);
            if (p != NULL) {
                while (!isdigit(*p) && (p - buf < bytes - 2)) {
                    p++;
                }
                if (isdigit(*p)) {
                    int vcpu_threads;
                    CONVERT_DIGITS_TO_NUM(p, vcpu_threads);
                    if ((vcpu_threads > 0) && (vcpu_threads <= num_cpus)) {
                        return vcpu_threads;
                    }
                }
            }
        }
    }
    return MAXINT;
}


uint64_t get_huge_page_size_in_bytes() {
    uint64_t huge_page_size = 0;;
    FILE *fs = fopen("/proc/meminfo", "r");
    if (!fs) {
        numad_log(LOG_CRIT, "Can't open /proc/meminfo\n");
        exit(EXIT_FAILURE);
    }
    char buf[BUF_SIZE];
    while (fgets(buf, BUF_SIZE, fs)) {
        if (!strncmp("Hugepagesize", buf, 12)) {
            char *p = &buf[12];
            while ((!isdigit(*p)) && (p < buf + BUF_SIZE)) {
                p++;
            }
            huge_page_size = atol(p);
            break;
        }
    }
    fclose(fs);
    return huge_page_size * KILOBYTE;
}


uint64_t get_time_stamp() {
    // Return time stamp in hundredths of a second
    struct timespec ts; 
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
        numad_log(LOG_CRIT, "Cannot get clock_gettime()\n");
        exit(EXIT_FAILURE);
    }
    return (ts.tv_sec * ONE_HUNDRED) +
           (ts.tv_nsec / (1000000000 / ONE_HUNDRED));
}


static int name_starts_with_digit(const struct dirent *dptr) {
    return (isdigit(dptr->d_name[0]));
}



#define BITS_IN_LONG (CHAR_BIT * sizeof(unsigned long))
#define   SET_BIT(i,a)   ((a)[(i) / BITS_IN_LONG] |=  (1ul << ((i) % BITS_IN_LONG)))
#define  TEST_BIT(i,a) (((a)[(i) / BITS_IN_LONG] &   (1ul << ((i) % BITS_IN_LONG))) != 0)
#define CLEAR_BIT(i,a)   ((a)[(i) / BITS_IN_LONG] &= ~(1ul << ((i) % BITS_IN_LONG)))

static inline int migrate_numnodes(void) {
    return (max_node_id_seen >= 0) ? (max_node_id_seen + 1) : 0;
}

static inline size_t migrate_mask_bytes(int numnodes) {
    size_t nwords = ((size_t)numnodes + BITS_IN_LONG - 1) / BITS_IN_LONG;
    return nwords * sizeof(unsigned long);
}


typedef struct cpu_data {
    uint64_t time_stamp;
    uint64_t *idle;
} cpu_data_t, *cpu_data_p;

cpu_data_t cpu_data_buf[2];  // Two sets, to calc deltas
int cur_cpu_data_buf = 0;

void update_cpu_data() {
    // Parse idle percents from CPU stats in /proc/stat cpu<N> lines
    static FILE *fs;
    if (fs != NULL) {
        rewind(fs);
    } else {
        fs = fopen("/proc/stat", "r");
        if (!fs) {
            numad_log(LOG_CRIT, "Cannot get /proc/stat contents\n");
            exit(EXIT_FAILURE);
        }
        cpu_data_buf[0].idle = malloc(num_cpus * sizeof(uint64_t));
        cpu_data_buf[1].idle = malloc(num_cpus * sizeof(uint64_t));
        if ((cpu_data_buf[0].idle == NULL) || (cpu_data_buf[1].idle == NULL)) {
            numad_log(LOG_CRIT, "cpu_data_buf malloc failed\n");
            exit(EXIT_FAILURE);
        }
    }
    // Use the other cpu_data buffer...
    int new = 1 - cur_cpu_data_buf;
    // First get the current time stamp
    cpu_data_buf[new].time_stamp = get_time_stamp();
    // Now pull the idle stat from each cpu<N> line
    char buf[BUF_SIZE];
    while (fgets(buf, BUF_SIZE, fs)) {
        /* 
        * Lines are of the form:
        *
        * cpu<N> user nice system idle iowait irq softirq steal guest guest_nice
        *
        * # cat /proc/stat
        * cpu  11105906 0 78639 3359578423 24607 151679 322319 0 0 0
        * cpu0 190540 0 1071 52232942 39 7538 234039 0 0 0
        * cpu1 124519 0 50 52545188 0 1443 6267 0 0 0
        * cpu2 143133 0 452 52531440 36 1573 834 0 0 0
        * . . . . 
        */
        if ( (buf[0] == 'c') && (buf[1] == 'p') && (buf[2] == 'u') && (isdigit(buf[3])) ) {
            char *p = &buf[3];
            int cpu_id = *p++ - '0'; while (isdigit(*p)) { cpu_id *= 10; cpu_id += (*p++ - '0'); }
            while (!isdigit(*p)) { p++; } while (isdigit(*p)) { p++; }  // skip user
            while (!isdigit(*p)) { p++; } while (isdigit(*p)) { p++; }  // skip nice
            while (!isdigit(*p)) { p++; } while (isdigit(*p)) { p++; }  // skip system
            while (!isdigit(*p)) { p++; }
            uint64_t idle;
            CONVERT_DIGITS_TO_NUM(p, idle);
            cpu_data_buf[new].idle[cpu_id] = idle;
        }
    }
    cur_cpu_data_buf = new;
}


int node_and_digits(const struct dirent *dptr) {
    char *p = (char *)(dptr->d_name);
    if (*p++ != 'n') return 0;
    if (*p++ != 'o') return 0;
    if (*p++ != 'd') return 0;
    if (*p++ != 'e') return 0;
    do {
        if (!isdigit(*p++))
            return 0;
    } while (*p != '\0');
    return 1;
}


uint64_t node_info_time_stamp = 0;
id_list_p all_cpus_list_p = NULL;
id_list_p all_nodes_list_p = NULL;
id_list_p reserved_cpu_mask_list_p = NULL;
char *reserved_cpu_str = NULL;


void show_nodes(int nodes) {
    assert(nodes == num_nodes);
    fprintf(log_fs, "\n");
    fprintf(log_fs, "Nodes: %d\n", num_nodes);
    for (int ix = 0;  (ix < num_nodes);  ix++) {
        fprintf(log_fs, "Node[%d] ID %ld, MBs_tot %ld, MBs_free %ld, CPUs_tot %ld, CPUs_free %ld, Dist ", 
            ix, node[ix].node_id, node[ix].MBs_total, node[ix].MBs_free, node[ix].CPUs_total, node[ix].CPUs_free);
        for (int d = 0;  (d < num_nodes);  d++) {
            fprintf(log_fs, "%d ", node[ix].distance[d]);
        }
        char buf[BUF_SIZE];
        str_from_id_list(buf, BUF_SIZE, node[ix].cpu_list_p);
        fprintf(log_fs, " CPUs %s\n", buf);
    }
    fprintf(log_fs, "Min CPUs free: %ld, Max CPUs: %ld, Avg CPUs: %ld, StdDev: %.2lf\n", 
        min_node_CPUs_free, max_node_CPUs_free, avg_node_CPUs_free, stddev_node_CPUs_free);
    fprintf(log_fs, "Min MBs free: %ld, Max MBs: %ld, Avg MBs: %ld, StdDev: %.2lf\n", 
        min_node_MBs_free, max_node_MBs_free, avg_node_MBs_free, stddev_node_MBs_free);
    fflush(log_fs);
}


int update_nodes() {
    char fname[FNAME_SIZE];
    char buf[BIG_BUF_SIZE];
    // First, check to see if we should refresh basic node info that probably never changes...
    uint64_t time_stamp = get_time_stamp();
#define STATIC_NODE_INFO_DELAY (600 * ONE_HUNDRED)
    if ((num_nodes == 0) || (node_info_time_stamp + STATIC_NODE_INFO_DELAY < time_stamp)) {
        node_info_time_stamp = time_stamp;
        // Count directory names of the form: /sys/devices/system/node/node<N>
        struct dirent **namelist;
        int num_files = scandir ("/sys/devices/system/node", &namelist, node_and_digits, versionsort);
        if (num_files < 1) {
            numad_log(LOG_CRIT, "Could not get NUMA node info\n");
            exit(EXIT_FAILURE);
        }
        int need_to_realloc = (num_files != num_nodes);
        if (need_to_realloc) {
            for (int ix = num_files;  (ix < num_nodes);  ix++) {
                // If new < old, free old node_data pointers
                free(node[ix].distance);
                FREE_LIST(node[ix].cpu_list_p);
            }
            node = realloc(node, (num_files * sizeof(node_data_t)));
            if (node == NULL) {
                numad_log(LOG_CRIT, "node realloc failed\n");
                exit(EXIT_FAILURE);
            }
            for (int ix = num_nodes;  (ix < num_files);  ix++) {
                // If new > old, nullify new node_data pointers
                node[ix].distance = NULL;
                node[ix].cpu_list_p = NULL;
            }
            num_nodes = num_files;
        }
        sum_MBs_total = 0;
        sum_CPUs_total = 0;
        CLEAR_CPU_LIST(all_cpus_list_p);
        CLEAR_NODE_LIST(all_nodes_list_p);
        // Figure out how many threads per core there are (for later discounting of hyper-threads)
        threads_per_core = count_set_bits_in_hex_list_file("/sys/devices/system/cpu/cpu0/topology/thread_siblings");
        if (threads_per_core < 1) {
            numad_log(LOG_CRIT, "Could not count threads per core\n");
            exit(EXIT_FAILURE);
        }
        // For each "node<N>" filename present, save <N> in node[ix].node_id
        // Note that the node id might not necessarily match the node ix.
        // Also populate the cpu lists and distance vectors for this node.
        for (int node_ix = 0;  (node_ix < num_nodes);  node_ix++) {
            char *p = &namelist[node_ix]->d_name[4];
            int node_id;
            CONVERT_DIGITS_TO_NUM(p, node_id);
            node[node_ix].node_id = node_id;
            free(namelist[node_ix]);
            ADD_ID_TO_LIST(node_ix, all_nodes_list_p);
            // Get all the CPU IDs in this node...  Read lines from node<N>/cpulist
            // file, and set the corresponding bits in the node cpu list.
            snprintf(fname, FNAME_SIZE, "/sys/devices/system/node/node%d/cpulist", node_id);
            int fd = open(fname, O_RDONLY, 0);
            if ((fd >= 0) && (read(fd, buf, BIG_BUF_SIZE) > 0)) {
                buf[BIG_BUF_SIZE - 1] = '\0';
                // get cpulist from the cpulist string
                CLEAR_CPU_LIST(node[node_ix].cpu_list_p);
                int n = add_ids_to_list_from_str(node[node_ix].cpu_list_p, buf);
                if (reserved_cpu_str != NULL) {
                    AND_LISTS(node[node_ix].cpu_list_p, node[node_ix].cpu_list_p, reserved_cpu_mask_list_p);
                    n = NUM_IDS_IN_LIST(node[node_ix].cpu_list_p);
                }
                OR_LISTS(all_cpus_list_p, all_cpus_list_p, node[node_ix].cpu_list_p);
                // Calculate total CPUs, but possibly discount hyper-threads
                if ((threads_per_core == 1) || (htt_percent >= 100)) {
                    node[node_ix].CPUs_total = n * ONE_HUNDRED;
                } else {
                    n /= threads_per_core;
                    node[node_ix].CPUs_total = n * ONE_HUNDRED;
                    node[node_ix].CPUs_total += n * (threads_per_core - 1) * htt_percent;
                }
                sum_MBs_total += node[node_ix].MBs_total;
                sum_CPUs_total += node[node_ix].CPUs_total;
                close(fd);
            } else {
                numad_log(LOG_CRIT, "Could not get node cpu list\n");
                exit(EXIT_FAILURE);
            }
            // Get distance vector of ACPI SLIT data from node<N>/distance file
            if (need_to_realloc) {
                node[node_ix].distance = realloc(node[node_ix].distance, (num_nodes * sizeof(uint8_t)));
                if (node[node_ix].distance == NULL) {
                    numad_log(LOG_CRIT, "node distance realloc failed\n");
                    exit(EXIT_FAILURE);
                }
            }
            snprintf(fname, FNAME_SIZE, "/sys/devices/system/node/node%d/distance", node_id);
            fd = open(fname, O_RDONLY, 0);
            if ((fd >= 0) && (read(fd, buf, BIG_BUF_SIZE) > 0)) {
                int rnode = 0;
                for (char *p = buf;  (*p != '\n'); ) {
                    int lat;
                    CONVERT_DIGITS_TO_NUM(p, lat);
                    node[node_ix].distance[rnode++] = lat;
                    while (*p == ' ') { p++; }
                }
                close(fd);
            } else {
                numad_log(LOG_CRIT, "Could not get node distance data\n");
                exit(EXIT_FAILURE);
            }
        }
        free(namelist);
    }
    rebuild_node_id_index();
    // Second, update the dynamic free memory and available CPU capacity
    while (cpu_data_buf[cur_cpu_data_buf].time_stamp + 7 >= time_stamp) {
        // Make sure at least 7/100 of a second has passed.
        // Otherwise sleep for 1/10 second.
        struct timespec ts = { 0, 100000000 }; 
        nanosleep(&ts, &ts);
        time_stamp = get_time_stamp();
    }
    update_cpu_data();
    max_node_MBs_free = 0;
    max_node_CPUs_free = 0;
    min_node_MBs_free = MAXINT;
    min_node_CPUs_free = MAXINT;
    uint64_t sum_of_node_MBs_free = 0;
    uint64_t sum_of_node_CPUs_free = 0;
    for (int node_ix = 0;  (node_ix < num_nodes);  node_ix++) {
        int node_id = node[node_ix].node_id;
        // Get available memory info from node<N>/meminfo file
        snprintf(fname, FNAME_SIZE, "/sys/devices/system/node/node%d/meminfo", node_id);
        int fd = open(fname, O_RDONLY, 0);
        if ((fd >= 0) && (read(fd, buf, BIG_BUF_SIZE) > 0)) {
            close(fd);
            uint64_t KB;
            buf[BIG_BUF_SIZE - 1] = '\0';
            char *p = strstr(buf, "MemTotal:");
            if (p != NULL) {
                p += 9;
            } else {
                numad_log(LOG_CRIT, "Could not get node MemTotal\n");
                exit(EXIT_FAILURE);
            }
            while (!isdigit(*p)) { p++; }
            CONVERT_DIGITS_TO_NUM(p, KB);
            node[node_ix].MBs_total = (KB / KILOBYTE);
/*
            if (node[node_ix].MBs_total < 1) {
                // If a node has zero memory, remove it from the all_nodes_list...
                CLR_ID_IN_LIST(node_id, all_nodes_list_p);
            }
*/
            p = strstr(p, "MemFree:");
            if (p != NULL) {
                p += 8;
            } else {
                numad_log(LOG_CRIT, "Could not get node MemFree\n");
                exit(EXIT_FAILURE);
            }
            while (!isdigit(*p)) { p++; }
            CONVERT_DIGITS_TO_NUM(p, KB);
            node[node_ix].MBs_free = (KB / KILOBYTE);
            if (use_inactive_file_cache) {
                // Add inactive file cache quantity to "free" memory
                p = strstr(p, "Inactive(file):");
                if (p != NULL) {
                    p += 15;
                } else {
                    numad_log(LOG_CRIT, "Could not get node Inactive(file)\n");
                    exit(EXIT_FAILURE);
                }
                while (!isdigit(*p)) { p++; }
                CONVERT_DIGITS_TO_NUM(p, KB);
                node[node_ix].MBs_free += (KB / KILOBYTE);
            }
            sum_of_node_MBs_free += node[node_ix].MBs_free;
            if (min_node_MBs_free > node[node_ix].MBs_free) {
                min_node_MBs_free = node[node_ix].MBs_free;
                min_node_MBs_free_id = node[node_ix].node_id;
            }
            if (max_node_MBs_free < node[node_ix].MBs_free) {
                max_node_MBs_free = node[node_ix].MBs_free;
            }
        } else {
            numad_log(LOG_CRIT, "Could not get node meminfo\n");
            exit(EXIT_FAILURE);
        }
        // If both buffers have been populated by now, sum CPU idle data
        // for each node in order to calculate available capacity
        int old_cpu_data_buf = 1 - cur_cpu_data_buf;
        if (cpu_data_buf[old_cpu_data_buf].time_stamp > 0) {
            uint64_t idle_ticks = 0;
            int cpu = 0;
            int num_lcpus = NUM_IDS_IN_LIST(node[node_ix].cpu_list_p);
            int num_cpus_to_process = num_lcpus;
            while (num_cpus_to_process) {
                if (ID_IS_IN_LIST(cpu, node[node_ix].cpu_list_p)) {
                    idle_ticks += cpu_data_buf[cur_cpu_data_buf].idle[cpu]
                                - cpu_data_buf[old_cpu_data_buf].idle[cpu];
                    num_cpus_to_process -= 1;
                }
                cpu += 1;
            }
            uint64_t time_diff = cpu_data_buf[cur_cpu_data_buf].time_stamp
                               - cpu_data_buf[old_cpu_data_buf].time_stamp;
            // printf("Node: %d   CPUs: %ld   time diff %ld   Idle ticks %ld\n", node_id, node[node_ix].CPUs_total, time_diff, idle_ticks);
	    if (time_diff == 0) {
		numad_log(LOG_WARNING, "Forcing time_diff to be 1\n");
		time_diff = 1;
	    }
            node[node_ix].CPUs_free = (idle_ticks * ONE_HUNDRED) / time_diff;
            // Possibly discount hyper-threads
            if ((threads_per_core > 1) && (htt_percent < 100)) {
                uint64_t htt_discount = (num_lcpus - (num_lcpus / threads_per_core)) * (100 - htt_percent);
                if (node[node_ix].CPUs_free > htt_discount) {
                    node[node_ix].CPUs_free -= htt_discount;
                } else {
                    node[node_ix].CPUs_free = 0;
                }
            }
            if (node[node_ix].CPUs_free > node[node_ix].CPUs_total) {
                node[node_ix].CPUs_free = node[node_ix].CPUs_total;
            }
            sum_of_node_CPUs_free += node[node_ix].CPUs_free;
            if (min_node_CPUs_free > node[node_ix].CPUs_free) {
                min_node_CPUs_free = node[node_ix].CPUs_free;
                min_node_CPUs_free_id = node[node_ix].node_id;
            }
            if (max_node_CPUs_free < node[node_ix].CPUs_free) {
                max_node_CPUs_free = node[node_ix].CPUs_free;
            }
            node[node_ix].magnitude = node[node_ix].CPUs_free * node[node_ix].MBs_free;
        } else {
            node[node_ix].CPUs_free = 0;
            node[node_ix].magnitude = 0;
        }
    }
    avg_node_MBs_free = sum_of_node_MBs_free / num_nodes;
    avg_node_CPUs_free = sum_of_node_CPUs_free / num_nodes;
    double MBs_variance_sum = 0.0;
    double CPUs_variance_sum = 0.0;
    for (int node_ix = 0;  (node_ix < num_nodes);  node_ix++) {
        double MBs_diff = (double)node[node_ix].MBs_free - (double)avg_node_MBs_free;
        double CPUs_diff = (double)node[node_ix].CPUs_free - (double)avg_node_CPUs_free;
        MBs_variance_sum += MBs_diff * MBs_diff;
        CPUs_variance_sum += CPUs_diff * CPUs_diff;
    }
    double MBs_variance = MBs_variance_sum / (num_nodes);
    double CPUs_variance = CPUs_variance_sum / (num_nodes);
    stddev_node_MBs_free = sqrt(MBs_variance);
    stddev_node_CPUs_free = sqrt(CPUs_variance);
    return num_nodes;
}


int all_digits(const struct dirent *dptr) {
    char *p = (char *)(dptr->d_name);
    if (p == NULL) {
        return 0;
    }
    while (*p != '\0') {
        if (!isdigit(*p++)) return 0;
    }
    return 1;
}


typedef struct stat_data {
    // This structure isn't actually used in numad -- it is here just to
    // document the field type and order of the /proc/<PID>/stat items, some of
    // which are used in the process_data_t structure.
    int pid;              // 0
    char *comm;           // 1
    char state;           // 2
    int ppid;
    int pgrp;
    int session;
    int tty_nr;
    int tpgid;
    unsigned flags;
#define PF_VCPU			0x00000001	/* I'm a virtual CPU */
#define PF_IDLE			0x00000002	/* I am an IDLE thread */
#define PF_EXITING		0x00000004	/* Getting shut down */
#define PF_POSTCOREDUMP		0x00000008	/* Coredumps should ignore this task */
#define PF_IO_WORKER		0x00000010	/* Task is an IO worker */
#define PF_WQ_WORKER		0x00000020	/* I'm a workqueue worker */
#define PF_FORKNOEXEC		0x00000040	/* Forked but didn't exec */
#define PF_MCE_PROCESS		0x00000080      /* Process policy on mce errors */
#define PF_SUPERPRIV		0x00000100	/* Used super-user privileges */
    uint64_t minflt;
    uint64_t cminflt;
    uint64_t majflt;
    uint64_t cmajflt;
    uint64_t utime;       // 13
    uint64_t stime;       // 14
    int64_t cutime;
    int64_t cstime;
    int64_t priority;     // 17
    int64_t nice;
    int64_t num_threads;  // 19
    int64_t itrealvalue;
    uint64_t starttime;
    uint64_t vsize;       // 22
    int64_t rss;          // 23
    uint64_t rsslim;
    uint64_t startcode;
    uint64_t endcode;
    uint64_t startstack;
    uint64_t kstkesp;
    uint64_t kstkeip;
    uint64_t signal;
    uint64_t blocked;
    uint64_t sigignore;
    uint64_t sigcatch;
    uint64_t wchan;
    uint64_t nswap;
    uint64_t cnswap;
    int exit_signal;
    int processor;        // 38
    unsigned rt_priority;
    unsigned policy;      // 40
    uint64_t delayacct_blkio_ticks;
    uint64_t guest_time;  // 42
    int64_t cguest_time;
    uint64_t start_data;
    uint64_t end_data;
    uint64_t start_brk;
    uint64_t arg_start;
    uint64_t arg_end;
    uint64_t env_start;
    uint64_t env_end;
    int exit_code;
} stat_data_t, *stat_data_p;


int get_stat_data_for_pid(int pid, process_data_p out) {
    if (out == NULL) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    char fname[FNAME_SIZE];
    char stat_buf[BUF_SIZE];
    if (snprintf(fname, FNAME_SIZE, "/proc/%d/stat", pid) >= FNAME_SIZE) {
        return -1;
    }
    ssize_t bytes = read_text_file(fname, stat_buf, sizeof(stat_buf));
    if (bytes < 0) {
        numad_log(LOG_WARNING, "Could not open/read stat file: %s\n", fname);
        return -1;
    }

    const char *tail = NULL;
    int parsed_pid = 0;
    if (parse_proc_stat_header(stat_buf, &parsed_pid, out->comm, sizeof(out->comm), &tail) < 0) {
        numad_log(LOG_WARNING, "Could not parse stat header for pid %d\n", pid);
        return -1;
    }
    out->pid = parsed_pid;

    tail = skip_token_const(tail); /* state */
    for (int ix = 0; (ix < 10); ix++) {
        tail = skip_token_const(tail); /* fields 4..13 */
    }

    if (parse_u64_token(&tail, &out->cpu_util) < 0) {
        return -1;
    }
    uint64_t stime = 0;
    if (parse_u64_token(&tail, &stime) < 0) {
        return -1;
    }
    out->cpu_util += stime;

    for (int ix = 0; (ix < 4); ix++) {
        tail = skip_token_const(tail); /* fields 16..19 */
    }
    if (parse_u64_token(&tail, &out->num_threads) < 0) {
        return -1;
    }
    tail = skip_token_const(tail); /* field 21 */
    if (parse_u64_token(&tail, &out->start_time_ticks) < 0) {
        return -1;
    }

    uint64_t vsize = 0;
    if (parse_u64_token(&tail, &vsize) < 0) {
        return -1;
    }
    out->MBs_size = vsize / MEGABYTE;

    int64_t rss_pages = 0;
    if (parse_i64_token(&tail, &rss_pages) < 0) {
        return -1;
    }
    if (rss_pages < 0) {
        rss_pages = 0;
    }
    out->MBs_used = ((uint64_t)rss_pages * page_size_in_bytes) / MEGABYTE;

    collect_active_threads_for_pid(pid, out->num_threads, &out->num_active_threads);

    id_list_p tmp_cpu_list_p = NULL;
    CLEAR_CPU_LIST(tmp_cpu_list_p);
    CLEAR_NODE_LIST(out->node_list_p);

    char status_buf[BIG_BUF_SIZE];
    if (snprintf(fname, FNAME_SIZE, "/proc/%d/status", pid) >= FNAME_SIZE) {
        FREE_LIST(tmp_cpu_list_p);
        return -1;
    }
    bytes = read_text_file(fname, status_buf, sizeof(status_buf));
    if (bytes < 0) {
        numad_log(LOG_WARNING, "Could not open/read status file: %s\n", fname);
        FREE_LIST(tmp_cpu_list_p);
        return -1;
    }

    char *p = strstr(status_buf, "Cpus_allowed_list:");
    if (p == NULL) {
        FREE_LIST(tmp_cpu_list_p);
        return -1;
    }
    p += 18;
    while ((*p != '\0') && (!isdigit(*p))) {
        p++;
    }
    add_ids_to_list_from_str(tmp_cpu_list_p, p);
    for (int cpu = 0; (cpu < num_cpus); cpu++) {
        if (ID_IS_IN_LIST(cpu, tmp_cpu_list_p) && (cpu_to_node_ix[cpu] >= 0)) {
            ADD_ID_TO_LIST(cpu_to_node_ix[cpu], out->node_list_p);
        }
    }
    if ((out->node_list_p != NULL) && (NUM_IDS_IN_LIST(out->node_list_p) == 0) && (all_nodes_list_p != NULL)) {
        COPY_LIST(all_nodes_list_p, out->node_list_p);
    }
    FREE_LIST(tmp_cpu_list_p);
    return 0;
}


int update_processes(uint64_t cycle_ts) {
    // Conditionally scan /proc/<PID>/stat files for processes we should
    // perhaps manage. For all processes, evaluate whether or not they should
    // be added to our hash table of managed processes candidates.  If so,
    // update the statistics, time stamp and utilization numbers for the select
    // processes in the hash table.
    int new_candidates = 0;  // limit number of new candidates per update
    int include_list_len = 0;
    int scan_candidate_limit = 0;
    int files = 0;

    pthread_mutex_lock(&pid_list_mutex);
    for (pid_list_p pid_ptr = exclude_pid_list; pid_ptr != NULL; pid_ptr = pid_ptr->next) {
        process_hash_remove(pid_ptr->pid);
    }

    for (pid_list_p count_ptr = include_pid_list; count_ptr != NULL; count_ptr = count_ptr->next) {
        include_list_len += 1;
    }
    // Reserve space for explicit includes before we start inserting them.
    process_hash_table_ensure_free_slots(include_list_len);

    pid_list_p pid_ptr = include_pid_list;
    while (pid_ptr != NULL) {
        process_data_t sample = {0};
        if (get_stat_data_for_pid(pid_ptr->pid, &sample) == 0) {
            sample.flags |= PROCESS_FLAG_EXPLICIT_PID;
            sample.data_time_stamp = cycle_ts;
            process_hash_update(&sample);
            FREE_LIST(sample.node_list_p);
            FREE_LIST(sample.gpu_list_p);
            if (!scan_all_processes) {
                files += 1;
            }
            pid_ptr = pid_ptr->next;
        } else {
            include_pid_list = remove_pid_from_pid_list(include_pid_list, pid_ptr->pid);
            pid_ptr = include_pid_list;
        }
    }
    pthread_mutex_unlock(&pid_list_mutex);

    if (scan_all_processes) {
        scan_candidate_limit = process_hash_table_size / 3;
        if (scan_candidate_limit < 1) {
            scan_candidate_limit = 1;
        }
        // Enter the bulk /proc scan with enough free slots for this cycle's
        // prospective new candidates instead of waiting for end-of-cycle cleanup.
        process_hash_table_ensure_free_slots(scan_candidate_limit);
        scan_candidate_limit = process_hash_table_size / 3;
        if (scan_candidate_limit < 1) {
            scan_candidate_limit = 1;
        }

        struct dirent **namelist;
        files = scandir("/proc", &namelist, name_starts_with_digit, NULL);
        if (files < 0) {
            numad_log(LOG_CRIT, "Could not open /proc\n");
            exit(EXIT_FAILURE);
        }
        for (int ix = 0;  (ix < files);  ix++) {
            int pid = atoi(namelist[ix]->d_name);
            pthread_mutex_lock(&pid_list_mutex);
            int skip = pid_is_in_list(include_pid_list, pid) || pid_is_in_list(exclude_pid_list, pid);
            pthread_mutex_unlock(&pid_list_mutex);
            if (!skip && (new_candidates < scan_candidate_limit)) {
                process_data_t sample = {0};
                if ((get_stat_data_for_pid(pid, &sample) == 0) && (sample.MBs_used > MEMORY_THRESHOLD)) {
                    sample.data_time_stamp = cycle_ts;
                    new_candidates += process_hash_update(&sample);
                }
                FREE_LIST(sample.node_list_p);
                FREE_LIST(sample.gpu_list_p);
            }
            free(namelist[ix]);
        }
        free(namelist);
    }

    process_hash_table_cleanup(cycle_ts);
    return files;
}


/*
static int cmp_ints_by_ref(const void *p1, const void *p2) {
    if (*(int *)p1 < *(int *)p2) {
        return 1;
    } else if (*(int *)p1 > *(int *)p2) {
        return -1;
    } else {
        return 0;
    }
}
*/


int bind_process_and_maybe_migrate_memory(process_data_p p, int migrate_memory, const char *migrate_reason) {
    uint64_t t0 = get_time_stamp();
    // Parameter p is a pointer to an element in the hash table
    if ((!p) || (p->pid < 1)) {
        numad_log(LOG_CRIT, "Bad PID to bind\n");
        exit(EXIT_FAILURE);
    }
    if ((!p->node_list_p) || (NUM_IDS_IN_LIST(p->node_list_p) == 0))
    {
        numad_log(LOG_CRIT, "Cannot bind to unspecified node(s)\n");
        exit(EXIT_FAILURE);
    }
    char node_list_str[BUF_SIZE];
    str_from_node_ix_list_as_node_ids(node_list_str, BUF_SIZE, p->node_list_p);
    int affinity_errors = 0;
    int migration_requested = migrate_memory;
    int migration_possible = migrate_memory;
    int migration_attempted = 0;
    int migration_passes = 0;
    int migration_partial_pages = 0;
    char runtime_migrate_reason[BUF_SIZE];
    runtime_migrate_reason[0] = '\0';
    // Generate CPU list derived from target node list.
    static id_list_p cpu_bind_list_p = NULL;
    cpu_bind_list_p = build_target_cpu_mask(p, cpu_bind_list_p);
    char fname[FNAME_SIZE];
    struct dirent **namelist;
    snprintf(fname, FNAME_SIZE, "/proc/%d/task", p->pid);
    int num_tasks = scandir(fname, &namelist, name_starts_with_digit, NULL);
    if (num_tasks <= 0) {
        numad_log(LOG_WARNING, "Could not scandir task list for PID: %d\n", p->pid);
        return 0;  // Assume the process terminated
    }
    // Set the affinity of each task in the process...
    for (int namelist_ix = 0;  (namelist_ix < num_tasks);  namelist_ix++) {
        int tid = atoi(namelist[namelist_ix]->d_name);
        int rc = sched_setaffinity(tid, ID_LIST_BYTES(cpu_bind_list_p), ID_LIST_SET_P(cpu_bind_list_p));
        if (rc < 0) {
            affinity_errors += 1;
            if (errno == ESRCH) {
                numad_log(LOG_WARNING, "Tried to bind PID %d, TID %d, but it apparently went away.\n", p->pid, tid);
            }
            numad_log(LOG_ERR, "Bad sched_setaffinity() on PID %d, TID %d -- errno: %d\n", p->pid, tid, errno);
        }
        free(namelist[namelist_ix]);
    }
    free(namelist);
    uint64_t t_affinity = get_time_stamp();
    if (migration_requested) {
        // Now move the memory to the target nodes....
        static unsigned long *dest_mask;
        static unsigned long *from_mask;
        static size_t allocated_bytes_in_masks;
        int numnodes = migrate_numnodes();
        size_t num_bytes_in_masks = migrate_mask_bytes(numnodes);
        if ((numnodes < 1) || (num_bytes_in_masks == 0)) {
            migration_possible = 0;
            snprintf(runtime_migrate_reason, sizeof(runtime_migrate_reason),
                     "no NUMA node IDs are available for migrate_pages");
        } else if (allocated_bytes_in_masks < num_bytes_in_masks) {
            unsigned long *new_dest_mask = realloc(dest_mask, num_bytes_in_masks);
            if (new_dest_mask == NULL) {
                numad_log(LOG_CRIT, "dest bit mask realloc failed\n");
                exit(EXIT_FAILURE);
            }
            dest_mask = new_dest_mask;
            unsigned long *new_from_mask = realloc(from_mask, num_bytes_in_masks);
            if (new_from_mask == NULL) {
                numad_log(LOG_CRIT, "from bit mask realloc failed\n");
                exit(EXIT_FAILURE);
            }
            from_mask = new_from_mask;
            allocated_bytes_in_masks = num_bytes_in_masks;
        }
        if (migration_possible) {
            // In an effort to put semi-balanced memory in each target node, move the
            // contents from the source node with the max amount of memory to the
            // destination node with the least amount of memory.  Repeat until done.
            int prev_from_node_ix = -1;
            for (;;) {
                int min_dest_node_ix = -1;
                int max_from_node_ix = -1;
                for (int node_ix = 0;  (node_ix < num_nodes);  node_ix++) {
                    if (ID_IS_IN_LIST(node_ix, p->node_list_p)) {
                        if ((min_dest_node_ix < 0) || (p->process_MBs[min_dest_node_ix] >= p->process_MBs[node_ix])) {
                            // The ">=" above is intentional, so we tend to move memory to higher numbered nodes
                            min_dest_node_ix = node_ix;
                        }
                    } else {
                        if ((max_from_node_ix < 0) || (p->process_MBs[max_from_node_ix] < p->process_MBs[node_ix])) {
                            max_from_node_ix = node_ix;
                        }
                    }
                }
                if ((max_from_node_ix < 0) || (min_dest_node_ix < 0)
                    || (p->process_MBs[max_from_node_ix] == 0) || (max_from_node_ix == prev_from_node_ix)) {
                    break;
                }
                memset(dest_mask, 0, num_bytes_in_masks);
                memset(from_mask, 0, num_bytes_in_masks);
                SET_BIT(node_id_from_ix(max_from_node_ix), from_mask);
                SET_BIT(node_id_from_ix(min_dest_node_ix), dest_mask);
#if defined(__NR_migrate_pages)
                migration_attempted = 1;
                migration_passes += 1;
                numad_log(LOG_DEBUG, "Moving memory from node: %d to node %d\n",
                          node_id_from_ix(max_from_node_ix), node_id_from_ix(min_dest_node_ix));
                errno = 0;
                int rc = syscall(__NR_migrate_pages, p->pid, numnodes, from_mask, dest_mask);
                numad_log(LOG_DEBUG, "Syscall migrate pages on PID %d, return code %d \n", p->pid, rc);
                if (rc >= 0) {
                    migration_partial_pages += rc;
                } else if (rc < 0) {
                    // Check errno
                    if (errno == ESRCH) {
                        numad_log(LOG_WARNING, "Tried to move PID %d, but it apparently went away.\n", p->pid);
                        return 0;  // Assume the process terminated
                    }
                    if (errno == EFAULT) {
                        numad_log(LOG_WARNING, "Tried to move PID %d, but some memory was out of range.\n", p->pid);
                        return 0;  // Assume the process terminated
                    }
                    if (errno == EINVAL) {
                        numad_log(LOG_WARNING, "Tried to move PID %d, but there are bad parameters.\n", p->pid);
                        return 0;  // Assume the process terminated
                    }
                    if (errno == EPERM) {
                        numad_log(LOG_WARNING, "Tried to move PID %d, but there is insufficient privilege.\n", p->pid);
                        return 0;  // Assume the process terminated
                    }
                    numad_log(LOG_WARNING, "Tried to move PID %d, but migrate_pages failed with errno %d.\n", p->pid, errno);
                    return 0;
                }
#else
                migration_possible = 0;
                snprintf(runtime_migrate_reason, sizeof(runtime_migrate_reason),
                         "__NR_migrate_pages is undefined at build time");
                break;
#endif
                // Assume memory did move for current accounting purposes...
                p->process_MBs[min_dest_node_ix] += p->process_MBs[max_from_node_ix];
                p->process_MBs[max_from_node_ix] = 0;
                prev_from_node_ix = max_from_node_ix;
            }
        }
    }
    // Check pid still active
    snprintf(fname, FNAME_SIZE, "/proc/%d", p->pid);
    if (access(fname, F_OK) < 0) {
        numad_log(LOG_WARNING, "Could not migrate pid %d.  Apparently it went away.\n", p->pid);
        return 0;
    } else {
        uint64_t t1 = get_time_stamp();
        p->bind_time_stamp = t1;
        if (affinity_errors > 0) {
            numad_log(LOG_WARNING, "PID %d affinity target node(s) %s applied with %d task affinity error(s) in %d.%d seconds\n",
                      p->pid, node_list_str, affinity_errors, (t_affinity - t0) / 100, (t_affinity - t0) % 100);
        } else {
            numad_log(LOG_NOTICE, "PID %d affinity applied to node(s) %s in %d.%d seconds\n",
                      p->pid, node_list_str, (t_affinity - t0) / 100, (t_affinity - t0) % 100);
        }
        if (!migration_requested) {
            numad_log(LOG_NOTICE, "PID %d memory migration skipped: %s\n", p->pid,
                      ((migrate_reason != NULL) && (*migrate_reason != '\0')) ? migrate_reason : "policy decision");
        } else if (!migration_possible) {
            numad_log(LOG_NOTICE, "PID %d memory migration skipped: %s\n", p->pid,
                      (runtime_migrate_reason[0] != '\0') ? runtime_migrate_reason : "runtime conditions");
        } else if (!migration_attempted) {
            numad_log(LOG_NOTICE, "PID %d memory migration not needed for target node(s) %s\n",
                      p->pid, node_list_str);
        } else if (migration_partial_pages > 0) {
            numad_log(LOG_NOTICE,
                      "PID %d memory migration partial toward node(s) %s: %d pages could not be moved across %d pass(es) in %d.%d seconds\n",
                      p->pid, node_list_str, migration_partial_pages, migration_passes,
                      (t1 - t_affinity) / 100, (t1 - t_affinity) % 100);
        } else {
            numad_log(LOG_NOTICE,
                      "PID %d memory migration completed toward node(s) %s across %d pass(es) in %d.%d seconds\n",
                      p->pid, node_list_str, migration_passes,
                      (t1 - t_affinity) / 100, (t1 - t_affinity) % 100);
        }
        return 1;
    }
}


static void build_gpu_preferred_nodes(const process_data_p p, id_list_p out) {
    CLEAR_NODE_LIST(out);

    if ((p == NULL) || !(p->flags & PROCESS_FLAG_GPU_ACTIVE) || (p->gpu_list_p == NULL)) {
        return;
    }
    for (int g = 0; g < gpu_count; g++) {
        if (ID_IS_IN_LIST(g, p->gpu_list_p) && (gpu[g].numa_node_ix >= 0)) {
            ADD_ID_TO_LIST(gpu[g].numa_node_ix, out);
        }
    }
}

static void log_gpu_placement_context(const process_data_p p, id_list_p preferred_nodes_p) {
    if ((p == NULL) || !(p->flags & PROCESS_FLAG_GPU_ACTIVE)) {
        return;
    }

    char gpu_nodes[BUF_SIZE];
    if ((preferred_nodes_p != NULL) && (NUM_IDS_IN_LIST(preferred_nodes_p) > 0)) {
        str_from_node_ix_list_as_node_ids(gpu_nodes, sizeof(gpu_nodes), preferred_nodes_p);
    } else {
        snprintf(gpu_nodes, sizeof(gpu_nodes), "none");
    }

    numad_log(LOG_NOTICE,
              "PID %d %s GPU context: kind=%s busy=%u%% vram=%lluMB gpu-node(s) (%s) graphics-placement=%s\n",
              p->pid, process_comm_name(p), gpu_kind_str(p->gpu_kind), p->gpu_busy_pct,
              (unsigned long long)p->gpu_vram_mb, gpu_nodes,
              gpu_graphics_placement_str(gpu_graphics_placement));
}

static id_list_p pick_numa_nodes_core(int pid, int cpus, int mbs, int assume_enough_cpus,
                                      id_list_p candidate_nodes_p) {
    if (log_level >= LOG_DEBUG) {
        numad_log(LOG_DEBUG, "PICK NODES FOR:  PID: %d,  CPUs %d,  MBs %d\n", pid, cpus, mbs);
    }
    char buf[BUF_SIZE];
    uint64_t proc_avg_node_CPUs_free = 0;
    // For existing processes, get miscellaneous process specific details
    int pid_ix;
    process_data_p p = NULL;
    if ((pid > 0) && ((pid_ix = process_hash_lookup(pid)) >= 0)) {
        p = &process_hash_table[pid_ix];
        // Add up per-node memory in use by this process.
        // This scanning is expensive and should be minimized.
        char fname[FNAME_SIZE];
        snprintf(fname, FNAME_SIZE, "/proc/%d/numa_maps", pid);
        FILE *fs = fopen(fname, "r");
        if (!fs) {
            numad_log(LOG_WARNING, "Tried to research PID %d numamaps, but it apparently went away.\n", p->pid);
            return NULL;  // Assume the process terminated
        }
        // Allocate and zero per node memory array.
        // The "+1 node" is for accumulating interleaved memory
        p->process_MBs = realloc(p->process_MBs, (num_nodes + 1) * sizeof(uint64_t));
        if (p->process_MBs == NULL) {
            numad_log(LOG_CRIT, "p->process_MBs realloc failed\n");
            exit(EXIT_FAILURE);
        }
        memset(p->process_MBs, 0, (num_nodes + 1) * sizeof(uint64_t));
        int process_has_interleaved_memory = 0;
        while (fgets(buf, BUF_SIZE, fs)) {
            int interleaved_memory = 0;
            uint64_t page_size = page_size_in_bytes;
            const char *delimiters = " \n";
            char *str_p = strtok(buf, delimiters);
            while (str_p) {
                if (!strncmp(str_p, "interleave", 10)) {
                    interleaved_memory = 1;
                    process_has_interleaved_memory = 1;
                } else if (!strcmp(str_p, "huge")) {
                    page_size = huge_page_size_in_bytes;
                } else if (*str_p++ == 'N') {
                    int node;
                    uint64_t pages;
                    CONVERT_DIGITS_TO_NUM(str_p, node);
                    if (*str_p++ != '=') {
                        numad_log(LOG_CRIT, "numa_maps node number parse error\n");
                        exit(EXIT_FAILURE);
                    }
                    CONVERT_DIGITS_TO_NUM(str_p, pages);
                    int node_ix = node_ix_from_id(node);
                    if (node_ix >= 0) {
                        p->process_MBs[node_ix] += (pages * page_size);
                    }
                    if (interleaved_memory && (node_ix >= 0)) {
                        // sum interleaved quantity in "extra node"
                        p->process_MBs[num_nodes] += (pages * page_size);
                    }
                }
                // Get next token on the line
                str_p = strtok(NULL, delimiters);
            }
        }
        fclose(fs);
        // Start with CPUs already used by process
        // proc_avg_node_CPUs_free = p->CPUs_used;
        proc_avg_node_CPUs_free = ((p->CPUs_used + 35) / 100) * 100 ;
        if (proc_avg_node_CPUs_free < p->CPUs_used) {
            proc_avg_node_CPUs_free = p->CPUs_used;
        }
        for (int ix = 0;  (ix <= num_nodes);  ix++) {
            p->process_MBs[ix] /= MEGABYTE;
            if ((log_level >= LOG_DEBUG) && (p->process_MBs[ix] > 0)) {
                if (ix == num_nodes) {
                    numad_log(LOG_DEBUG, "Interleaved MBs: %ld\n", ix, p->process_MBs[ix]);
                } else {
                    numad_log(LOG_DEBUG, "PROCESS_MBs[node %d]: %ld\n", node_id_from_ix(ix), p->process_MBs[ix]);
                }
            }
            if ((ix < num_nodes) && ID_IS_IN_LIST(ix, p->node_list_p)) {
                proc_avg_node_CPUs_free += node[ix].CPUs_free;
            }
        }
        proc_avg_node_CPUs_free /= NUM_IDS_IN_LIST(p->node_list_p);
        if ((process_has_interleaved_memory) && (keep_interleaved_memory)) {
            // Mark this process as having interleaved memory so we do not
            // merge the interleaved memory.  Time stamp it as done and return.
            p->flags |= PROCESS_FLAG_INTERLEAVED;
            p->bind_time_stamp = get_time_stamp();
            if (log_level >= LOG_DEBUG) {
                numad_log(LOG_DEBUG, "Skipping evaluation of PID %d because of interleaved memory.\n", p->pid);
            }
            return NULL;
        }
    }  // end of existing PID conditional
    // Make a copy of node available resources array.  Add in info specific to
    // this process to equalize available resource quantities wrt locations of
    // resources already in use by this process.
    static node_data_p tmp_node;
    tmp_node = realloc(tmp_node, num_nodes * sizeof(node_data_t) );
    if (tmp_node == NULL) {
        numad_log(LOG_CRIT, "tmp_node realloc failed\n");
        exit(EXIT_FAILURE);
    }
    memcpy(tmp_node, node, num_nodes * sizeof(node_data_t) );
    // Adjust how many MBs and CPUs are available per node and calculate the node magnitude
    uint64_t sum_of_node_CPUs_free = 0;
    for (int ix = 0;  (ix < num_nodes);  ix++) {
        if (pid > 0) {
            if (NUM_IDS_IN_LIST(p->node_list_p) < num_nodes) {
                // If the process is currently bound running on less than all the
                // nodes, first add back (biased) memory already used by this
                // process on this node, then assign average process CPU / node
                // for this process iff the process is present on this node.
                tmp_node[ix].MBs_free += ((p->process_MBs[ix] * 11) / 8);  // Apply heavy mem bias
                if (ID_IS_IN_LIST(ix, p->node_list_p)) {
                    tmp_node[ix].CPUs_free = proc_avg_node_CPUs_free;
                    if (assume_enough_cpus) {
                        long overage = ((tmp_node[ix].active_threads * 100) - tmp_node[ix].CPUs_total);
                        if (overage > 0) {
                            if (log_level >= LOG_DEBUG) {
                                numad_log(LOG_DEBUG, "Reducing Node[%d] CPUs_free by %ld.\n", ix, overage);
                            }
                            if (overage > tmp_node[ix].CPUs_free) {
                                tmp_node[ix].CPUs_free = 0;
                            } else {
                                tmp_node[ix].CPUs_free -= overage;
                            }
                        }
                    }
                }
            } else {
                // Process not yet bound to a subset of nodes.
                // Add back memory used by this process on this node.
                tmp_node[ix].MBs_free += ((p->process_MBs[ix] * 17) / 16);  // Apply light mem bias
                // Add back CPU used by this process in proportion to the memory used on this node.
                tmp_node[ix].CPUs_free += ((p->CPUs_used * p->process_MBs[ix]) / MAX(p->MBs_used, 1));
            }
            if (tmp_node[ix].CPUs_free > tmp_node[ix].CPUs_total) {
                tmp_node[ix].CPUs_free = tmp_node[ix].CPUs_total;
            }
            sum_of_node_CPUs_free += tmp_node[ix].CPUs_free;
            if (tmp_node[ix].MBs_free > tmp_node[ix].MBs_total) {
                tmp_node[ix].MBs_free = tmp_node[ix].MBs_total;
            }
        }
        // Enforce 1/100th CPU minimum
        if (tmp_node[ix].CPUs_free < 1) {
            tmp_node[ix].CPUs_free = 1;
        }
        tmp_node[ix].magnitude = (tmp_node[ix].MBs_free * tmp_node[ix].CPUs_free);
        numad_log(LOG_DEBUG, "Node[%d]:  MBs_free: %ld  CPUs_free: %ld  Magnitude: %ld\n",
            ix, tmp_node[ix].MBs_free, tmp_node[ix].CPUs_free, tmp_node[ix].magnitude);
    }

    // Now figure out where to get resources for this request....
    static id_list_p target_node_list_p;
    CLEAR_NODE_LIST(target_node_list_p);
    if ((candidate_nodes_p == NULL) || (NUM_IDS_IN_LIST(candidate_nodes_p) == 0)) {
        candidate_nodes_p = all_nodes_list_p;
    }
    // Establish a CPU flex fudge factor, on the presumption it is OK if not
    // quite all the CPU request is met.  However, if trying to find resources
    // for pre-placement advice request, do not underestimate the amount of
    // CPUs needed.  Instead, err on the side of providing too many resources.
    int cpu_flex = 0;
    if (pid > 0) {
        if (target_utilization < 100) {
            // Is half of the utilization margin a good amount of CPU flexing?
            cpu_flex = ((100 - target_utilization) * node[0].CPUs_total) / 200;
        } else {
            cpu_flex = 50;  // Just use half a CPU for cpu_flex
        }
    }

    // Use an index to sort NUMA connected resource chain for each node
    int index[num_nodes];
    uint64_t totmag[num_nodes];
    for (int ix = 0;  (ix < num_nodes);  ix++) {
        if (!ID_IS_IN_LIST(ix, candidate_nodes_p)) {
            totmag[ix] = 0;
            continue;
        }
        // Reset the index each time
        for (int n = 0;  (n < num_nodes);  n++) {
            index[n] = n;
        }
        // Sort by minimum relative NUMA distance from node[ix],
        // breaking distance ties with magnitude of available resources
        for (int ij = 0;  (ij < num_nodes);  ij++) {
            int best_ix = ij;
            for (int ik = ij + 1;  (ik < num_nodes);  ik++) {
                int ik_dist = tmp_node[index[ik]].distance[ix];
                int best_ix_dist = tmp_node[index[best_ix]].distance[ix];
                if (best_ix_dist > ik_dist) {
                    best_ix = ik;
                } else if (best_ix_dist == ik_dist) {
                    if (tmp_node[index[best_ix]].magnitude < tmp_node[index[ik]].magnitude ) {
                        best_ix = ik;
                    }
                }
            }
            if (best_ix != ij) {
                int tmp = index[ij];
                index[ij] = index[best_ix];
                index[best_ix] = tmp;
            }
        }

#if 0
        if (log_level >= LOG_DEBUG) {
            for (int iq = 0;  (iq < num_nodes);  iq++) {
                numad_log(LOG_DEBUG, "Node: %d  Dist: %d  Magnitude: %ld\n",
                    tmp_node[index[iq]].node_id, tmp_node[index[iq]].distance[ix], tmp_node[index[iq]].magnitude);
            }
        }
#endif

        // Save the totmag[] sum of the magnitudes of expected needed nodes,
        // "normalized" by NUMA distance (by dividing each magnitude by the
        // relative distance squared).
        totmag[ix] = 0;
        for (int ij = 0;  (ij < num_nodes);  ij++) {
            if (!ID_IS_IN_LIST(index[ij], candidate_nodes_p)) {
                continue;
            }
            int dist = tmp_node[index[ij]].distance[ix];
            totmag[ix] += (tmp_node[index[ij]].magnitude / (dist * dist));
        }
        numad_log(LOG_DEBUG, "Totmag[%d]: %ld\n", ix, totmag[ix]);
    }

    // Now find the best NUMA node based on the normalized sum of node
    // magnitudes expected to be used.
    int best_node_ix = -1;
    for (int ix = 0;  (ix < num_nodes);  ix++) {
        if (!ID_IS_IN_LIST(ix, candidate_nodes_p)) {
            continue;
        }
        if ((best_node_ix < 0) || (totmag[best_node_ix] < totmag[ix])) {
            best_node_ix = ix;
        }
    }
    if (best_node_ix < 0) {
        return target_node_list_p;
    }
    numad_log(LOG_DEBUG, "best_node_ix: %d\n", best_node_ix);
    // Reset sorting index again
    for (int n = 0;  (n < num_nodes);  n++) {
        index[n] = n;
    }
    // Sort index by distance from node[best_node_ix],
    // breaking distance ties with magnitude
    for (int ij = 0;  (ij < num_nodes);  ij++) {
        int best_ix = ij;
        for (int ik = ij + 1;  (ik < num_nodes);  ik++) {
            int ik_dist = tmp_node[index[ik]].distance[best_node_ix];
            int best_ix_dist = tmp_node[index[best_ix]].distance[best_node_ix];
            if (best_ix_dist > ik_dist) {
                best_ix = ik;
            } else if (best_ix_dist == ik_dist) {
                if (tmp_node[index[best_ix]].magnitude < tmp_node[index[ik]].magnitude ) {
                    best_ix = ik;
                }
            }
        }
        if (best_ix != ij) {
            int tmp = index[ij];
            index[ij] = index[best_ix];
            index[best_ix] = tmp;
        }
    }

#if 0
    if (log_level >= LOG_DEBUG) {
        for (int iq = 0;  (iq < num_nodes);  iq++) {
            numad_log(LOG_DEBUG, "Node: %d  Dist: %d  Magnitude: %ld\n",
                tmp_node[index[iq]].node_id, tmp_node[index[iq]].distance[best_node_ix], tmp_node[index[iq]].magnitude);
        }
    }
#endif

    // Allocate more resources until request is met.
    for (int ix = 0;  (ix < num_nodes);  ix++) {
        if (!ID_IS_IN_LIST(index[ix], candidate_nodes_p)) {
            continue;
        }
        if (log_level >= LOG_DEBUG) {
            numad_log(LOG_DEBUG, "MBs: %d,  CPUs: %d\n", mbs, cpus);
        }
        if (tmp_node[index[ix]].CPUs_free > 10) {
            numad_log(LOG_DEBUG, "Assigning resources from node %d\n", tmp_node[index[ix]].node_id);
            ADD_ID_TO_LIST(index[ix], target_node_list_p);
        }
        if (EQUAL_LISTS(target_node_list_p, all_nodes_list_p)) {
            // Apparently we must use all resource nodes...
            break;
        }
        // "Consume" the resources on this node
#define CPUS_MARGIN 0
#define MBS_MARGIN 100
        if (tmp_node[index[ix]].MBs_free >= (mbs + MBS_MARGIN)) {
            tmp_node[index[ix]].MBs_free -= mbs;
            mbs = 0;
        } else {
            if (tmp_node[index[ix]].MBs_free > MBS_MARGIN) {
                mbs -= (tmp_node[index[ix]].MBs_free - MBS_MARGIN);
            }
            tmp_node[index[ix]].MBs_free = MBS_MARGIN;
        }
        if (tmp_node[index[ix]].CPUs_free >= (cpus + CPUS_MARGIN)) {
            tmp_node[index[ix]].CPUs_free -= cpus;
            cpus = 0;
        } else {
            if (tmp_node[index[ix]].CPUs_free > CPUS_MARGIN) {
                cpus -= (tmp_node[index[ix]].CPUs_free - CPUS_MARGIN);
            }
            tmp_node[index[ix]].CPUs_free = CPUS_MARGIN;
        }
        if ((mbs <= 0) && ((cpus <= cpu_flex) || (assume_enough_cpus))) {
            break;
        }
    }
    if ((mbs > 0 || ((cpus > cpu_flex) && !assume_enough_cpus))
        && !EQUAL_LISTS(candidate_nodes_p, all_nodes_list_p)) {
        CLEAR_NODE_LIST(target_node_list_p);
        return target_node_list_p;
    }
    // int avg_mbs_per_node = p->MBs_used / NUM_IDS_IN_LIST(target_node_list_p);

    // For existing processes, calculate the non-local memory percent to see if
    // process is already in the right place.
    if ((pid > 0) && (p != NULL)) {
        uint64_t nonlocal_memory = 0;
        for (int ix = 0;  (ix < num_nodes);  ix++) {
            if (!ID_IS_IN_LIST(ix, target_node_list_p)) {
                // Accumulate total of nonlocal memory
                nonlocal_memory += p->process_MBs[ix];
            }
        }
        int disp_percent = (p->MBs_used > 0) ? ((100 * nonlocal_memory) / p->MBs_used) : 0;
        // If this existing process is already located where we want it, then just
        // return NULL indicating no need to change binding this time.  Check the
        // ammount of nonlocal memory against the target_memlocality_perecent.
        if ((disp_percent <= (100 - target_memlocality)) && (p->bind_time_stamp) && (EQUAL_LISTS(target_node_list_p, p->node_list_p))) {
            // Already bound to targets, and enough of the memory is located where we want it, so no need to rebind
            if (log_level >= LOG_DEBUG) {
                numad_log(LOG_DEBUG, "Process %d already %d percent localized to target nodes.\n", p->pid, 100 - disp_percent);
            }
            p->bind_time_stamp = get_time_stamp();
            return NULL;
        }
    }
    // Must always provide at least one node for pre-placement advice.
    // And hack to unbind when apparently no resources.
    if (NUM_IDS_IN_LIST(target_node_list_p) <= 0) {
        if (!EQUAL_LISTS(candidate_nodes_p, all_nodes_list_p)) {
            return target_node_list_p;
        }
        numad_log(LOG_WARNING, "Empty target node list -- using all nodes.\n");
        COPY_LIST(all_nodes_list_p, target_node_list_p);
    }
  
    // Log advice, and return target node list
    if ((pid > 0) && (p->bind_time_stamp)) {
        str_from_node_ix_list_as_node_ids(buf,  BUF_SIZE, p->node_list_p);
    } else {
        str_from_node_ix_list_as_node_ids(buf,  BUF_SIZE, all_nodes_list_p);
    }
    char buf2[BUF_SIZE];
    str_from_node_ix_list_as_node_ids(buf2, BUF_SIZE, target_node_list_p);
    char *cmd_name = "(unknown)";
    if ((p) && (p->comm)) {
        cmd_name = p->comm;
    }
    numad_log(LOG_NOTICE, "Advising pid %d %s move from nodes (%s) to nodes (%s)\n", pid, cmd_name, buf, buf2);
    if (pid > 0) {
        COPY_LIST(target_node_list_p, p->node_list_p);
    }
    return target_node_list_p;
}

id_list_p pick_numa_nodes(int pid, int cpus, int mbs, int assume_enough_cpus) {
    process_data_p p = NULL;
    int pid_ix = (pid > 0) ? process_hash_lookup(pid) : -1;

    if (pid_ix >= 0) {
        p = &process_hash_table[pid_ix];
    }

    static id_list_p preferred_nodes_p;
    if ((p != NULL) && (p->flags & PROCESS_FLAG_GPU_ACTIVE)) {
        build_gpu_preferred_nodes(p, preferred_nodes_p);
        if (pid > 0) {
            log_gpu_placement_context(p, preferred_nodes_p);
        }
        if ((preferred_nodes_p != NULL) && (NUM_IDS_IN_LIST(preferred_nodes_p) > 0)) {
            char preferred_buf[BUF_SIZE];
            str_from_node_ix_list_as_node_ids(preferred_buf, BUF_SIZE, preferred_nodes_p);
            id_list_p preferred = pick_numa_nodes_core(pid, cpus, mbs,
                                                       assume_enough_cpus,
                                                       preferred_nodes_p);
            if ((preferred != NULL) && (NUM_IDS_IN_LIST(preferred) > 0)) {
                if ((pid > 0) && process_has_graphics_gpu_context(p)
                    && (gpu_graphics_placement != GPU_GRAPHICS_PLACEMENT_AUTO)) {
                    numad_log(LOG_NOTICE,
                              "PID %d %s graphics GPU placement policy %s selected GPU-local node(s) (%s)\n",
                              p->pid, process_comm_name(p),
                              gpu_graphics_placement_str(gpu_graphics_placement), preferred_buf);
                }
                return preferred;
            }
            if ((pid > 0) && process_has_graphics_gpu_context(p)
                && (gpu_graphics_placement == GPU_GRAPHICS_PLACEMENT_STRICT)) {
                numad_log(LOG_NOTICE,
                          "PID %d %s strict graphics GPU placement refused fallback from GPU-local node(s) (%s); keeping current binding\n",
                          p->pid, process_comm_name(p), preferred_buf);
                return preferred;
            }
            if (pid > 0) {
                numad_log(LOG_NOTICE,
                          "PID %d %s GPU-local node(s) (%s) could not satisfy the request; falling back to generic NUMA placement (policy=%s)\n",
                          p->pid, process_comm_name(p), preferred_buf,
                          gpu_graphics_placement_str(gpu_graphics_placement));
            }
        } else if (pid > 0) {
            numad_log(LOG_NOTICE,
                      "PID %d %s has GPU activity but no GPU-local NUMA nodes were discovered; falling back to generic NUMA placement\n",
                      p->pid, process_comm_name(p));
        }
    }

    return pick_numa_nodes_core(pid, cpus, mbs, assume_enough_cpus, all_nodes_list_p);
}



int manage_loads() {
    uint64_t time_stamp = get_time_stamp();
    // Use temporary index to access and sort hash table entries
    static process_data_p *pindex;
    static int pindex_size;

    // Update pindex[] with active candidates.  First, check if we need to resize.
    if (pindex_size < process_hash_table_size) {
        pindex_size = process_hash_table_size;
        pindex = realloc(pindex, pindex_size * sizeof(process_data_p));
        if (pindex == NULL) {
            numad_log(LOG_CRIT, "pindex realloc failed\n");
            exit(EXIT_FAILURE);
        }
        // Quick round trip whenever we resize the hash table.
        // This is mostly to avoid max_interval wait at start up.
        return (min_interval);
    }
    memset(pindex, 0, pindex_size * sizeof(process_data_p));
    // Use pindex to access and sort hash table entries.
    // Copy live candidate pointers to the index for sorting
    // if they meet the threshold for memory usage and CPU usage.
    int nprocs = 0;
    static int old_nprocs;
    static int quick_turn_around;
    long sum_active_threads = 0;
    static long old_sum_active_threads;
    for (int ix = 0;  (ix < process_hash_table_size);  ix++) {
        process_data_p p = &process_hash_table[ix];
        if ((p->pid) && process_is_manageable_now(p, time_stamp)) {
            pindex[nprocs] = p;
            nprocs += 1;
            sum_active_threads += p->num_active_threads;
        }
    }
    int nprocs_changed = (nprocs != old_nprocs);
    int active_threads_changed = (sum_active_threads != old_sum_active_threads);
    old_nprocs = nprocs;
    old_sum_active_threads = sum_active_threads;
    if (active_threads_changed || nprocs_changed) {
        quick_turn_around += 3;
    }
    // Order candidate considerations using timestamps and magnitude: amount of
    // CPU used * amount of memory used.  Not expecting a long list here.  Use
    // a simplistic sort -- however move all not yet bound to front of list and
    // order by decreasing magnitude.  Previously bound processes follow in
    // bins of increasing magnitude treating values within 20% as aquivalent.
    // Within bins, order by bind_time_stamp so oldest bound will be higher
    // priority to evaluate.  Start by moving all never bound to beginning.
    // FIXME: should num_unbound here include those set to all cpus?
    int num_unbound = 0;
    for (int ij = 0;  (ij < nprocs);  ij++) {
        if (pindex[ij]->bind_time_stamp == 0) {
            process_data_p tmp = pindex[num_unbound];
            pindex[num_unbound++] = pindex[ij];
            pindex[ij] = tmp;
        }
    }
    // Sort all unbound so biggest magnitude comes first
    for (int ij = 0;  (ij < num_unbound);  ij++) {
        int best = ij;
        for (int ik = ij + 1;  (ik < num_unbound);  ik++) {
            uint64_t   ik_mag = process_effective_magnitude_now(pindex[ik], time_stamp);
            uint64_t best_mag = process_effective_magnitude_now(pindex[best], time_stamp);
            if (ik_mag <= best_mag) continue;
            best = ik;
        }
        if (best != ij) {
            process_data_p tmp = pindex[ij];
            pindex[ij] = pindex[best];
            pindex[best] = tmp;
        }
    }
    // Sort the remaining candidates into bins of increasting magnitude, and by
    // timestamp within bins.
    for (int ij = num_unbound;  (ij < nprocs);  ij++) {
        int best = ij;
        for (int ik = ij + 1;  (ik < nprocs);  ik++) {
            uint64_t   ik_mag = process_effective_magnitude_now(pindex[ik], time_stamp);
            uint64_t best_mag = process_effective_magnitude_now(pindex[best], time_stamp);
            uint64_t  min_mag = (ik_mag < best_mag) ? ik_mag : best_mag;
            uint64_t diff_mag = abs_u64_diff(best_mag, ik_mag);
            if ((diff_mag > 0) && (min_mag / diff_mag < 5)) {
                // difference > 20 percent.  Use magnitude ordering
                if (ik_mag <= best_mag) continue;
            } else {
                // difference within 20 percent.  Sort these by bind_time_stamp.
                if (pindex[ik]->bind_time_stamp > pindex[best]->bind_time_stamp) continue;
            }
            best = ik;
        }
        if (best != ij) {
            process_data_p tmp = pindex[ij];
            pindex[ij] = pindex[best];
            pindex[best] = tmp;
        }
    }
    // Show the candidate processes in the log file
    if ((log_level >= LOG_INFO) && (nprocs > 0)) {
        numad_log(LOG_INFO, "Candidates: %d\n", nprocs);
        for (int ix = 0;  (ix < nprocs);  ix++) {
            process_data_p p = pindex[ix];
            char buf[BUF_SIZE];
            str_from_node_ix_list_as_node_ids(buf, BUF_SIZE, p->node_list_p);
            fprintf(log_fs, "Timestamp %ld PID %d %s, Flags %x, Threads %ld/%ld, CPU %ld, MBs %ld/%ld, Magnitude %ld, Node(s) %s\n", 
                p->data_time_stamp, p->pid, p->comm, p->flags, p->num_active_threads, p->num_threads, 
                p->CPUs_used, p->MBs_used, p->MBs_size, process_effective_magnitude_now(p, time_stamp), buf);
        }
        fflush(log_fs);
    }

    // Calculate per node active_threads;
    for (int ix = 0;  (ix < num_nodes);  ix++) {
        node[ix].active_threads = 0;
    }
    for (int ij = 0;  (ij < nprocs);  ij++) {
        process_data_p p = pindex[ij];
        if (NUM_IDS_IN_LIST(p->node_list_p) < num_nodes) {
            for (int ix = 0;  (ix < num_nodes);  ix++) {
                if (ID_IS_IN_LIST(ix, p->node_list_p)) {
                    node[ix].active_threads += (p->num_active_threads / NUM_IDS_IN_LIST(p->node_list_p));
                }
            }
        }
    }

    // Estimate desired size (+ margin capacity) and
    // make resource requests for each candidate process
    for (int ix = 0;  (ix < nprocs);  ix++) {
        process_data_p p = pindex[ix];
        // If this process has interleaved memory, recheck it only every 30 minutes...
#define MIN_DELAY_FOR_INTERLEAVE (1800 * ONE_HUNDRED)
        if (((p->flags & PROCESS_FLAG_INTERLEAVED) > 0)
          && (p->bind_time_stamp + MIN_DELAY_FOR_INTERLEAVE > time_stamp)) {
            if (log_level >= LOG_DEBUG) {
                numad_log(LOG_DEBUG, "Skipping evaluation of PID %d because of interleaved memory.\n", p->pid);
            }
            continue;
        }

        // Expand resources needed estimate using target_utilization factor.
        // Start with the CPUs actually used (capped by number of threads) for
        // CPUs required, and the RSS MBs actually used for the MBs
        // requirement, but try to anticipate growing processes, and make sure
        // they are not artificially constrained by the current node bindings.
        int mem_target_utilization = target_utilization;
        int cpu_target_utilization = target_utilization;
        // Cap memory utilization at 100 percent (but allow CPUs to oversubscribe)
        if (mem_target_utilization > 100) {
            mem_target_utilization = 100;
        }
        int mb_request  = (p->MBs_used  * 100) / mem_target_utilization;
        int cpu_request = (p->CPUs_used * 100) / cpu_target_utilization;
        if (cpu_request > p->num_active_threads * 100) {
            cpu_request = p->num_active_threads * 100;
        }
        int boosted = 0;
        int kvm_vcpu_threads = 0;
        // If process looks like a KVM guest, try to limit thread count to the
        // number of vCPU threads. FIXME: Opportunity for IO thread alignment?
        if (strcmp(p->comm, "qemu-kvm") == 0) {
            kvm_vcpu_threads = get_num_kvm_vcpu_threads(p->pid);
            if (cpu_request > kvm_vcpu_threads * 100) {
                cpu_request = kvm_vcpu_threads * 100;
            }
        }
        
        // Check RAM and CPU margin on nodes where PID currently bound
        uint64_t node_MBs_free = 0;
        uint64_t node_MBs_total = 0;
        uint64_t node_CPUs_free = 0;
        uint64_t node_CPUs_total = 0;
        uint64_t other_node_CPUs_free = 0;
        for (int ix = 0;  (ix < num_nodes);  ix++) {
            if (ID_IS_IN_LIST(ix, p->node_list_p)) {
                node_MBs_free += node[ix].MBs_free;
                node_MBs_total += node[ix].MBs_total;
                node_CPUs_free += node[ix].CPUs_free;
                node_CPUs_total += node[ix].CPUs_total;
            } else {
                other_node_CPUs_free += node[ix].CPUs_free;
            }
        }
        if (log_level >= LOG_DEBUG) {
            numad_log(LOG_DEBUG,
            "NODE RESOURCES for  PID %d: node_MBs_tot %ld node_MBs_free %ld node_CPUs_tot %ld node_CPUs_free %ld Other_node_CPUs_Free %ld\n",
                p->pid, node_MBs_total, node_MBs_free, node_CPUs_total, node_CPUs_free, other_node_CPUs_free );
        }

        // If unbound go ahead and try to get resources and bind.
        // Otherwise, if bound, do some extra steps to see if about to run out of resources on node.
        if (NUM_IDS_IN_LIST(p->node_list_p) < num_nodes) {

            // If the process virtual memory size is bigger than the currently
            // assigned node resources, and we already used approximately 95% of
            // the target utilization percent of node memory, then request the
            // potential memory size of the process regardless of current usage.
            if ((p->MBs_size > (p->MBs_used + node_MBs_free)) &&
                ((node_MBs_total - node_MBs_free) * 105 > (node_MBs_total * mem_target_utilization))) {
                mb_request = (p->MBs_size * 100) / mem_target_utilization;
            } else {
                mb_request = (p->MBs_used * 100) / mem_target_utilization;
            }
            // If not a KVM guest and process might be artificially constrained
            if ( (kvm_vcpu_threads == 0) 
                && (cpu_request < p->num_active_threads * 100)
                && (node_CPUs_free < 80) ) {
                    boosted = 1;
                    cpu_request = p->num_active_threads * 100;
            }

            // FIXME: worry about balance here?
            // if (ID_IS_IN_LIST(min_node_CPUs_free_id, p->node_list_p) || ID_IS_IN_LIST(min_node_MBs_free_id, p->node_list_p))
           
        } // bound process

        // See if there should be enough cores
        int assume_enough_cpus = ((sum_active_threads * 100) <= sum_CPUs_total);
        for (int ix = 0;  (ix < num_nodes);  ix++) {
            assume_enough_cpus &= ((p->num_active_threads * 100) <= node[ix].CPUs_total);
        }
        if (assume_enough_cpus) {
            if ((NUM_IDS_IN_LIST(p->node_list_p) > 1)
                || ((node_CPUs_free < 75) && (other_node_CPUs_free >= p->num_active_threads * 100))) {
                    boosted = 1;
                }
        }

        // If this process was recently bound, enforce a minimum delay
        // between repeated attempts to potentially move the process.
        if ((!quick_turn_around) && (!boosted)
            && (p->bind_time_stamp + ((uint64_t)bind_cooldown_sec * ONE_HUNDRED) > time_stamp)) {
            // Skip re-evaluation because we just did it recently,
            if (log_level >= LOG_DEBUG) {
                numad_log(LOG_DEBUG, "Skipping evaluation of PID %d because done too recently.\n", p->pid);
            }
            continue;
        }

        // OK, now pick NUMA nodes for this process and bind it!
        if (log_level >= LOG_DEBUG) {
            numad_log(LOG_DEBUG, "Picking NUMA nodes for PID %d: cpu_reqest %d, mb_request %d, enough_cpus %d\n",
                p->pid, cpu_request, mb_request, assume_enough_cpus);
        }
        pthread_mutex_lock(&node_info_mutex);
        id_list_p node_list_p = pick_numa_nodes(p->pid, cpu_request, mb_request, assume_enough_cpus);
        // check return value same as p->node_list_p
        int rc = 0;
        if ((node_list_p != NULL) && (NUM_IDS_IN_LIST(node_list_p) > 0)) {
            if (scx_mode == SCX_MODE_OBSERVE) {
                numad_log(LOG_NOTICE, "SCX observe mode: PID %d would be rebound now\n", p->pid);
            } else {
                char migrate_reason[BUF_SIZE];
                int migrate_memory = should_migrate_process_memory(p, migrate_reason, sizeof(migrate_reason));
                rc = bind_process_and_maybe_migrate_memory(p, migrate_memory, migrate_reason);
            }
        }
        pthread_mutex_unlock(&node_info_mutex);
        // Return minimum interval when actively moving processes
        if (rc > 0) {
            return min_interval;
        }

    }

    // Return maximum interval when no process movement
    if (quick_turn_around) {
        quick_turn_around -= 1;
        return (2 * min_interval);
    }
    return max_interval;
}







void *set_dynamic_options(void *arg) {
    // int arg_value = *(int *)arg;
    char buf[BUF_SIZE];
    for (;;) {
        // Loop here forever waiting for a msg to do something...
        msg_t msg;
        recv_msg(&msg);
        switch (msg.body.cmd) {
        case 'C':
            use_inactive_file_cache = (msg.body.arg1 != 0);
            if (use_inactive_file_cache) {
                numad_log(LOG_NOTICE, "Counting inactive file cache as available\n");
            } else {
                numad_log(LOG_NOTICE, "Counting inactive file cache as unavailable\n");
            }
            break;
        case 'H':
            thp_scan_sleep_ms = msg.body.arg1;
            set_thp_scan_sleep_ms(thp_scan_sleep_ms);
            break;
        case 'i':
            min_interval = msg.body.arg1;
            max_interval = msg.body.arg2;
            if (max_interval <= 0) {
                shut_down_numad();
            }
            numad_log(LOG_NOTICE, "Changing interval to %d:%d\n", msg.body.arg1, msg.body.arg2);
            break;
        case 'K':
            keep_interleaved_memory = (msg.body.arg1 != 0);
            if (keep_interleaved_memory) {
                numad_log(LOG_NOTICE, "Keeping interleaved memory spread across nodes\n");
            } else {
                numad_log(LOG_NOTICE, "Merging interleaved memory to localized NUMA nodes\n");
            }
            break;
        case 'l':
            numad_log(LOG_NOTICE, "Changing log level to %d\n", msg.body.arg1);
            log_level = msg.body.arg1;
            break;
        case 'm':
            numad_log(LOG_NOTICE, "Changing target memory locality to %d\n", msg.body.arg1);
            target_memlocality = msg.body.arg1;
            break;
        case 'p':
            numad_log(LOG_NOTICE, "Adding PID %d to inclusion PID list\n", msg.body.arg1);
            pthread_mutex_lock(&pid_list_mutex);
            exclude_pid_list = remove_pid_from_pid_list(exclude_pid_list, msg.body.arg1);
            include_pid_list = insert_pid_into_pid_list(include_pid_list, msg.body.arg1);
            pthread_mutex_unlock(&pid_list_mutex);
            break;
        case 'r':
            numad_log(LOG_NOTICE, "Removing PID %d from explicit PID lists\n", msg.body.arg1);
            pthread_mutex_lock(&pid_list_mutex);
            include_pid_list = remove_pid_from_pid_list(include_pid_list, msg.body.arg1);
            exclude_pid_list = remove_pid_from_pid_list(exclude_pid_list, msg.body.arg1);
            pthread_mutex_unlock(&pid_list_mutex);
            break;
        case 'S':
            scan_all_processes = (msg.body.arg1 != 0);
            if (scan_all_processes) {
                numad_log(LOG_NOTICE, "Scanning all processes\n");
            } else {
                numad_log(LOG_NOTICE, "Scanning only explicit PID list processes\n");
            }
            break;
        case 't':
            numad_log(LOG_NOTICE, "Changing logical CPU thread percent to %d\n", msg.body.arg1);
            htt_percent = msg.body.arg1;
            node_info_time_stamp = 0; // to force rescan of nodes/cpus soon
            break;
        case 'u':
            numad_log(LOG_NOTICE, "Changing target utilization to %d\n", msg.body.arg1);
            target_utilization = msg.body.arg1;
            break;
        case 'w':
            numad_log(LOG_NOTICE, "Getting NUMA pre-placement advice for %d CPUs and %d MBs\n",
                                    msg.body.arg1, msg.body.arg2);
            pthread_mutex_lock(&node_info_mutex);
            update_nodes();
            id_list_p node_list_p = pick_numa_nodes(-1, msg.body.arg1, msg.body.arg2, 0);
            str_from_node_ix_list_as_node_ids(buf, BUF_SIZE, node_list_p);
            pthread_mutex_unlock(&node_info_mutex);
            send_msg(msg.body.src_pid, 'w', 0, 0, buf);
            break;
        case 'x':
            numad_log(LOG_NOTICE, "Adding PID %d to exclusion PID list\n", msg.body.arg1);
            pthread_mutex_lock(&pid_list_mutex);
            include_pid_list = remove_pid_from_pid_list(include_pid_list, msg.body.arg1);
            exclude_pid_list = insert_pid_into_pid_list(exclude_pid_list, msg.body.arg1);
            pthread_mutex_unlock(&pid_list_mutex);
            break;
        default:
            numad_log(LOG_WARNING, "Unexpected msg command: %c %d %d %s from PID %d\n",
                                    msg.body.cmd, msg.body.arg1, msg.body.arg1, msg.body.text,
                                    msg.body.src_pid);
            break;
        }
    }  // for (;;)
}



void parse_two_arg_values(char *p, int *first_ptr, int *second_ptr, int first_is_optional, int first_scale_digits) {
    char *orig_p = p;
    char *q = NULL;
    int second = -1;
    errno = 0;
    int first = (int) strtol(p, &p, 10);
    if ((errno != 0) || (p == orig_p) || (first < 0)) {
        fprintf(stderr, "Can't parse arg value(s): %s\n", orig_p);
        exit(EXIT_FAILURE);
    }
    if (*p == '.') {
        p++;
        while ((first_scale_digits > 0) && (isdigit(*p))) {
            first *= 10;
            first += (*p++ - '0');
            first_scale_digits -= 1;
        }
        while (isdigit(*p)) { p++; }
    }
    while (first_scale_digits > 0) {
        first *= 10;
        first_scale_digits -= 1;
    }
    if (*p == ':') {
        q = p + 1;
        errno = 0;
        second = (int) strtol(q, &p, 10);
        if ((errno != 0) || (p == q) || (second < 0)) {
            fprintf(stderr, "Can't parse arg value(s): %s\n", orig_p);
            exit(EXIT_FAILURE);
        }
    }
    if (q != NULL) {
        // Two numbers are present
        if (first_ptr  != NULL) *first_ptr = first;
        if (second_ptr != NULL) *second_ptr = second;
    } else if (first_is_optional) {
        if (second_ptr != NULL) *second_ptr = first;
    } else {
        if (first_ptr != NULL) *first_ptr = first;
    }
}

enum {
    OPT_GPU_AWARE = 1000,
    OPT_GPU_BACKEND,
    OPT_GPU_MIN_BUSY,
    OPT_GPU_MIN_VRAM,
    OPT_GPU_FDINFO_DISCOVERY,
    OPT_GPU_MIGRATE,
    OPT_GPU_MIGRATE_BUSY_MAX,
    OPT_GPU_GRAPHICS_PLACEMENT,
    OPT_SCX_MODE,
    OPT_SCX_SCHED,
    OPT_BIND_COOLDOWN,
};

static const struct option long_opts[] = {
    {"gpu-aware", required_argument, NULL, OPT_GPU_AWARE},
    {"gpu-backend", required_argument, NULL, OPT_GPU_BACKEND},
    {"gpu-min-busy", required_argument, NULL, OPT_GPU_MIN_BUSY},
    {"gpu-min-vram", required_argument, NULL, OPT_GPU_MIN_VRAM},
    {"gpu-fdinfo-discovery", required_argument, NULL, OPT_GPU_FDINFO_DISCOVERY},
    {"gpu-migrate", required_argument, NULL, OPT_GPU_MIGRATE},
    {"gpu-migrate-busy-max", required_argument, NULL, OPT_GPU_MIGRATE_BUSY_MAX},
    {"gpu-graphics-placement", required_argument, NULL, OPT_GPU_GRAPHICS_PLACEMENT},
    {"scx-mode", required_argument, NULL, OPT_SCX_MODE},
    {"scx-sched", required_argument, NULL, OPT_SCX_SCHED},
    {"bind-cooldown", required_argument, NULL, OPT_BIND_COOLDOWN},
    {0, 0, 0, 0}
};

static int parse_nonneg_int_arg(const char *s, int *out) {
    if ((s == NULL) || (out == NULL)) {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if ((errno != 0) || (end == s) || (*end != '\0') || (v < 0) || (v > INT_MAX)) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

static void parse_startup_long_option_or_exit(int opt, const char *arg) {
    int value = 0;

    startup_only_longopt_seen = 1;

    switch (opt) {
    case OPT_GPU_AWARE:
        if (parse_nonneg_int_arg(arg, &value) < 0 || (value != 0 && value != 1)) {
            fprintf(stderr, "--gpu-aware must be 0 or 1\n");
            exit(EXIT_FAILURE);
        }
        gpu_aware = value;
        break;
    case OPT_GPU_BACKEND:
        if (strcmp(arg, "auto") == 0) {
            gpu_backend = GPU_BACKEND_AUTO;
        } else if (strcmp(arg, "fdinfo") == 0) {
            gpu_backend = GPU_BACKEND_FDINFO;
        } else if (strcmp(arg, "amdsmi") == 0) {
            gpu_backend = GPU_BACKEND_AMDSMI;
        } else {
            fprintf(stderr, "--gpu-backend must be auto, fdinfo, or amdsmi\n");
            exit(EXIT_FAILURE);
        }
        break;
    case OPT_GPU_MIN_BUSY:
        if (parse_nonneg_int_arg(arg, &value) < 0) {
            fprintf(stderr, "--gpu-min-busy must be a non-negative integer\n");
            exit(EXIT_FAILURE);
        }
        gpu_min_busy_pct = value;
        break;
    case OPT_GPU_MIN_VRAM:
        if (parse_nonneg_int_arg(arg, &value) < 0) {
            fprintf(stderr, "--gpu-min-vram must be a non-negative integer\n");
            exit(EXIT_FAILURE);
        }
        gpu_min_vram_mb = value;
        break;
    case OPT_GPU_FDINFO_DISCOVERY:
        if (parse_nonneg_int_arg(arg, &value) < 0 || value < 1) {
            fprintf(stderr, "--gpu-fdinfo-discovery must be >= 1\n");
            exit(EXIT_FAILURE);
        }
        gpu_fdinfo_discovery_interval = value;
        break;
    case OPT_GPU_MIGRATE:
        if (strcmp(arg, "auto") == 0) {
            gpu_migrate_policy = MIGRATE_AUTO;
        } else if (strcmp(arg, "always") == 0) {
            gpu_migrate_policy = MIGRATE_ALWAYS;
        } else if (strcmp(arg, "never") == 0) {
            gpu_migrate_policy = MIGRATE_NEVER;
        } else {
            fprintf(stderr, "--gpu-migrate must be auto, always, or never\n");
            exit(EXIT_FAILURE);
        }
        break;
    case OPT_GPU_MIGRATE_BUSY_MAX:
        if (parse_nonneg_int_arg(arg, &value) < 0) {
            fprintf(stderr, "--gpu-migrate-busy-max must be a non-negative integer\n");
            exit(EXIT_FAILURE);
        }
        gpu_migrate_busy_max = value;
        break;
    case OPT_GPU_GRAPHICS_PLACEMENT:
        if (strcmp(arg, "auto") == 0) {
            gpu_graphics_placement = GPU_GRAPHICS_PLACEMENT_AUTO;
        } else if (strcmp(arg, "prefer") == 0) {
            gpu_graphics_placement = GPU_GRAPHICS_PLACEMENT_PREFER;
        } else if (strcmp(arg, "strict") == 0) {
            gpu_graphics_placement = GPU_GRAPHICS_PLACEMENT_STRICT;
        } else {
            fprintf(stderr, "--gpu-graphics-placement must be auto, prefer, or strict\n");
            exit(EXIT_FAILURE);
        }
        break;
    case OPT_SCX_MODE:
        if (strcmp(arg, "legacy") == 0) {
            scx_mode = SCX_MODE_LEGACY;
        } else if (strcmp(arg, "cooperate") == 0) {
            scx_mode = SCX_MODE_COOPERATE;
        } else if (strcmp(arg, "observe") == 0) {
            scx_mode = SCX_MODE_OBSERVE;
        } else {
            fprintf(stderr, "--scx-mode must be legacy, cooperate, or observe\n");
            exit(EXIT_FAILURE);
        }
        break;
    case OPT_SCX_SCHED:
        if (strcmp(arg, "auto") == 0) {
            scx_sched = SCX_SCHED_NONE;
        } else if (strcmp(arg, "beerland") == 0) {
            scx_sched = SCX_SCHED_BEERLAND;
        } else if (strcmp(arg, "p2dq") == 0) {
            scx_sched = SCX_SCHED_P2DQ;
        } else {
            scx_sched = SCX_SCHED_OTHER;
        }
        break;
    case OPT_BIND_COOLDOWN:
        if (parse_nonneg_int_arg(arg, &value) < 0 || value < 1) {
            fprintf(stderr, "--bind-cooldown must be >= 1\n");
            exit(EXIT_FAILURE);
        }
        bind_cooldown_sec = value;
        break;
    default:
        fprintf(stderr, "Unsupported long option\n");
        exit(EXIT_FAILURE);
    }
}



int main(int argc, char *argv[]) {
    int opt;
    int C_flag = 0;
    int d_flag = 0;
    int H_flag = 0;
    int i_flag = 0;
    int K_flag = 0;
    int l_flag = 0;
    int m_flag = 0;
    int p_flag = 0;
    int r_flag = 0;
    int S_flag = 0;
    int t_flag = 0;
    int u_flag = 0;
    int v_flag = 0;
    int w_flag = 0;
    int x_flag = 0;
    int tmp_int = 0;
    long list_pid = 0;
    while ((opt = getopt_long(argc, argv, "C:dD:hH:i:K:l:m:p:r:R:S:t:u:vVw:x:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'C':
            C_flag = 1;
            use_inactive_file_cache = (atoi(optarg) != 0);
            break;
        case 'd':
            d_flag = 1;
            log_level = LOG_DEBUG;
            break;
        case 'D':
            // obsoleted
            break;
        case 'h':
            print_usage_and_exit(argv[0]);
            break;
        case 'H':
            tmp_int = atoi(optarg);
            if ((tmp_int == 0) || ((tmp_int > 9) && (tmp_int < 1000001))) {
                // 0 means do not change the system default value
                H_flag = 1;
                thp_scan_sleep_ms = tmp_int;
            } else {
                fprintf(stderr, "THP scan_sleep_ms must be > 9 and < 1000001\n");
                exit(EXIT_FAILURE);
            }
            break;
        case 'i':
            i_flag = 1;
            parse_two_arg_values(optarg, &min_interval, &max_interval, 1, 0);
            break;
        case 'K':
            K_flag = 1;
            keep_interleaved_memory = (atoi(optarg) != 0);
            break;
        case 'l':
            l_flag = 1;
            log_level = atoi(optarg);
            break;
        case 'm':
            tmp_int = atoi(optarg);
            if ((tmp_int >= 50) && (tmp_int <= 100)) {
                m_flag = 1;
                target_memlocality = tmp_int;
            }
            break;
        case 'p':
            p_flag = 1;
            list_pid = atol(optarg);
            exclude_pid_list = remove_pid_from_pid_list(exclude_pid_list, list_pid);
            include_pid_list = insert_pid_into_pid_list(include_pid_list, list_pid);
            break;
        case 'r':
            r_flag = 1;
            list_pid = atol(optarg);
            // Remove this PID from both explicit pid lists.
            include_pid_list = remove_pid_from_pid_list(include_pid_list, list_pid);
            exclude_pid_list = remove_pid_from_pid_list(exclude_pid_list, list_pid);
            break;
        case 'R':
            reserved_cpu_str = strdup(optarg);
            break;
        case 'S':
            S_flag = 1;
            scan_all_processes = (atoi(optarg) != 0);
            break;
        case 't':
            tmp_int = atoi(optarg);
            if ((tmp_int >= 0) && (tmp_int <= 100)) {
                t_flag = 1;
                htt_percent = tmp_int;
            }
            break;
        case 'u':
            tmp_int = atoi(optarg);
            if ((tmp_int >= 10) && (tmp_int <= 130)) {
                u_flag = 1;
                target_utilization = tmp_int;
            }
            break;
        case 'v':
            v_flag = 1;
            log_level = LOG_INFO;
            break;
        case 'V':
            print_version_and_exit(argv[0]);
            break;
        case 'w':
            w_flag = 1;
            parse_two_arg_values(optarg, &requested_cpus, &requested_mbs, 0, 2);
            break;
        case 'x':
            x_flag = 1;
            list_pid = atol(optarg);
            include_pid_list = remove_pid_from_pid_list(include_pid_list, list_pid);
            exclude_pid_list = insert_pid_into_pid_list(exclude_pid_list, list_pid);
            break;
        case OPT_GPU_AWARE:
        case OPT_GPU_BACKEND:
        case OPT_GPU_MIN_BUSY:
        case OPT_GPU_MIN_VRAM:
        case OPT_GPU_FDINFO_DISCOVERY:
        case OPT_GPU_MIGRATE:
        case OPT_GPU_MIGRATE_BUSY_MAX:
        case OPT_GPU_GRAPHICS_PLACEMENT:
        case OPT_SCX_MODE:
        case OPT_SCX_SCHED:
        case OPT_BIND_COOLDOWN:
            parse_startup_long_option_or_exit(opt, optarg);
            break;
        default:
            print_usage_and_exit(argv[0]);
            break;
        }
    }
    if (argc > optind) {
        fprintf(stderr, "Unexpected arg = %s\n", argv[optind]);
        exit(EXIT_FAILURE);
    }
    if (i_flag) {
        if ((max_interval < min_interval) && (max_interval != 0)) {
            fprintf(stderr, "Max interval (%d) must be greater than min interval (%d)\n", max_interval, min_interval);
            exit(EXIT_FAILURE);
        }
    }
    init_run_file();
    open_log_file();
    init_msg_queue();
    num_cpus = get_num_cpus();
    page_size_in_bytes = sysconf(_SC_PAGESIZE);
    huge_page_size_in_bytes = get_huge_page_size_in_bytes();
    // Figure out if this is the daemon, or a subsequent invocation
    int daemon_pid = get_daemon_pid(1);
    if (daemon_pid > 0) {
        if (startup_only_longopt_seen) {
            fprintf(stderr,
                    "GPU/SCX long options (including --gpu-graphics-placement) and --bind-cooldown are startup-only. Edit /etc/numad.conf and restart the numad service.\n");
            close_log_file();
            exit(EXIT_FAILURE);
        }
        // Daemon is already running.  So send dynamic options to persistant
        // thread to handle requests, get the response (if any), and finish.
        msg_t msg; 
        if (C_flag) {
            send_msg(daemon_pid, 'C', use_inactive_file_cache, 0, "");
        }
        if (H_flag) {
            send_msg(daemon_pid, 'H', thp_scan_sleep_ms, 0, "");
        }
        if (i_flag) {
            send_msg(daemon_pid, 'i', min_interval, max_interval, "");
        }
        if (K_flag) {
            send_msg(daemon_pid, 'K', keep_interleaved_memory, 0, "");
        }
        if (d_flag || l_flag || v_flag) {
            send_msg(daemon_pid, 'l', log_level, 0, "");
        }
        if (m_flag) {
            send_msg(daemon_pid, 'm', target_memlocality, 0, "");
        }
        if (p_flag) {
            send_msg(daemon_pid, 'p', list_pid, 0, "");
        }
        if (r_flag) {
            send_msg(daemon_pid, 'r', list_pid, 0, "");
        }
        if (S_flag) {
            send_msg(daemon_pid, 'S', scan_all_processes, 0, "");
        }
        if (t_flag) {
            send_msg(daemon_pid, 't', htt_percent, 0, "");
        }
        if (u_flag) {
            send_msg(daemon_pid, 'u', target_utilization, 0, "");
        }
        if (w_flag) {
            send_msg(daemon_pid, 'w', requested_cpus, requested_mbs, "");
            recv_msg(&msg);
            fprintf(stdout, "%s\n", msg.body.text);
        }
        if (x_flag) {
            send_msg(daemon_pid, 'x', list_pid, 0, "");
        }
        close_log_file();
        exit(EXIT_SUCCESS);
    }
    // No numad daemon running yet.
    // First, make note of any reserved CPUs....
    if (reserved_cpu_str != NULL) {
        CLEAR_CPU_LIST(reserved_cpu_mask_list_p);
        int n = add_ids_to_list_from_str(reserved_cpu_mask_list_p, reserved_cpu_str);
        char buf[BUF_SIZE];
        str_from_id_list(buf, BUF_SIZE, reserved_cpu_mask_list_p);
        numad_log(LOG_NOTICE, "Reserving %d CPUs (%s) for non-numad use\n", n, buf);
        // turn reserved list into a negated mask for later ANDing use...
        negate_cpu_list(reserved_cpu_mask_list_p);
    }
    // If it is a "-w" pre-placement request, handle that without starting
    // the daemon.  Otherwise start the numad daemon.
    if (w_flag) {
        // Get pre-placement NUMA advice without starting daemon
        update_nodes();
        sleep(2);
        update_nodes();
        numad_log(LOG_NOTICE, "Getting NUMA pre-placement advice for %d CPUs and %d MBs\n", requested_cpus, requested_mbs);
        id_list_p node_list_p = pick_numa_nodes(-1, requested_cpus, requested_mbs, 0);
        char buf[BUF_SIZE];
        str_from_node_ix_list_as_node_ids(buf, BUF_SIZE, node_list_p);
        fprintf(stdout, "%s\n", buf);
        close_log_file();
        exit(EXIT_SUCCESS);
    } else if (max_interval > 0) {
        // Start the numad daemon...
        check_prereqs(argv[0]);
#if (!NO_DAEMON)
        // Daemonize self...
        daemon_pid = fork();
        if (daemon_pid < 0) { numad_log(LOG_CRIT, "fork() failed\n"); exit(EXIT_FAILURE); }
        // Parent process now exits
        if (daemon_pid > 0) { exit(EXIT_SUCCESS); }
        // Child process continues...
        umask(S_IWGRP | S_IWOTH); // Reset the file mode
        int sid = setsid();  // Start a new session
        if (sid < 0) { numad_log(LOG_CRIT, "setsid() failed\n"); exit(EXIT_FAILURE); }
        if ((chdir("/")) < 0) { numad_log(LOG_CRIT, "chdir() failed"); exit(EXIT_FAILURE); }
        daemon_pid = register_numad_pid();
        if (daemon_pid != getpid()) {
            numad_log(LOG_CRIT, "Could not register daemon PID\n");
            exit(EXIT_FAILURE);
        }
        fclose(stdin);
        fclose(stdout);
        if (log_fs != stderr) {
            fclose(stderr);
        }
#endif
        // Set up signal handlers
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa)); 
        sa.sa_handler = sig_handler;
        if (sigaction(SIGHUP, &sa, NULL)
            || sigaction(SIGTERM, &sa, NULL)
            || sigaction(SIGQUIT, &sa, NULL)) {
            numad_log(LOG_CRIT, "sigaction does not work?\n");
            exit(EXIT_FAILURE);
        }
        // Allocate initial process hash table
        process_hash_table_expand();
        // Spawn a thread to handle messages from subsequent invocation requests
        pthread_mutex_init(&pid_list_mutex, NULL);
        pthread_mutex_init(&node_info_mutex, NULL);
        pthread_attr_t attr;
        if (pthread_attr_init(&attr) != 0) {
            numad_log(LOG_CRIT, "pthread_attr_init failure\n");
            exit(EXIT_FAILURE);
        }
#if (!NO_DAEMON)
        pthread_t tid;
        if (pthread_create(&tid, &attr, &set_dynamic_options, NULL) != 0) {
            numad_log(LOG_CRIT, "pthread_create failure: setting thread\n");
            exit(EXIT_FAILURE);
        }
#endif
        // Loop here forwever...
        for (;;) {
            uint64_t cycle_ts = get_time_stamp();
            int interval = max_interval;
            pthread_mutex_lock(&node_info_mutex);
            int nodes = update_nodes();
            pthread_mutex_unlock(&node_info_mutex);
            if (log_level >= LOG_INFO) {
                show_nodes(nodes);
            }
            int processes = update_processes(cycle_ts);
            update_gpu_processes(cycle_ts);
            refresh_scx_state();
            if (log_level >= LOG_INFO) {
                show_processes(processes);
            }
            if (nodes > 1) {
                interval = manage_loads();
            }
#if (!NO_DAEMON)
            sleep(interval);
#endif
            if (got_sigterm || got_sigquit) {
                shut_down_numad();
            }
            if (got_sighup) {
                got_sighup = 0;
                close_log_file();
                open_log_file();
            }
        }
        if (pthread_attr_destroy(&attr) != 0) {
            numad_log(LOG_WARNING, "pthread_attr_destroy failure\n");
        }
        pthread_mutex_destroy(&pid_list_mutex);
        pthread_mutex_destroy(&node_info_mutex);
    }
    exit(EXIT_SUCCESS);
}

