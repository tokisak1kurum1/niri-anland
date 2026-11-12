/* Test the Anland completion channel without requiring an Android device.
 *
 * gcc -std=gnu11 -O2 -Wall -Wextra anland-sys/tests/completion_handshake.c \
 *     anland-sys/src/vendor/socket_utils.c -ldl -o /tmp/anland-test
 * /tmp/anland-test
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

/* Include the producer for access to its private connection state. */
#include "../src/vendor/display_producer.c"

static display_ctx new_context(int producer_fd)
{
    display_ctx ctx = {0};
    ctx.ctrl_fd = ctx.data_fd = ctx.buf_ready_efd = -1;
    ctx.fence_fd = producer_fd;
    ctx.shm_fd = ctx.audio_fd = ctx.pending_render_fence = -1;
    for (int i = 0; i < MAX_BUFS; ++i)
        ctx.dmabuf_fds[i] = -1;
    return ctx;
}

static void count_disconnect(void *userdata)
{
    ++*(int *)userdata;
}

static void test_completion_without_fence(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    display_ctx ctx = new_context(sockets[0]);

    assert(trigger_refresh(&ctx) == 0);
    char byte = 1;
    assert(recv(sockets[1], &byte, 1, 0) == 1);
    assert(byte == 0 && !ctx.fallback);

    release_consumer_resources(&ctx);
    close(sockets[1]);
}

static void test_failed_completion_releases_fence_and_disconnects(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    display_ctx ctx = new_context(sockets[0]);
    int disconnects = 0;
    assert(set_fallback_callback(&ctx, count_disconnect, &disconnects) == 0);

    int fence = eventfd(0, 0);
    assert(fence >= 0);
    set_render_fence(&ctx, fence);
    close(sockets[1]);
    assert(trigger_refresh(&ctx) == -1);
    assert(ctx.fallback && ctx.fence_fd == -1);
    assert(ctx.pending_render_fence == -1 && disconnects == 1);
    errno = 0;
    assert(fcntl(fence, F_GETFD) == -1 && errno == EBADF);

    invalidate_consumer(&ctx);
    assert(disconnects == 1); /* Fallback callback is idempotent. */
}

int main(void)
{
    test_completion_without_fence();
    test_failed_completion_releases_fence_and_disconnects();
    puts("Anland completion channel tests passed");
    return 0;
}
