#include "shm.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <tllist.h>

#define LOG_MODULE "shm"
#include "log.h"
#include "stride.h"

#if !defined(MAP_UNINITIALIZED)
#define MAP_UNINITIALIZED 0
#endif

#if !defined(MFD_NOEXEC_SEAL)
#define MFD_NOEXEC_SEAL 0
#endif

#define BUFFER_TIMEOUT_SEC 3

static tll(struct buffer *) buffer_queue = tll_init();

static void buffer_destroy(struct buffer *buf) {
  pixman_image_unref(buf->pix);
  wl_buffer_destroy(buf->wl_buf);
  munmap(buf->mmapped, buf->size);
  free(buf);
}

static void buffer_release(void *data, struct wl_buffer *wl_buffer) {
  struct buffer *buffer = data;

  // Mark buffer as not busy and update last used time
  buffer->busy = false;
  buffer->last_used = time(NULL);

  // Move the buffer to the reusable queue
  tll_push_back(buffer_queue, buffer);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = &buffer_release,
};

static void cleanup_old_buffers() {
  time_t now = time(NULL);

  tll_foreach(buffer_queue, it) {
    if (!it->item->busy &&
        difftime(now, it->item->last_used) >= BUFFER_TIMEOUT_SEC) {
      buffer_destroy(it->item);
      tll_remove(buffer_queue, it);
    }
  }
}

struct buffer *shm_get_buffer(struct wl_shm *shm, int width, int height,
                              unsigned long cookie) {
  cleanup_old_buffers();

  // Try to reuse a buffer from the queue
  tll_foreach(buffer_queue, it) {
    struct buffer *buffer = it->item;
    if (!buffer->busy && buffer->width == width && buffer->height == height &&
        buffer->cookie == cookie) {
      tll_remove(buffer_queue, it);
      buffer->busy = true;
      buffer->cookie = cookie;
      return buffer;
    }
  }

  // If no reusable buffer is found, create a new one
  int pool_fd = -1;
  void *mmapped = NULL;
  size_t size = 0;

  struct wl_shm_pool *pool = NULL;
  struct wl_buffer *buf = NULL;
  pixman_image_t *pix = NULL;

  errno = 0;
  pool_fd = memfd_create("mhalo-wayland-shm-buffer-pool",
                         MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL);

  if (pool_fd < 0 && errno == EINVAL) {
    pool_fd = memfd_create("mhalo-wayland-shm-buffer-pool",
                           MFD_CLOEXEC | MFD_ALLOW_SEALING);
  }

  if (pool_fd == -1) {
    LOG_ERRNO("failed to create SHM backing memory file");
    goto err;
  }

  const uint32_t stride = stride_for_format_and_width(PIXMAN_a8r8g8b8, width);
  size = stride * height;
  if (ftruncate(pool_fd, size) == -1) {
    LOG_ERRNO("failed to truncate SHM pool");
    goto err;
  }

  mmapped = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, pool_fd, 0);
  if (mmapped == MAP_FAILED) {
    LOG_ERR("failed to mmap SHM backing memory file");
    goto err;
  }

  if (fcntl(pool_fd, F_ADD_SEALS,
            F_SEAL_GROW | F_SEAL_SHRINK |
                /*F_SEAL_FUTURE_WRITE |*/ F_SEAL_SEAL) < 0) {
    LOG_ERRNO("failed to seal SHM backing memory file");
  }

  pool = wl_shm_create_pool(shm, pool_fd, size);
  if (pool == NULL) {
    LOG_ERR("failed to create SHM pool");
    goto err;
  }

  buf = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                  WL_SHM_FORMAT_ARGB8888);
  if (buf == NULL) {
    LOG_ERR("failed to create SHM buffer");
    goto err;
  }

  wl_shm_pool_destroy(pool);
  pool = NULL;
  close(pool_fd);
  pool_fd = -1;

  pix = pixman_image_create_bits_no_clear(PIXMAN_x8r8g8b8, width, height,
                                          mmapped, stride);
  if (pix == NULL) {
    LOG_ERR("failed to create pixman image");
    goto err;
  }

  struct buffer *buffer = malloc(sizeof(*buffer));
  *buffer = (struct buffer){
      .width = width,
      .height = height,
      .stride = stride,
      .cookie = cookie,
      .busy = true,
      .size = size,
      .mmapped = mmapped,
      .wl_buf = buf,
      .pix = pix,
      .last_used = time(NULL), // Initialize with current time
  };

  wl_buffer_add_listener(buffer->wl_buf, &buffer_listener, buffer);
  return buffer;

err:
  if (pix != NULL)
    pixman_image_unref(pix);
  if (buf != NULL)
    wl_buffer_destroy(buf);
  if (pool != NULL)
    wl_shm_pool_destroy(pool);
  if (pool_fd != -1)
    close(pool_fd);
  if (mmapped != NULL)
    munmap(mmapped, size);

  return NULL;
}
