//
// SPDX-FileCopyrightText: 2025-2026 Roger Ortiz <roger@r0rt1z2.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//

#include <stdbool.h>

#include <lib/debug.h>
#include <lib/fastboot.h>
#include <lib/string.h>
#include <main/main.h>

#if CONFIG_FASTBOOT_CMDLIST_ADDRESS
static struct fastboot_cmd* fastboot_cmdlist(void) {
    return *(struct fastboot_cmd**)CONFIG_FASTBOOT_CMDLIST_ADDRESS;
}

static bool cmd_is_shadowed(const struct fastboot_cmd* cmd) {
    for (const struct fastboot_cmd* p = fastboot_cmdlist(); p && p != cmd; p = p->next) {
        if (p->prefix && strcmp(p->prefix, cmd->prefix) == 0) {
            return true;
        }
    }

    return false;
}
#endif

static const char* reloc_str(const char* s) {
    return (const char*)((uintptr_t)s + kaeru_reloc_delta);
}

static void help_line(const char* usage) {
    char line[FASTBOOT_INFO_MAX + 1];

    npf_snprintf(line, sizeof(line), "  %s", usage);
    fastboot_info(line);
}

void cmd_help(const char* arg, void* data, unsigned sz) {
    (void)arg;
    (void)data;
    (void)sz;

    fastboot_info("kaeru commands:");
    for (const struct fastboot_command* c = __fastboot_cmds_start; c < __fastboot_cmds_end; c++) {
        if (!c->prefix) {
            continue;
        }

        const char* prefix = reloc_str(c->prefix);
        if (strncmp(prefix, "oem ", 4) != 0) {
            continue;
        }

        help_line(c->usage ? reloc_str(c->usage) : prefix);
    }

#if CONFIG_FASTBOOT_CMDLIST_ADDRESS
    const struct fastboot_cmd* cmd = fastboot_cmdlist();
    if (cmd) {
        fastboot_info("stock oem commands:");
        for (; cmd; cmd = cmd->next) {
            if (!cmd->prefix || strncmp(cmd->prefix, "oem ", 4) != 0 || cmd_is_shadowed(cmd)) {
                continue;
            }

            help_line(cmd->prefix);
        }
    }
#endif

    fastboot_okay("");
}

FASTBOOT_CMD_USAGE(help, "oem help", cmd_help, 1, "oem help");
