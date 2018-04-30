/*******************************************************************************
 *
 *
 *    WashingtonDC Dreamcast Emulator
 *    Copyright (C) 2018 snickerbockers
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

#ifndef JIT_MEM_H_
#define JIT_MEM_H_

#include "jit/code_block.h"
#include "types.h"
#include "hw/sh4/sh4.h"

/*
 * this function can intelligently bypass the memory-mapping and go straight
 * to reading/writing from memory since the address is a constant.
 */
void jit_sh4_mem_read_constaddr_32(struct Sh4 *sh4, struct il_code_block *block,
                                   addr32_t addr, unsigned slot_no);
void jit_sh4_mem_read_constaddr_16(struct Sh4 *sh4, struct il_code_block *block,
                                   addr32_t addr, unsigned slot_no);

#endif
