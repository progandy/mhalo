#include "svg.h"
#include <stdlib.h>
#include <stdio.h>

#include <nanosvgrast.h>

#define LOG_MODULE "svg"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "stride.h"

pixman_image_t *
svg_load(FILE *fp, const char *path)
{
    pixman_image_t *pix = NULL;
    uint8_t *data = NULL;
    int width, height, stride;
    bool ok = false;
    struct NSVGimage *image = NULL;
    struct NSVGrasterizer *rast = NULL;

    image = nsvgParseFromFile(path, "px", 96);
    width = image->width;
    height = image->height;
    if (width == 0 || height == 0) {
        LOG_DBG("%s: width and/or heigth is zero, not a SVG?", path);
        nsvgDelete(image);
        return NULL;
    }
    stride = stride_for_format_and_width(PIXMAN_a8b8g8r8, image->width);

    if (!(data = calloc(1, height * stride)))
        goto out;

    if (!(rast = nsvgCreateRasterizer()))
        goto out;

    nsvgRasterize(rast, image, 0, 0, 1, data, width, height, stride);

    ok = NULL != (pix = pixman_image_create_bits_no_clear(
            PIXMAN_a8b8g8r8, width, height, (uint32_t *)data, stride));

    if (!ok) {
        LOG_ERR("%s: failed to instanciate pixman image", path);
        goto out;
    }

    /* Copied from fuzzel */
    /* Nanosvg produces non-premultiplied ABGR, while pixman expects
     * premultiplied */
    for (uint32_t *abgr = (uint32_t *)data;
         abgr < (uint32_t *)(data + (size_t)image->width * (size_t)image->height * 4);
         abgr++) {
        uint8_t alpha = (*abgr >> 24) & 0xff;
        uint8_t blue = (*abgr >> 16) & 0xff;
        uint8_t green = (*abgr >> 8) & 0xff;
        uint8_t red = (*abgr >> 0) & 0xff;

        if (alpha == 0xff)
            continue;

        if (alpha == 0x00)
            blue = green = red = 0x00;
        else {
            blue = blue * alpha / 0xff;
            green = green * alpha / 0xff;
            red = red * alpha / 0xff;
        }

        *abgr = (uint32_t)alpha << 24 | blue << 16 | green << 8 | red;
    }

out:
    if (image)
        nsvgDelete(image);
    if (rast)
        nsvgDeleteRasterizer(rast);

    if (!ok)
        free(data);
    return pix;
}
