/*******************************************************************************
 *
 *
 *    WashingtonDC Dreamcast Emulator
 *    Copyright (C) 2017, 2018 snickerbockers
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 ******************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "error.h"
#include "hw/pvr2/spg.h"
#include "hw/pvr2/pvr2_core_reg.h"
#include "hw/pvr2/pvr2_tex_mem.h"
#include "hw/pvr2/pvr2_tex_cache.h"
#include "hw/pvr2/pvr2_gfx_obj.h"
#include "gfx/gfx.h"
#include "gfx/gfx_il.h"
#include "gfx/gfx_obj.h"
#include "log.h"

#include "framebuffer.h"

static DEF_ERROR_INT_ATTR(width)
static DEF_ERROR_INT_ATTR(height)
static DEF_ERROR_INT_ATTR(fb_pix_fmt)

#define OGL_FB_W_MAX (0x3ff + 1)
#define OGL_FB_H_MAX (0x3ff + 1)
#define OGL_FB_BYTES (OGL_FB_W_MAX * OGL_FB_H_MAX * 4)
static uint8_t ogl_fb[OGL_FB_BYTES];
static unsigned stamp;

enum fb_state {
    FB_STATE_INVALID = 0,
    FB_STATE_VIRT = 1,
    FB_STATE_GFX = 2,
    FB_STATE_VIRT_AND_GFX = 3
};

enum fb_pix_fmt {
    FB_PIX_FMT_RGB_555,
    FB_PIX_FMT_RGB_565,
    FB_PIX_FMT_RGB_888,
    FB_PIX_FMT_0RGB_0888
};

struct fb_flags {
    uint8_t state : 2;
    uint8_t fmt : 2;
    uint8_t vert_flip : 1;
    uint8_t intl : 1;
};

#define FB_HEAP_SIZE 8
struct framebuffer {
    int obj_handle;
    unsigned fb_width, fb_height;

    // only used for writing back to texture memory
    unsigned linestride;

    uint32_t addr_first[2], addr_last[2];
    uint32_t addr_key; // min of addr_first[0], addr_first[1]

    unsigned stamp;

    unsigned tile_w, tile_h;
    unsigned x_clip_min, x_clip_max, y_clip_min, y_clip_max;

    struct fb_flags flags;
};

static unsigned bytes_per_pix(uint32_t fb_r_ctrl) {
    unsigned px_tp = (fb_r_ctrl & 0xc) >> 2;

    if (px_tp == 0 || px_tp == 1)
        return 2;
    if (px_tp == 3)
        return 4;

    RAISE_ERROR(ERROR_UNIMPLEMENTED);
}

static uint8_t *get_tex_mem_area(addr32_t addr);

/*
 * The concat parameter in these functions corresponds to the fb_concat value
 * in FB_R_CTRL; it is appended as the lower 3/2 bits to each color component
 * to convert that component from 5/6 bits to 8 bits.
 *
 * One "gotcha" to note about the below functions is that
 * conv_rgb555_to_argb8888 and conv_rgb565_to_rgb8888 expect their inputs to be
 * arrays of uint16_t with each element representing one pixel, and
 * conv_rgb0888_to_argb8888 expects its input to be uint32_t with each element
 * representing one pixel BUT conv_rgb888_to_argb8888 expects its input to be
 * uint8_t with every *three* elements representing one pixel.
 */
static void
conv_rgb565_to_rgba8888(uint32_t *pixels_out,
                        uint16_t const *pixels_in,
                        unsigned n_pixels, uint8_t concat);
__attribute__((unused))
static void
conv_rgb888_to_argb8888(uint32_t *pixels_out,
                        uint8_t const *pixels_in,
                        unsigned n_pixels);
static void
conv_rgb0888_to_rgba8888(uint32_t *pixels_out,
                         uint32_t const *pixels_in,
                         unsigned n_pixels);

static void
sync_fb_from_tex_mem_rgb565_intl(struct framebuffer *fb,
                                 unsigned fb_width, unsigned fb_height,
                                 uint32_t sof1, uint32_t sof2,
                                 unsigned modulus, unsigned concat) {
    /*
     * field_adv represents the distand between the start of one row and the
     * start of the next row in the same field in terms of bytes.
     */
    unsigned field_adv = fb_width * 2 + modulus * 4 - 4;
    unsigned rows_per_field = fb_height / 2;

    addr32_t first_addr_field1 = sof1;
    addr32_t last_addr_field1 = sof1 +
        field_adv * (rows_per_field - 1) + 2 * (fb_width - 1);
    addr32_t first_addr_field2 = sof2;
    addr32_t last_addr_field2 = sof2 +
        field_adv * (rows_per_field - 1) + 2 * (fb_width - 1);

    // bounds checking.
    addr32_t bounds_field1[2] = {
        first_addr_field1 + ADDR_TEX32_FIRST,
        last_addr_field1 + ADDR_TEX32_FIRST
    };
    addr32_t bounds_field2[2] = {
        first_addr_field2 + ADDR_TEX32_FIRST,
        last_addr_field2 + ADDR_TEX32_FIRST
    };
    if (bounds_field1[0] < ADDR_TEX32_FIRST ||
        bounds_field1[0] > ADDR_TEX32_LAST ||
        bounds_field1[1] < ADDR_TEX32_FIRST ||
        bounds_field1[1] > ADDR_TEX32_LAST ||
        bounds_field2[0] < ADDR_TEX32_FIRST ||
        bounds_field2[0] > ADDR_TEX32_LAST ||
        bounds_field2[1] < ADDR_TEX32_FIRST ||
        bounds_field2[1] > ADDR_TEX32_LAST) {
        error_set_feature("whatever happens when a framebuffer is configured "
                          "to read outside of texture memory");
        RAISE_ERROR(ERROR_UNIMPLEMENTED);
    }

    uint32_t *dst_fb = (uint32_t*)ogl_fb;

    unsigned row;
    for (row = 0; row < rows_per_field; row++) {
        uint16_t const *ptr_row1 =
            (uint16_t const*)(pvr2_tex32_mem + sof1 + row * field_adv);
        uint16_t const *ptr_row2 =
            (uint16_t const*)(pvr2_tex32_mem + sof2 + row * field_adv);

        conv_rgb565_to_rgba8888(dst_fb + row * 2 * fb_width,
                                ptr_row1, fb_width, concat);
        conv_rgb565_to_rgba8888(dst_fb + (row * 2 + 1) * fb_width,
                                ptr_row2, fb_width, concat);
    }

    fb->addr_key = first_addr_field1  < first_addr_field2 ?
        first_addr_field1 : first_addr_field2;

    fb->addr_first[0] = first_addr_field1;
    fb->addr_first[1] = first_addr_field2;
    fb->addr_last[0] = last_addr_field1;
    fb->addr_last[1] = last_addr_field2;

    fb->fb_width = fb_width;
    fb->fb_height = fb_height;

    fb->flags.state = FB_STATE_VIRT_AND_GFX;

    fb->flags.vert_flip = true;
    fb->flags.intl = true;
    fb->stamp = stamp;
    fb->flags.fmt = FB_PIX_FMT_RGB_565;

    struct gfx_il_inst cmd;

    cmd.op = GFX_IL_WRITE_OBJ;
    cmd.arg.write_obj.dat = dst_fb;
    cmd.arg.write_obj.obj_no = fb->obj_handle;
    cmd.arg.write_obj.n_bytes = OGL_FB_W_MAX * OGL_FB_H_MAX * 4;

    rend_exec_il(&cmd, 1);
}

static void
sync_fb_from_tex_mem_rgb565_prog(struct framebuffer *fb,
                                 unsigned fb_width, unsigned fb_height,
                                 uint32_t sof1, unsigned concat) {
    unsigned field_adv = fb_width;
    uint16_t const *pixels_in = (uint16_t*)(pvr2_tex32_mem + sof1);
    /*
     * bounds checking
     *
     * TODO: is it really necessary to test for
     * (last_byte < ADDR_TEX32_FIRST || first_byte > ADDR_TEX32_LAST) ?
     */
    addr32_t last_byte = sof1 + fb_width * fb_height * 2;
    addr32_t first_byte = sof1;

    // bounds checking.
    addr32_t bounds_field1[2] = {
        first_byte + ADDR_TEX32_FIRST,
        last_byte + ADDR_TEX32_FIRST
    };
    if (bounds_field1[0] < ADDR_TEX32_FIRST ||
        bounds_field1[0] > ADDR_TEX32_LAST ||
        bounds_field1[1] < ADDR_TEX32_FIRST ||
        bounds_field1[1] > ADDR_TEX32_LAST) {
        error_set_feature("whatever happens when a framebuffer is configured "
                          "to read outside of texture memory");
        RAISE_ERROR(ERROR_UNIMPLEMENTED);
    }

    uint32_t *dst_fb = (uint32_t*)ogl_fb;
    memset(ogl_fb, 0xff, sizeof(ogl_fb));

    unsigned row;
    for (row = 0; row < fb_height; row++) {
        uint16_t const *in_col_start = pixels_in + field_adv * row;
        uint32_t *out_col_start = dst_fb + row * fb_width;

        conv_rgb565_to_rgba8888(out_col_start, in_col_start, fb_width, concat);
    }

    fb->fb_width = fb_width;
    fb->fb_height = fb_height;
    fb->addr_key = first_byte;
    fb->addr_first[0] = first_byte;
    fb->addr_first[1] = first_byte;
    fb->addr_last[0] = last_byte;
    fb->addr_last[1] = last_byte;

    fb->flags.state = FB_STATE_VIRT_AND_GFX;

    fb->flags.vert_flip = true;
    fb->flags.intl = false;
    fb->stamp = stamp;
    fb->flags.fmt = FB_PIX_FMT_RGB_565;

    struct gfx_il_inst cmd;

    cmd.op = GFX_IL_WRITE_OBJ;
    cmd.arg.write_obj.dat = dst_fb;
    cmd.arg.write_obj.obj_no = fb->obj_handle;
    cmd.arg.write_obj.n_bytes = OGL_FB_W_MAX * OGL_FB_H_MAX * 4;

    rend_exec_il(&cmd, 1);
}

static void
sync_fb_from_tex_mem_rgb0888_intl(struct framebuffer *fb,
                                  unsigned fb_width, unsigned fb_height,
                                  uint32_t sof1, uint32_t sof2,
                                  unsigned modulus) {
    /*
     * field_adv represents the distand between the start of one row and the
     * start of the next row in the same field in terms of bytes.
     */
    unsigned field_adv = (fb_width * 4) + (modulus * 4) - 4;
    unsigned rows_per_field = fb_height /* / 2 */;

    addr32_t first_addr_field1 = sof1;
    addr32_t last_addr_field1 = sof1 +
        field_adv * (rows_per_field - 1) + 2 * (fb_width - 1);
    addr32_t first_addr_field2 = sof2;
    addr32_t last_addr_field2 = sof2 +
        field_adv * (rows_per_field - 1) + 2 * (fb_width - 1);

    // bounds checking.
    addr32_t bounds_field1[2] = {
        first_addr_field1 + ADDR_TEX32_FIRST,
        last_addr_field1 + ADDR_TEX32_FIRST
    };
    addr32_t bounds_field2[2] = {
        first_addr_field2 + ADDR_TEX32_FIRST,
        last_addr_field2 + ADDR_TEX32_FIRST
    };
    if (bounds_field1[0] < ADDR_TEX32_FIRST ||
        bounds_field1[0] > ADDR_TEX32_LAST ||
        bounds_field1[1] < ADDR_TEX32_FIRST ||
        bounds_field1[1] > ADDR_TEX32_LAST ||
        bounds_field2[0] < ADDR_TEX32_FIRST ||
        bounds_field2[0] > ADDR_TEX32_LAST ||
        bounds_field2[1] < ADDR_TEX32_FIRST ||
        bounds_field2[1] > ADDR_TEX32_LAST) {
        error_set_feature("whatever happens when a framebuffer is configured "
                          "to read outside of texture memory");
        RAISE_ERROR(ERROR_UNIMPLEMENTED);
    }

    uint32_t *dst_fb = (uint32_t*)ogl_fb;

    unsigned row;
    for (row = 0; row < rows_per_field; row++) {
        uint32_t const *ptr_row1 =
            (uint32_t const*)(pvr2_tex32_mem + sof1 + row * field_adv);
        uint32_t const *ptr_row2 =
            (uint32_t const*)(pvr2_tex32_mem + sof2 + row * field_adv);

        conv_rgb0888_to_rgba8888(dst_fb + (row << 1) * fb_width,
                                 ptr_row1, fb_width);
        conv_rgb0888_to_rgba8888(dst_fb + ((row << 1) + 1) * fb_width,
                                 ptr_row2, fb_width);
    }

    fb->fb_width = fb_width;
    fb->fb_height = fb_height;

    fb->addr_key = first_addr_field1 < first_addr_field2 ?
        first_addr_field1 : first_addr_field2;

    fb->addr_first[0] = first_addr_field1;
    fb->addr_first[1] = first_addr_field2;
    fb->addr_last[0] = last_addr_field1;
    fb->addr_last[1] = last_addr_field2;

    fb->flags.state = FB_STATE_VIRT_AND_GFX;

    fb->flags.vert_flip = true;
    fb->flags.intl = true;
    fb->stamp = stamp;
    fb->flags.fmt = FB_PIX_FMT_0RGB_0888;

    struct gfx_il_inst cmd;

    cmd.op = GFX_IL_WRITE_OBJ;
    cmd.arg.write_obj.dat = dst_fb;
    cmd.arg.write_obj.obj_no = fb->obj_handle;
    cmd.arg.write_obj.n_bytes = OGL_FB_W_MAX * OGL_FB_H_MAX * 4;

    rend_exec_il(&cmd, 1);
}

static void
sync_fb_from_tex_mem_rgb0888_prog(struct framebuffer *fb,
                                  unsigned fb_width, unsigned fb_height,
                                  uint32_t sof1) {
    uint32_t const *pixels_in = (uint32_t const*)(pvr2_tex32_mem + sof1);
    addr32_t last_byte = sof1 + fb_width * fb_height * 4;
    addr32_t first_byte = sof1;

    /*
     * bounds checking
     *
     * TODO: is it really necessary to test for
     * (last_byte < ADDR_TEX32_FIRST || first_byte > ADDR_TEX32_LAST) ?
     */
    addr32_t bounds_field1[2] = {
        first_byte + ADDR_TEX32_FIRST,
        last_byte + ADDR_TEX32_FIRST
    };
    if (bounds_field1[0] < ADDR_TEX32_FIRST ||
        bounds_field1[0] > ADDR_TEX32_LAST ||
        bounds_field1[1] < ADDR_TEX32_FIRST ||
        bounds_field1[1] > ADDR_TEX32_LAST) {
        error_set_feature("whatever happens when a framebuffer is configured "
                          "to read outside of texture memory");
        RAISE_ERROR(ERROR_UNIMPLEMENTED);
    }

    uint32_t *dst_fb = (uint32_t*)ogl_fb;

    unsigned row;
    for (row = 0; row < fb_height; row++) {
        uint32_t const *in_col_start = pixels_in + fb_width * row;
        uint32_t *out_col_start = dst_fb + row * fb_width;

        conv_rgb0888_to_rgba8888(out_col_start, in_col_start, fb_width);
    }

    fb->fb_width = fb_width;
    fb->fb_height = fb_height;
    fb->addr_key = first_byte;
    fb->addr_first[0] = first_byte;
    fb->addr_first[1] = first_byte;
    fb->addr_last[0] = last_byte;
    fb->addr_last[1] = last_byte;

    fb->flags.state = FB_STATE_VIRT_AND_GFX;

    fb->flags.vert_flip = true;
    fb->flags.intl = false;
    fb->stamp = stamp;
    fb->flags.fmt = FB_PIX_FMT_0RGB_0888;

    struct gfx_il_inst cmd;

    cmd.op = GFX_IL_WRITE_OBJ;
    cmd.arg.write_obj.dat = dst_fb;
    cmd.arg.write_obj.obj_no = fb->obj_handle;
    cmd.arg.write_obj.n_bytes = OGL_FB_W_MAX * OGL_FB_H_MAX * 4;

    rend_exec_il(&cmd, 1);
}

static void
sync_fb_from_tex_mem(struct framebuffer *fb, unsigned width, unsigned height,
                     unsigned modulus, unsigned concat) {
    bool interlace = get_spg_control() & (1 << 4);

    uint32_t fb_r_sof1 = get_fb_r_sof1() & ~3;
    uint32_t fb_r_sof2 = get_fb_r_sof2() & ~3;

    uint32_t fb_r_ctrl = get_fb_r_ctrl();
    unsigned px_tp = (fb_r_ctrl & 0xc) >> 2;
    switch (px_tp) {
    case 0:
        // 16-bit 555 RGB
        error_set_feature("video mode RGB555");
        RAISE_ERROR(ERROR_UNIMPLEMENTED);
        break;
    case 1:
        // 16-bit 565 RGB
        if (interlace) {
            sync_fb_from_tex_mem_rgb565_intl(fb, width, height, fb_r_sof1,
                                             fb_r_sof2, modulus, concat);
        } else {
            sync_fb_from_tex_mem_rgb565_prog(fb, width, height,
                                             fb_r_sof1, concat);
        }
        break;
    case 2:
        // 24-bit 888 RGB
        error_set_feature("video mode RGB888");
        RAISE_ERROR(ERROR_UNIMPLEMENTED);
        break;
    case 3:
        // 32-bit 08888 RGB
        if (interlace) {
            sync_fb_from_tex_mem_rgb0888_intl(fb, width, height, fb_r_sof1,
                                              fb_r_sof2, modulus);
        } else {
            sync_fb_from_tex_mem_rgb0888_prog(fb, width, height,
                                              fb_r_sof1);
        }
    }
}

static struct framebuffer fb_heap[FB_HEAP_SIZE];

/*
 * this is a simple "dumb" memcpy function that doesn't handle the framebuffer
 * state (this is what makes it different from pvr2_tex_mem_area32_write).  It
 * does, however, perform bounds-checking and raise an error for out-of-bounds
 * memory access.
 */
static void copy_to_tex_mem(void const *in, addr32_t offs, size_t len);

static void conv_rgb565_to_rgba8888(uint32_t *pixels_out,
                                    uint16_t const *pixels_in,
                                    unsigned n_pixels, uint8_t concat) {
    for (unsigned idx = 0; idx < n_pixels; idx++) {
        uint16_t pix = pixels_in[idx];
        uint32_t r = (((pix & 0xf800) >> 11) << 3) | concat;
        uint32_t g = (((pix & 0x07e0) >> 5) << 2) | (concat & 0x3);
        uint32_t b = ((pix & 0x001f) << 3) | concat;

        pixels_out[idx] = (255 << 24) | (b << 16) | (g << 8) | r;
    }
}

static void
conv_rgb888_to_argb8888(uint32_t *pixels_out,
                        uint8_t const *pixels_in,
                        unsigned n_pixels) {
    for (unsigned idx = 0; idx < n_pixels; idx++) {
        uint8_t const *pix = pixels_in + idx * 3;
        uint32_t r = pix[0];
        uint32_t g = pix[1];
        uint32_t b = pix[2];

        pixels_out[idx] = (255 << 24) | (r << 16) | (g << 8) | b;
    }
}

static void
conv_rgb0888_to_rgba8888(uint32_t *pixels_out,
                         uint32_t const *pixels_in,
                         unsigned n_pixels) {
    for (unsigned idx = 0; idx < n_pixels; idx++) {
        uint32_t pix = pixels_in[idx];
        uint32_t r = (pix & 0x00ff0000) >> 16;
        uint32_t g = (pix & 0x0000ff00) >> 8;
        uint32_t b = (pix & 0x000000ff);
        pixels_out[idx] = (255 << 24) | (b << 16) | (g << 8) | r;
    }
}

static int pick_fb(unsigned width, unsigned height, uint32_t addr);

void framebuffer_init(unsigned width, unsigned height) {
    struct gfx_il_inst cmd;

    int fb_no;
    for (fb_no = 0; fb_no < FB_HEAP_SIZE; fb_no++) {
        fb_heap[fb_no].obj_handle = pvr2_alloc_gfx_obj();

        cmd.op = GFX_IL_INIT_OBJ;
        cmd.arg.init_obj.obj_no = fb_heap[fb_no].obj_handle;
        cmd.arg.init_obj.n_bytes = OGL_FB_W_MAX * OGL_FB_H_MAX *
            4 * sizeof(uint8_t);

        rend_exec_il(&cmd, 1);
    }
}

void framebuffer_render() {
    uint32_t fb_r_ctrl = get_fb_r_ctrl();
    if (!(fb_r_ctrl & 1)) {
        LOG_DBG("framebuffer disabled\n");
        // framebuffer is not enabled.
        // TODO: display all-white or all black here instead of letting
        // the screen look corrupted?
        return;
    }

    bool interlace = get_spg_control() & (1 << 4);
    uint32_t fb_r_size = get_fb_r_size();
    uint32_t fb_r_sof1 = get_fb_r_sof1() & ~3;

    unsigned modulus = (fb_r_size >> 20) & 0x3ff;
    unsigned concat = (fb_r_ctrl >> 4) & 7;

    unsigned width_scale = 4 / bytes_per_pix(get_fb_r_ctrl());
    unsigned width = ((get_fb_r_size() & 0x3ff) + 1) * width_scale;
    unsigned height = ((fb_r_size >> 10) & 0x3ff) + 1;
    if (interlace)
        height *= 2;

    struct gfx_il_inst cmd;

    uint32_t addr_first = fb_r_sof1;

    int fb_idx;
    for (fb_idx = 0; fb_idx < FB_HEAP_SIZE; fb_idx++) {
        struct framebuffer *fb = fb_heap + fb_idx;
        if (fb->flags.intl == interlace) {
            if (fb->fb_width == width &&
                fb->fb_height == height &&
                fb->addr_key == addr_first &&
                fb->flags.state != FB_STATE_INVALID) {

                if (!(fb_heap[fb_idx].flags.state & FB_STATE_GFX))
                    sync_fb_from_tex_mem(fb_heap + fb_idx, width, height, modulus, concat);

                goto submit_the_fb;
            }
        }
    }

    fb_idx = pick_fb(width, height, fb_r_sof1);
    sync_fb_from_tex_mem(fb_heap + fb_idx, width, height, modulus, concat);

submit_the_fb:
    stamp++;

    cmd.op = GFX_IL_POST_FRAMEBUFFER;
    cmd.arg.post_framebuffer.obj_handle = fb_heap[fb_idx].obj_handle;
    cmd.arg.post_framebuffer.width = fb_heap[fb_idx].fb_width;
    cmd.arg.post_framebuffer.height = fb_heap[fb_idx].fb_height;
    cmd.arg.post_framebuffer.vert_flip = fb_heap[fb_idx].flags.vert_flip;

    rend_exec_il(&cmd, 1);
}

static void
fb_sync_from_host_0565_krgb_prog(struct framebuffer *fb) {
    uint32_t addr = fb->addr_first[0];
    unsigned width = fb->fb_width;
    unsigned height = fb->fb_height;
    unsigned stride = fb->linestride;

    uint16_t const k_val = 0;

    assert((width * height * 4) < OGL_FB_BYTES);

    unsigned row, col;
    for (row = 0; row < height; row++) {
        unsigned line_offs = addr + (height - (row + 1)) * stride;

        for (col = 0; col < width; col++) {
            unsigned ogl_fb_idx = row * width + col;

            uint16_t pix_out = ((ogl_fb[4 * ogl_fb_idx + 2] & 0xf8) >> 3) |
                ((ogl_fb[4 * ogl_fb_idx + 1] & 0xfc) << 3) |
                ((ogl_fb[4 * ogl_fb_idx] & 0xf8) << 8) | k_val;

            /*
             * XXX this is suboptimal because it does the bounds-checking once
             * per pixel.
             */
            copy_to_tex_mem(&pix_out, line_offs + 2 * col, sizeof(pix_out));
        }
    }
}

static void
fb_sync_from_host_0565_krgb_intl(struct framebuffer *fb) {
    unsigned x_min = fb->x_clip_min;
    unsigned y_min = fb->y_clip_min;
    unsigned x_max = fb->tile_w < fb->x_clip_max ? fb->tile_w : fb->x_clip_max;
    unsigned y_max = fb->tile_h < fb->y_clip_max ? fb->tile_h : fb->y_clip_max;
    unsigned width = x_max - x_min + 1;
    unsigned height = y_max - y_min + 1;

    unsigned stride = fb->linestride;
    uint32_t const *addr = fb->addr_first;

    uint16_t const k_val = 0;

    assert((width * height * 4) < OGL_FB_BYTES);

    unsigned row, col;
    __attribute__((unused)) unsigned rows_per_field = height / 2;
    printf("Sync to texture memory sof1=0x%08x, sof2=0x%08x\n",
           (unsigned)addr[0], (unsigned)addr[1]);
    printf("%ux%u, linestride = %u\n", width, height, stride);
    for (row = y_min; row <= y_max; row++) {
        /*
         * TODO: figure out how this is supposed to work with interlacing.
         *
         * The below commented-out code implements this for interlacing, but
         * it doesn't work right.  The other code block (the one that actually
         * gets compiled) implements this as if it was progressive-scan, and
         * that works perfectly.  Obviously this means that either my
         * understanding of interlace-scan is incorrect, or it's actually
         * supposed to be progressive-scan and WashingtonDC is not figuring
         * that out right.
         */
#if 0
        unsigned row_actual[2] = { 2 * row, 2 * row + 1 };
        unsigned line_offs[2] = {
            addr[0] + (rows_per_field - (row + 1)) * stride,
            addr[1] + (rows_per_field - (row + 1)) * stride
        };

        for (col = x_min; col <= x_max; col++) {
            unsigned ogl_fb_idx[2] = {
                row_actual[0] * width + col,
                row_actual[1] * width + col
            };

            uint16_t pix_out[2] = {
                ((ogl_fb[4 * ogl_fb_idx[0] + 2] & 0xf8) >> 3) |
                ((ogl_fb[4 * ogl_fb_idx[0] + 1] & 0xfc) << 3) |
                ((ogl_fb[4 * ogl_fb_idx[0]] & 0xf8) << 8) | k_val,

                ((ogl_fb[4 * ogl_fb_idx[1] + 2] & 0xf8) >> 3) |
                ((ogl_fb[4 * ogl_fb_idx[1] + 1] & 0xfc) << 3) |
                ((ogl_fb[4 * ogl_fb_idx[1]] & 0xf8) << 8) | k_val
            };

            copy_to_tex_mem(pix_out + 0, line_offs[0] + 2 * col, sizeof(pix_out));
            copy_to_tex_mem(pix_out + 1, line_offs[1] + 2 * col, sizeof(pix_out));
        }
#else
        // TODO: account for interlacing
        unsigned line_offs = addr[0] + (height - (row + 1)) * stride;
        for (col = x_min; col <= x_max; col++) {
            unsigned fb_idx = row * width + col;
            uint16_t pix_out = ((ogl_fb[4 * fb_idx + 2] & 0xf8) >> 3) |
                ((ogl_fb[4 * fb_idx + 1] & 0xfc) << 3) |
                ((ogl_fb[4 * fb_idx] & 0xf8) << 8) | k_val;
            copy_to_tex_mem(&pix_out, line_offs + 2 * col, sizeof(pix_out));
        }
#endif
    }
}

static void fb_sync_from_host_rgb0888_prog(struct framebuffer *fb) {
    /* printf("FRAMEBUFFER SYNC %s\n", __func__); */

    unsigned width = fb->fb_width;
    unsigned height = fb->fb_height;
    unsigned stride = fb->linestride;
    uint32_t const *fb_in = (uint32_t*)ogl_fb;

    assert((width * height * 4) < OGL_FB_BYTES);

    unsigned row, col;
    for (row = 0; row < height; row++) {
        unsigned line_offs = fb->addr_first[0] + (height - (row + 1)) * stride;

        for (col = 0; col < width; col++) {
            unsigned ogl_fb_idx = row * width + col;

            uint32_t pix_in = fb_in[ogl_fb_idx];
            uint32_t pix_out = pix_in & 0x00ffffff;

            copy_to_tex_mem(&pix_out, line_offs + 4 * col, sizeof(pix_out));
        }
    }
}

static void fb_sync_from_host_rgb0888_intl(struct framebuffer *fb) {
    /* printf("FRAMEBUFFER SYNC %s\n", __func__); */

    unsigned width = fb->fb_width;
    unsigned height = fb->fb_height;
    unsigned stride = fb->linestride;
    uint32_t const *fb_in = (uint32_t*)ogl_fb;
    unsigned rows_per_field = height / 2;
    unsigned const *addr = fb->addr_first;

    /* printf("Sync to texture memory sof1=0x%08x, sof2=0x%08x\n", */
    /*        (unsigned)addr[0], (unsigned)addr[1]); */
    /* printf("%ux%u, linestride = %u\n", width, height, stride); */
    assert((width * height * 4) < OGL_FB_BYTES);

    unsigned row, col;
    for (row = 0; row < rows_per_field; row++) {
        unsigned row_actual[2] = { 2 * row, 2 * row + 1 };
        unsigned line_offs[2] = {
            addr[0] + (rows_per_field - (row + 1)) * stride,
            addr[1] + (rows_per_field - (row + 1)) * stride
            /* addr[0] + row * stride, */
            /* addr[1] + row * stride */
        };
        /* printf("line_offs[0] is 0x%08x\n", line_offs[0]); */

        for (col = 0; col < width; col++) {
            unsigned ogl_fb_idx[2] = {
                row_actual[0] * width + col,
                row_actual[1] * width + col
            };

            uint32_t pix_in[2] = {
                fb_in[ogl_fb_idx[0]],
                fb_in[ogl_fb_idx[1]]
            };

            uint32_t pix_out[2] = {
                pix_in[0] & 0x00ffffff,
                pix_in[1] & 0x00ffffff
            };

            copy_to_tex_mem(pix_out + 0, line_offs[0] + 2 * col, sizeof(pix_out));
            copy_to_tex_mem(pix_out + 1, line_offs[1] + 2 * col, sizeof(pix_out));
        }
    }
}

static void
sync_fb_to_tex_mem(struct framebuffer *fb) {
    if (!(fb->flags.state & FB_STATE_GFX) || (fb->flags.state & FB_STATE_VIRT))
        return;

    fb->flags.state |= ~FB_STATE_VIRT;

    struct gfx_il_inst cmd = {
        .op = GFX_IL_READ_OBJ,
        .arg = { .read_obj = {
            .dat = ogl_fb,
            .obj_no = fb->obj_handle,
            .n_bytes = sizeof(ogl_fb)/* OGL_FB_W_MAX * OGL_FB_H_MAX * 4 */
            } }
    };
    rend_exec_il(&cmd, 1);
    switch (fb->flags.fmt) {
    case FB_PIX_FMT_RGB_565:
        if (!fb->flags.intl) {
            fb_sync_from_host_0565_krgb_prog(fb);
        } else {
            fb_sync_from_host_0565_krgb_intl(fb);
        }
        break;
    case FB_PIX_FMT_0RGB_0888:
        if (!fb->flags.intl) {
            fb_sync_from_host_rgb0888_prog(fb);
        } else {
            fb_sync_from_host_rgb0888_intl(fb);
        }
        break;
    default:
        printf("fb->flags.fmt is %d\n", fb->flags.fmt);
        RAISE_ERROR(ERROR_UNIMPLEMENTED);
    }
}

/*
 * returns a pointer to the area that addr belongs in.
 * Keep in mind that this pointer points to the beginning of that area,
 * it doesn NOT point to the actual byte that corresponds to the addr.
 */
static uint8_t *get_tex_mem_area(addr32_t addr) {
    switch (addr & 0xff000000) {
    case 0x04000000:
    case 0x06000000:
        return pvr2_tex64_mem;
    case 0x05000000:
    case 0x07000000:
        return pvr2_tex32_mem;
    default:
        return NULL;
    }
}

static uint32_t get_tex_mem_offs(addr32_t addr) {
    switch (addr & 0xff000000) {
    case 0x04000000:
    case 0x06000000:
        return ADDR_TEX64_FIRST;
    case 0x05000000:
    case 0x07000000:
        return ADDR_TEX32_FIRST;
    default:
        LOG_ERROR("%s - 0x%08x is not a texture memory pointer\n",
                  __func__, (unsigned)addr);
        return ADDR_TEX32_FIRST;
    }
}

#define TEX_MIRROR_MASK 0x7fffff

static void copy_to_tex_mem(void const *in, addr32_t offs, size_t len) {
    addr32_t last_byte = offs - 1 + len;

    if ((last_byte & 0xff000000) != (offs & 0xff000000)) {
        error_set_length(len);
        error_set_address(offs + ADDR_TEX32_FIRST);
        error_set_feature("texture memory writes across boundaries");
        RAISE_ERROR(ERROR_UNIMPLEMENTED);
    }

    uint8_t *tex_mem_ptr = get_tex_mem_area(offs + ADDR_TEX32_FIRST);
    if (!tex_mem_ptr) {
        error_set_length(len);
        error_set_address(offs + ADDR_TEX32_FIRST);
        RAISE_ERROR(ERROR_MEM_OUT_OF_BOUNDS);
    }

    /*
     * AND'ing offs with 0x7fffff here serves two purposes: it makes the offs
     * relative to whatever memor area tex_mem_ptr points at, and it also
     * implements mirroring across adjacent versions of the same area (the
     * memory areas are laid out as two mirrors of the 64-bit area at
     * 0x04000000 and 0x04800000, followed by two mirrors of the 32-bit area at
     * 0x05000000 and 0x05800000, and so forth up until 0x08000000)
     */
    offs &= TEX_MIRROR_MASK;
    memcpy(tex_mem_ptr + offs, in, len);

    /*
     * let the texture tracking system know we may have just overwritten a
     * texture in the cache.
     */
    if (tex_mem_ptr == pvr2_tex64_mem)
        pvr2_tex_cache_notify_write(offs + ADDR_TEX64_FIRST, len);
}

static int pick_fb(unsigned width, unsigned height, uint32_t addr) {
    int first_invalid = -1;
    int idx;
    int oldest_stamp = stamp;
    int oldest_stamp_idx = -1;
    for (idx = 0; idx < FB_HEAP_SIZE; idx++) {
        if (fb_heap[idx].flags.state != FB_STATE_INVALID) {
            if (fb_heap[idx].fb_width == width &&
                fb_heap[idx].fb_height == height &&
                fb_heap[idx].addr_key == addr) {
                break;
            }
            if (fb_heap[idx].stamp <= oldest_stamp) {
                oldest_stamp = fb_heap[idx].stamp;
                oldest_stamp_idx = idx;
            }
        } else if (first_invalid < 0) {
            first_invalid = idx;
        }
    }

    // If there are no unused framebuffers
    if (idx == FB_HEAP_SIZE) {
        if (first_invalid >= 0) {
            idx = first_invalid;
        } else {
            idx = oldest_stamp_idx;
        }

        // sync the framebuffer to memory because it's about to get overwritten
        sync_fb_to_tex_mem(fb_heap + idx);
    }

    return idx;
}

int framebuffer_set_render_target(void) {
    /*
     * TODO: this is almost certainly not the correct way to get the screen
     * dimensions as they are seen by PVR
     * TODO: also, use fb_w_linestride
     * TODO: seriously though, the _r_ registers are supposed to be for
     * reading, not writing.  This is bound to cause problems eventually if I
     * don't fix it.
     */
    bool interlace = get_spg_control() & (1 << 4);
    unsigned width = ((get_fb_r_size() & 0x3ff) + 1) *
        (4 / bytes_per_pix(get_fb_r_ctrl()));
    unsigned height = ((get_fb_r_size() >> 10) & 0x3ff) + 1;
    uint32_t addr = get_fb_w_sof1();

    if (interlace)
        height *= 2;
    int idx = pick_fb(width, height, addr);

    struct framebuffer *fb = fb_heap + idx;

    fb->flags.state = FB_STATE_GFX;
    fb->flags.vert_flip = false;
    fb->fb_width = width;
    fb->fb_height = height;
    fb->stamp = stamp;
    fb->flags.intl = interlace;
    fb->linestride = get_fb_w_linestride() * 8;

    // set addr_first and addr_last
    uint32_t sof1 = get_fb_w_sof1() & ~3;
    uint32_t sof2 = get_fb_w_sof2() & ~3;
    uint32_t rows_per_field;
    uint32_t first_addr_field1, last_addr_field1,
        first_addr_field2, last_addr_field2;
    unsigned field_adv;
    unsigned modulus = (get_fb_r_size() >> 20) & 0x3ff;

    // TODO: the k-bit
    switch (get_fb_w_ctrl() & 0x7) {
    case 0:
        // 16-bit 555 KRGB
    case 2:
        // 16-bit 4444 RGB
    case 3:
        // 16-bit 1555 ARGB
    case 4:
        // 888 RGB 24-bit
    case 6:
        // 8888 ARGB
    case 7:
        // absolutely haram
        error_set_fb_pix_fmt(get_fb_w_ctrl() & 0x7);
        RAISE_ERROR(ERROR_UNIMPLEMENTED);
    case 1:
        // 16-bit 565 RGB
        if (interlace) {
            field_adv = width * 2 + modulus * 4 - 4;
            rows_per_field = height / 2;
            first_addr_field1 = sof1;
            last_addr_field1 = sof1 +
                field_adv * (rows_per_field - 1) + 2 * (width - 1);
            first_addr_field2 = sof2;
            last_addr_field2 = sof2 +
                field_adv * (rows_per_field - 1) + 2 * (width - 1);

            fb->addr_key = first_addr_field1 < first_addr_field2 ?
                first_addr_field1 : first_addr_field2;

            fb->addr_first[0] = first_addr_field1;
            fb->addr_first[1] = first_addr_field2;
            fb->addr_last[0] = last_addr_field1;
            fb->addr_last[1] = last_addr_field2;
        } else {
            uint32_t first_byte = sof1;
            uint32_t last_byte = sof1 + width * height * 2;
            fb->addr_key = first_byte;
            fb->addr_first[0] = first_byte;
            fb->addr_first[1] = first_byte;
            fb->addr_last[0] = last_byte;
            fb->addr_last[1] = last_byte;
        }
        fb_heap[idx].flags.fmt = FB_PIX_FMT_RGB_565;
        break;
    case 5:
        // 32-bit 0888 KRGB
        if (interlace) {
            field_adv = (width * 4) + (modulus * 4) - 4;
            rows_per_field = height;
            first_addr_field1 = sof1;
            last_addr_field1 = sof1 +
                field_adv * (rows_per_field - 1) + 2 * (width - 1);
            first_addr_field2 = sof2;
            last_addr_field2 = sof2 +
                field_adv * (rows_per_field - 1) + 2 * (width - 1);

            fb->addr_key = first_addr_field1 < first_addr_field2 ?
                first_addr_field1 : first_addr_field2;

            fb->addr_first[0] = first_addr_field1;
            fb->addr_first[1] = first_addr_field2;
            fb->addr_last[0] = last_addr_field1;
            fb->addr_last[1] = last_addr_field2;
        } else {
            uint32_t first_byte = sof1;
            uint32_t last_byte = sof1 + width * height * 4;
            fb->addr_key = first_byte;
            fb->addr_first[0] = first_byte;
            fb->addr_first[1] = first_byte;
            fb->addr_last[0] = last_byte;
            fb->addr_last[1] = last_byte;
        }
        fb_heap[idx].flags.fmt = FB_PIX_FMT_0RGB_0888;
        break;
    }

    /* unsigned tile_w, tile_h; */
    /* unsigned x_clip_min, x_clip_max, y_clip_min, y_clip_max; */
    fb->tile_w = get_glob_tile_clip_x() << 5;
    fb->tile_h = get_glob_tile_clip_y() << 5;
    fb->x_clip_min = get_fb_x_clip_min();
    fb->x_clip_max = get_fb_x_clip_max();
    fb->y_clip_min = get_fb_y_clip_min();
    fb->y_clip_max = get_fb_y_clip_max();

    /*
     * It's safe to re-bind an object that is already bound as a render target
     * without first unbinding it.
     */
    struct gfx_il_inst cmd;
    cmd.op = GFX_IL_BIND_RENDER_TARGET;
    cmd.arg.bind_render_target.gfx_obj_handle = fb_heap[idx].obj_handle;
    rend_exec_il(&cmd, 1);

    return fb_heap[idx].obj_handle;
}

static inline bool check_overlap(uint32_t range1_start, uint32_t range1_end,
                                 uint32_t range2_start, uint32_t range2_end) {
    if ((range1_start >= range2_start) && (range1_start <= range2_end))
        return true;
    if ((range1_end >= range2_start) && (range1_end <= range2_end))
        return true;
    if ((range2_start >= range1_start) && (range2_start <= range1_end))
        return true;
    if ((range2_end >= range1_start) && (range2_end <= range1_end))
        return true;
    return false;
}

void pvr2_framebuffer_notify_write(uint32_t addr, unsigned n_bytes) {
    uint32_t first_byte = addr - ADDR_TEX32_FIRST;
    uint32_t last_byte = n_bytes - 1 + first_byte;

    unsigned fb_idx;
    for (fb_idx = 0; fb_idx < FB_HEAP_SIZE; fb_idx++) {
        /*
         * TODO: this overlap check is naive because it will issue a
         * false-postive in situations where the bytes written to fall between
         * the beginning and end of a field but aren't supposed to be part of
         * the field because the linestride would skip over them.  So far this
         * doesn't seem to be causing any troubles, but it is something to keep
         * in mind.
         */
        struct framebuffer *fb = fb_heap + fb_idx;
        if ((fb->flags.state & FB_STATE_GFX) &&
            (check_overlap(first_byte, last_byte,
                          fb->addr_first[0],
                          fb->addr_last[0]) ||
            check_overlap(first_byte, last_byte,
                          fb->addr_first[1],
                          fb->addr_last[1]))) {
            fb->flags.state = FB_STATE_VIRT;
        }
    }
}

void pvr2_framebuffer_notify_texture(uint32_t first_tex_addr,
                                     uint32_t last_tex_addr) {
    first_tex_addr &= TEX_MIRROR_MASK;
    last_tex_addr &= TEX_MIRROR_MASK;

    int sync_count = 0;
    unsigned fb_idx;
    for (fb_idx = 0; fb_idx < FB_HEAP_SIZE; fb_idx++) {
        if (fb_heap[fb_idx].flags.state != FB_STATE_GFX)
            continue;

        uint32_t const *addr_first = fb_heap[fb_idx].addr_first;
        uint32_t const *addr_last = fb_heap[fb_idx].addr_last;

        uint32_t addr_first_total[2] = {
            addr_first[0] + ADDR_TEX32_FIRST,
            addr_first[1] + ADDR_TEX32_FIRST
        };

        uint32_t addr_last_total[2] = {
            addr_last[0] + ADDR_TEX32_FIRST,
            addr_last[1] + ADDR_TEX32_FIRST
        };

        uint32_t offs =  get_tex_mem_offs(addr_first_total[0]);

        if (get_tex_mem_offs(addr_first_total[1]) != offs ||
            get_tex_mem_offs(addr_last_total[0]) != offs ||
            get_tex_mem_offs(addr_last_total[1]) != offs) {
            error_set_feature("framebuffers that span the 32-bit and 64-bit "
                              "texture memory areas");
        }

        if (offs != ADDR_TEX64_FIRST)
            continue;

        if (check_overlap(first_tex_addr, last_tex_addr,
                          addr_first[0] & TEX_MIRROR_MASK, addr_last[0] & TEX_MIRROR_MASK) ||
            check_overlap(first_tex_addr, last_tex_addr,
                          addr_first[1] & TEX_MIRROR_MASK, addr_last[1] & TEX_MIRROR_MASK)) {
            sync_fb_to_tex_mem(fb_heap + fb_idx);
            fb_heap[fb_idx].flags.state = FB_STATE_VIRT_AND_GFX;
            sync_count++;
        }
    }
    if (sync_count)
        printf("%d overlapping framebuffers synced\n", sync_count);
}
