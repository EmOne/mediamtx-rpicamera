#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/ioctl.h>

#include <linux/dma-buf.h>

#include "parameters.h"
#include "pipe.h"
#include "camera.h"
#include "text.h"
#include "encoder.h"

static int pipe_video_fd;
static pthread_mutex_t pipe_video_mutex;
static text_t *text;
static encoder_t *enc;

static void on_frame(
    uint8_t *mapped,
    int fd,
    uint64_t size,
    uint64_t ts) {
    // mapped DMA buffers require a DMA_BUF_IOCTL_SYNC before and after usage.
    // https://forums.raspberrypi.com/viewtopic.php?t=352554
    struct dma_buf_sync dma_sync = {0};
    dma_sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &dma_sync);

    text_draw(text, mapped);

    dma_sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &dma_sync);

    encoder_encode(enc, mapped, fd, size, ts);
}

static void on_encoder_output(const uint8_t *mapped, uint64_t size, uint64_t ts) {
    pthread_mutex_lock(&pipe_video_mutex);
    pipe_write_buf(pipe_video_fd, mapped, size, ts);
    pthread_mutex_unlock(&pipe_video_mutex);
}

int main() {
    if (getenv("TEST") != NULL) {
        printf("test passed\n");
        return 0;
    }

    int pipe_conf_fd = atoi(getenv("PIPE_CONF_FD"));
    pipe_video_fd = atoi(getenv("PIPE_VIDEO_FD"));

    uint8_t *buf;
    uint32_t n = pipe_read(pipe_conf_fd, &buf);

    parameters_t params;
    bool ok = parameters_unserialize(&params, &buf[1], n-1);
    free(buf);
    if (!ok) {
        pipe_write_error(pipe_video_fd, "parameters_unserialize(): %s", parameters_get_error());
        return -1;
    }

    pthread_mutex_init(&pipe_video_mutex, NULL);
    pthread_mutex_lock(&pipe_video_mutex);

    camera_t *cam;
    ok = camera_create(
        &params,
        on_frame,
        &cam);
    if (!ok) {
        pipe_write_error(pipe_video_fd, "camera_create(): %s", camera_get_error());
        return -1;
    }

    ok = text_create(
        &params,
        camera_get_stride(cam),
        &text);
    if (!ok) {
        pipe_write_error(pipe_video_fd, "text_create(): %s", text_get_error());
        return -1;
    }

    ok = encoder_create(
        &params,
        camera_get_stride(cam),
        camera_get_colorspace(cam),
        on_encoder_output,
        &enc);
    if (!ok) {
        pipe_write_error(pipe_video_fd, "encoder_create(): %s", encoder_get_error());
        return -1;
    }

    ok = camera_start(cam);
    if (!ok) {
        pipe_write_error(pipe_video_fd, "camera_start(): %s", camera_get_error());
        return -1;
    }

    pipe_write_ready(pipe_video_fd);
    pthread_mutex_unlock(&pipe_video_mutex);

    while (true) {
        uint8_t *buf;
        uint32_t n = pipe_read(pipe_conf_fd, &buf);

        switch (buf[0]) {
        case 'e':
            return 0;

        case 'c':
            {
                parameters_t params;
                bool ok = parameters_unserialize(&params, &buf[1], n-1);
                free(buf);
                if (!ok) {
                    printf("skipping reloading parameters since they are invalid: %s\n", parameters_get_error());
                    continue;
                }

                camera_reload_params(cam, &params);
                encoder_reload_params(enc, &params);
                parameters_destroy(&params);
            }
        }
    }

    return 0;
}
