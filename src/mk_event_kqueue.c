/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Monkey HTTP Server
 *  ==================
 *  Copyright 2001-2015 Monkey Software LLC <eduardo@monkey.io>
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <monkey/mk_event.h>
#include <monkey/mk_memory.h>
#include <monkey/mk_utils.h>

static inline void *_mk_event_loop_create(int size)
{
    mk_event_ctx_t *ctx;

    /* Main event context */
    ctx = mk_mem_malloc_z(sizeof(mk_event_ctx_t));
    if (!ctx) {
        return NULL;
    }

    /* Create the epoll instance */
    ctx->kfd = kqueue();
    if (ctx->kfd == -1) {
        mk_libc_error("kqueue");
        mk_mem_free(ctx);
        return NULL;
    }

    /* Allocate space for events queue */
    ctx->events = mk_mem_malloc_z(sizeof(struct kevent) * size);
    if (!ctx->events) {
        close(ctx->kfd);
        mk_mem_free(ctx);
        return NULL;
    }
    ctx->queue_size = size;
    return ctx;
}

/* Close handlers and memory */
static inline void _mk_event_loop_destroy(mk_event_ctx_t *ctx)
{
    close(ctx->kfd);
    mk_mem_free(ctx->events);
    mk_mem_free(ctx);
}

static inline int _mk_event_add(mk_event_ctx_t *ctx, int fd, int events)
{
    int ret;
    int set = MK_FALSE;
    struct kevent ke = {0, 0, 0, 0, 0, 0};
    struct mk_event_fd_state *fds;

    fds = mk_event_get_state(fd);

    /* Read flag */
    if ((fds->mask ^ MK_EVENT_READ) && (events & MK_EVENT_READ)) {
        EV_SET(&ke, fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
        set = MK_TRUE;
        //printf("[ADD] fd=%i READ\n", fd);
    }
    else if ((fds->mask & MK_EVENT_READ) && (events ^ MK_EVENT_READ)) {
        EV_SET(&ke, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        set = MK_TRUE;
        //printf("[DEL] fd=%i READ\n", fd);
    }

    if (set == MK_TRUE) {
        ret = kevent(ctx->kfd, &ke, 1, NULL, 0, NULL);
        if (ret < 0) {
            mk_libc_error("kevent");
            return ret;
        }
    }

    /* Write flag */
    set = MK_FALSE;
    if ((fds->mask ^ MK_EVENT_WRITE) && (events & MK_EVENT_WRITE)) {
        EV_SET(&ke, fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
        set = MK_TRUE;
        //printf("[ADD] fd=%i WRITE\n", fd);
    }
    else if ((fds->mask & MK_EVENT_WRITE) && (events ^ MK_EVENT_WRITE)) {
        EV_SET(&ke, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        set = MK_TRUE;
        //printf("[DEL] fd=%i WRITE\n", fd);
    }

    if (set == MK_TRUE) {
        ret = kevent(ctx->kfd, &ke, 1, NULL, 0, NULL);
        if (ret < 0) {
            mk_libc_error("kevent");
            return ret;
        }
    }

    return 0;
}

static inline int _mk_event_del(mk_event_ctx_t *ctx, int fd)
{
    int ret;
    struct kevent ke = {0, 0, 0, 0, 0, 0};
    struct mk_event_fd_state *fds;

    fds = mk_event_get_state(fd);
    if (fds->mask & MK_EVENT_READ) {
        EV_SET(&ke, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        ret = kevent(ctx->kfd, &ke, 1, NULL, 0, NULL);
        if (ret < 0) {
            mk_libc_error("kevent");
            return ret;
        }
        //printf("!DEL READ %i\n", fd);
    }

    if (fds->mask & MK_EVENT_WRITE) {
        EV_SET(&ke, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        ret = kevent(ctx->kfd, &ke, 1, NULL, 0, NULL);
        if (ret < 0) {
            mk_libc_error("kevent");
            return ret;
        }
        //printf("!DEL WRITE %i\n", fd);
    }

    return 0;
}

static inline int _mk_event_timeout_create(mk_event_ctx_t *ctx, int expire)
{
    int fd;
    int ret;
    struct kevent ke;

    /*
     * We just need a file descriptor number, we don't care from where it
     * comes from.
     */
    fd = open("/dev/null", 0);
    if (fd == -1) {
        mk_libc_error("open");
        return -1;
    }

    EV_SET(&ke, fd, EVFILT_TIMER, EV_ADD, NOTE_SECONDS, expire, NULL);
    ret = kevent(ctx->kfd, &ke, 1, NULL, 0, NULL);
    if (ret < 0) {
        close(fd);
        mk_libc_error("kevent");
        return -1;
    }

    /*
     * FIXME: the timeout event is not triggered when using libkqueue, need
     * to confirm how it behave on native OSX.
     */

    return fd;
}

static inline int _mk_event_channel_create(mk_event_ctx_t *ctx, int *r_fd, int *w_fd)
{
    int ret;
    int fd[2];

    ret = pipe(fd);
    if (ret < 0) {
        mk_libc_error("pipe");
        return ret;
    }

    ret = _mk_event_add(ctx, fd[0], MK_EVENT_READ);
    if (ret != 0) {
        close(fd[0]);
        close(fd[1]);
        return ret;
    }

    *r_fd = fd[0];
    *w_fd = fd[1];

    return 0;
}

static inline int _mk_event_wait(mk_event_loop_t *loop)
{
    mk_event_ctx_t *ctx = loop->data;

    loop->n_events = kevent(ctx->kfd, NULL, 0, ctx->events, ctx->queue_size, NULL);
    return loop->n_events;
}

static inline int _mk_event_translate(mk_event_loop_t *loop)
{
    int i;
    int fd;
    int mask = 0;
    struct mk_event_fd_state *st;
    mk_event_ctx_t *ctx = loop->data;

    for (i = 0; i < loop->n_events; i++) {
        fd = ctx->events[i].ident;
        st = &mk_events_fdt->states[fd];

        if (ctx->events[i].filter == EVFILT_READ) {
            mask |= MK_EVENT_READ;
        }

        if (ctx->events[i].filter == EVFILT_WRITE) {
            mask |= MK_EVENT_WRITE;
        }

        loop->events[i].fd   = fd;
        loop->events[i].mask = mask;
        loop->events[i].data = st->data;
    }

    return loop->n_events;
}

static inline char *_mk_event_backend()
{
#ifdef LINUX_KQUEUE
    return "libkqueue";
#else
    return "kqueue";
#endif
}
