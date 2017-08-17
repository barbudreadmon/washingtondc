/*******************************************************************************
 *
 *
 *    WashingtonDC Dreamcast Emulator
 *    Copyright (C) 2017 snickerbockers
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

#include <stdbool.h>

#define CONFIG_DEF_BOOL(prop)                           \
    static bool config_ ## prop;                        \
    bool config_get_ ## prop(void) {                    \
        return config_ ## prop;                         \
    }                                                   \
    void config_set_ ## prop(bool new_val) {            \
        config_ ## prop = new_val;                      \
    }


#ifdef ENABLE_DEBUGGER
CONFIG_DEF_BOOL(dbg_enable)
#endif

CONFIG_DEF_BOOL(ser_srv_enable)
