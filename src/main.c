/*******************************************************************************
 *
 *
 *    WashingtonDC Dreamcast Emulator
 *    Copyright (C) 2016-2019 snickerbockers
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

#include <unistd.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "dreamcast.h"
#include "config.h"

static void print_usage(char const *cmd) {
    fprintf(stderr, "USAGE: %s [options] [-d IP.BIN] [-u 1ST_READ.BIN]\n\n", cmd);

    fprintf(stderr, "WashingtonDC Dreamcast Emulator\n\n");

    fprintf(stderr, "OPTIONS:\n"
            "\t-b <bios_path>\tpath to dreamcast boot ROM\n"
            "\t-f <flash_path>\tpath to dreamcast flash ROM image\n"
            "\t-g\t\tenable remote GDB backend\n"
            "\t-w\t\tenable remote WashDbg backend\n"
            "\t-d\t\tenable direct boot (skip BIOS)\n"
            "\t-u\t\tskip IP.BIN and boot straight to 1ST_READ.BIN\n"
            "\t-s\t\tpath to dreamcast system call image (only needed for "
            "direct boot)\n"
            "\t-t\t\testablish serial server over TCP port 1998\n"
            "\t-h\t\tdisplay this message and exit\n"
            "\t-l\t\tdump logs to stdout\n"
            "\t-m\t\tmount the given image in the GD-ROM drive\n"
            "\t-n\t\tdon't inline memory reads/writes into the jit\n"
            "\t-p\t\tdisable the dynarec and enable the interpreter instead\n"
            "\t-j\t\tenable dynamic recompiler (as opposed to interpreter)\n"
            "\t-v\t\tenable verbose logging\n"
            "\t-x\t\tenable native x86_64 dynamic recompiler backend "
            "(default)\n");
}

int main(int argc, char **argv) {
    int opt;
    char const *bios_path = NULL, *flash_path = NULL;
    char const *cmd = argv[0];
    bool enable_debugger = false;
    bool enable_washdbg = false;
    bool boot_direct = false, skip_ip_bin = false;
    char *path_1st_read_bin = NULL, *path_ip_bin = NULL;
    char *path_syscalls_bin = NULL;
    char *path_gdi = NULL;
    bool enable_serial = false;
    bool enable_cmd_tcp = false;
    bool enable_jit = false, enable_native_jit = false,
        enable_interpreter = false, inline_mem = true;
    bool log_stdout = false, log_verbose = false;

    while ((opt = getopt(argc, argv, "cb:f:s:m:d:u:ghtjxpnwlv")) != -1) {
        switch (opt) {
        case 'b':
            bios_path = optarg;
            break;
        case 'c':
            enable_cmd_tcp = true;
            break;
        case 'f':
            flash_path = optarg;
            break;
        case 'g':
            enable_debugger = true;
            break;
        case 'w':
            enable_washdbg = true;
            break;
        case 'd':
            boot_direct = true;
            path_ip_bin = optarg;
            break;
        case 'u':
            skip_ip_bin = true;
            path_1st_read_bin = optarg;
            break;
        case 's':
            path_syscalls_bin = optarg;
            break;
        case 't':
            enable_serial = true;
            break;
        case 'm':
            path_gdi = optarg;
            break;
        case 'h':
            print_usage(cmd);
            exit(0);
        case 'j':
            enable_jit = true;
            break;
        case 'x':
            enable_native_jit = true;
            break;
        case 'p':
            enable_interpreter = true;
            break;
        case 'n':
            inline_mem = false;
            break;
        case 'l':
            log_stdout = true;
            break;
        case 'v':
            log_verbose = true;
            break;
        }
    }

    argv += optind;
    argc -= optind;

    config_set_log_stdout(log_stdout);
    config_set_log_verbose(log_verbose);

    if (enable_debugger && enable_washdbg) {
        fprintf(stderr, "You can't enable WashDbg and GDB at the same time\n");
        exit(1);
    }

    if (enable_debugger || enable_washdbg) {
        if (enable_jit || enable_native_jit) {
            fprintf(stderr, "Debugger enabled - this overrides the jit "
                    "compiler and sets WashingtonDC to interpreter mode\n");
            enable_jit = false;
            enable_native_jit = false;
        }
        enable_interpreter = true;

#ifdef ENABLE_DEBUGGER
        config_set_dbg_enable(true);
        config_set_washdbg_enable(enable_washdbg);
#else
        fprintf(stderr, "ERROR: Unable to enable remote gdb stub.\n"
                "Please rebuild with -DENABLE_DEBUGGER=On\n");
        exit(1);
#endif
    } else {
#ifdef ENABLE_DEBUGGER
        config_set_dbg_enable(false);
#endif
    }

    if (enable_interpreter && (enable_jit || enable_native_jit)) {
        fprintf(stderr, "ERROR: You can\'t use the interpreter and the JIT at "
                "the same time, silly!\n");
        exit(1);
    }

#ifdef ENABLE_JIT_X86_64
    // enable the jit (with x86_64 backend) by default
    if (!(enable_jit || enable_native_jit || enable_interpreter))
        enable_native_jit = true;
#else
    // enable the jit (with jit-interpreter) by default
    if (!(enable_jit || enable_interpreter))
        enable_jit = true;
#endif

    config_set_inline_mem(inline_mem);
    config_set_jit(enable_jit || enable_native_jit);

#ifdef ENABLE_JIT_X86_64
    config_set_native_jit(enable_native_jit);
#else
    if (enable_native_jit) {
        fprintf(stderr, "ERROR: the native x86_64 jit backend was not enabled "
                "for this build configuration.\n"
                "Rebuild WashingtonDC with -DENABLE_JIT_X86_64=On to enable "
                "the native x86_64 jit backend.\n");
        exit(1);
    }
#endif

    if (skip_ip_bin) {
        if (!path_syscalls_bin) {
            fprintf(stderr, "Error: cannot direct-boot without a system call "
                    "table (-s flag).\n");
            exit(1);
        }

        if (!path_1st_read_bin) {
            fprintf(stderr, "Error: cannot direct-boot without a "
                    "1ST-READ.BIN\n");
            exit(1);
        }

        config_set_boot_mode(DC_BOOT_DIRECT);
        config_set_ip_bin_path(path_ip_bin);
        config_set_exec_bin_path(path_1st_read_bin);
        config_set_syscall_path(path_syscalls_bin);
    } else if (boot_direct) {
        if (!path_syscalls_bin) {
            fprintf(stderr, "Error: cannot direct-boot without a system call "
                    "table (-s flag).\n");
            exit(1);
        }

        config_set_boot_mode(DC_BOOT_IP_BIN);
        config_set_ip_bin_path(path_ip_bin);
        config_set_syscall_path(path_syscalls_bin);
    } else {
        config_set_boot_mode(DC_BOOT_FIRMWARE);
    }

    config_set_dc_bios_path(bios_path);
    config_set_dc_flash_path(flash_path);

    dreamcast_init(path_gdi, enable_cmd_tcp);

    config_set_enable_cmd_tcp(enable_cmd_tcp);
    config_set_ser_srv_enable(enable_serial);

    dreamcast_run();

    dreamcast_cleanup();

    exit(0);
}
