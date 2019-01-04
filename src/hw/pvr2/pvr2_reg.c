/*******************************************************************************
 *
 *
 *    WashingtonDC Dreamcast Emulator
 *    Copyright (C) 2017-2019 snickerbockers
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

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "mem_code.h"
#include "types.h"
#include "MemoryMap.h"
#include "log.h"
#include "error.h"
#include "pvr2_ta.h"
#include "spg.h"
#include "pvr2_tex_cache.h"
#include "pvr2_yuv.h"
#include "intmath.h"

#include "pvr2_reg.h"

// bit in the VO_CONTROL register that causes each pixel to be sent twice
#define PVR2_PIXEL_DOUBLE_SHIFT 8
#define PVR2_PIXEL_DOUBLE_MASK (1 << PVR2_PIXEL_DOUBLE_SHIFT)

#define N_PVR2_REGS (ADDR_PVR2_LAST - ADDR_PVR2_FIRST + 1)
static uint32_t reg_backing[N_PVR2_REGS / sizeof(uint32_t)];

#define PVR2_SB_PDSTAP  0
#define PVR2_SB_PDSTAR  1
#define PVR2_SB_PDLEN   2
#define PVR2_SB_PDDIR   3
#define PVR2_SB_PDTSEL  4
#define PVR2_SB_PDEN    5
#define PVR2_SB_PDST    6
#define PVR2_SB_PDAPRO 32

#define PVR2_REG_IDX(addr) (((addr) - ADDR_PVR2_FIRST) / sizeof(uint32_t))

#define PVR2_ID PVR2_REG_IDX(0x5f8000)
#define PVR2_REV PVR2_REG_IDX(0x5f8004)
#define PVR2_SOFTRESET PVR2_REG_IDX(0x5f8008)
#define PVR2_STARTRENDER PVR2_REG_IDX(0x5f8014)
#define PVR2_PARAM_BASE PVR2_REG_IDX(0x5f8020)
#define PVR2_REGION_BASE PVR2_REG_IDX(0x5f802c)
#define PVR2_SPAN_SORT_CFG PVR2_REG_IDX(0x5f8030)
#define PVR2_VO_BORDER_COL PVR2_REG_IDX(0x5f8040)
#define PVR2_FB_R_CTRL PVR2_REG_IDX(0x5f8044)
#define PVR2_FB_W_CTRL PVR2_REG_IDX(0x5f8048)
#define PVR2_FB_W_LINESTRIDE PVR2_REG_IDX(0x5f804c)
#define PVR2_FB_R_SOF1 PVR2_REG_IDX(0x5f8050)
#define PVR2_FB_R_SOF2 PVR2_REG_IDX(0x5f8054)
#define PVR2_FB_R_SIZE PVR2_REG_IDX(0x5f805c)
#define PVR2_FB_W_SOF1 PVR2_REG_IDX(0x5f8060)
#define PVR2_FB_W_SOF2 PVR2_REG_IDX(0x5f8064)
#define PVR2_FB_X_CLIP PVR2_REG_IDX(0x5f8068)
#define PVR2_FB_Y_CLIP PVR2_REG_IDX(0x5f806c)
#define PVR2_FPU_SHAD_SCALE PVR2_REG_IDX(0x5f8074)
#define PVR2_FPU_CULL_VAL PVR2_REG_IDX(0x5f8078)
#define PVR2_FPU_PARAM_CFG PVR2_REG_IDX(0x5f807c)
#define PVR2_HALF_OFFSET PVR2_REG_IDX(0x5f8080)
#define PVR2_FPU_PERP_VAL PVR2_REG_IDX(0x5f8084)
#define PVR2_ISP_BACKGND_D PVR2_REG_IDX(0x5f8088)
#define PVR2_ISP_BACKGND_T PVR2_REG_IDX(0x5f808c)
#define PVR2_ISP_FEED_CFG PVR2_REG_IDX(0x5f8098)
#define PVR2_FOG_CLAMP_MAX PVR2_REG_IDX(0x5f80bc)
#define PVR2_FOG_CLAMP_MIN PVR2_REG_IDX(0x5f80c0)
#define PVR2_TEXT_CONTROL PVR2_REG_IDX(0x5f80e4)
#define PVR2_SCALER_CTL PVR2_REG_IDX(0x5f80f4)
#define PVR2_FB_BURSTXTRL PVR2_REG_IDX(0x5f8110)
#define PVR2_Y_COEFF PVR2_REG_IDX(0x5f8118)
#define PVR2_SDRAM_REFRESH PVR2_REG_IDX(0x5f80a0)
#define PVR2_SDRAM_CFG PVR2_REG_IDX(0x5f80a8)
#define PVR2_FOG_COL_RAM PVR2_REG_IDX(0x5f80b0)
#define PVR2_FOG_COL_VERT PVR2_REG_IDX(0x5f80b4)
#define PVR2_FOG_DENSITY PVR2_REG_IDX(0x5f80b8)
#define PVR2_SPG_HBLANK_INT PVR2_REG_IDX(0x5f80c8)
#define PVR2_SPG_VBLANK_INT PVR2_REG_IDX(0x5f80cc)
#define PVR2_SPG_CONTROL PVR2_REG_IDX(0x5f80d0)
#define PVR2_SPG_HBLANK PVR2_REG_IDX(0x5f80d4)
#define PVR2_SPG_LOAD PVR2_REG_IDX(0x5f80d8)
#define PVR2_SPG_VBLANK PVR2_REG_IDX(0x5f80dc)
#define PVR2_SPG_WIDTH PVR2_REG_IDX(0x5f80e0)
#define PVR2_VO_CONTROL PVR2_REG_IDX(0x5f80e8)
#define PVR2_VO_STARTX PVR2_REG_IDX(0x5f80ec)
#define PVR2_VO_STARTY PVR2_REG_IDX(0x5f80f0)
#define PVR2_PALETTE_TP PVR2_REG_IDX(0x5f8108)
#define PVR2_SPG_STATUS PVR2_REG_IDX(0x5f810c)
#define PVR2_PT_ALPHA_REF PVR2_REG_IDX(0x5f811c)
#define PVR2_TA_OL_BASE PVR2_REG_IDX(0x5f8124)
#define PVR2_TA_VERTBUF_START PVR2_REG_IDX(0x5f8128)
#define PVR2_TA_ISP_LIMIT PVR2_REG_IDX(0x5f8130)
#define PVR2_TA_NEXT_OPB PVR2_REG_IDX(0x5f8134)
#define PVR2_TA_VERTBUF_POS PVR2_REG_IDX(0x5f8138)
#define PVR2_TA_OL_LIMIT PVR2_REG_IDX(0x5f812c)
#define PVR2_TA_GLOB_TILE_CLIP PVR2_REG_IDX(0x5f813c)
#define PVR2_TA_ALLOC_CTRL PVR2_REG_IDX(0x5f8140)
#define PVR2_TA_RESET PVR2_REG_IDX(0x5f8144)
#define PVR2_TA_YUV_TEX_BASE PVR2_REG_IDX(0x5f8148)
#define PVR2_TA_YUV_TEX_CTRL PVR2_REG_IDX(0x5f814c)
#define PVR2_TA_NEXT_OPB_INIT PVR2_REG_IDX(0x5f8164)

#define PVR2_FOG_TABLE_FIRST PVR2_REG_IDX(0x5f8200)
#define PVR2_FOG_TABLE_LAST PVR2_REG_IDX(0x5f8200 + 4 * 127)

#define PVR2_PAL_RAM_FIRST PVR2_REG_IDX(0x5f9000)
#define PVR2_PAL_RAM_LAST PVR2_REG_IDX(0x5f9000 + 4 * 1023)

#define PVR2_TRACE(msg, ...)                                            \
    do {                                                                \
        LOG_DBG("PVR2: ");                                              \
        LOG_DBG(msg, ##__VA_ARGS__);                                    \
    } while (0)

#define PVR2_REG_WRITE_CASE(idx_const)                              \
    case idx_const:                                                 \
    reg_backing[idx_const] = val;                                   \
    PVR2_TRACE("Write 0x%08x to " #idx_const "\n", (unsigned)val);  \
    break

static void
pvr2_reg_do_write(unsigned idx, uint32_t val) {
    switch (idx) {
    PVR2_REG_WRITE_CASE(PVR2_SB_PDSTAP);
    PVR2_REG_WRITE_CASE(PVR2_SB_PDSTAR);
    PVR2_REG_WRITE_CASE(PVR2_SB_PDLEN);
    PVR2_REG_WRITE_CASE(PVR2_SB_PDDIR);
    PVR2_REG_WRITE_CASE(PVR2_SB_PDTSEL);
    PVR2_REG_WRITE_CASE(PVR2_SB_PDEN);
    PVR2_REG_WRITE_CASE(PVR2_SB_PDST);
    PVR2_REG_WRITE_CASE(PVR2_SB_PDAPRO);
    PVR2_REG_WRITE_CASE(PVR2_SOFTRESET);
    PVR2_REG_WRITE_CASE(PVR2_PARAM_BASE);
    PVR2_REG_WRITE_CASE(PVR2_REGION_BASE);
    PVR2_REG_WRITE_CASE(PVR2_SPAN_SORT_CFG);
    PVR2_REG_WRITE_CASE(PVR2_VO_BORDER_COL);
    PVR2_REG_WRITE_CASE(PVR2_FB_R_SOF1);
    PVR2_REG_WRITE_CASE(PVR2_FB_R_SOF2);
    PVR2_REG_WRITE_CASE(PVR2_FB_R_SIZE);
    PVR2_REG_WRITE_CASE(PVR2_FB_W_SOF1);
    PVR2_REG_WRITE_CASE(PVR2_FB_W_SOF2);
    PVR2_REG_WRITE_CASE(PVR2_ISP_BACKGND_T);
    PVR2_REG_WRITE_CASE(PVR2_ISP_BACKGND_D);
    PVR2_REG_WRITE_CASE(PVR2_TA_GLOB_TILE_CLIP);
    PVR2_REG_WRITE_CASE(PVR2_FB_X_CLIP);
    PVR2_REG_WRITE_CASE(PVR2_FB_Y_CLIP);
    PVR2_REG_WRITE_CASE(PVR2_FPU_SHAD_SCALE);
    PVR2_REG_WRITE_CASE(PVR2_FPU_CULL_VAL);
    PVR2_REG_WRITE_CASE(PVR2_FPU_PARAM_CFG);
    PVR2_REG_WRITE_CASE(PVR2_HALF_OFFSET);
    PVR2_REG_WRITE_CASE(PVR2_FPU_PERP_VAL);
    PVR2_REG_WRITE_CASE(PVR2_ISP_FEED_CFG);
    PVR2_REG_WRITE_CASE(PVR2_FOG_CLAMP_MAX);
    PVR2_REG_WRITE_CASE(PVR2_FOG_CLAMP_MIN);
    PVR2_REG_WRITE_CASE(PVR2_TEXT_CONTROL);
    PVR2_REG_WRITE_CASE(PVR2_SCALER_CTL);
    PVR2_REG_WRITE_CASE(PVR2_FB_BURSTXTRL);
    PVR2_REG_WRITE_CASE(PVR2_Y_COEFF);
    PVR2_REG_WRITE_CASE(PVR2_SDRAM_REFRESH);
    PVR2_REG_WRITE_CASE(PVR2_SDRAM_CFG);
    PVR2_REG_WRITE_CASE(PVR2_FOG_COL_RAM);
    PVR2_REG_WRITE_CASE(PVR2_FOG_COL_VERT);
    PVR2_REG_WRITE_CASE(PVR2_FOG_DENSITY);
    PVR2_REG_WRITE_CASE(PVR2_SPG_WIDTH);
    PVR2_REG_WRITE_CASE(PVR2_VO_STARTX);
    PVR2_REG_WRITE_CASE(PVR2_VO_STARTY);
    PVR2_REG_WRITE_CASE(PVR2_TA_OL_BASE);
    PVR2_REG_WRITE_CASE(PVR2_TA_ISP_LIMIT);
    PVR2_REG_WRITE_CASE(PVR2_TA_OL_LIMIT);
    PVR2_REG_WRITE_CASE(PVR2_TA_ALLOC_CTRL);
    PVR2_REG_WRITE_CASE(PVR2_TA_NEXT_OPB_INIT);
    PVR2_REG_WRITE_CASE(PVR2_ID);
    case PVR2_REV:
    case PVR2_SPG_STATUS:
    case PVR2_TA_VERTBUF_POS:
        // read-only error
        error_set_index(idx);
        error_set_feature("writing to PVR2 read-only register\n");
        RAISE_ERROR(ERROR_UNIMPLEMENTED);
        break;
    case PVR2_PT_ALPHA_REF:
        // let it go through silently
        reg_backing[PVR2_PT_ALPHA_REF] = val;
        break;
    case PVR2_STARTRENDER:
        reg_backing[PVR2_STARTRENDER] = val;
        pvr2_ta_startrender();
        break;
    case PVR2_FB_R_CTRL:
        reg_backing[PVR2_FB_R_CTRL] = val;
        if (val & PVR2_VCLK_DIV_MASK)
            spg_set_pclk_div(1);
        else
            spg_set_pclk_div(2);

        spg_set_pix_double_y((bool)(val & PVR2_LINE_DOUBLE_MASK));
        break;
    case PVR2_FB_W_CTRL:
        reg_backing[PVR2_FB_W_CTRL] = val;
        // should it sync framebuffer from host here?
        break;
    case PVR2_FB_W_LINESTRIDE:
        reg_backing[PVR2_FB_W_LINESTRIDE] = val;
        // should it sync framebuffer from host here?
        break;
    case PVR2_SPG_HBLANK_INT:
        reg_backing[PVR2_SPG_HBLANK_INT] = val;
        pvr2_spg_set_hblank_int(val);
        break;
    case PVR2_SPG_VBLANK_INT:
        reg_backing[PVR2_SPG_VBLANK_INT] = val;
        pvr2_spg_set_vblank_int(val);
        break;
    case PVR2_SPG_CONTROL:
        reg_backing[PVR2_SPG_CONTROL] = val;
        pvr2_spg_set_control(val);
        break;
    case PVR2_SPG_HBLANK:
        reg_backing[PVR2_SPG_HBLANK] = val;
        pvr2_spg_set_hblank(val);
        break;
    case PVR2_SPG_LOAD:
        reg_backing[PVR2_SPG_LOAD] = val;
        pvr2_spg_set_load(val);
        break;
    case PVR2_SPG_VBLANK:
        reg_backing[PVR2_SPG_VBLANK] = val;
        pvr2_spg_set_vblank(val);
        break;
    case PVR2_VO_CONTROL:
        reg_backing[PVR2_VO_CONTROL] = val;
        spg_set_pix_double_x((bool)(val & PVR2_PIXEL_DOUBLE_MASK));
        PVR2_TRACE("Write 0x%08x to PVR2_VO_CONTROL\n",
                   (unsigned)val);
        break;
    case PVR2_PALETTE_TP:
        reg_backing[PVR2_PALETTE_TP] = val;
        LOG_DBG("PVR2: palette type set to: ");

        switch (val) {
        case PALETTE_TP_ARGB_1555:
            LOG_DBG("ARGB1555\n");
            break;
        case PALETTE_TP_RGB_565:
            LOG_DBG("RGB565\n");
            break;
        case PALETTE_TP_ARGB_4444:
            LOG_DBG("ARGB4444\n");
            break;
        case PALETTE_TP_ARGB_8888:
            LOG_DBG("ARGB8888\n");
            break;
        default:
            LOG_DBG("<unknown %u>\n", (unsigned)val);
        }

        pvr2_tex_cache_notify_palette_tp_change();
        break;
    case PVR2_TA_VERTBUF_START:
        reg_backing[PVR2_TA_VERTBUF_START] = val & ~3;
        break;
    case PVR2_TA_RESET:
        reg_backing[PVR2_TA_RESET] = val;
        if (val & 0x80000000) {
            LOG_DBG("TA_RESET!\n");
            reg_backing[PVR2_TA_VERTBUF_POS] =
                reg_backing[PVR2_TA_VERTBUF_START];
        } else {
            LOG_WARN("WARNING: TA_RESET was written to, but the one bit that "
                     "actually matters was not set\n");
        }
        pvr2_ta_reinit();
        break;
    case PVR2_TA_YUV_TEX_BASE:
        reg_backing[PVR2_TA_YUV_TEX_BASE] = val;
        PVR2_TRACE("Writing 0x%08x to TA_YUV_TEX_BASE\n", (unsigned)val);
        pvr2_yuv_set_base(val & BIT_RANGE(3, 23));
        break;
    case PVR2_TA_YUV_TEX_CTRL:
        reg_backing[PVR2_TA_YUV_TEX_CTRL] = val;
        PVR2_TRACE("Writing 0x%08x to TA_YUV_CTRL\n", (unsigned)val);
#ifdef ENABLE_LOG_DEBUG
        unsigned u_res = ((val & 0x3f) + 1) * 16;
        unsigned v_res = (((val >> 8) & 0x3f) + 1) * 16;
        PVR2_TRACE("dimensions are %ux%u\n", u_res, v_res);
#endif
        if (val & (1 << 16)) {
            PVR2_TRACE("an array of 16x16 macroblocks\n");
        } else {
            PVR2_TRACE("a single texture\n");
        }
        PVR2_TRACE("Format is %s\n", val & (1 << 24) ? "YUV422" : "YUV420");
        break;
    default:
        if (idx >= PVR2_FOG_TABLE_FIRST && idx <= PVR2_FOG_TABLE_LAST) {
            PVR2_TRACE("Writing 0x%08x to fog table index %u\n",
                       (unsigned)reg_backing[idx], idx);
        } else if (idx >= PVR2_PAL_RAM_FIRST && idx <= PVR2_PAL_RAM_LAST) {
            reg_backing[idx] = val;
            pvr2_tex_cache_notify_palette_write(idx * 4 + ADDR_PVR2_FIRST, 4);
        } else {
            error_set_index(idx);
            error_set_feature("writing to an unknown PVR2 register");
            RAISE_ERROR(ERROR_UNIMPLEMENTED);
        }
    }
}

#define PVR2_REG_READ_CASE(idx_const)                  \
    case idx_const:                                    \
    PVR2_TRACE("Read 0x%08x from " #idx_const "\n",    \
               (unsigned)reg_backing[idx_const]);      \
    return reg_backing[idx_const]

static uint32_t
pvr2_reg_do_read(unsigned idx) {
    switch (idx) {
    PVR2_REG_READ_CASE(PVR2_SB_PDSTAP);
    PVR2_REG_READ_CASE(PVR2_SB_PDSTAR);
    PVR2_REG_READ_CASE(PVR2_SB_PDLEN);
    PVR2_REG_READ_CASE(PVR2_SB_PDDIR);
    PVR2_REG_READ_CASE(PVR2_SB_PDTSEL);
    PVR2_REG_READ_CASE(PVR2_SB_PDEN);
    PVR2_REG_READ_CASE(PVR2_SB_PDST);
    PVR2_REG_READ_CASE(PVR2_SB_PDAPRO);
    PVR2_REG_READ_CASE(PVR2_SOFTRESET);
    PVR2_REG_READ_CASE(PVR2_PARAM_BASE);
    PVR2_REG_READ_CASE(PVR2_REGION_BASE);
    PVR2_REG_READ_CASE(PVR2_SPAN_SORT_CFG);
    PVR2_REG_READ_CASE(PVR2_VO_BORDER_COL);
    PVR2_REG_READ_CASE(PVR2_FB_R_CTRL);
    PVR2_REG_READ_CASE(PVR2_FB_W_CTRL);
    PVR2_REG_READ_CASE(PVR2_FB_W_LINESTRIDE);
    PVR2_REG_READ_CASE(PVR2_FB_R_SOF1);
    PVR2_REG_READ_CASE(PVR2_FB_R_SOF2);
    PVR2_REG_READ_CASE(PVR2_FB_R_SIZE);
    PVR2_REG_READ_CASE(PVR2_FB_W_SOF1);
    PVR2_REG_READ_CASE(PVR2_FB_W_SOF2);
    PVR2_REG_READ_CASE(PVR2_ISP_BACKGND_T);
    PVR2_REG_READ_CASE(PVR2_ISP_BACKGND_D);
    PVR2_REG_READ_CASE(PVR2_TA_GLOB_TILE_CLIP);
    PVR2_REG_READ_CASE(PVR2_FB_X_CLIP);
    PVR2_REG_READ_CASE(PVR2_FB_Y_CLIP);
    PVR2_REG_READ_CASE(PVR2_FPU_SHAD_SCALE);
    PVR2_REG_READ_CASE(PVR2_FPU_CULL_VAL);
    PVR2_REG_READ_CASE(PVR2_FPU_PARAM_CFG);
    PVR2_REG_READ_CASE(PVR2_HALF_OFFSET);
    PVR2_REG_READ_CASE(PVR2_FPU_PERP_VAL);
    PVR2_REG_READ_CASE(PVR2_ISP_FEED_CFG);
    PVR2_REG_READ_CASE(PVR2_FOG_CLAMP_MAX);
    PVR2_REG_READ_CASE(PVR2_FOG_CLAMP_MIN);
    PVR2_REG_READ_CASE(PVR2_TEXT_CONTROL);
    PVR2_REG_READ_CASE(PVR2_SCALER_CTL);
    PVR2_REG_READ_CASE(PVR2_FB_BURSTXTRL);
    PVR2_REG_READ_CASE(PVR2_Y_COEFF);
    PVR2_REG_READ_CASE(PVR2_SDRAM_REFRESH);
    PVR2_REG_READ_CASE(PVR2_SDRAM_CFG);
    PVR2_REG_READ_CASE(PVR2_FOG_COL_RAM);
    PVR2_REG_READ_CASE(PVR2_FOG_COL_VERT);
    PVR2_REG_READ_CASE(PVR2_FOG_DENSITY);
    PVR2_REG_READ_CASE(PVR2_SPG_WIDTH);
    PVR2_REG_READ_CASE(PVR2_VO_CONTROL);
    PVR2_REG_READ_CASE(PVR2_VO_STARTX);
    PVR2_REG_READ_CASE(PVR2_VO_STARTY);
    PVR2_REG_READ_CASE(PVR2_PALETTE_TP);
    PVR2_REG_READ_CASE(PVR2_TA_OL_BASE);
    PVR2_REG_READ_CASE(PVR2_TA_ISP_LIMIT);
    PVR2_REG_READ_CASE(PVR2_TA_VERTBUF_POS);
    PVR2_REG_READ_CASE(PVR2_TA_OL_LIMIT);
    PVR2_REG_READ_CASE(PVR2_TA_ALLOC_CTRL);
    PVR2_REG_READ_CASE(PVR2_TA_YUV_TEX_BASE);
    PVR2_REG_READ_CASE(PVR2_TA_YUV_TEX_CTRL);
    PVR2_REG_READ_CASE(PVR2_TA_NEXT_OPB_INIT);
    case PVR2_PT_ALPHA_REF:
        // let it go through silently
        return reg_backing[PVR2_PT_ALPHA_REF];
    case PVR2_ID:
        /* hardcoded hardware ID */
        return 0x17fd11db;
        break;
    case PVR2_REV:
        return 17;
        break;
    case PVR2_STARTRENDER:
        error_set_index(idx);
        error_set_feature("reading from a PVR2 write-only register\n");
        RAISE_ERROR(ERROR_UNIMPLEMENTED);
        break;
    case PVR2_SPG_HBLANK_INT:
        return pvr2_spg_get_hblank_int();
    case PVR2_SPG_VBLANK_INT:
        return pvr2_spg_get_vblank_int();
    case PVR2_SPG_CONTROL:
        return pvr2_spg_get_control();
    case PVR2_SPG_HBLANK:
        return pvr2_spg_get_hblank();
    case PVR2_SPG_LOAD:
        return pvr2_spg_get_load();
    case PVR2_SPG_VBLANK:
        return pvr2_spg_get_vblank();
    case PVR2_SPG_STATUS:
        return pvr2_spg_get_status();
    case PVR2_TA_VERTBUF_START:
        return reg_backing[PVR2_TA_VERTBUF_START];
    case PVR2_TA_NEXT_OPB:
            // TODO: actually track the positions of where the OPB blocks should go
        LOG_WARN("You should *really* come up with a real implementation of "
             "%s at line %d of %s\n", __func__, __LINE__, __FILE__);
        PVR2_TRACE("reading 0x%08x from TA_NEXT_OPB\n",
                   (unsigned)reg_backing[PVR2_TA_NEXT_OPB]);
        return reg_backing[PVR2_TA_NEXT_OPB];
    case PVR2_TA_RESET:
        LOG_DBG("reading 0 from TA_RESET\n");
        return 0;
    default:
        if (idx >= PVR2_FOG_TABLE_FIRST && idx <= PVR2_FOG_TABLE_LAST) {
            PVR2_TRACE("Reading 0x%08x from fog table index %u\n",
                       (unsigned)reg_backing[idx], idx);
            return reg_backing[idx];
        } else if (idx >= PVR2_PAL_RAM_FIRST && idx <= PVR2_PAL_RAM_LAST) {
            return reg_backing[idx];
        } else {
            error_set_index(idx);
            error_set_feature("reading from an unknown PVR2 register");
            RAISE_ERROR(ERROR_UNIMPLEMENTED);
        }
    }

    error_set_index(idx);
    RAISE_ERROR(ERROR_INTEGRITY);
}

void pvr2_reg_init(void) {
    memset(reg_backing, 0, sizeof(reg_backing));
}

void pvr2_reg_cleanup(void) {
}

#define PVR2_REG_READ_TMPL(tp)                                          \
    if (addr % sizeof(uint32_t)) {                                      \
        error_set_feature("unaligned pvr2 register reads\n");           \
        error_set_address(addr);                                        \
        error_set_length(sizeof(tp));                                   \
        RAISE_ERROR(ERROR_UNIMPLEMENTED);                               \
    }                                                                   \
    unsigned offs = addr - ADDR_PVR2_FIRST;                             \
    unsigned idx = offs / sizeof(uint32_t);                             \
    return pvr2_reg_do_read(idx)

#define PVR2_REG_WRITE_TMPL(tp)                                         \
    if (addr % sizeof(uint32_t)) {                                      \
        error_set_feature("unaligned pvr2 register writes\n");          \
        error_set_address(addr);                                        \
        error_set_length(sizeof(tp));                                   \
        RAISE_ERROR(ERROR_UNIMPLEMENTED);                               \
    }                                                                   \
    unsigned offs = addr - ADDR_PVR2_FIRST;                             \
    unsigned idx = offs / sizeof(uint32_t);                             \
    pvr2_reg_do_write(idx, val)

double pvr2_reg_read_double(addr32_t addr, void *ctxt) {
    error_set_address(addr);
    error_set_length(sizeof(double));
    RAISE_ERROR(ERROR_UNIMPLEMENTED);
}

void pvr2_reg_write_double(addr32_t addr, double val, void *ctxt) {
    error_set_address(addr);
    error_set_length(sizeof(double));
    RAISE_ERROR(ERROR_UNIMPLEMENTED);
}

float pvr2_reg_read_float(addr32_t addr, void *ctxt) {
    uint32_t val = pvr2_reg_read_32(addr, ctxt);
    float ret;
    memcpy(&ret, &val, sizeof(ret));
    return ret;
}

void pvr2_reg_write_float(addr32_t addr, float val, void *ctxt) {
    uint32_t val32;
    memcpy(&val32, &val, sizeof(val32));
    pvr2_reg_write_32(addr, val32, ctxt);
}

uint32_t pvr2_reg_read_32(addr32_t addr, void *ctxt) {
    PVR2_REG_READ_TMPL(uint32_t);
}

void pvr2_reg_write_32(addr32_t addr, uint32_t val, void *ctxt) {
    PVR2_REG_WRITE_TMPL(uint32_t);
}

uint16_t pvr2_reg_read_16(addr32_t addr, void *ctxt) {
    PVR2_REG_READ_TMPL(uint16_t);
}

void pvr2_reg_write_16(addr32_t addr, uint16_t val, void *ctxt) {
    error_set_address(addr);
    error_set_length(sizeof(uint16_t));
    RAISE_ERROR(ERROR_UNIMPLEMENTED);
}

uint8_t pvr2_reg_read_8(addr32_t addr, void *ctxt) {
    error_set_address(addr);
    error_set_length(sizeof(uint8_t));
    RAISE_ERROR(ERROR_UNIMPLEMENTED);
}

void pvr2_reg_write_8(addr32_t addr, uint8_t val, void *ctxt) {
    error_set_address(addr);
    error_set_length(sizeof(uint8_t));
    RAISE_ERROR(ERROR_UNIMPLEMENTED);
}

struct memory_interface pvr2_reg_intf = {
    .read32 = pvr2_reg_read_32,
    .read16 = pvr2_reg_read_16,
    .read8 = pvr2_reg_read_8,
    .readfloat = pvr2_reg_read_float,
    .readdouble = pvr2_reg_read_double,

    .write32 = pvr2_reg_write_32,
    .write16 = pvr2_reg_write_16,
    .write8 = pvr2_reg_write_8,
    .writefloat = pvr2_reg_write_float,
    .writedouble = pvr2_reg_write_double
};

uint32_t get_fb_r_ctrl(void) {
    return reg_backing[PVR2_FB_R_CTRL];
}

uint32_t get_fb_w_ctrl(void) {
    return reg_backing[PVR2_FB_W_CTRL];
}

uint32_t get_fb_w_linestride(void) {
    return reg_backing[PVR2_FB_W_LINESTRIDE] & 0x1ff;
}

uint32_t get_fb_r_sof1(void) {
    return reg_backing[PVR2_FB_R_SOF1];
}

uint32_t get_fb_r_sof2(void) {
    return reg_backing[PVR2_FB_R_SOF2];
}

uint32_t get_fb_r_size(void) {
    return reg_backing[PVR2_FB_R_SIZE];
}

uint32_t get_fb_w_sof1(void) {
    return reg_backing[PVR2_FB_W_SOF1];
}

uint32_t get_fb_w_sof2(void) {
    return reg_backing[PVR2_FB_W_SOF2];
}

uint32_t get_isp_backgnd_t(void) {
    return reg_backing[PVR2_ISP_BACKGND_T];
}

uint32_t get_isp_backgnd_d(void) {
    return reg_backing[PVR2_ISP_BACKGND_D];
}

uint32_t get_glob_tile_clip(void) {
    return reg_backing[PVR2_TA_GLOB_TILE_CLIP];
}

uint32_t get_glob_tile_clip_x(void) {
    return (get_glob_tile_clip() & 0x3f) + 1;
}

uint32_t get_glob_tile_clip_y(void) {
    return ((get_glob_tile_clip() >> 16) & 0xf) + 1;
}

uint32_t get_fb_x_clip(void) {
    return reg_backing[PVR2_FB_X_CLIP];
}

uint32_t get_fb_y_clip(void) {
    return reg_backing[PVR2_FB_Y_CLIP];
}

unsigned get_fb_x_clip_min(void) {
    return get_fb_x_clip() & 0x7ff;
}

unsigned get_fb_y_clip_min(void) {
    return get_fb_y_clip() & 0x3ff;
}

unsigned get_fb_x_clip_max(void) {
    return (get_fb_x_clip() >> 16) & 0x7ff;
}

unsigned get_fb_y_clip_max(void) {
    return (get_fb_y_clip() >> 16) & 0x3ff;
}

enum palette_tp get_palette_tp(void) {
    return (enum palette_tp)reg_backing[PVR2_PALETTE_TP];
}

uint32_t get_ta_yuv_tex_base(void) {
    return reg_backing[PVR2_TA_YUV_TEX_BASE];
}

uint32_t get_ta_yuv_tex_ctrl(void) {
    return reg_backing[PVR2_TA_YUV_TEX_CTRL];
}

uint8_t *pvr2_get_palette_ram(void) {
    return (uint8_t*)(reg_backing + PVR2_PAL_RAM_FIRST);
}
