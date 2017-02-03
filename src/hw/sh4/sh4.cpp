/*******************************************************************************
 *
 *
 *    WashingtonDC Dreamcast Emulator
 *    Copyright (C) 2016, 2017 snickerbockers
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

#include <fenv.h>

#include <cstring>

#ifdef ENABLE_SH4_ICACHE
#include "Icache.hpp"
#endif

#ifdef ENABLE_SH4_OCACHE
#include "Ocache.hpp"
#endif

#include "BaseException.hpp"
#include "sh4_mmu.hpp"

#include "sh4.hpp"

typedef boost::error_info<struct tag_excp_code_error_info,
                          enum Sh4::ExceptionCode> excp_code_error_info;

class UnknownExcpCodeException : public BaseException {
public:
    char const *what() const throw() {
        return "unrecognized sh4 exception code";
    }
};

Sh4::Sh4() {
    reg_area = new uint8_t[P4_REGEND - P4_REGSTART];

    sh4_mmu_init(&mmu);

    memset(&reg, 0, sizeof(reg));
    memset(&cache_reg, 0, sizeof(cache_reg));

#ifdef ENABLE_SH4_ICACHE
    sh4_icache_init(&this->inst_cache);
#endif

#ifdef ENABLE_SH4_OCACHE
    sh4_ocache_init(&this->op_cache);
#else
    this->oc_ram_area = new uint8_t[OC_RAM_AREA_SIZE];
#endif

    init_regs();

    sh4_compile_instructions();

    on_hard_reset();
}

Sh4::~Sh4() {
#ifdef ENABLE_SH4_OCACHE
    sh4_ocache_cleanup(&op_cache);
#else
    delete[] oc_ram_area;
#endif

#ifdef ENABLE_SH4_ICACHE
    sh4_icache_cleanup(&inst_cache);
#endif

    delete[] reg_area;
}

void Sh4::on_hard_reset() {
    reg[SH4_REG_SR] = SR_MD_MASK | SR_RB_MASK | SR_BL_MASK |
        SR_FD_MASK | SR_IMASK_MASK;
    reg[SH4_REG_VBR] = 0;
    reg[SH4_REG_PC] = 0xa0000000;

    fpu.fpscr = 0x41;

    std::fill(fpu.reg_bank0.fr, fpu.reg_bank0.fr + N_FLOAT_REGS, 0.0f);
    std::fill(fpu.reg_bank1.fr, fpu.reg_bank1.fr + N_FLOAT_REGS, 0.0f);

    delayed_branch = false;
    delayed_branch_addr = 0;

#ifdef ENABLE_SH4_OCACHE
    sh4_ocache_reset(&op_cache);
#else
    memset(oc_ram_area, 0, sizeof(uint8_t) * OC_RAM_AREA_SIZE);
#endif

#ifdef ENABLE_SH4_ICACHE
    sh4_icache_reset(&inst_cache);
#endif
}

reg32_t Sh4::get_pc() const {
    return reg[SH4_REG_PC];
}

void Sh4::set_exception(unsigned excp_code) {
    excp_reg.expevt = (excp_code << EXPEVT_CODE_SHIFT) & EXPEVT_CODE_MASK;

    enter_exception((ExceptionCode)excp_code);
}

void Sh4::set_interrupt(unsigned intp_code) {
    excp_reg.intevt = (intp_code << INTEVT_CODE_SHIFT) & INTEVT_CODE_MASK;

    enter_exception((ExceptionCode)intp_code);
}

void Sh4::get_regs(reg32_t reg_out[SH4_REGISTER_COUNT]) const {
    memcpy(reg_out, reg, sizeof(reg_out[0]) * SH4_REGISTER_COUNT);
}

Sh4::FpuReg Sh4::get_fpu() const {
    return fpu;
}

void Sh4::set_regs(reg32_t const reg_in[SH4_REGISTER_COUNT]) {
    memcpy(reg, reg_in, sizeof(reg[0]) * SH4_REGISTER_COUNT);
}

void Sh4::set_fpu(const Sh4::FpuReg& src) {
    this->fpu = src;
}

const Sh4::ExcpMeta Sh4::excp_meta[Sh4::EXCP_COUNT] = {
    // exception code                         prio_level   prio_order   offset
    { EXCP_POWER_ON_RESET,                    1,           1,           0      },
    { EXCP_MANUAL_RESET,                      1,           2,           0      },
    { EXCP_HUDI_RESET,                        1,           1,           0      },
    { EXCP_INST_TLB_MULT_HIT,                 1,           3,           0      },
    { EXCP_DATA_TLB_MULT_HIT,                 1,           4,           0      },
    { EXCP_USER_BREAK_BEFORE,                 2,           0,           0x100  },
    { EXCP_INST_ADDR_ERR,                     2,           1,           0x100  },
    { EXCP_INST_TLB_MISS,                     2,           2,           0x400  },
    { EXCP_INST_TLB_PROT_VIOL,                2,           3,           0x100  },
    { EXCP_GEN_ILLEGAL_INST,                  2,           4,           0x100  },
    { EXCP_SLOT_ILLEGAL_INST,                 2,           4,           0x100  },
    { EXCP_GEN_FPU_DISABLE,                   2,           4,           0x100  },
    { EXCP_SLOT_FPU_DISABLE,                  2,           4,           0x100  },
    { EXCP_DATA_ADDR_READ,                    2,           5,           0x100  },
    { EXCP_DATA_ADDR_WRITE,                   2,           5,           0x100  },
    { EXCP_DATA_TLB_READ_MISS,                2,           6,           0x400  },
    { EXCP_DATA_TLB_WRITE_MISS,               2,           6,           0x400  },
    { EXCP_DATA_TLB_READ_PROT_VIOL,           2,           7,           0x100  },
    { EXCP_DATA_TLB_WRITE_PROT_VIOL,          2,           7,           0x100  },
    { EXCP_FPU,                               2,           8,           0x100  },
    { EXCP_INITIAL_PAGE_WRITE,                2,           9,           0x100  },
    { EXCP_UNCONDITIONAL_TRAP,                2,           4,           0x100  },
    { EXCP_USER_BREAK_AFTER,                  2,          10,           0x100  },
    { EXCP_NMI,                               3,           0,           0x600  },
    { EXCP_EXT_0,                             4,           2,           0x600  },
    { EXCP_EXT_1,                             4,           2,           0x600  },
    { EXCP_EXT_2,                             4,           2,           0x600  },
    { EXCP_EXT_3,                             4,           2,           0x600  },
    { EXCP_EXT_4,                             4,           2,           0x600  },
    { EXCP_EXT_5,                             4,           2,           0x600  },
    { EXCP_EXT_6,                             4,           2,           0x600  },
    { EXCP_EXT_7,                             4,           2,           0x600  },
    { EXCP_EXT_8,                             4,           2,           0x600  },
    { EXCP_EXT_9,                             4,           2,           0x600  },
    { EXCP_EXT_A,                             4,           2,           0x600  },
    { EXCP_EXT_B,                             4,           2,           0x600  },
    { EXCP_EXT_C,                             4,           2,           0x600  },
    { EXCP_EXT_D,                             4,           2,           0x600  },
    { EXCP_EXT_E,                             4,           2,           0x600  },
    { EXCP_TMU0_TUNI0,                        4,           2,           0x600  },
    { EXCP_TMU1_TUNI1,                        4,           2,           0x600  },
    { EXCP_TMU2_TUNI2,                        4,           2,           0x600  },
    { EXCP_TMU2_TICPI2,                       4,           2,           0x600  },
    { EXCP_RTC_ATI,                           4,           2,           0x600  },
    { EXCP_RTC_PRI,                           4,           2,           0x600  },
    { EXCP_RTC_CUI,                           4,           2,           0x600  },
    { EXCP_SCI_ERI,                           4,           2,           0x600  },
    { EXCP_SCI_RXI,                           4,           2,           0x600  },
    { EXCP_SCI_TXI,                           4,           2,           0x600  },
    { EXCP_SCI_TEI,                           4,           2,           0x600  },
    { EXCP_WDT_ITI,                           4,           2,           0x600  },
    { EXCP_REF_RCMI,                          4,           2,           0x600  },
    { EXCP_REF_ROVI,                          4,           2,           0x600  },
    { EXCP_GPIO_GPIOI,                        4,           2,           0x600  },
    { EXCP_DMAC_DMTE0,                        4,           2,           0x600  },
    { EXCP_DMAC_DMTE1,                        4,           2,           0x600  },
    { EXCP_DMAC_DMTE2,                        4,           2,           0x600  },
    { EXCP_DMAC_DMTE3,                        4,           2,           0x600  },
    { EXCP_DMAC_DMAE,                         4,           2,           0x600  },
    { EXCP_SCIF_ERI,                          4,           2,           0x600  },
    { EXCP_SCIF_RXI,                          4,           2,           0x600  },
    { EXCP_SCIF_BRI,                          4,           2,           0x600  },
    { EXCP_SCIF_TXI,                          4,           2,           0x600  }
};

void Sh4::enter_exception(enum ExceptionCode vector) {
    struct ExcpMeta const *meta = NULL;

    for (unsigned idx = 0; idx < EXCP_COUNT; idx++) {
        if (excp_meta[idx].code == vector) {
            meta = excp_meta + idx;
            break;
        }
    }

    if (!meta)
        BOOST_THROW_EXCEPTION(UnknownExcpCodeException() <<
                              excp_code_error_info(vector));

    reg[SH4_REG_SPC] = reg[SH4_REG_PC];
    reg[SH4_REG_SSR] = reg[SH4_REG_SR];
    reg[SH4_REG_SGR] = reg[SH4_REG_R15];

    reg[SH4_REG_SR] |= SR_BL_MASK;
    reg[SH4_REG_SR] |= SR_MD_MASK;
    reg[SH4_REG_SR] |= SR_RB_MASK;
    reg[SH4_REG_SR] &= ~SR_FD_MASK;

    if (vector == EXCP_POWER_ON_RESET ||
        vector == EXCP_MANUAL_RESET ||
        vector == EXCP_HUDI_RESET ||
        vector == EXCP_INST_TLB_MULT_HIT ||
        vector == EXCP_INST_TLB_MULT_HIT) {
        reg[SH4_REG_PC] = 0xa0000000;
    } else if (vector == EXCP_USER_BREAK_BEFORE ||
               vector == EXCP_USER_BREAK_AFTER) {
        // TODO: check brcr.ubde and use DBR instead of VBR if it is set
        reg[SH4_REG_PC] = reg[SH4_REG_VBR] + meta->offset;
    } else {
        reg[SH4_REG_PC] = reg[SH4_REG_VBR] + meta->offset;
    }
}

void Sh4::sh4_enter() {
    if (fpu.fpscr & FPSCR_RM_MASK)
        fesetround(FE_TOWARDZERO);
    else
        fesetround(FE_TONEAREST);
}

void Sh4::set_fpscr(reg32_t new_val) {
    fpu.fpscr = new_val;
    if (fpu.fpscr & FPSCR_RM_MASK)
        fesetround(FE_TOWARDZERO);
    else
        fesetround(FE_TONEAREST);
}
