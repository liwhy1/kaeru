
//
// SPDX-FileCopyrightText: 2026 R0rt1z2 <roger@r0rt1z2.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//

#include <lib/spoof.h>
#include <lib/common.h>

int is_spoofing_enabled(void) {
    const char *val = get_env(KAERU_ENV_BLDR_SPOOF);
    return val && strcmp(val, "1") == 0;
}

int get_lock_state(uint32_t *lock_state) {
    int spoofing = is_spoofing_enabled();
#if KAERU_DEBUG
    printf("Attempted to get lock state, spoofing is %s\n",
            spoofing ? "enabled" : "disabled");
#endif
    // The function this replaces validates the out pointer before writing,
    // which means some callers do query it with NULL (to probe whether the
    // seccfg can be read at all). Writing through that would fault during
    // boot, so keep the original behaviour.
    if (!lock_state)
        return 1;

    *lock_state = spoofing ? LKS_LOCK : LKS_UNLOCK;
    return 0;
}

// Stubs that boards patch over the LK security checks so download mode,
// secure boot and the lock status always report the values we want.
int sec_usbdl_enabled(void) {
    return 0;
}

unsigned int seclib_sec_boot_enabled(unsigned int) {
    return 0;
}

unsigned get_unlocked_status(void) {
    return 1;
}

void cmd_spoof_bootloader_lock(const char *arg, void *data, unsigned sz) {
    (void)data;
    (void)sz;

    int status = is_spoofing_enabled();
    // The prefix registered for this command carries no trailing space, so the
    // argument arrives with a leading one ('on') - or as NULL when the command
    // is typed without arguments. Skip whatever separator is there instead of
    // blindly stepping over one byte, which both reads past the string when
    // there is no argument and drops a character if the dispatcher already
    // trimmed it.
    const char *option = arg ? arg : "";
    while (*option == ' ')
        option++;
    int target = -1;

    if (!option[0])                  target = !status;
    else if (!strcmp(option, "on"))  target = 1;
    else if (!strcmp(option, "off")) target = 0;

    if (target != -1) {
        if (status != target) {
            set_env(KAERU_ENV_BLDR_SPOOF, target ? "1" : "0");
            fastboot_info(target ?
                "Bootloader spoofing enabled." :
                "Bootloader spoofing disabled.");
            fastboot_info("A factory reset may be required.");
        } else {
            fastboot_info(target ?
                "Bootloader spoofing is already enabled." :
                "Bootloader spoofing is already disabled.");
        }
        fastboot_publish("is-spoofing", target ? "1" : "0");
        fastboot_okay("");
        return;
    }

    if (!strcmp(option, "status")) {
        fastboot_info(status ?
            "Bootloader spoofing is currently enabled." :
            "Bootloader spoofing is currently disabled.");
        fastboot_info(status ?
            "Device is currently spoofed as bootloader locked." :
            "Device is not being spoofed as bootloader locked.");
        fastboot_okay("");
        return;
    }

    fastboot_info("kaeru bootloader lock spoofing control");
    fastboot_info("");
    fastboot_info("When enabled, device reports as 'locked' to TEE");
    fastboot_info("while maintaining full fastboot and root capabilities.");
    fastboot_info("");
    fastboot_info("Commands:");
    fastboot_info("  (none) - Toggle the shortcut");
    fastboot_info("  on     - Enable spoofing (reboot required)");
    fastboot_info("  off    - Disable spoofing (reboot required)");
    fastboot_info("  status - Show current state");
    fastboot_fail("Usage: fastboot oem bldr_spoof <on|off|status>");
}