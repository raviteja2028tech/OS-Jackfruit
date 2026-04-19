/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Implementation covers Tasks 1-3 and partial Task 4:
 *   - Multi-container supervisor with clone() + namespaces
 *   - UNIX domain socket control-plane IPC
 *   - Bounded-buffer logging pipeline (producer/consumer threads)
 *   - Signal handling (SIGCHLD, SIGINT, SIGTERM)
 *   - Kernel monitor integration via ioctl
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */
#define STACK_SIZE          (1024 * 1024)
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define LOG_DIR             "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN   256
#define LOG_CHUNK_SIZE      4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT  (40UL << 20)
#define DEFAULT_HARD_LIMIT  (64UL << 20)
#define MAX_CONTAINERS      64
/* max args we'll split a command string into inside the container */
#define MAX_ARGS            32

/* ------------------------------------------------------------------ */
/* Enumerations                                                        */
/* ------------------------------------------------------------------ */
typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,   /* graceful stop via engine stop           */
    CONTAINER_KILLED,    /* hard-limit kill from kernel monitor     */
    CONTAINER_EXITED     /* process exited on its own               */
} container_state_t;

/* ------------------------------------------------------------------ */
/* Data structures                                                     */
/* ------------------------------------------------------------------ */

typedef struct container_record {
    char            id[CONTAINER_ID_LEN];
    pid_t           host_pid;
    time_t          started_at;
    container_state_t state;
    unsigned long   soft_limit_bytes;
    unsigned long   hard_limit_bytes;
    int             exit_code;
    int             exit_signal;
    int             stop_requested;   /* set before we signal the container */
    char            log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head;
    size_t          tail;
    size_t          count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t  kind;
    char            container_id[CONTAINER_ID_LEN];
    char            rootfs[PATH_MAX];
    char            command[CHILD_COMMAND_LEN];
    unsigned long   soft_limit_bytes;
    unsigned long   hard_limit_bytes;
    int             nice_value;
} control_request_t;

typedef struct {
    int  status;         /* 0 = ok, non-zero = error */
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

/*
 * Passed through the clone() stack into the container child.
 * Must remain valid until child_fn() returns.
 */
typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  log_write_fd;   /* write-end of the stdout/stderr pipe    */
    int  ready_pipe[2];  /* child writes 0 when setup is complete  */
} child_config_t;

/*
 * Per-container producer thread argument.
 */
typedef struct {
    char            container_id[CONTAINER_ID_LEN];
    int             read_fd;          /* read-end of the container's pipe */
    bounded_buffer_t *buffer;
} producer_arg_t;

typedef struct {
    int              server_fd;
    int              monitor_fd;
    volatile int     should_stop;
    pthread_t        logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t  metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* ------------------------------------------------------------------ */
/* Globals (supervisor side only)                                      */
/* ------------------------------------------------------------------ */

/* Pointer to the supervisor context – used by signal handlers. */
static supervisor_ctx_t *g_ctx = NULL;

/* ------------------------------------------------------------------ */
/* Utilities                                                           */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;
    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc, char *argv[], int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nv;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nv = strtol(argv[i+1], &end, 10);
            if (errno != 0 || end == argv[i+1] || *end != '\0' ||
                nv < -20 || nv > 19) {
                fprintf(stderr, "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i+1]);
                return -1;
            }
            req->nice_value = (int)nv;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Bounded buffer                                                      */
/* ------------------------------------------------------------------ */

static int bounded_buffer_init(bounded_buffer_t *buf)
{
    int rc;
    memset(buf, 0, sizeof(*buf));
    rc = pthread_mutex_init(&buf->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&buf->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buf->mutex); return rc; }
    rc = pthread_cond_init(&buf->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buf->not_empty);
        pthread_mutex_destroy(&buf->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buf)
{
    pthread_cond_destroy(&buf->not_full);
    pthread_cond_destroy(&buf->not_empty);
    pthread_mutex_destroy(&buf->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buf)
{
    pthread_mutex_lock(&buf->mutex);
    buf->shutting_down = 1;
    pthread_cond_broadcast(&buf->not_empty);
    pthread_cond_broadcast(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
}

/*
 * bounded_buffer_push – producer side.
 *
 * Blocks while the buffer is full.  Returns 0 on success, -1 if shutdown
 * has begun (caller should stop producing).
 *
 * Race condition without the mutex: two producers could read the same
 * tail index, both see count < CAPACITY, and both write to the same slot,
 * causing one log line to silently overwrite the other.
 * The condition variable prevents busy-waiting on a full buffer.
 */
int bounded_buffer_push(bounded_buffer_t *buf, const log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);

    /* Wait until there is space, or we are shutting down. */
    while (buf->count == LOG_BUFFER_CAPACITY && !buf->shutting_down)
        pthread_cond_wait(&buf->not_full, &buf->mutex);

    if (buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }

    buf->items[buf->tail] = *item;
    buf->tail = (buf->tail + 1) % LOG_BUFFER_CAPACITY;
    buf->count++;

    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/*
 * bounded_buffer_pop – consumer side.
 *
 * Returns 1 on success (item filled), 0 when shutdown and buffer empty
 * (consumer should exit).
 *
 * Race condition without the mutex: consumer could read a slot that a
 * concurrent producer has not finished writing, yielding partial data.
 */
int bounded_buffer_pop(bounded_buffer_t *buf, log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);

    /* Wait while empty, but keep draining if shutting down with data. */
    while (buf->count == 0) {
        if (buf->shutting_down) {
            pthread_mutex_unlock(&buf->mutex);
            return 0;   /* signal consumer to exit */
        }
        pthread_cond_wait(&buf->not_empty, &buf->mutex);
    }

    *item = buf->items[buf->head];
    buf->head = (buf->head + 1) % LOG_BUFFER_CAPACITY;
    buf->count--;

    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Logging consumer thread                                             */
/* ------------------------------------------------------------------ */

/*
 * logging_thread – single consumer that drains the bounded buffer and
 * writes each chunk to the container's per-container log file.
 *
 * Routing: container_id embedded in each log_item_t determines the
 * file path (logs/<id>.log).  Files are opened on first use and kept
 * open for the lifetime of the thread.
 *
 * Shutdown: bounded_buffer_pop returns 0 when the buffer is empty AND
 * shutting_down is set; at that point we flush and exit.
 */
void *logging_thread(void *arg)
{
    bounded_buffer_t *buf = (bounded_buffer_t *)arg;
    log_item_t item;
    int rc;

    /* Simple open-file cache: id -> fd (up to MAX_CONTAINERS entries). */
    struct { char id[CONTAINER_ID_LEN]; int fd; } cache[MAX_CONTAINERS];
    int cache_size = 0;

    /* Ensure log directory exists. */
    mkdir(LOG_DIR, 0755);

    while (1) {
        rc = bounded_buffer_pop(buf, &item);
        if (rc == 0)
            break;   /* shutdown and buffer drained */

        /* Find or open the log file for this container. */
        int log_fd = -1;
        for (int i = 0; i < cache_size; i++) {
            if (strncmp(cache[i].id, item.container_id, CONTAINER_ID_LEN) == 0) {
                log_fd = cache[i].fd;
                break;
            }
        }
        if (log_fd == -1 && cache_size < MAX_CONTAINERS) {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
            log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (log_fd >= 0) {
                strncpy(cache[cache_size].id, item.container_id, CONTAINER_ID_LEN - 1);
                cache[cache_size].fd = log_fd;
                cache_size++;
            }
        }

        if (log_fd >= 0) {
            ssize_t written = 0, total = (ssize_t)item.length;
            while (written < total) {
                ssize_t n = write(log_fd, item.data + written,
                                  (size_t)(total - written));
                if (n <= 0) break;
                written += n;
            }
        }
    }

    /* Close all open log files. */
    for (int i = 0; i < cache_size; i++)
        close(cache[i].fd);

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Producer thread (one per container)                                 */
/* ------------------------------------------------------------------ */

/*
 * producer_thread – reads from the container's pipe and pushes chunks
 * into the shared bounded buffer until EOF (container exited).
 */
static void *producer_thread(void *arg)
{
    producer_arg_t *pa = (producer_arg_t *)arg;
    log_item_t item;
    ssize_t n;

    while (1) {
        n = read(pa->read_fd, item.data, LOG_CHUNK_SIZE - 1);
        if (n <= 0)
            break;   /* EOF or error – container exited */

        item.data[n] = '\0';
        item.length = (size_t)n;
        strncpy(item.container_id, pa->container_id, CONTAINER_ID_LEN - 1);
        item.container_id[CONTAINER_ID_LEN - 1] = '\0';

        if (bounded_buffer_push(pa->buffer, &item) < 0)
            break;   /* supervisor shutting down */
    }

    close(pa->read_fd);
    free(pa);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Container child function (runs inside clone'd namespace)            */
/* ------------------------------------------------------------------ */

/*
 * child_fn – entrypoint for the cloned container process.
 *
 * Sequence:
 *   1. Set hostname to container ID (UTS namespace).
 *   2. Mount the new root filesystem.
 *   3. Mount /proc inside the container.
 *   4. chroot into the rootfs.
 *   5. Redirect stdout/stderr to the log pipe.
 *   6. Signal the supervisor that setup is complete.
 *   7. exec() the requested command.
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
    char *argv_exec[MAX_ARGS + 1];
    char cmd_copy[CHILD_COMMAND_LEN];
    int i = 0;

    /* 1. UTS: set hostname */
    sethostname(cfg->id, strlen(cfg->id));

    /* 2. Mount namespace: make rootfs a private bind mount so our
     *    pivot_root / chroot doesn't affect the host.             */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        perror("mount --make-rprivate /");
        /* non-fatal for chroot approach */
    }

    /* 3. Mount /proc inside the container's rootfs */
    char proc_path[PATH_MAX + 6];
    snprintf(proc_path, sizeof(proc_path), "%s/proc", cfg->rootfs);
    mkdir(proc_path, 0555);
    if (mount("proc", proc_path, "proc", 0, NULL) < 0)
        perror("mount proc");   /* non-fatal; container can still run */

    /* 4. chroot into the container's own rootfs */
    if (chroot(cfg->rootfs) < 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("chdir /");
        return 1;
    }

    /* 5. Redirect stdout and stderr to the log pipe */
    if (cfg->log_write_fd >= 0) {
        dup2(cfg->log_write_fd, STDOUT_FILENO);
        dup2(cfg->log_write_fd, STDERR_FILENO);
        close(cfg->log_write_fd);
    }

    /* 6. Signal supervisor: setup done */
    if (cfg->ready_pipe[1] >= 0) {
        char ok = 0;
        write(cfg->ready_pipe[1], &ok, 1);
        close(cfg->ready_pipe[1]);
    }
    if (cfg->ready_pipe[0] >= 0)
        close(cfg->ready_pipe[0]);

    /* 7. Apply nice value if requested */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* 8. Parse and exec the command */
    strncpy(cmd_copy, cfg->command, sizeof(cmd_copy) - 1);
    cmd_copy[sizeof(cmd_copy) - 1] = '\0';

    char *tok = strtok(cmd_copy, " \t");
    while (tok && i < MAX_ARGS) {
        argv_exec[i++] = tok;
        tok = strtok(NULL, " \t");
    }
    argv_exec[i] = NULL;

    if (i == 0) {
        fprintf(stderr, "child_fn: empty command\n");
        return 1;
    }

    execvp(argv_exec[0], argv_exec);
    perror("execvp");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Monitor registration helpers                                        */
/* ------------------------------------------------------------------ */

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes  = soft_limit_bytes;
    req.hard_limit_bytes  = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd,
                            const char *container_id,
                            pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Container metadata helpers (must be called with metadata_lock held) */
/* ------------------------------------------------------------------ */

static container_record_t *find_container(supervisor_ctx_t *ctx,
                                          const char *id)
{
    container_record_t *r = ctx->containers;
    while (r) {
        if (strncmp(r->id, id, CONTAINER_ID_LEN) == 0)
            return r;
        r = r->next;
    }
    return NULL;
}

static container_record_t *alloc_container(supervisor_ctx_t *ctx,
                                           const control_request_t *req)
{
    container_record_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    strncpy(r->id,       req->container_id, CONTAINER_ID_LEN - 1);
    r->soft_limit_bytes = req->soft_limit_bytes;
    r->hard_limit_bytes = req->hard_limit_bytes;
    r->state            = CONTAINER_STARTING;
    r->started_at       = time(NULL);
    snprintf(r->log_path, sizeof(r->log_path),
             "%s/%s.log", LOG_DIR, req->container_id);

    /* Prepend to the linked list */
    r->next = ctx->containers;
    ctx->containers = r;
    return r;
}

/* ------------------------------------------------------------------ */
/* SIGCHLD handler – reap children, update metadata                   */
/* ------------------------------------------------------------------ */

/*
 * We use a self-pipe so the signal handler is async-signal-safe:
 * the handler writes one byte; the event loop reads it and calls waitpid.
 */
static int g_sigchld_pipe[2] = {-1, -1};
static int g_shutdown_pipe[2] = {-1, -1};

static void sigchld_handler(int sig)
{
    (void)sig;
    char byte = 'C';
    write(g_sigchld_pipe[1], &byte, 1);
}

static void sigterm_handler(int sig)
{
    (void)sig;
    char byte = 'T';
    write(g_shutdown_pipe[1], &byte, 1);
}

/*
 * reap_children – called from the event loop when SIGCHLD arrives.
 * Updates metadata: distinguishes normal exit, manual stop, and hard-
 * limit kill per the spec's attribution rule.
 */
static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *r = ctx->containers;
        while (r) {
            if (r->host_pid == pid) {
                if (WIFEXITED(status)) {
                    r->exit_code   = WEXITSTATUS(status);
                    r->exit_signal = 0;
                    r->state = r->stop_requested
                               ? CONTAINER_STOPPED
                               : CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    r->exit_signal = WTERMSIG(status);
                    r->exit_code   = 0;
                    /* Attribution rule from spec:
                     * - stop_requested → STOPPED (graceful stop)
                     * - SIGKILL without stop_requested → KILLED (hard limit)
                     * - other signal → EXITED                              */
                    if (r->stop_requested)
                        r->state = CONTAINER_STOPPED;
                    else if (r->exit_signal == SIGKILL)
                        r->state = CONTAINER_KILLED;
                    else
                        r->state = CONTAINER_EXITED;
                }
                /* Unregister from kernel monitor */
                if (ctx->monitor_fd >= 0)
                    unregister_from_monitor(ctx->monitor_fd, r->id, pid);
                break;
            }
            r = r->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

/* ------------------------------------------------------------------ */
/* Launch a container                                                  */
/* ------------------------------------------------------------------ */

/*
 * launch_container – allocate stack, create pipe, clone(), and start
 * a producer thread for the container's log output.
 *
 * Returns the new container_record_t on success, NULL on failure.
 * Called with metadata_lock held.
 */
static container_record_t *launch_container(supervisor_ctx_t *ctx,
                                             const control_request_t *req)
{
    /* 1. Check for duplicate ID */
    if (find_container(ctx, req->container_id)) {
        fprintf(stderr, "Container '%s' already exists\n", req->container_id);
        return NULL;
    }

    /* 2. Allocate record */
    container_record_t *rec = alloc_container(ctx, req);
    if (!rec) return NULL;

    /* 3. Create pipe: container writes, supervisor reads */
    int pipefds[2];
    if (pipe(pipefds) < 0) {
        perror("pipe");
        free(rec);
        ctx->containers = ctx->containers->next;
        return NULL;
    }

    /* 4. Ready pipe so we know namespace setup finished */
    int ready[2];
    if (pipe(ready) < 0) {
        perror("pipe ready");
        close(pipefds[0]); close(pipefds[1]);
        free(rec);
        ctx->containers = ctx->containers->next;
        return NULL;
    }

    /* 5. Prepare child config on the heap (child_fn frees nothing; it execs) */
    child_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        close(pipefds[0]); close(pipefds[1]);
        close(ready[0]);   close(ready[1]);
        free(rec);
        ctx->containers = ctx->containers->next;
        return NULL;
    }
    strncpy(cfg->id,      req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,  req->rootfs,        PATH_MAX - 1);
    strncpy(cfg->command, req->command,       CHILD_COMMAND_LEN - 1);
    cfg->nice_value   = req->nice_value;
    cfg->log_write_fd = pipefds[1];
    cfg->ready_pipe[0] = ready[0];
    cfg->ready_pipe[1] = ready[1];

    /* 6. Allocate clone stack */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc stack");
        close(pipefds[0]); close(pipefds[1]);
        close(ready[0]);   close(ready[1]);
        free(cfg);
        free(rec);
        ctx->containers = ctx->containers->next;
        return NULL;
    }

    /* 7. clone() with PID, UTS, and mount namespaces */
    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(child_fn, stack + STACK_SIZE, clone_flags, cfg);
    if (pid < 0) {
        perror("clone");
        free(stack);
        close(pipefds[0]); close(pipefds[1]);
        close(ready[0]);   close(ready[1]);
        free(cfg);
        free(rec);
        ctx->containers = ctx->containers->next;
        return NULL;
    }

    /* 8. Close write-end in supervisor; close child's ready write-end */
    close(pipefds[1]);
    close(ready[1]);

    /* 9. Wait for child to finish namespace setup */
    char ok;
    read(ready[0], &ok, 1);
    close(ready[0]);

    /* 10. Update metadata */
    rec->host_pid = pid;
    rec->state    = CONTAINER_RUNNING;

    /* 11. Register with kernel monitor */
    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, rec->id, pid,
                              rec->soft_limit_bytes, rec->hard_limit_bytes);

    /* 12. Spawn producer thread for this container */
    producer_arg_t *pa = calloc(1, sizeof(*pa));
    if (pa) {
        strncpy(pa->container_id, rec->id, CONTAINER_ID_LEN - 1);
        pa->read_fd = pipefds[0];
        pa->buffer  = &ctx->log_buffer;
        pthread_t tid;
        if (pthread_create(&tid, NULL, producer_thread, pa) == 0)
            pthread_detach(tid);
        else {
            close(pipefds[0]);
            free(pa);
        }
    } else {
        close(pipefds[0]);
    }

    /* stack is leaked intentionally (child exec'd; we can't free it from
     * supervisor because the child might still be using it until exec).
     * A production runtime would track and free it after exec. */
    (void)stack;

    return rec;
}

/* ------------------------------------------------------------------ */
/* Handle a single control request from a CLI client                  */
/* ------------------------------------------------------------------ */

static void handle_start(supervisor_ctx_t *ctx,
                         const control_request_t *req,
                         control_response_t *resp)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *rec = launch_container(ctx, req);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!rec) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "Failed to start container '%s'", req->container_id);
    } else {
        resp->status = 0;
        snprintf(resp->message, sizeof(resp->message),
                 "Started container '%s' pid=%d", req->container_id, rec->host_pid);
    }
}

static void handle_run(supervisor_ctx_t *ctx,
                       const control_request_t *req,
                       int client_fd)
{
    control_response_t resp = {0};

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *rec = launch_container(ctx, req);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!rec) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "Failed to start container '%s'", req->container_id);
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }

    pid_t pid = rec->host_pid;
    char id[CONTAINER_ID_LEN];
    strncpy(id, req->container_id, CONTAINER_ID_LEN - 1);

    /* Send an in-progress acknowledgement */
    resp.status = 0;
    snprintf(resp.message, sizeof(resp.message),
             "Running container '%s' pid=%d (waiting…)", id, pid);
    send(client_fd, &resp, sizeof(resp), 0);

    /* Wait for the container to finish */
    int wstatus;
    waitpid(pid, &wstatus, 0);

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *r = find_container(ctx, id);
    if (r) {
        if (WIFEXITED(wstatus)) {
            r->exit_code   = WEXITSTATUS(wstatus);
            r->exit_signal = 0;
            r->state = CONTAINER_EXITED;
        } else if (WIFSIGNALED(wstatus)) {
            r->exit_signal = WTERMSIG(wstatus);
            r->exit_code   = 0;
            r->state = r->stop_requested ? CONTAINER_STOPPED : CONTAINER_KILLED;
        }
        if (ctx->monitor_fd >= 0)
            unregister_from_monitor(ctx->monitor_fd, id, pid);
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Send final status */
    memset(&resp, 0, sizeof(resp));
    resp.status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus)
                                     : (128 + WTERMSIG(wstatus));
    snprintf(resp.message, sizeof(resp.message),
             "Container '%s' finished with exit_code=%d", id, resp.status);
    send(client_fd, &resp, sizeof(resp), 0);
}

static void handle_ps(supervisor_ctx_t *ctx, control_response_t *resp)
{
    char buf[4096] = {0};
    int  off = 0;

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *r = ctx->containers;
    off += snprintf(buf + off, sizeof(buf) - off,
                    "%-16s %-8s %-10s %-10s %-12s %-12s\n",
                    "ID", "PID", "STATE", "EXIT",
                    "SOFT-MIB", "HARD-MIB");
    while (r && off < (int)sizeof(buf) - 1) {
        off += snprintf(buf + off, sizeof(buf) - off,
                        "%-16s %-8d %-10s %-10d %-12lu %-12lu\n",
                        r->id,
                        r->host_pid,
                        state_to_string(r->state),
                        r->exit_code,
                        r->soft_limit_bytes >> 20,
                        r->hard_limit_bytes >> 20);
        r = r->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    resp->status = 0;
    strncpy(resp->message, buf, sizeof(resp->message) - 1);
}

static void handle_logs(supervisor_ctx_t *ctx,
                        const control_request_t *req,
                        int client_fd)
{
    char path[PATH_MAX];
    control_response_t resp = {0};

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *r = find_container(ctx, req->container_id);
    if (r)
        strncpy(path, r->log_path, PATH_MAX - 1);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!r) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "No container '%s'", req->container_id);
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }

    /* Read the log file and send it back in chunks */
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "Log file not found");
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }

    /* First send a header response */
    resp.status = 0;
    snprintf(resp.message, sizeof(resp.message), "LOG:ok");
    send(client_fd, &resp, sizeof(resp), 0);

    /* Then stream the file contents */
    char fbuf[4096];
    ssize_t n;
    while ((n = read(fd, fbuf, sizeof(fbuf))) > 0)
        send(client_fd, fbuf, (size_t)n, 0);
    close(fd);
}

static void handle_stop(supervisor_ctx_t *ctx,
                        const control_request_t *req,
                        control_response_t *resp)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *r = find_container(ctx, req->container_id);
    if (!r) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "No container '%s'", req->container_id);
        pthread_mutex_unlock(&ctx->metadata_lock);
        return;
    }
    if (r->state != CONTAINER_RUNNING) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "Container '%s' is not running (state=%s)",
                 r->id, state_to_string(r->state));
        pthread_mutex_unlock(&ctx->metadata_lock);
        return;
    }

    /* Set stop_requested BEFORE sending the signal (spec requirement). */
    r->stop_requested = 1;
    kill(r->host_pid, SIGTERM);

    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message),
             "SIGTERM sent to container '%s' (pid=%d)", r->id, r->host_pid);
    pthread_mutex_unlock(&ctx->metadata_lock);
}

/* ------------------------------------------------------------------ */
/* Supervisor event loop                                               */
/* ------------------------------------------------------------------ */

static void handle_one_client(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t req;
    control_response_t resp;

    ssize_t n = recv(client_fd, &req, sizeof(req), MSG_WAITALL);
    if (n != (ssize_t)sizeof(req)) {
        close(client_fd);
        return;
    }

    memset(&resp, 0, sizeof(resp));

    switch (req.kind) {
    case CMD_START:
        handle_start(ctx, &req, &resp);
        send(client_fd, &resp, sizeof(resp), 0);
        break;

    case CMD_RUN:
        handle_run(ctx, &req, client_fd);
        break;

    case CMD_PS:
        handle_ps(ctx, &resp);
        send(client_fd, &resp, sizeof(resp), 0);
        break;

    case CMD_LOGS:
        handle_logs(ctx, &req, client_fd);
        break;

    case CMD_STOP:
        handle_stop(ctx, &req, &resp);
        send(client_fd, &resp, sizeof(resp), 0);
        break;

    default:
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "Unknown command");
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    close(client_fd);
}

/* ------------------------------------------------------------------ */
/* Supervisor main                                                     */
/* ------------------------------------------------------------------ */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    int rc;

    (void)rootfs;   /* base rootfs noted but containers use their own copies */

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    /* -- metadata lock ------------------------------------------------ */
    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { errno = rc; perror("pthread_mutex_init"); return 1; }

    /* -- bounded buffer ----------------------------------------------- */
    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) { errno = rc; perror("bounded_buffer_init"); return 1; }

    /* -- log directory ------------------------------------------------ */
    mkdir(LOG_DIR, 0755);

    /* -- open kernel monitor (optional: don't fail if absent) --------- */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "Warning: /dev/container_monitor not available (module not loaded?)\n");

    /* -- self-pipe for SIGCHLD ---------------------------------------- */
    if (pipe(g_sigchld_pipe) < 0)  { perror("pipe sigchld"); return 1; }
    if (pipe(g_shutdown_pipe) < 0) { perror("pipe shutdown"); return 1; }
    fcntl(g_sigchld_pipe[0],  F_SETFL, O_NONBLOCK);
    fcntl(g_shutdown_pipe[0], F_SETFL, O_NONBLOCK);

    struct sigaction sa_chld = { .sa_handler = sigchld_handler,
                                 .sa_flags   = SA_RESTART | SA_NOCLDSTOP };
    sigemptyset(&sa_chld.sa_mask);
    sigaction(SIGCHLD, &sa_chld, NULL);

    struct sigaction sa_term = { .sa_handler = sigterm_handler,
                                 .sa_flags   = SA_RESTART };
    sigemptyset(&sa_term.sa_mask);
    sigaction(SIGINT,  &sa_term, NULL);
    sigaction(SIGTERM, &sa_term, NULL);

    /* -- UNIX domain socket ------------------------------------------- */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(ctx.server_fd, 8) < 0) {
        perror("listen"); return 1;
    }

    /* -- logger thread ------------------------------------------------ */
    rc = pthread_create(&ctx.logger_thread, NULL,
                        logging_thread, &ctx.log_buffer);
    if (rc != 0) { errno = rc; perror("pthread_create logger"); return 1; }

    fprintf(stderr, "[supervisor] Ready. Listening on %s\n", CONTROL_PATH);

    /* -- event loop --------------------------------------------------- */
    while (!ctx.should_stop) {
        fd_set rfds;
        int maxfd;

        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd,        &rfds);
        FD_SET(g_sigchld_pipe[0],    &rfds);
        FD_SET(g_shutdown_pipe[0],   &rfds);
        maxfd = ctx.server_fd;
        if (g_sigchld_pipe[0]  > maxfd) maxfd = g_sigchld_pipe[0];
        if (g_shutdown_pipe[0] > maxfd) maxfd = g_shutdown_pipe[0];

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int sel = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        /* SIGCHLD notification */
        if (FD_ISSET(g_sigchld_pipe[0], &rfds)) {
            char b[16];
            read(g_sigchld_pipe[0], b, sizeof(b));
            reap_children(&ctx);
        }

        /* SIGINT / SIGTERM notification */
        if (FD_ISSET(g_shutdown_pipe[0], &rfds)) {
            fprintf(stderr, "[supervisor] Shutdown signal received.\n");
            ctx.should_stop = 1;
            break;
        }

        /* New CLI client */
        if (FD_ISSET(ctx.server_fd, &rfds)) {
            int client_fd = accept(ctx.server_fd, NULL, NULL);
            if (client_fd >= 0)
                handle_one_client(&ctx, client_fd);
        }
    }

    /* -- orderly shutdown --------------------------------------------- */
    fprintf(stderr, "[supervisor] Shutting down…\n");

    /* Stop all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *r = ctx.containers;
    while (r) {
        if (r->state == CONTAINER_RUNNING) {
            r->stop_requested = 1;
            kill(r->host_pid, SIGTERM);
        }
        r = r->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Give them a moment, then SIGKILL stragglers */
    sleep(2);
    pthread_mutex_lock(&ctx.metadata_lock);
    r = ctx.containers;
    while (r) {
        if (r->state == CONTAINER_RUNNING)
            kill(r->host_pid, SIGKILL);
        r = r->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Drain SIGCHLD */
    sleep(1);
    reap_children(&ctx);

    /* Shut down logger */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    /* Free container records */
    pthread_mutex_lock(&ctx.metadata_lock);
    r = ctx.containers;
    while (r) {
        container_record_t *next = r->next;
        free(r);
        r = next;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);
    pthread_mutex_destroy(&ctx.metadata_lock);

    /* Close FDs */
    if (ctx.server_fd >= 0) close(ctx.server_fd);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    close(g_sigchld_pipe[0]); close(g_sigchld_pipe[1]);
    close(g_shutdown_pipe[0]); close(g_shutdown_pipe[1]);
    unlink(CONTROL_PATH);

    fprintf(stderr, "[supervisor] Clean exit.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* CLI client – send_control_request                                   */
/* ------------------------------------------------------------------ */

/*
 * send_control_request – connect to the supervisor's UNIX domain socket,
 * send the control_request_t, receive and print the response.
 *
 * For CMD_RUN the supervisor sends two responses: an ACK and a final
 * status when the container finishes.
 * For CMD_LOGS the supervisor sends a header response followed by raw
 * file data until it closes the connection.
 */
static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(fd);
        return 1;
    }

    if (send(fd, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send");
        close(fd);
        return 1;
    }

    /* Receive responses */
    if (req->kind == CMD_LOGS) {
        /* First packet is a header */
        ssize_t n = recv(fd, &resp, sizeof(resp), MSG_WAITALL);
        if (n == (ssize_t)sizeof(resp)) {
            if (resp.status != 0) {
                fprintf(stderr, "Error: %s\n", resp.message);
                close(fd);
                return 1;
            }
            /* Stream log data to stdout */
            char buf[4096];
            while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
                fwrite(buf, 1, (size_t)n, stdout);
        }
    } else if (req->kind == CMD_RUN) {
        /* ACK */
        recv(fd, &resp, sizeof(resp), MSG_WAITALL);
        if (resp.status < 0) {
            fprintf(stderr, "Error: %s\n", resp.message);
            close(fd);
            return 1;
        }
        fprintf(stderr, "%s\n", resp.message);
        /* Final status */
        recv(fd, &resp, sizeof(resp), MSG_WAITALL);
        printf("%s\n", resp.message);
        close(fd);
        return resp.status;
    } else {
        recv(fd, &resp, sizeof(resp), MSG_WAITALL);
        if (resp.status != 0)
            fprintf(stderr, "Error: %s\n", resp.message);
        else
            printf("%s\n", resp.message);
    }

    close(fd);
    return resp.status != 0 ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* CLI command handlers                                                */
/* ------------------------------------------------------------------ */

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs)        - 1);
    strncpy(req.command,      argv[4], sizeof(req.command)       - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs)        - 1);
    strncpy(req.command,      argv[4], sizeof(req.command)       - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
