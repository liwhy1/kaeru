//
// SPDX-FileCopyrightText: 2026 R0rt1z2 <roger@r0rt1z2.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//

#include <lib/common.h>
#include <lib/string.h>
#include <lib/recovery.h>

#ifdef CONFIG_RECOVERY_CMDLINE_PATCH
#include <arch/cache.h>
#include <lib/bootargs.h>
#include <lib/spoof.h>
#endif

// Oh Amazon...
bootmode_t misc_command_to_bootmode(const char* c) {
    if (!c || !c[0])
        return BOOTMODE_NORMAL;

    if (!strncasecmp(c, "boot-recovery",       13)) return BOOTMODE_RECOVERY;
    if (!strncasecmp(c, "boot-fastboot",       13)) return BOOTMODE_RECOVERY;
    if (!strncasecmp(c, "boot-bootloader",     15)) return BOOTMODE_FASTBOOT;
    if (!strncasecmp(c, "bootonce-bootloader", 19)) return BOOTMODE_FASTBOOT;
    if (!strncasecmp(c, "fastboot_please",     15)) return BOOTMODE_FASTBOOT;
    return BOOTMODE_NORMAL;
}

bool misc_command_is_system(const char* c) {
    if (!c || !c[0])
        return false;

    return !strncasecmp(c, "boot-system", 11);
}

// 'boot-fastboot' is not ours to act on beyond picking the recovery image.
// It is what tells the recovery ramdisk to start fastbootd instead of the
// recovery UI, so it has to survive into the boot it selected. AOSP recovery
// clears the BCB itself once it has read the args out of it.
bool misc_command_is_for_ramdisk(const char* c) {
    if (!c || !c[0])
        return false;

    return !strncasecmp(c, "boot-fastboot", 13);
}

bool misc_command_is_sticky(const char* c) {
    if (!c || !c[0])
        return false;

    return !strncasecmp(c, "fastboot_please", 15);
}

// Maps the target of a fastboot 'reboot-*' command to the misc command that
// requests it, so boards handling those themselves can just write it back.
// The leading dash is what the command dispatcher leaves behind when it
// matches on the 'reboot' prefix, so accept it either way.
const char* reboot_target_to_misc_command(const char* target) {
    if (!target || !target[0])
        return NULL;

    if (target[0] == '-')
        target++;

    if (!strcmp(target, "bootloader"))      return "bootonce-bootloader";
    if (!strcmp(target, "recovery"))        return "boot-recovery";
    if (!strcmp(target, "fastboot"))        return "boot-fastboot";
    if (!strcmp(target, "fastboot_please")) return "fastboot_please";
    return NULL;
}

#ifdef CONFIG_RECOVERY_CMDLINE_PATCH
#ifdef CONFIG_RECOVERY_CMDLINE_END_ADDRESS
// LK keeps a cursor into the global cmdline so later appends know where to
// continue. The helper below moves the tail around, so keep it in sync.
static void adjust_cmdline_end(int delta) {
    if (!CONFIG_RECOVERY_CMDLINE_END_ADDRESS)
        return;

    volatile uint32_t *end =
        (volatile uint32_t*)CONFIG_RECOVERY_CMDLINE_END_ADDRESS;
    uint32_t base = CONFIG_RECOVERY_CMDLINE1_ADDRESS;
    uint32_t cur = *end;

    if (base && cur >= base && cur <= base + CMDLINE_LEN)
        *end = cur + delta;
}
#endif

// When booting into recovery, we need to ensure verifiedbootstate is set
// to "orange" so adbd and fastbootd detect the device as unlocked. With
// the spoofed locked state they both refuse to do anything, no adb access
// and no flashing.
//
// Don't touch vbmeta.device_state here, rewriting it to "unlocked" hangs
// recovery on some devices.
void handle_recovery_boot(void) {
    if (get_bootmode() != BOOTMODE_RECOVERY || !is_spoofing_enabled())
        return;

    printf("Recovery boot detected, modifying cmdline for unlocked state.\n");

    static const uint32_t cmdline_addrs[] = {
        CONFIG_RECOVERY_CMDLINE1_ADDRESS,
        CONFIG_RECOVERY_CMDLINE2_ADDRESS,
    };

    for (int i = 0; i < ARRAY_SIZE(cmdline_addrs); i++) {
        if (!cmdline_addrs[i])
            continue;

        printf("Patching cmdline at 0x%08X\n", cmdline_addrs[i]);
        if (cmdline_replace((char*)cmdline_addrs[i],
                            "androidboot.verifiedbootstate=", "green", "orange")) {
#ifdef CONFIG_RECOVERY_CMDLINE_END_ADDRESS
            if (cmdline_addrs[i] == CONFIG_RECOVERY_CMDLINE1_ADDRESS)
                adjust_cmdline_end(1);
#endif
        }
    }
}
#endif
