#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/signalfd.h>

#include <wayland-client.h>
#include <wayland-cursor.h>

#include <pixman.h>
#include <tllist.h>
#include <wlr-layer-shell-unstable-v1.h>

#define LOG_MODULE "mhalo"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "shm.h"
#include "version.h"

static int cursor_x = 100;
static int cursor_y = 100;

/* Top-level globals */
static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_seat *seat;

static struct output *current_output = NULL;

static bool should_exit = false;
static bool have_argb8888 = false;

static pixman_image_t *fill = NULL;

struct output {
  struct wl_output *wl_output;
  uint32_t wl_name;

  char *make;
  char *model;

  int scale;
  int width;
  int height;

  int render_width;
  int render_height;

  struct wl_surface *surf;
  struct zwlr_layer_surface_v1 *layer;
  bool configured;

  int last_x;
  int last_y;

  // Add a frame_done flag for each output
  bool frame_done;
  bool wants_render;
  bool rendered_without_cursor;
};
static tll(struct output) outputs;

static bool stretch = false;

static void render(struct output *output);

static void frame_done_callback(void *data, struct wl_callback *callback,
                                uint32_t time) {
  struct output *output = data;
  output->frame_done = true; // Mark frame as done for this specific output
  wl_callback_destroy(callback);
  if (output->wants_render) {
    output->wants_render = false;
    render(output);
  }
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done_callback,
};

static void draw_circle(pixman_image_t *pix, int x, int y, int radius) {
  int width = pixman_image_get_width(pix);
  int height = pixman_image_get_height(pix);

  pixman_color_t white = {0xFFFF, 0xFFFF, 0xFFFF, 0x3FFF};

  for (int j = y - radius; j <= y + radius; j++) {
      int start = -1, end = -1;
    for (int i = x - radius; i <= x + radius; i++) {
      if (i >= 0 && i < width && j >= 0 && j < height) {
        int dx = i - x;
        int dy = j - y;
        if (dx * dx + dy * dy <= radius * radius) {
          if (start < 0) start = i;
          end = i;
        }
      }
    }
    if (end > 0) {
        pixman_image_fill_rectangles(PIXMAN_OP_HSL_LUMINOSITY, pix, &white, 1,
                                       &(pixman_rectangle16_t){start, j, end-start, 1});
    }
  }
}

static void render(struct output *output) {
  if (!output->frame_done) {
    output->wants_render = true;
    return; // Skip rendering if the previous frame isn't done
  }
  //
  // If the output is not the current output and has already been rendered
  // without the cursor, skip rendering
  if (output != current_output && output->rendered_without_cursor) {
    return;
  }

  const int width = output->render_width;
  const int height = output->render_height;
  const int scale = output->scale;

  struct buffer *buf =
      shm_get_buffer(shm, width * scale, height * scale, (uintptr_t)output);

  if (!buf)
    return;
  
  output->frame_done = false;

  pixman_image_t *src = fill;
  pixman_image_composite32(PIXMAN_OP_SRC, src, NULL, buf->pix, 0, 0, 0, 0, 0, 0,
                           width * scale, height * scale);

  wl_surface_set_buffer_scale(output->surf, scale);
  wl_surface_attach(output->surf, buf->wl_buf, 0, 0);
  // Draw the circle only on the current output
  wl_surface_damage_buffer(output->surf, (output->last_x - 50) * scale,
                           (output->last_y - 50) * scale, 100 * scale,
                           100 * scale);
  if (output->last_x == 0 && output->last_y == 0) {
    wl_surface_damage_buffer(output->surf, 0, 0, width * scale, height * scale);
  }
  if (output == current_output) {
    output->last_x = cursor_x;
    output->last_y = cursor_y;
    draw_circle(buf->pix, cursor_x * scale, cursor_y * scale, 40 * scale);
    wl_surface_damage_buffer(output->surf, (cursor_x - 50) * scale,
                             (cursor_y - 50) * scale, 100 * scale, 100 * scale);
    output->rendered_without_cursor =
        false; // Reset the flag as we're rendering the cursor
  } else {
    output->rendered_without_cursor =
        true; // Set the flag since the output is rendered without the cursor
  }

  // Create a callback to know when the frame is done
  struct wl_callback *callback = wl_surface_frame(output->surf);
  wl_callback_add_listener(callback, &frame_listener,
                           output); // Pass output as data

  wl_surface_commit(output->surf);
}

static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *surface,
                                    uint32_t serial, uint32_t w, uint32_t h) {
  struct output *output = data;
  zwlr_layer_surface_v1_ack_configure(surface, serial);

  /* If the size of the last committed buffer has not change, do not
   * render a new buffer because it will be identical to the old one. */
  /* TODO: should we check the scale? */
  if (output->configured && output->render_width == w &&
      output->render_height == h) {
    wl_surface_commit(output->surf);
    return;
  }
  // TODO: (Re)-Create double buffers
  output->render_width = w;
  output->render_height = h;
  output->configured = true;
  render(output);
}

static void output_layer_destroy(struct output *output) {
  if (output->layer != NULL)
    zwlr_layer_surface_v1_destroy(output->layer);
  if (output->surf != NULL)
    wl_surface_destroy(output->surf);

  output->layer = NULL;
  output->surf = NULL;
  output->configured = false;
}

static void layer_surface_closed(void *data,
                                 struct zwlr_layer_surface_v1 *surface) {
  struct output *output = data;

  /* Don’t trust ‘output’ to be valid, in case compositor destroyed
   * if before calling closed() */
  tll_foreach(outputs, it) {
    if (&it->item == output) {
      output_layer_destroy(output);
      break;
    }
  }
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = &layer_surface_configure,
    .closed = &layer_surface_closed,
};

static void output_destroy(struct output *output) {
  output_layer_destroy(output);

  if (output->wl_output != NULL)
    wl_output_release(output->wl_output);
  output->wl_output = NULL;

  free(output->make);
  free(output->model);
}

static void output_geometry(void *data, struct wl_output *wl_output, int32_t x,
                            int32_t y, int32_t physical_width,
                            int32_t physical_height, int32_t subpixel,
                            const char *make, const char *model,
                            int32_t transform) {
  struct output *output = data;

  free(output->make);
  free(output->model);

  output->make = make != NULL ? strdup(make) : NULL;
  output->model = model != NULL ? strdup(model) : NULL;
}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh) {
  if ((flags & WL_OUTPUT_MODE_CURRENT) == 0)
    return;

  struct output *output = data;
  output->width = width;
  output->height = height;
}

static void output_done(void *data, struct wl_output *wl_output) {
  struct output *output = data;
  const int width = output->width;
  const int height = output->height;
  const int scale = output->scale;

  LOG_INFO("output: %s %s (%dx%d, scale=%d)", output->make, output->model,
           width, height, scale);
}

static void output_scale(void *data, struct wl_output *wl_output,
                         int32_t factor) {
  struct output *output = data;
  output->scale = factor;

  if (output->configured)
    render(output);
}

static const struct wl_output_listener output_listener = {
    .geometry = &output_geometry,
    .mode = &output_mode,
    .done = &output_done,
    .scale = &output_scale,
};

static void shm_format(void *data, struct wl_shm *wl_shm, uint32_t format) {
  if (format == WL_SHM_FORMAT_ARGB8888)
    have_argb8888 = true;
}

static const struct wl_shm_listener shm_listener = {
    .format = &shm_format,
};

static void add_surface_to_output(struct output *output) {
  if (compositor == NULL || layer_shell == NULL)
    return;

  if (output->surf != NULL)
    return;

  struct wl_surface *surf = wl_compositor_create_surface(compositor);

  struct zwlr_layer_surface_v1 *layer = zwlr_layer_shell_v1_get_layer_surface(
      layer_shell, surf, output->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      "mouse_halo");

  zwlr_layer_surface_v1_set_exclusive_zone(layer, -1);
  zwlr_layer_surface_v1_set_keyboard_interactivity(layer, 0);
  zwlr_layer_surface_v1_set_anchor(layer,
                                   ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);

  output->surf = surf;
  output->layer = layer;

  zwlr_layer_surface_v1_add_listener(layer, &layer_surface_listener, output);
  wl_surface_commit(surf);
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t surface_x,
                           wl_fixed_t surface_y) {
  cursor_x = wl_fixed_to_int(surface_x);
  cursor_y = wl_fixed_to_int(surface_y);
  LOG_DBG("%u %u", cursor_x, cursor_y);

  // Redraw the surface when the cursor moves
  tll_foreach(outputs, it) render(&it->item);
}

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t surface_x, wl_fixed_t surface_y) {
  LOG_DBG("ENTER");
  cursor_x = wl_fixed_to_int(surface_x);
  cursor_y = wl_fixed_to_int(surface_y);

  tll_foreach(outputs, it) {
    if (it->item.surf == surface) {
      current_output = &it->item;
      break;
    }
  }
  render(current_output);
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface) {
  // When the pointer leaves the current output, set current_output to NULL
  current_output = NULL;
}

static void pointer_button(void *data, struct wl_pointer *wl_pointer,
                           uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state) {
  should_exit = true;
}

static void pointer_axis(void *data, struct wl_pointer *wl_pointer,
                         uint32_t time, uint32_t axis, wl_fixed_t value) {
  should_exit = true;
}

static void pointer_frame(void *data, struct wl_pointer *wl_pointer) {}

static void pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
                                uint32_t axis_source) {}

static void pointer_axis_stop(void *data, struct wl_pointer *wl_pointer,
                              uint32_t time, uint32_t axis) {}

static void pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
                                  uint32_t axis, int32_t discrete) {
  should_exit = true;
}

struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static struct wl_pointer *pointer;

static bool verify_iface_version(const char *iface, uint32_t version,
                                 uint32_t wanted) {
  if (version >= wanted)
    return true;

  LOG_ERR("%s: need interface version %u, but compositor only implements %u",
          iface, wanted, version);
  return false;
}

static void seat_capabilities(void *data, struct wl_seat *seat,
                              enum wl_seat_capability capabilities) {
  if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
    LOG_DBG("ADDED POINTER");
    pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(pointer, &pointer_listener, NULL);
  }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {}

static const struct wl_seat_listener seat_listener = {
    .capabilities = &seat_capabilities,
    .name = &seat_name,
};

static void handle_global(void *data, struct wl_registry *registry,
                          uint32_t name, const char *interface,
                          uint32_t version) {
  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    const uint32_t required = 4;
    if (!verify_iface_version(interface, version, required))
      return;

    compositor =
        wl_registry_bind(registry, name, &wl_compositor_interface, required);
  }

  else if (strcmp(interface, wl_shm_interface.name) == 0) {
    const uint32_t required = 1;
    if (!verify_iface_version(interface, version, required))
      return;

    shm = wl_registry_bind(registry, name, &wl_shm_interface, required);
    wl_shm_add_listener(shm, &shm_listener, NULL);
  }

  else if (strcmp(interface, wl_output_interface.name) == 0) {
    const uint32_t required = 3;
    if (!verify_iface_version(interface, version, required))
      return;

    struct wl_output *wl_output =
        wl_registry_bind(registry, name, &wl_output_interface, required);

    tll_push_back(outputs, ((struct output){.wl_output = wl_output,
                                            .wl_name = name,
                                            .surf = NULL,
                                            .layer = NULL,
                                            .frame_done = true}));

    struct output *output = &tll_back(outputs);
    wl_output_add_listener(wl_output, &output_listener, output);
    add_surface_to_output(output);
  }

  else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
    const uint32_t required = 2;
    if (!verify_iface_version(interface, version, required))
      return;

    layer_shell = wl_registry_bind(registry, name,
                                   &zwlr_layer_shell_v1_interface, required);
  } else if (strcmp(interface, wl_seat_interface.name) == 0) {
    seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    wl_seat_add_listener(seat, &seat_listener, NULL);
  }
}

static void handle_global_remove(void *data, struct wl_registry *registry,
                                 uint32_t name) {
  tll_foreach(outputs, it) {
    if (it->item.wl_name == name) {
      LOG_DBG("destroyed: %s %s", it->item.make, it->item.model);
      output_destroy(&it->item);
      tll_remove(outputs, it);
      return;
    }
  }
}

static const struct wl_registry_listener registry_listener = {
    .global = &handle_global,
    .global_remove = &handle_global_remove,
};

static void usage(const char *progname) {
  printf("Usage: %s [OPTIONS] \n"
         "\n"
         "Options:\n"
         "  -v,--version     show the version number and quit\n",
         progname);
}

static const char *version_and_features(void) {
  static char buf[256];
  snprintf(buf, sizeof(buf), "version: %s", WBG_VERSION);
  return buf;
}

int main(int argc, char *const *argv) {
  const char *progname = argv[0];

  const struct option longopts[] = {
      {"version", no_argument, 0, 'v'},
      {"help", no_argument, 0, 'h'},
      {NULL, no_argument, 0, 0},
  };

  while (true) {
    int c = getopt_long(argc, argv, "vh", longopts, NULL);
    if (c < 0)
      break;

    switch (c) {

    case 'v':
      printf("mhalo version: %s\n", version_and_features());
      return EXIT_SUCCESS;

    case 'h':
      usage(progname);
      return EXIT_SUCCESS;

    case ':':
      fprintf(stderr, "error: -%c: missing required argument\n", optopt);
      return EXIT_FAILURE;

    case '?':
      fprintf(stderr, "error: -%c: invalid option\n", optopt);
      return EXIT_FAILURE;
    }
  }

  stretch = (argc == 3);

  setlocale(LC_CTYPE, "");
  log_init(LOG_COLORIZE_AUTO, false, LOG_FACILITY_DAEMON, LOG_CLASS_WARNING);

  LOG_INFO("%s", WBG_VERSION);

  pixman_color_t black = {0, 0, 0, 0xbfff};
  fill = pixman_image_create_solid_fill(&black);

  int exit_code = EXIT_FAILURE;
  int sig_fd = -1;

  display = wl_display_connect(NULL);
  if (display == NULL) {
    LOG_ERR("failed to connect to wayland; no compositor running?");
    goto out;
  }

  registry = wl_display_get_registry(display);
  if (registry == NULL) {
    LOG_ERR("failed to get wayland registry");
    goto out;
  }

  wl_registry_add_listener(registry, &registry_listener, NULL);
  wl_display_roundtrip(display);

  if (compositor == NULL) {
    LOG_ERR("no compositor");
    goto out;
  }
  if (shm == NULL) {
    LOG_ERR("no shared memory buffers interface");
    goto out;
  }
  if (layer_shell == NULL) {
    LOG_ERR("no layer shell interface");
    goto out;
  }

  tll_foreach(outputs, it) add_surface_to_output(&it->item);

  wl_display_roundtrip(display);

  if (!have_argb8888) {
    LOG_ERR("shm: XRGB image format not available");
    goto out;
  }

  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGQUIT);

  sigprocmask(SIG_BLOCK, &mask, NULL);

  if ((sig_fd = signalfd(-1, &mask, 0)) < 0) {
    LOG_ERRNO("failed to create signal FD");
    goto out;
  }

  while (true) {
    wl_display_flush(display);

    struct pollfd fds[] = {
        {.fd = wl_display_get_fd(display), .events = POLLIN},
        {.fd = sig_fd, .events = POLLIN},
    };
    int ret = poll(fds, sizeof(fds) / sizeof(fds[0]), -1);

    if (ret < 0) {
      if (errno == EINTR)
        continue;

      LOG_ERRNO("failed to poll");
      break;
    }

    if (fds[0].revents & POLLHUP) {
      LOG_WARN("disconnected by compositor");
      break;
    }

    if (fds[0].revents & POLLIN) {
      if (wl_display_dispatch(display) < 0) {
        LOG_ERRNO("failed to dispatch Wayland events");
        break;
      }
    }

    if (fds[1].revents & POLLHUP)
      abort();

    if (fds[1].revents & POLLIN) {
      struct signalfd_siginfo info;
      ssize_t count = read(sig_fd, &info, sizeof(info));
      if (count < 0) {
        if (errno == EINTR)
          continue;

        LOG_ERRNO("failed to read from signal FD");
        break;
      }

      assert(count == sizeof(info));
      assert(info.ssi_signo == SIGINT || info.ssi_signo == SIGQUIT);

      LOG_INFO("goodbye");
      exit_code = EXIT_SUCCESS;
      break;
    }
    
    if (should_exit) {
      LOG_INFO("goodbye");
      exit_code = EXIT_SUCCESS;
      break;
    }
  }

out:

  if (sig_fd >= 0)
    close(sig_fd);

  tll_foreach(outputs, it) output_destroy(&it->item);
  tll_free(outputs);
  
  if (pointer != NULL)
    wl_pointer_destroy(pointer);
  if (seat != NULL)
    wl_seat_destroy(seat);
  if (layer_shell != NULL)
    zwlr_layer_shell_v1_destroy(layer_shell);
  if (shm != NULL)
    wl_shm_destroy(shm);
  if (compositor != NULL)
    wl_compositor_destroy(compositor);
  if (registry != NULL)
    wl_registry_destroy(registry);
  if (display != NULL)
    wl_display_disconnect(display);
  if (fill != NULL)
    pixman_image_unref(fill);
  log_deinit();
  return exit_code;
}
