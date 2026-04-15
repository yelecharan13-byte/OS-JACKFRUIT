/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Complete implementation: all TODOs filled in.
 *
 * Architecture:
 *   - Supervisor: long-running daemon that holds a UNIX domain socket,
 *     manages container lifecycle, reaps children, and runs a logger thread.
 *   - Client: any invocation of engine with start/run/ps/logs/stop sends
 *     a control_request_t over the UNIX socket and reads a control_response_t.
 *   - Logger thread: consumer of the bounded_buffer; writes chunks to per-
 *     container log files under LOG_DIR/.
 *   - Each container: spawned via clone(2) with PID+UTS+mount namespaces,
 *     chrooted into its rootfs, stdout/stderr piped back to the supervisor.
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
#include <sys/syscall.h>
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
#define MONITOR_DEV         "/dev/container_monitor"

/* ------------------------------------------------------------------ */
/* Types                                                               */
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
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* ------------------------------------------------------------------ */
/* Global supervisor context (used by signal handlers)                */
/* ------------------------------------------------------------------ */
static supervisor_ctx_t *g_ctx = NULL;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
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
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
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
/* Bounded Buffer                                                      */
/* ------------------------------------------------------------------ */
static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;
    memset(buffer, 0, sizeof(*buffer));
    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buffer->mutex); return rc; }
    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * bounded_buffer_push — producer side.
 *
 * Blocks while the buffer is full (unless shutdown begins).
 * Returns 0 on success, -1 if shutdown started before we could push.
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * bounded_buffer_pop — consumer side.
 *
 * Blocks while the buffer is empty.
 * Returns 0 on success, 1 if shutdown is in progress AND buffer is empty
 * (consumer should exit).
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return 1; /* shutdown, nothing left */
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Logger Thread                                                       */
/* ------------------------------------------------------------------ */

/*
 * logging_thread — drains log chunks from the bounded buffer and writes
 * them to per-container log files.  Runs until shutdown is signalled AND
 * the buffer is empty.
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    int rc;

    while (1) {
        rc = bounded_buffer_pop(&ctx->log_buffer, &item);
        if (rc != 0)
            break; /* shutdown + empty */

        /* Find the log path for this container */
        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/%s.log",
                 LOG_DIR, item.container_id);

        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            /* Best-effort: fall back to stderr */
            fprintf(stderr, "[logger] cannot open %s: %s\n",
                    log_path, strerror(errno));
            fwrite(item.data, 1, item.length, stderr);
            continue;
        }

        size_t written = 0;
        while (written < item.length) {
            ssize_t n = write(fd, item.data + written, item.length - written);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            written += (size_t)n;
        }
        close(fd);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Log pipe reader — runs in its own thread per container             */
/* ------------------------------------------------------------------ */
typedef struct {
    int pipe_fd;
    char container_id[CONTAINER_ID_LEN];
    supervisor_ctx_t *ctx;
} pipe_reader_args_t;

static void *pipe_reader_thread(void *arg)
{
    pipe_reader_args_t *args = (pipe_reader_args_t *)arg;
    log_item_t item;
    ssize_t n;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, args->container_id,
            sizeof(item.container_id) - 1);

    while ((n = read(args->pipe_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        bounded_buffer_push(&args->ctx->log_buffer, &item);
        memset(item.data, 0, sizeof(item.data));
    }

    close(args->pipe_fd);
    free(args);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Container child entrypoint                                         */
/* ------------------------------------------------------------------ */

/*
 * child_fn — runs after clone(2).
 *
 * Sets up the container environment:
 *   1. Redirect stdout/stderr to the write end of the log pipe.
 *   2. Set hostname to the container ID (UTS namespace).
 *   3. Mount /proc inside the new rootfs.
 *   4. chroot into the rootfs.
 *   5. Apply nice value.
 *   6. exec the requested command.
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
    char proc_path[PATH_MAX];

    /* 1. Redirect stdout + stderr to the log pipe */
    if (cfg->log_write_fd >= 0) {
        dup2(cfg->log_write_fd, STDOUT_FILENO);
        dup2(cfg->log_write_fd, STDERR_FILENO);
        close(cfg->log_write_fd);
    }

    /* 2. Set hostname to container ID */
    if (sethostname(cfg->id, strlen(cfg->id)) < 0)
        perror("sethostname");

    /* 3. Mount /proc so commands like ps work inside the container */
    snprintf(proc_path, sizeof(proc_path), "%s/proc", cfg->rootfs);
    if (mkdir(proc_path, 0555) < 0 && errno != EEXIST)
        perror("mkdir /proc");

    if (mount("proc", proc_path, "proc",
              MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) < 0)
        perror("mount proc");

    /* 4. chroot into the container rootfs */
    if (chroot(cfg->rootfs) < 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("chdir /");
        return 1;
    }

    /* 5. Apply nice value */
    if (cfg->nice_value != 0) {
        if (nice(cfg->nice_value) == -1 && errno != 0)
            perror("nice");
    }

    /* 6. Execute the command via /bin/sh -c "..." for flexibility */
    execl("/bin/sh", "sh", "-c", cfg->command, (char *)NULL);

    /* If exec fails, try direct execution */
    execlp(cfg->command, cfg->command, (char *)NULL);

    perror("exec");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Monitor ioctl helpers                                               */
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
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
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
/* Spawn a container                                                   */
/* ------------------------------------------------------------------ */
static container_record_t *spawn_container(supervisor_ctx_t *ctx,
                                            const control_request_t *req)
{
    static char stack[STACK_SIZE];
    int pipefd[2];
    pid_t child_pid;
    container_record_t *record;
    child_config_t *cfg;

    /* Check for duplicate ID */
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *existing = ctx->containers;
    while (existing) {
        if (strcmp(existing->id, req->container_id) == 0 &&
            existing->state == CONTAINER_RUNNING) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            fprintf(stderr, "Container '%s' already running\n", req->container_id);
            return NULL;
        }
        existing = existing->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Create log directory */
    mkdir(LOG_DIR, 0755);

    /* Create pipe for container stdout/stderr */
    if (pipe2(pipefd, O_CLOEXEC) < 0) {
        perror("pipe2");
        return NULL;
    }

    /* Build child config */
    cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }
    strncpy(cfg->id, req->container_id, sizeof(cfg->id) - 1);
    strncpy(cfg->rootfs, req->rootfs, sizeof(cfg->rootfs) - 1);
    strncpy(cfg->command, req->command, sizeof(cfg->command) - 1);
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1]; /* write end goes to child */

    /* Spawn container in new PID + UTS + mount namespaces */
    child_pid = clone(child_fn, stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      cfg);
    if (child_pid < 0) {
        perror("clone");
        free(cfg);
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }

    /* Close the write end in the parent */
    close(pipefd[1]);

    /* Spawn a pipe-reader thread that feeds the log buffer */
    pipe_reader_args_t *reader_args = malloc(sizeof(*reader_args));
    if (reader_args) {
        reader_args->pipe_fd = pipefd[0];
        reader_args->ctx = ctx;
        strncpy(reader_args->container_id, req->container_id,
                sizeof(reader_args->container_id) - 1);
        pthread_t reader_tid;
        pthread_create(&reader_tid, NULL, pipe_reader_thread, reader_args);
        pthread_detach(reader_tid);
    } else {
        close(pipefd[0]);
    }

    /* Register with kernel monitor */
    if (ctx->monitor_fd >= 0) {
        if (register_with_monitor(ctx->monitor_fd, req->container_id,
                                  child_pid, req->soft_limit_bytes,
                                  req->hard_limit_bytes) < 0)
            perror("register_with_monitor");
    }

    /* Build and insert container record */
    record = calloc(1, sizeof(*record));
    if (!record) {
        free(cfg);
        return NULL;
    }
    strncpy(record->id, req->container_id, sizeof(record->id) - 1);
    record->host_pid         = child_pid;
    record->started_at       = time(NULL);
    record->state            = CONTAINER_RUNNING;
    record->soft_limit_bytes = req->soft_limit_bytes;
    record->hard_limit_bytes = req->hard_limit_bytes;
    snprintf(record->log_path, sizeof(record->log_path),
             "%s/%s.log", LOG_DIR, req->container_id);

    pthread_mutex_lock(&ctx->metadata_lock);
    record->next     = ctx->containers;
    ctx->containers  = record;
    pthread_mutex_unlock(&ctx->metadata_lock);

    free(cfg); /* child has already used this (clone copies stack) */
    return record;
}

/* ------------------------------------------------------------------ */
/* SIGCHLD handler — reap children, update state                      */
/* ------------------------------------------------------------------ */
static void sigchld_handler(int sig)
{
    (void)sig;
    if (!g_ctx) return;

    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *rec = g_ctx->containers;
        while (rec) {
            if (rec->host_pid == pid) {
                if (WIFEXITED(status)) {
                    rec->state     = CONTAINER_EXITED;
                    rec->exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    rec->state      = (WTERMSIG(status) == SIGKILL)
                                       ? CONTAINER_KILLED
                                       : CONTAINER_STOPPED;
                    rec->exit_signal = WTERMSIG(status);
                }
                /* Unregister from monitor */
                if (g_ctx->monitor_fd >= 0)
                    unregister_from_monitor(g_ctx->monitor_fd,
                                            rec->id, pid);
                break;
            }
            rec = rec->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx) g_ctx->should_stop = 1;
}

/* ------------------------------------------------------------------ */
/* Handle one client connection                                        */
/* ------------------------------------------------------------------ */
static void handle_client(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t req;
    control_response_t resp;
    ssize_t n;

    memset(&resp, 0, sizeof(resp));

    n = recv(client_fd, &req, sizeof(req), MSG_WAITALL);
    if (n != (ssize_t)sizeof(req)) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "bad request size");
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }

    switch (req.kind) {

    case CMD_START:
    case CMD_RUN: {
        container_record_t *rec = spawn_container(ctx, &req);
        if (!rec) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "Failed to start container '%s'", req.container_id);
        } else {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "Started container '%s' pid=%d",
                     rec->id, rec->host_pid);

            /* CMD_RUN: wait for the container to finish */
            if (req.kind == CMD_RUN) {
                int status;
                waitpid(rec->host_pid, &status, 0);
                if (WIFEXITED(status)) {
                    rec->state     = CONTAINER_EXITED;
                    rec->exit_code = WEXITSTATUS(status);
                    snprintf(resp.message, sizeof(resp.message),
                             "Container '%s' exited code=%d",
                             rec->id, rec->exit_code);
                } else if (WIFSIGNALED(status)) {
                    rec->state = CONTAINER_KILLED;
                    rec->exit_signal = WTERMSIG(status);
                    snprintf(resp.message, sizeof(resp.message),
                             "Container '%s' killed signal=%d",
                             rec->id, rec->exit_signal);
                }
                if (ctx->monitor_fd >= 0)
                    unregister_from_monitor(ctx->monitor_fd,
                                            rec->id, rec->host_pid);
            }
        }
        break;
    }

    case CMD_PS: {
        /* Build a text table of all containers */
        char buf[4096] = {0};
        int off = 0;
        off += snprintf(buf + off, sizeof(buf) - off,
                        "%-16s %-8s %-10s %-26s\n",
                        "ID", "PID", "STATE", "STARTED");

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *rec = ctx->containers;
        while (rec && off < (int)sizeof(buf) - 1) {
            char ts[26];
            ctime_r(&rec->started_at, ts);
            ts[24] = '\0'; /* strip newline */
            off += snprintf(buf + off, sizeof(buf) - off,
                            "%-16s %-8d %-10s %-26s\n",
                            rec->id, rec->host_pid,
                            state_to_string(rec->state), ts);
            rec = rec->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        resp.status = 0;
        strncpy(resp.message, buf, sizeof(resp.message) - 1);
        break;
    }

    case CMD_LOGS: {
        /* Read and return the log file contents */
        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/%s.log",
                 LOG_DIR, req.container_id);

        int fd = open(log_path, O_RDONLY);
        if (fd < 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "No logs found for '%s'", req.container_id);
            send(client_fd, &resp, sizeof(resp), 0);

            /* Stream the file in chunks */
            char chunk[4096];
            ssize_t r;
            while ((r = read(fd, chunk, sizeof(chunk))) > 0)
                send(client_fd, chunk, (size_t)r, 0);
            close(fd);
            return;
        }

        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "Log file: %s", log_path);
        send(client_fd, &resp, sizeof(resp), 0);

        /* Stream the file */
        char chunk[4096];
        ssize_t r;
        while ((r = read(fd, chunk, sizeof(chunk))) > 0)
            send(client_fd, chunk, (size_t)r, 0);
        close(fd);
        return;
    }

    case CMD_STOP: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *rec = ctx->containers;
        int found = 0;
        while (rec) {
            if (strcmp(rec->id, req.container_id) == 0 &&
                rec->state == CONTAINER_RUNNING) {
                kill(rec->host_pid, SIGTERM);
                rec->state = CONTAINER_STOPPED;
                found = 1;
                break;
            }
            rec = rec->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (found) {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "Sent SIGTERM to container '%s'", req.container_id);
        } else {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "Container '%s' not found or not running",
                     req.container_id);
        }
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "unknown command");
        break;
    }

    send(client_fd, &resp, sizeof(resp), 0);
}

/* ------------------------------------------------------------------ */
/* Supervisor                                                          */
/* ------------------------------------------------------------------ */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { errno = rc; perror("pthread_mutex_init"); return 1; }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc; perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* 1. Open kernel monitor device (best-effort) */
    ctx.monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr,
                "[supervisor] Warning: cannot open %s (%s); "
                "memory monitoring disabled\n",
                MONITOR_DEV, strerror(errno));

    /* 2. Create the control UNIX domain socket */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(ctx.server_fd, 16) < 0) {
        perror("listen"); return 1;
    }

    /* 3. Signal handling */
    struct sigaction sa_chld = {0};
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    struct sigaction sa_term = {0};
    sa_term.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT,  &sa_term, NULL);

    /* 4. Start the logger thread */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc; perror("pthread_create logger");
        return 1;
    }

    fprintf(stderr,
            "[supervisor] Started. base-rootfs=%s socket=%s\n",
            rootfs, CONTROL_PATH);

    /* 5. Event loop */
    while (!ctx.should_stop) {
        fd_set rfds;
        struct timeval tv = {1, 0}; /* 1s timeout so we check should_stop */

        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);

        int sel = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (sel == 0) continue;

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        handle_client(&ctx, client_fd);
        close(client_fd);
    }

    /* Shutdown */
    fprintf(stderr, "[supervisor] Shutting down...\n");

    /* Kill all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *rec = ctx.containers;
    while (rec) {
        if (rec->state == CONTAINER_RUNNING)
            kill(rec->host_pid, SIGTERM);
        rec = rec->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    sleep(1);

    /* Flush log buffer and wait for logger thread */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    /* Free container records */
    pthread_mutex_lock(&ctx.metadata_lock);
    rec = ctx.containers;
    while (rec) {
        container_record_t *next = rec->next;
        free(rec);
        rec = next;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);

    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Client side                                                         */
/* ------------------------------------------------------------------ */
static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;
    ssize_t n;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
                "Cannot connect to supervisor at %s: %s\n"
                "Is the supervisor running? Try: ./engine supervisor <rootfs>\n",
                CONTROL_PATH, strerror(errno));
        close(fd);
        return 1;
    }

    if (send(fd, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send");
        close(fd);
        return 1;
    }

    n = recv(fd, &resp, sizeof(resp), MSG_WAITALL);
    if (n != (ssize_t)sizeof(resp)) {
        fprintf(stderr, "Unexpected response size from supervisor\n");
        close(fd);
        return 1;
    }

    /* For CMD_LOGS: print remaining streamed data */
    if (req->kind == CMD_LOGS) {
        char buf[4096];
        ssize_t r;
        while ((r = recv(fd, buf, sizeof(buf), 0)) > 0)
            fwrite(buf, 1, (size_t)r, stdout);
    }

    close(fd);

    if (resp.status != 0) {
        fprintf(stderr, "Error: %s\n", resp.message);
        return 1;
    }

    printf("%s\n", resp.message);
    return 0;
}

/* ------------------------------------------------------------------ */
/* CLI command handlers                                                */
/* ------------------------------------------------------------------ */
static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> "
                "[--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,      argv[4], sizeof(req.command) - 1);
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
                "Usage: %s run <id> <container-rootfs> <command> "
                "[--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,      argv[4], sizeof(req.command) - 1);
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
