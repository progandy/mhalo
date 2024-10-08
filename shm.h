#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include <pixman.h>
#include <wayland-client.h>

struct buffer {
    int width;
    int height;
    int stride;
    unsigned long cookie;

    bool busy;
    bool purge;
    size_t size;
    void *mmapped;

    struct wl_buffer *wl_buf;
    pixman_image_t *pix;
    
    time_t last_used;  // Timestamp for last use
};

struct buffer *shm_get_buffer(struct wl_shm *shm, int width, int height, unsigned long cookie);
