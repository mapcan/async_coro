/* © Copyright 2012 jingmi. All Rights Reserved.
 *
 * +----------------------------------------------------------------------+
 * | test suites for acore.c                                              |
 * +----------------------------------------------------------------------+
 * | Author: nosqldev@gmail.com                                           |
 * +----------------------------------------------------------------------+
 * | Created: 2012-02-02 11:50                                            |
 * +----------------------------------------------------------------------+
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <CUnit/Basic.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>

#include "acoro.h"

/* {{{ copied from acoro.c for internal tests */

#include <string.h>
#include <ucontext.h>
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include "ev.h"
#include "bsdqueue.h"

/* {{{ hooks & predefined subs */

#ifndef acoro_malloc
#define acoro_malloc malloc
#endif /* ! acoro_malloc */

#ifndef acoro_free
#define acoro_free free
#endif /* ! acoro_free */

#define Lock(name) list_lock(coroutine_env.name)
#define UnLock(name) list_unlock(coroutine_env.name)

#define Shift(name, ptr) do { Lock(name); list_shift(coroutine_env.name, ptr); UnLock(name); } while (0)
#define Push(name, ptr) do { Lock(name); list_push(coroutine_env.name, ptr); UnLock(name); } while (0)

#define IO_WATCHER_REF_TASKPTR(io_w_ptr) (list_item_ptr(task_queue))((char *)io_w_ptr - __offset_of(struct list_item_name(task_queue), ec.sock_watcher))

/* }}} */
/* {{{ config */

/* TODO define MAX_* instead */

#ifndef MANAGER_CNT
/* ONLY 1 manager thread now */
#define MANAGER_CNT (1)
#endif /* ! MANAGER_CNT */

#ifndef BACKGROUND_WORKER_CNT
#define BACKGROUND_WORKER_CNT (2)
#endif /* ! BACKGROUND_WORKER_CNT */

#ifndef COROUTINE_STACK_SIZE
#define COROUTINE_STACK_SIZE (1024 * 4)
#endif /* ! COROUTINE_STACK_SIZE */

/* }}} */
/* {{{ enums & structures */

/* TODO
 * 1. rename *sock* to *tcp*
 * 2. add tcp_connect
 */
enum action_t
{
    act_new_coroutine,
    act_finished_coroutine,

    act_disk_open,
    act_disk_read,
    act_disk_write,

    act_sock_read,
    act_sock_read_done,
    act_sock_write,

    act_disk_open_done,
    act_disk_read_done,
    act_disk_write_done,

    act_tcp_connect,
    act_tcp_done,

    act_usleep,
};

struct init_arg_s
{
    begin_routine_t func;
    size_t stack_size;
    void *func_arg;
};

struct open_arg_s
{
    const char *pathname;
    int flags;
    mode_t mode;
};

struct io_arg_s
{
    int timeout_ms; /* for sock io only */

    int fd;
    void *buf;
    size_t count;
};

union args_u
{
    struct init_arg_s init_arg;
    struct open_arg_s open_arg;
    struct io_arg_s io_arg;
};

struct ev_controller_s
{
    ev_io sock_watcher;
    ev_timer sock_timer;
    ssize_t have_io;
    ssize_t need_io;
};

list_def(task_queue);
struct list_item_name(task_queue)
{
    enum action_t action;
    union args_u args;
    struct
    {
        ssize_t val;
        int err_code;
    } ret;

    ucontext_t task_context;
    void *stack_ptr;
    struct ev_controller_s ec;

    list_next_ptr(task_queue);
};

struct coroutine_env_s
{
    pthread_t manager_tid[MANAGER_CNT];
    pthread_t background_worker_tid[BACKGROUND_WORKER_CNT];

    sem_t manager_sem[MANAGER_CNT];
    int pipe_channel[BACKGROUND_WORKER_CNT * 2];

    struct
    {
        volatile uint64_t cid; /* coroutine id */
        volatile uint64_t ran; /* how many coroutines had been ran */
        volatile uint64_t bg_worker_id;
    } info;

    /* TODO manager_context should be an array */
    ucontext_t manager_context;
    list_item_ptr(task_queue) curr_task_ptr[MANAGER_CNT];
    struct
    {
        struct ev_loop *loop;
        ev_io watcher;
    } worker_ev[BACKGROUND_WORKER_CNT];

    list_head_ptr(task_queue) timer_queue;
    list_head_ptr(task_queue) todo_queue;
    list_head_ptr(task_queue) doing_queue;
    list_head_ptr(task_queue) done_queue;
};

/* }}} */

extern __thread volatile uint64_t g_thread_id;
struct coroutine_env_s coroutine_env;

/* }}} */

int lowest_fd;

void
test_init_coroutine_env(void)
{
    int ret;
    lowest_fd = dup(0);
    close(lowest_fd);

    ret = init_coroutine_env();

    CU_ASSERT(ret == 0);
    CU_ASSERT(coroutine_env.timer_queue != NULL);
    CU_ASSERT(coroutine_env.todo_queue  != NULL);
    CU_ASSERT(coroutine_env.doing_queue != NULL);
    CU_ASSERT(coroutine_env.done_queue  != NULL);

    for (int i=0; i<BACKGROUND_WORKER_CNT*2; i++)
    {
        CU_ASSERT(coroutine_env.pipe_channel[i] != 0);
    }
}

int
check_init(void)
{
    /* {{{ init CU suite check_init */
    CU_pSuite pSuite = NULL;
    pSuite = CU_add_suite("check_init", NULL, NULL);
    if (pSuite == NULL)
    {
        CU_cleanup_registry();
        return CU_get_error();
    }
    /* }}} */

    /* {{{ CU_add_test: test_init_coroutine_env */
    if (CU_add_test(pSuite, "test_init_coroutine_env", test_init_coroutine_env) == NULL)
    {
        CU_cleanup_registry();
        return CU_get_error();
    }
    /* }}} */

    return 0;
}

static void *
disk_read(void *arg)
{
    (void)arg;
    int fd = 0;
    char buf[32];

    Lock(doing_queue);
    CU_ASSERT(list_size(coroutine_env.doing_queue) == 0);
    UnLock(doing_queue);

    fd = crt_disk_open("./test_acoro.c", O_RDONLY);
    CU_ASSERT(fd > 0);
    ssize_t nread = crt_disk_read(fd, buf, sizeof buf);
    CU_ASSERT(crt_get_err_code() == 0);
    CU_ASSERT(nread == sizeof buf);
    CU_ASSERT(memcmp(buf, "/* © Copyright 2012 jingmi. All", sizeof buf) == 0);

    int ret = crt_disk_close(fd);
    CU_ASSERT(ret == 0);

    CU_ASSERT((uintptr_t)&buf[0] > (uintptr_t)coroutine_env.curr_task_ptr[ g_thread_id ]->stack_ptr);
    CU_ASSERT((uintptr_t)&buf[0] < (uintptr_t)coroutine_env.curr_task_ptr[ g_thread_id ]->stack_ptr + (uintptr_t)COROUTINE_STACK_SIZE);
    CU_ASSERT((uintptr_t)&buf[31] > (uintptr_t)coroutine_env.curr_task_ptr[ g_thread_id ]->stack_ptr);
    CU_ASSERT((uintptr_t)&buf[31] < (uintptr_t)coroutine_env.curr_task_ptr[ g_thread_id ]->stack_ptr + (uintptr_t)COROUTINE_STACK_SIZE);

    crt_exit(NULL);
}

void
test_disk_read(void)
{
    int ret;
    int lowestfd = dup(0);
    close(lowestfd);
    CU_ASSERT(coroutine_env.info.ran == 2);
    ret = crt_create(NULL, NULL, disk_read, NULL);
    CU_ASSERT(ret == 0);

    usleep(1000*20);
    CU_ASSERT(coroutine_env.info.ran == 3);
    int curr_lowestfd = dup(0);
    close(curr_lowestfd);
    CU_ASSERT(lowestfd == curr_lowestfd);
}

static void *
null_coroutine(void *arg)
{
    /* XXX Why crash when using printf() with arguments? */
    /*printf("hello, %s\n", "world");*/
    CU_ASSERT((intptr_t)arg == 0xbeef);

    crt_exit(NULL);
}

void
test_null_coroutine(void)
{
    coroutine_t cid;
    int ret;

    CU_ASSERT(coroutine_env.info.cid == 0);
    CU_ASSERT(coroutine_env.info.ran == 0);

    ret = crt_create(&cid, NULL, null_coroutine, (void*)(intptr_t)0xbeef);

    CU_ASSERT(coroutine_env.info.cid == 1);
    CU_ASSERT(cid == 1);

    CU_ASSERT(ret == 0);

    usleep(1000*10);
    CU_ASSERT(coroutine_env.info.ran == 1);
}

static int
mul(int a, int b)
{
    CU_ASSERT((uintptr_t)&a > (uintptr_t)coroutine_env.curr_task_ptr[ g_thread_id ]->stack_ptr);
    CU_ASSERT((uintptr_t)&a < (uintptr_t)coroutine_env.curr_task_ptr[ g_thread_id ]->stack_ptr + (uintptr_t)COROUTINE_STACK_SIZE);

    CU_ASSERT((uintptr_t)&b > (uintptr_t)coroutine_env.curr_task_ptr[ g_thread_id ]->stack_ptr);
    CU_ASSERT((uintptr_t)&b < (uintptr_t)coroutine_env.curr_task_ptr[ g_thread_id ]->stack_ptr + (uintptr_t)COROUTINE_STACK_SIZE);

    return a * b;
}

static void *
call_in_coroutine(void *arg)
{
    (void)arg;

    int c = mul(1, 2);
    CU_ASSERT(c == 2);
    CU_ASSERT((uintptr_t)&c > (uintptr_t)coroutine_env.curr_task_ptr[ g_thread_id ]->stack_ptr);
    CU_ASSERT((uintptr_t)&c < (uintptr_t)coroutine_env.curr_task_ptr[ g_thread_id ]->stack_ptr + (uintptr_t)COROUTINE_STACK_SIZE);

    c = mul(100, 200);
    CU_ASSERT(c == 20000);

    crt_exit(NULL);
}

void
test_call_in_coroutine(void)
{
    CU_ASSERT(crt_create(NULL, NULL, call_in_coroutine, NULL) == 0);
    usleep(1000*10);
    CU_ASSERT(coroutine_env.info.ran == 2);
}

static void *
disk_write(void *arg)
{
    (void)arg;

    int fd;

    fd = crt_disk_open("/tmp/test_acoro.dat", O_WRONLY | O_CREAT | O_APPEND | O_TRUNC, S_IREAD | S_IWRITE);
    CU_ASSERT(fd > 0);

    ssize_t nwrite = crt_disk_write(fd, "abc", 3);
    CU_ASSERT(nwrite == 3);
    CU_ASSERT(crt_get_err_code() == 0);

    CU_ASSERT(crt_disk_close(fd) == 0);

    fd = open("/tmp/test_acoro.dat", O_RDONLY);
    char buf[16];
    ssize_t nread = read(fd, buf, sizeof buf);
    CU_ASSERT(nread == 3);
    CU_ASSERT(memcmp(buf, "abc", 3) == 0);
    close(fd);
    struct stat sb;
    stat("/tmp/test_acoro.dat", &sb);
    CU_ASSERT(sb.st_size == 3);

    crt_exit(NULL);
}

void
test_disk_write(void)
{
    int lowestfd = dup(0);
    close(lowestfd);

    CU_ASSERT(crt_create(NULL, NULL, disk_write, NULL) == 0);
    usleep(1000 * 10);
    CU_ASSERT(coroutine_env.info.ran == 4);

    int curr_lowestfd = dup(0);
    close(curr_lowestfd);
    CU_ASSERT(lowestfd == curr_lowestfd);

    assert(unlink("/tmp/test_acoro.dat") == 0);
}

void *
disk_io_failed(void *arg)
{
    (void)arg;

    int fd = crt_disk_open("/UnableOpened.dat", O_CREAT, S_IREAD | S_IWRITE);
    CU_ASSERT(fd == -1);
    int err_code = crt_get_err_code();
    CU_ASSERT(err_code == EACCES);

    fd = crt_disk_open("/tmp/test_acoro.dat", O_RDONLY | O_CREAT | O_APPEND | O_TRUNC, S_IREAD);
    CU_ASSERT(fd > 0);
    char buf[16] = "abcdefg";
    ssize_t nwrite = crt_disk_write(fd, buf, sizeof buf);
    CU_ASSERT(errno == 0); /* means we can NOT use system errno in coroutine */
    CU_ASSERT(nwrite < 0);
    err_code = crt_get_err_code();
    CU_ASSERT(err_code == EBADF);

    CU_ASSERT(crt_disk_close(fd) == 0);

    crt_exit(NULL);
}

void
test_disk_io_failed(void)
{
    int lowestfd = dup(0);
    close(lowestfd);

    coroutine_t cid;
    CU_ASSERT(crt_create(&cid, NULL, disk_io_failed, NULL) == 0);
    usleep(1000 * 10);
    CU_ASSERT(coroutine_env.info.ran == 5);
    CU_ASSERT(cid == 5);
    assert(unlink("/tmp/test_acoro.dat") == 0);

    int curr_lowestfd = dup(0);
    close(curr_lowestfd);
    CU_ASSERT(lowestfd == curr_lowestfd);
}

void
test_utils(void)
{
    list_item_ptr(task_queue) task_ptr = NULL;
    list_item_ptr(task_queue) computed_addr = IO_WATCHER_REF_TASKPTR(&task_ptr->ec.sock_watcher);

    CU_ASSERT(computed_addr == task_ptr);
}

void *
dummy_write_server(void *arg)
{
    int listenfd;
    struct sockaddr_in server_addr;
    int flag = 1;
    (void)arg;

    listenfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(listenfd > 0);
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof flag);
    bzero(&server_addr, sizeof server_addr);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(2000);
    assert(bind(listenfd, (struct sockaddr *) &server_addr, sizeof server_addr) == 0);
    assert(listen(listenfd, 5) == 0);

    int sockfd = accept(listenfd, NULL, NULL);
    assert(sockfd > 0);
    CU_ASSERT(write(sockfd, "abc", 3) == 3);
    CU_ASSERT(write(sockfd, "abc", 3) == 3);
    close(sockfd);
    close(listenfd);

    return NULL;
}

static void *
sock_read(void *arg)
{
    (void)arg;
    struct sockaddr_in server_addr;
    char buf[32];

    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(2000);
    int sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(sockfd > 0);
    CU_ASSERT(connect(sockfd, (struct sockaddr *)&server_addr, sizeof server_addr) == 0);
    crt_set_nonblock(sockfd);
    CU_ASSERT(crt_tcp_read(sockfd, buf, sizeof buf) == 6);
    CU_ASSERT(crt_get_err_code() == EAGAIN);
    CU_ASSERT(memcmp(buf, "abcabc", 6) == 0);

    int ret = crt_tcp_read(sockfd, buf, sizeof buf);
    CU_ASSERT(ret == -1);
    CU_ASSERT(crt_get_err_code() == EBADF);

    close(sockfd);

    crt_exit(NULL);
}

void
test_sock_read(void)
{
    pthread_t tid;
    pthread_create(&tid, NULL, dummy_write_server, NULL);
    sched_yield();
    usleep(1000);

    crt_create(NULL, NULL, sock_read, NULL);
    usleep(1000);
    CU_ASSERT(coroutine_env.info.ran == 6);

    pthread_join(tid, NULL);
}

int
check_coroutine(void)
{
    /* {{{ init CU suite check_coroutine */
    CU_pSuite pSuite = NULL;
    pSuite = CU_add_suite("check_coroutine", NULL, NULL);
    if (pSuite == NULL)
    {
        CU_cleanup_registry();
        return CU_get_error();
    }
    /* }}} */

    /* {{{ CU_add_test: test_null_coroutine */
    if (CU_add_test(pSuite, "test_null_coroutine", test_null_coroutine) == NULL)
    {
        CU_cleanup_registry();
        return CU_get_error();
    }
    /* }}} */
    /* {{{ CU_add_test: test_call_in_coroutine */
    if (CU_add_test(pSuite, "test_call_in_coroutine", test_call_in_coroutine) == NULL)
    {
        CU_cleanup_registry();
        return CU_get_error();
    }
    /* }}} */
    /* {{{ CU_add_test: test_disk_read */
    if (CU_add_test(pSuite, "test_disk_read", test_disk_read) == NULL)
    {
        CU_cleanup_registry();
        return CU_get_error();
    }
    /* }}} */
    /* {{{ CU_add_test: test_disk_write */
    if (CU_add_test(pSuite, "test_disk_write", test_disk_write) == NULL)
    {
        CU_cleanup_registry();
        return CU_get_error();
    }
    /* }}} */
    /* {{{ CU_add_test: test_disk_io_failed */
    if (CU_add_test(pSuite, "test_disk_io_failed", test_disk_io_failed) == NULL)
    {
        CU_cleanup_registry();
        return CU_get_error();
    }
    /* }}} */
    /* {{{ CU_add_test: test_utils */
    if (CU_add_test(pSuite, "test_utils", test_utils) == NULL)
    {
        CU_cleanup_registry();
        return CU_get_error();
    }
    /* }}} */
    /* {{{ CU_add_test: test_sock_read */
    if (CU_add_test(pSuite, "test_sock_read", test_sock_read) == NULL)
    {
        CU_cleanup_registry();
        return CU_get_error();
    }
    /* }}} */

    return 0;
}

void
test_destroy_coroutine_env(void)
{
    int ret;
    ret = destroy_coroutine_env();
    CU_ASSERT(ret == 0);

    int curr_lowest_fd = dup(0);
    CU_ASSERT(lowest_fd == curr_lowest_fd);
    close(curr_lowest_fd);
}

int
check_destroy_coroutine_env(void)
{
    /* {{{ init CU suite check_destroy_coroutine_env */
    CU_pSuite pSuite = NULL;
    pSuite = CU_add_suite("check_destroy_coroutine_env", NULL, NULL);
    if (pSuite == NULL)
    {
        CU_cleanup_registry();
        return CU_get_error();
    }
    /* }}} */

    /* {{{ CU_add_test: test_destroy_coroutine_env */
    if (CU_add_test(pSuite, "test_destroy_coroutine_env", test_destroy_coroutine_env) == NULL)
    {
        CU_cleanup_registry();
        return CU_get_error();
    }
    /* }}} */

    return 0;
}

int
main(void)
{
    if (CUE_SUCCESS != CU_initialize_registry())
        return CU_get_error();

    check_init();

    check_coroutine();

    check_destroy_coroutine_env();

    /* {{{ CU run & cleanup */
    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    CU_cleanup_registry();
    /* }}} */

    return 0;
}

/* vim: set expandtab tabstop=4 shiftwidth=4 foldmethod=marker: */
