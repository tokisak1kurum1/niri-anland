/*
 * Standalone C tests for Anland's producer->consumer completion channel.
 *
 * Build on Linux (no Android device or EGL context required):
 *   gcc -std=gnu11 -O2 -Wall -Wextra \
 *       anland-sys/tests/completion_handshake.c \
 *       anland-sys/src/vendor/socket_utils.c \
 *       -ldl -o /tmp/anland-completion-test
 *   /tmp/anland-completion-test
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

/* Include the implementation to exercise its internal fallback state and
 * descriptor ownership without adding test-only symbols to the public API. */
#include "../src/vendor/display_producer.c"

static display_ctx test_context(int producer_fd)
{
    display_ctx ctx = {0};
    ctx.ctrl_fd = -1;
    ctx.data_fd = -1;
    ctx.buf_ready_efd = -1;
    ctx.fence_fd = producer_fd;
    ctx.shm_fd = -1;
    ctx.audio_fd = -1;
    ctx.pending_render_fence = -1;
    ctx.fallback = false;
    for (int i = 0; i < MAX_BUFS; ++i)
        ctx.dmabuf_fds[i] = -1;
    return ctx;
}

static void test_no_damage_completion_sends_one_byte(void)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    display_ctx ctx = test_context(sv[0]);
    assert(trigger_refresh(&ctx) == 0);
    char byte = -1;
    assert(recv(sv[1], &byte, 1, 0) == 1);
    assert(byte == 0);
    assert(!ctx.fallback);
    release_consumer_resources(&ctx);
    close(sv[1]);
}

static void test_native_fence_descriptor_transfer(void)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    display_ctx ctx = test_context(sv[0]);
    int fence = eventfd(0, 0);
    assert(fence >= 0);
    set_render_fence(&ctx, fence);
    assert(trigger_refresh(&ctx) == 0);
    assert(ctx.pending_render_fence == -1);
    errno = 0;
    assert(fcntl(fence, F_GETFD) == -1 && errno == EBADF);

    char byte = -1;
    struct iovec iov = {.iov_base = &byte, .iov_len = 1};
    union {
        char data[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } ancillary = {0};
    struct msghdr msg = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = ancillary.data,
        .msg_controllen = sizeof(ancillary.data),
    };
    assert(recvmsg(sv[1], &msg, 0) == 1);
    assert(byte == 0);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    assert(cmsg != NULL);
    assert(cmsg->cmsg_level == SOL_SOCKET);
    assert(cmsg->cmsg_type == SCM_RIGHTS);
    int received = -1;
    memcpy(&received, CMSG_DATA(cmsg), sizeof(received));
    assert(received >= 0);
    uint64_t one = 1;
    assert(write(received, &one, sizeof(one)) == sizeof(one));
    close(received);
    release_consumer_resources(&ctx);
    close(sv[1]);
}

static void count_disconnect(void *userdata)
{
    ++*(int *)userdata;
}

static void test_send_failure_enters_fallback(void)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    display_ctx ctx = test_context(sv[0]);
    int disconnects = 0;
    assert(set_fallback_callback(&ctx, count_disconnect, &disconnects) == 0);
    close(sv[1]); /* EPIPE, without SIGPIPE thanks to MSG_NOSIGNAL */
    assert(trigger_refresh(&ctx) == -1);
    assert(ctx.fallback);
    assert(ctx.fence_fd == -1);
    assert(ctx.pending_render_fence == -1);
    assert(disconnects == 1);
    invalidate_consumer(&ctx); /* repeated transitions must be idempotent */
    assert(disconnects == 1);
}

static void test_out_of_order_request_invalidation(void)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    display_ctx ctx = test_context(sv[0]);
    int disconnects = 0;
    assert(set_fallback_callback(&ctx, count_disconnect, &disconnects) == 0);
    invalidate_consumer(&ctx);
    assert(ctx.fallback);
    assert(ctx.fence_fd == -1);
    assert(disconnects == 1);
    close(sv[1]);
}

int main(void)
{
    test_no_damage_completion_sends_one_byte();
    test_native_fence_descriptor_transfer();
    test_send_failure_enters_fallback();
    test_out_of_order_request_invalidation();
    puts("Anland completion channel tests passed");
    return 0;
}
