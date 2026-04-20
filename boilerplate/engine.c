/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
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
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

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
    int stop_requested;
    int nice_value;
    void *stack;
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
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
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
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static long get_rss_kb(pid_t pid)
{
    char path[64], line[256];
    FILE *f;
    long rss_pages = 0;

    snprintf(path, sizeof(path), "/proc/%d/statm", pid);
    f = fopen(path, "r");
    if (f) {
        if (fgets(line, sizeof(line), f))
            sscanf(line, "%*s %ld", &rss_pages);
        fclose(f);
    }
    return rss_pages * 4;
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

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
 * Producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

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
 * Consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);

    return 0;
}

/*
 * Logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    char log_path[PATH_MAX];

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, item.container_id);
        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
    }

    return NULL;
}

typedef struct {
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} producer_args_t;

void *producer_thread_fn(void *arg)
{
    producer_args_t *args = (producer_args_t *)arg;
    log_item_t item;
    ssize_t n;

    memset(&item, 0, sizeof(item));
    snprintf(item.container_id, sizeof(item.container_id), "%s", args->container_id);

    while ((n = read(args->read_fd, item.data, sizeof(item.data))) > 0) {
        item.length = n;
        if (bounded_buffer_push(args->buffer, &item) != 0) {
            break;
        }
    }

    close(args->read_fd);
    free(args);
    return NULL;
}

/*
 * Clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *config = (child_config_t *)arg;

    if (config->log_write_fd >= 0) {
        dup2(config->log_write_fd, STDOUT_FILENO);
        dup2(config->log_write_fd, STDERR_FILENO);
        close(config->log_write_fd);
    }

    if (mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) != 0) {
        perror("mount private");
        return 1;
    }

    if (chroot(config->rootfs) != 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("chdir");
        return 1;
    }

    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount proc");
        return 1;
    }

    if (config->nice_value != 0) {
        if (setpriority(PRIO_PROCESS, 0, config->nice_value) != 0) {
            perror("setpriority");
            return 1;
        }
    }

    char *argv[64];
    int argc = 0;
    char *cmd_copy = strdup(config->command);
    if (!cmd_copy) {
        perror("strdup");
        return 1;
    }

    char *token = strtok(cmd_copy, " ");
    while (token != NULL && argc < 63) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }
    argv[argc] = NULL;

    execvp(argv[0], argv);
    perror("execvp");
    free(cmd_copy);
    return 1;
}

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
    snprintf(req.container_id, sizeof(req.container_id), "%s", container_id);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    snprintf(req.container_id, sizeof(req.container_id), "%s", container_id);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

volatile sig_atomic_t child_exited_flag = 0;
volatile sig_atomic_t supervisor_stop_flag = 0;

static void sigchld_handler(int signum) {
    (void)signum;
    child_exited_flag = 1;
}

static void supervisor_sigint_handler(int signum) {
    (void)signum;
    supervisor_stop_flag = 1;
}

static void reap_children(supervisor_ctx_t *ctx) {
    if (child_exited_flag) {
        child_exited_flag = 0;
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            char exited_id[CONTAINER_ID_LEN] = {0};
            int should_unregister = 0;

            pthread_mutex_lock(&ctx->metadata_lock);
            container_record_t *curr = ctx->containers;
            while (curr) {
                if (curr->host_pid == pid) {
                    if (WIFEXITED(status)) {
                        if (curr->stop_requested) {
                            curr->state = CONTAINER_STOPPED;
                        } else {
                            curr->state = CONTAINER_EXITED;
                        }
                        curr->exit_code = WEXITSTATUS(status);
                        printf("Container %s exited/stopped with code %d\n", curr->id, curr->exit_code);
                    } else if (WIFSIGNALED(status)) {
                        if (curr->stop_requested) {
                            curr->state = CONTAINER_STOPPED;
                        } else {
                            curr->state = CONTAINER_KILLED;
                        }
                        curr->exit_signal = WTERMSIG(status);
                        printf("Container %s killed/stopped by signal %d\n", curr->id, curr->exit_signal);
                    }
                    snprintf(exited_id, sizeof(exited_id), "%s", curr->id);
                    should_unregister = 1;
                    break;
                }
                curr = curr->next;
            }
            pthread_mutex_unlock(&ctx->metadata_lock);

            if (should_unregister && ctx->monitor_fd >= 0) {
                unregister_from_monitor(ctx->monitor_fd, exited_id, pid);
            }
        }
    }
}

static int spawn_container(supervisor_ctx_t *ctx, child_config_t *config)
{
    void *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc stack");
        return -1;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        free(stack);
        return -1;
    }
    config->log_write_fd = pipefd[1];

    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(child_fn, (char *)stack + STACK_SIZE, clone_flags, config);

    close(pipefd[1]);

    if (pid < 0) {
        perror("clone");
        close(pipefd[0]);
        free(stack);
        return -1;
    }

    producer_args_t *p_args = malloc(sizeof(producer_args_t));
    if (p_args) {
        p_args->read_fd = pipefd[0];
        strncpy(p_args->container_id, config->id, sizeof(p_args->container_id) - 1);
        p_args->container_id[sizeof(p_args->container_id) - 1] = '\0';
        p_args->buffer = &ctx->log_buffer;

        pthread_t ptid;
        if (pthread_create(&ptid, NULL, producer_thread_fn, p_args) == 0) {
            pthread_detach(ptid);
        } else {
            close(pipefd[0]);
            free(p_args);
        }
    } else {
        close(pipefd[0]);
    }

    container_record_t *record = calloc(1, sizeof(container_record_t));
    if (!record) {
        perror("calloc record");
        return -1;
    }

    snprintf(record->id, sizeof(record->id), "%s", config->id);
    record->host_pid = pid;
    record->started_at = time(NULL);
    record->state = CONTAINER_RUNNING;
    record->stack = stack;

    pthread_mutex_lock(&ctx->metadata_lock);
    record->next = ctx->containers;
    ctx->containers = record;
    pthread_mutex_unlock(&ctx->metadata_lock);

    record->soft_limit_bytes = config->soft_limit_bytes;
    record->hard_limit_bytes = config->hard_limit_bytes;
    record->nice_value = config->nice_value;

    if (ctx->monitor_fd >= 0) {
        register_with_monitor(ctx->monitor_fd, record->id, pid,
                              config->soft_limit_bytes,
                              config->hard_limit_bytes);
    }

    return 0;
}

typedef struct {
    int client_fd;
    supervisor_ctx_t *ctx;
} client_handler_args_t;

void *client_handler_thread_fn(void *arg) {
    client_handler_args_t *args = (client_handler_args_t *)arg;
    int client_fd = args->client_fd;
    supervisor_ctx_t *ctx = args->ctx;
    free(args);

    control_request_t req;
    if (recv(client_fd, &req, sizeof(req), MSG_WAITALL) != sizeof(req)) {
        close(client_fd);
        return NULL;
    }

    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    if (req.kind == CMD_START || req.kind == CMD_RUN) {
        child_config_t config;
        memset(&config, 0, sizeof(config));
        snprintf(config.id, sizeof(config.id), "%s", req.container_id);
        snprintf(config.rootfs, sizeof(config.rootfs), "%s", req.rootfs);
        snprintf(config.command, sizeof(config.command), "%s", req.command);
        config.nice_value = req.nice_value;
        config.soft_limit_bytes = req.soft_limit_bytes;
        config.hard_limit_bytes = req.hard_limit_bytes;

        if (spawn_container(ctx, &config) < 0) {
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message), "Failed to spawn container");
            send(client_fd, &resp, sizeof(resp), MSG_NOSIGNAL);
            close(client_fd);
            return NULL;
        }

        if (req.kind == CMD_START) {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message), "Container %s started", req.container_id);
            send(client_fd, &resp, sizeof(resp), MSG_NOSIGNAL);
            close(client_fd);
            return NULL;
        }

        if (req.kind == CMD_RUN) {
            int exit_status = 1;
            while (!ctx->should_stop && !supervisor_stop_flag) {
                pthread_mutex_lock(&ctx->metadata_lock);
                container_record_t *curr = ctx->containers;
                int found = 0;
                while (curr) {
                    if (strcmp(curr->id, req.container_id) == 0) {
                        found = 1;
                        if (curr->state == CONTAINER_EXITED) {
                            exit_status = curr->exit_code;
                            found = 2;
                        } else if (curr->state == CONTAINER_KILLED || curr->state == CONTAINER_STOPPED) {
                            exit_status = 128 + curr->exit_signal;
                            found = 2;
                        }
                        break;
                    }
                    curr = curr->next;
                }
                pthread_mutex_unlock(&ctx->metadata_lock);

                if (found == 2 || found == 0) {
                    break;
                }
                usleep(100000);
            }
            resp.status = exit_status;
            send(client_fd, &resp, sizeof(resp), MSG_NOSIGNAL);
            close(client_fd);
            return NULL;
        }
    } else if (req.kind == CMD_STOP) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *curr = ctx->containers;
        int found = 0;
        while (curr) {
            if (strcmp(curr->id, req.container_id) == 0) {
                found = 1;
                if (curr->state == CONTAINER_RUNNING || curr->state == CONTAINER_STARTING) {
                    curr->stop_requested = 1;
                    kill(curr->host_pid, SIGKILL);
                    resp.status = 0;
                    snprintf(resp.message, sizeof(resp.message), "Container %s stopped", req.container_id);
                } else {
                    resp.status = 1;
                    snprintf(resp.message, sizeof(resp.message), "Container %s not running", req.container_id);
                }
                break;
            }
            curr = curr->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (!found) {
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message), "Container %s not found", req.container_id);
        }
        send(client_fd, &resp, sizeof(resp), MSG_NOSIGNAL);
    } else if (req.kind == CMD_PS) {
        resp.status = 0;
        send(client_fd, &resp, sizeof(resp), MSG_NOSIGNAL);

        char buf[1024];
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *curr = ctx->containers;
        snprintf(buf, sizeof(buf), "%-16s %-8s %-16s %-6s %-10s %-8s\n", "CONTAINER ID", "PID", "STATE", "NICE", "RSS(KB)", "EXIT CODE");
        send(client_fd, buf, strlen(buf), MSG_NOSIGNAL);
        while (curr) {
            snprintf(buf, sizeof(buf), "%-16s %-8d %-16s %-6d %-10ld %-8d\n",
                     curr->id, curr->host_pid, state_to_string(curr->state),
                     curr->nice_value, get_rss_kb(curr->host_pid),
                     (curr->state == CONTAINER_EXITED) ? curr->exit_code :
                     ((curr->state == CONTAINER_KILLED || curr->state == CONTAINER_STOPPED) ? curr->exit_signal : 0));
            send(client_fd, buf, strlen(buf), MSG_NOSIGNAL);
            curr = curr->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    } else if (req.kind == CMD_LOGS) {
        resp.status = 0;
        send(client_fd, &resp, sizeof(resp), MSG_NOSIGNAL);

        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, req.container_id);
        int fd = open(log_path, O_RDONLY);
        if (fd >= 0) {
            char buf[1024];
            ssize_t n;
            while ((n = read(fd, buf, sizeof(buf))) > 0) {
                send(client_fd, buf, n, MSG_NOSIGNAL);
            }
            close(fd);
        } else {
            char *msg = "No logs found.\n";
            send(client_fd, msg, strlen(msg), MSG_NOSIGNAL);
        }
    }

    close(client_fd);
    return NULL;
}

void *ipc_server_thread_fn(void *arg) {
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    while (!ctx->should_stop && !supervisor_stop_flag) {
        int client_fd = accept(ctx->server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        pthread_t tid;
        client_handler_args_t *args = malloc(sizeof(client_handler_args_t));
        args->client_fd = client_fd;
        args->ctx = ctx;
        pthread_create(&tid, NULL, client_handler_thread_fn, args);
        pthread_detach(tid);
    }
    return NULL;
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = supervisor_sigint_handler;
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    mkdir(LOG_DIR, 0755);

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        return 1;
    }
    unlink(CONTROL_PATH);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(ctx.server_fd, 10) < 0) {
        perror("listen");
        return 1;
    }

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        perror("open /dev/container_monitor");
    }

    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    pthread_t ipc_tid;
    pthread_create(&ipc_tid, NULL, ipc_server_thread_fn, &ctx);

    fprintf(stderr, "Supervisor started for base-rootfs: %s\n", rootfs);

    while (!ctx.should_stop && !supervisor_stop_flag) {
        reap_children(&ctx);
        sleep(1);
    }

    printf("\nSupervisor shutting down. Stopping all containers...\n");
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *curr = ctx.containers;
    while (curr) {
        if (curr->state == CONTAINER_RUNNING || curr->state == CONTAINER_STARTING) {
            curr->stop_requested = 1;
            kill(curr->host_pid, SIGKILL);
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    int running_count = 1;
    while (running_count > 0) {
        reap_children(&ctx);
        running_count = 0;
        pthread_mutex_lock(&ctx.metadata_lock);
        curr = ctx.containers;
        while (curr) {
            if (curr->state == CONTAINER_RUNNING || curr->state == CONTAINER_STARTING) {
                running_count++;
            }
            curr = curr->next;
        }
        pthread_mutex_unlock(&ctx.metadata_lock);
        if (running_count > 0) usleep(100000);
    }

    close(ctx.server_fd);
    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);
    unlink(CONTROL_PATH);
    pthread_cancel(ipc_tid);
    pthread_join(ipc_tid, NULL);

    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *rec = ctx.containers;
    while (rec) {
        container_record_t *next = rec->next;
        free(rec->stack);
        free(rec);
        rec = next;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

static char run_container_id[CONTAINER_ID_LEN];

static void client_sigint_handler(int signum) {
    (void)signum;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            control_request_t req;
            memset(&req, 0, sizeof(req));
            req.kind = CMD_STOP;
            snprintf(req.container_id, sizeof(req.container_id), "%s", run_container_id);
            send(fd, &req, sizeof(req), MSG_NOSIGNAL);
        }
        close(fd);
    }
}

static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect to supervisor");
        close(fd);
        return 1;
    }

    if (send(fd, req, sizeof(*req), MSG_NOSIGNAL) != sizeof(*req)) {
        perror("send");
        close(fd);
        return 1;
    }

    if (req->kind == CMD_RUN) {
        snprintf(run_container_id, sizeof(run_container_id), "%s", req->container_id);
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = client_sigint_handler;
        sa.sa_flags = SA_RESTART;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }

    control_response_t resp;
    ssize_t n;
    while ((n = recv(fd, &resp, sizeof(resp), MSG_WAITALL)) < 0) {
        if (errno == EINTR) continue;
        break;
    }

    if (n < (ssize_t)sizeof(resp)) {
        fprintf(stderr, "Invalid response from supervisor\n");
        close(fd);
        return 1;
    }

    if (resp.message[0] != '\0') {
        printf("%s\n", resp.message);
    }

    if (req->kind == CMD_PS || req->kind == CMD_LOGS) {
        char buf[1024];
        while ((n = recv(fd, buf, sizeof(buf) - 1, 0)) > 0) {
            buf[n] = '\0';
            printf("%s", buf);
        }
    }

    int status = resp.status;
    close(fd);
    return status;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

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

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
