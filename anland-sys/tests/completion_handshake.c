/* Standalone regression test for Android render completion and disconnect.
 * Compile with display_producer.c's socket_utils.c, as in build.yml. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/vendor/display_producer.c"

static display_ctx context_with_socket(int fd)
{
    display_ctx ctx = {0};
    ctx.ctrl_fd = ctx.data_fd = ctx.buf_ready_efd = -1;
    ctx.fence_fd = fd;
    ctx.shm_fd = ctx.audio_fd = ctx.pending_render_fence = -1;
    for (int i = 0; i < MAX_BUFS; ++i)
        ctx.dmabuf_fds[i] = -1;
    return ctx;
}

static void disconnected(void *count)
{
    ++*(int *)count;
}

int main(void)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    display_ctx ctx = context_with_socket(sv[0]);
    int disconnects = 0;
    assert(set_fallback_callback(&ctx, disconnected, &disconnects) == 0);

    /* No-damage frames still send exactly one completion byte. */
    assert(trigger_refresh(&ctx) == 0);
    char byte = 1;
    assert(recv(sv[1], &byte, 1, 0) == 1 && byte == 0);
    assert(!ctx.fallback);

    /* A lost consumer must close the fence and enter fallback once. */
    int fence = eventfd(0, 0);
    assert(fence >= 0);
    set_render_fence(&ctx, fence);
    close(sv[1]);
    assert(trigger_refresh(&ctx) == -1);
    assert(ctx.fallback && ctx.fence_fd == -1);
    assert(ctx.pending_render_fence == -1 && disconnects == 1);
    errno = 0;
    assert(fcntl(fence, F_GETFD) == -1 && errno == EBADF);
    invalidate_consumer(&ctx);
    assert(disconnects == 1);
    puts("Anland completion channel tests passed");
    return 0;
}
