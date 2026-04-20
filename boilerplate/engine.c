// engine_full_fixed.c

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
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16

typedef enum {
    CMD_START = 1,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_RUNNING,
    CONTAINER_STOPPED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t pid;
    container_state_t state;
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    int head, tail, count;
    int shutdown;
    pthread_mutex_t mtx;
    pthread_cond_t not_empty, not_full;
} buffer_t;

typedef struct {
    command_kind_t kind;
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[256];
} request_t;

typedef struct {
    int status;
    char msg[256];
} response_t;

typedef struct {
    int server_fd;
    buffer_t buffer;
    pthread_t logger;
    container_record_t *list;
    pthread_mutex_t lock;
} ctx_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[256];
    int logfd;
} child_cfg_t;

//////////////////////////////////////////////////
// BUFFER
//////////////////////////////////////////////////

void buffer_init(buffer_t *b)
{
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->mtx, NULL);
    pthread_cond_init(&b->not_empty, NULL);
    pthread_cond_init(&b->not_full, NULL);
}

int buffer_push(buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mtx);

    while (b->count == LOG_BUFFER_CAPACITY && !b->shutdown)
        pthread_cond_wait(&b->not_full, &b->mtx);

    if (b->shutdown) {
        pthread_mutex_unlock(&b->mtx);
        return -1;
    }

    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;

    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mtx);
    return 0;
}

int buffer_pop(buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mtx);

    while (b->count == 0 && !b->shutdown)
        pthread_cond_wait(&b->not_empty, &b->mtx);

    if (b->count == 0 && b->shutdown) {
        pthread_mutex_unlock(&b->mtx);
        return -1;
    }

    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;

    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mtx);
    return 0;
}

//////////////////////////////////////////////////
// LOGGER THREAD
//////////////////////////////////////////////////

void *logger_thread(void *arg)
{
    ctx_t *ctx = arg;
    log_item_t item;

    mkdir(LOG_DIR, 0755);

    while (buffer_pop(&ctx->buffer, &item) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), LOG_DIR "/%s.log", item.container_id);

        int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) {
            if (write(fd, item.data, item.length) < 0)
                perror("log write");
            close(fd);
        }
    }
    return NULL;
}

//////////////////////////////////////////////////
// CHILD
//////////////////////////////////////////////////

int child_fn(void *arg)
{
    child_cfg_t *cfg = arg;

    sethostname(cfg->id, strlen(cfg->id));

    if (chroot(cfg->rootfs) < 0) {
        perror("chroot");
        return 1;
    }

    chdir("/");

    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);

    dup2(cfg->logfd, STDOUT_FILENO);
    dup2(cfg->logfd, STDERR_FILENO);
    close(cfg->logfd);

    execl("/bin/sh", "sh", "-c", cfg->command, NULL);
    perror("exec");
    return 1;
}

//////////////////////////////////////////////////
// START
//////////////////////////////////////////////////

void handle_start(ctx_t *ctx, request_t *req, response_t *res)
{
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        res->status = 1;
        return;
    }

    child_cfg_t *cfg = malloc(sizeof(*cfg));
    strcpy(cfg->id, req->id);
    strcpy(cfg->rootfs, req->rootfs);
    strcpy(cfg->command, req->command);
    cfg->logfd = pipefd[1];

    void *stack = malloc(STACK_SIZE);

    pid_t pid = clone(child_fn, stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      cfg);

    close(pipefd[1]);

    if (pid < 0) {
        perror("clone");
        res->status = 1;
        return;
    }

    // log reader
    if (fork() == 0) {
        while (1) {
            log_item_t item;
            ssize_t n = read(pipefd[0], item.data, LOG_CHUNK_SIZE);
            if (n <= 0) break;

            strcpy(item.container_id, req->id);
            item.length = n;
            buffer_push(&ctx->buffer, &item);
        }
        close(pipefd[0]);
        exit(0);
    }

    pthread_mutex_lock(&ctx->lock);

    container_record_t *rec = malloc(sizeof(*rec));
    strcpy(rec->id, req->id);
    rec->pid = pid;
    rec->state = CONTAINER_RUNNING;
    rec->next = ctx->list;
    ctx->list = rec;

    pthread_mutex_unlock(&ctx->lock);

    snprintf(res->msg, sizeof(res->msg), "Started %s (pid=%d)", req->id, pid);
    res->status = 0;
}

//////////////////////////////////////////////////
// SUPERVISOR
//////////////////////////////////////////////////

int run_supervisor()
{
    ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    pthread_mutex_init(&ctx.lock, NULL);
    buffer_init(&ctx.buffer);
    pthread_create(&ctx.logger, NULL, logger_thread, &ctx);

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    unlink(CONTROL_PATH);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    listen(ctx.server_fd, 10);

    printf("Supervisor running...\n");

    while (1) {
        int cfd = accept(ctx.server_fd, NULL, NULL);
        if (cfd < 0) continue;

        request_t req;
        response_t res;

        ssize_t n = read(cfd, &req, sizeof(req));
        if (n != sizeof(req)) {
            close(cfd);
            continue;
        }

        switch (req.kind) {
            case CMD_START:
            case CMD_RUN:
                handle_start(&ctx, &req, &res);
                break;
            default:
                res.status = 1;
                strcpy(res.msg, "Unsupported");
        }

        if (write(cfd, &res, sizeof(res)) != sizeof(res))
            perror("write");

        close(cfd);
    }
}

//////////////////////////////////////////////////
// CLIENT
//////////////////////////////////////////////////

int send_req(request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    if (write(fd, req, sizeof(*req)) != sizeof(*req))
        perror("write");

    response_t res;
    if (read(fd, &res, sizeof(res)) == sizeof(res))
        printf("%s\n", res.msg);

    close(fd);
    return res.status;
}

//////////////////////////////////////////////////
// MAIN
//////////////////////////////////////////////////

int main(int argc, char *argv[])
{
    if (argc < 2) return 1;

    if (!strcmp(argv[1], "supervisor"))
        return run_supervisor();

    request_t req = {0};

    if (!strcmp(argv[1], "start") || !strcmp(argv[1], "run")) {
        if (argc < 5) return 1;
        req.kind = CMD_START;
        strcpy(req.id, argv[2]);
        strcpy(req.rootfs, argv[3]);
        strcpy(req.command, argv[4]);
    }

    return send_req(&req);
}
