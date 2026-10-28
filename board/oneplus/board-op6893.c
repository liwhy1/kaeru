//
// SPDX-FileCopyrightText: 2026 liwhy <liwhy@ilt.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//

#include <board_ops.h>
#include <arch/cache.h>

#define VENEER_SIZE 12u

// Volume-down is checked with the stock OnePlus helper, which configures GPIO
// 155 before reading it. Volume-up is not a GPIO: it is the MT6359P HOMEKEY,
// exposed as TOPSTATUS bit 3 and read through LK's own PMIC helper. The stock
// app entry itself uses these same two functions for its key combo.
#define VOLUME_DOWN_CHECK_ADDRESS 0x4822C2B4u
#define PMIC_HOMEKEY_PRESSED_ADDRESS 0x4821F678u
#define BOOT_DELAY_ADDRESS 0x4825D840u

// The stock early boot path calls this RTC check before choosing the image.
// Redirect that one call so volume-up selects recovery in the same place the
// stock RTC factory-reset bit does, before mt_boot has picked an image.
#define EARLY_RTC_CHECK_CALL_ADDRESS 0x48204732u
#define RTC_RECOVERY_CHECK_ADDRESS 0x48220FD4u

#define SECCFG_GET_LOCK_STATE_ADDRESS 0x4827766Cu

#define GET_VFY_POLICY_ADDRESS 0x4821ACB8u
#define GET_DL_POLICY_ADDRESS 0x4821ACC4u
#define FASTBOOT_GATE_ADDRESS 0x4822F408u
#define FASTBOOT_GATE_CALL_ADDRESS 0x4827802Cu
#define FASTBOOT_HANDLER_ADDRESS 0x4822F41Cu

#define ORANGE_WARNING_ADDRESS 0x4824EB00u
#define DM_VERITY_WARNING_ADDRESS 0x48264958u

// load_and_verify_vbmeta(): the vbmeta chain is re-signed by us, so the key
// length, the key comparison and the trusted flag all have to be neutralised
// while the bootloader reports itself locked.
#define AVB_CHAIN_KEY_LENGTH_ADDRESS 0x482613C0u
#define AVB_CHAIN_KEY_CHECK_ADDRESS 0x482616EAu
#define AVB_KEY_TRUSTED_ADDRESS 0x48261758u

static void boot_delay(unsigned ms) {
    ((void (*)(unsigned))(BOOT_DELAY_ADDRESS | 1))(ms);
}

static bool pmic_homekey_pressed(void) {
    return ((bool (*)(void))(PMIC_HOMEKEY_PRESSED_ADDRESS | 1))();
}

// TOPSTATUS jitters when no key is pressed, so require three consecutive
// pressed samples. A key that is actually held passes all three.
static bool volume_up_pressed(void) {
    if (!pmic_homekey_pressed())
        return false;

    boot_delay(20);
    if (!pmic_homekey_pressed())
        return false;

    boot_delay(20);
    return pmic_homekey_pressed();
}

// The stock helper configures GPIO 155, waits for the debounce, then reads it.
// It is only called when volume-up is already pressed, so a normal boot pays
// nothing for it.
static bool volume_down_pressed(void) {
    return ((uint32_t (*)(void))(VOLUME_DOWN_CHECK_ADDRESS | 1))() != 0;
}

// Replaces the early boot path's RTC factory-reset check. Returning true makes
// the stock code set BOOTMODE_RECOVERY before the app is selected, so no reset
// is needed and holding the key through the next boot cannot loop.
static bool recovery_check_hook(void) {
    if (((bool (*)(void))(RTC_RECOVERY_CHECK_ADDRESS | 1))())
        return true;

    if (!is_recovery_key_enabled())
        return false;

    return volume_up_pressed() && !volume_down_pressed();
}

static void install_veneer(uint32_t slot, void *target) {
    WRITE32(slot + 0, 0xC004F8DFu);
    WRITE16(slot + 4, 0x4760u);
    WRITE16(slot + 6, 0xBF00u);
    WRITE32(slot + 8, (uint32_t)target | 1u);

    arch_sync_cache_range(slot, VENEER_SIZE);
}

// Every patch is applied to a hardcoded address from the stock image. Check the
// original instruction first: a mismatch must leave the bootloader untouched,
// never branch into whatever happens to be there.
static bool expect16(uint32_t addr, uint16_t value) {
    uint16_t cur = READ16(addr);

    if (cur == value)
        return true;

    printf("Skipping patch at 0x%08X: expected 0x%04X, found 0x%04X\n",
           (unsigned)addr, (unsigned)value, (unsigned)cur);
    return false;
}

static bool expect_bl(uint32_t addr, uint32_t target) {
    target &= ~1u;

    if (DECODE_BL_TARGET(addr) == target)
        return true;

    printf("Skipping hook at 0x%08X: does not call 0x%08X\n",
           (unsigned)addr, (unsigned)target);
    return false;
}

// Replacement for the bootloader's seccfg lock state getter.
//
// The veneer is only installed while the spoof is active, so this always
// reports the device as locked. It stays self-contained: LK calls it from
// every context that cares about the lock state, so it must not touch the
// environment or the console.
static int lock_state_hook(uint32_t *lock_state) {
    if (!lock_state)
        return 1;

    *lock_state = LKS_LOCK;
    return 0;
}

// Apply the lock-state spoof. Returns true when the getter was redirected, so
// the caller knows the boot path is actually spoofed.
static bool spoof_lock_state(void) {
    bool applied = is_spoofing_enabled() &&
                   expect16(SECCFG_GET_LOCK_STATE_ADDRESS, 0xB1D0);

    if (applied)
        install_veneer(SECCFG_GET_LOCK_STATE_ADDRESS, (void *)lock_state_hook);

    fastboot_publish("is-spoofing", applied ? "1" : "0");

    if (!applied)
        return false;

    // In fastboot mode the kernel is not booted; keep the getter patch so the
    // lock state is consistent, but leave the kernel/AVB path alone.
    if (get_bootmode() == BOOTMODE_FASTBOOT)
        return true;

    if (expect16(AVB_CHAIN_KEY_LENGTH_ADDRESS, 0x429A))
        PATCH_MEM(AVB_CHAIN_KEY_LENGTH_ADDRESS, 0x451B); /* cmp r3, r3 */

    if (expect16(AVB_CHAIN_KEY_CHECK_ADDRESS, 0xF47F) &&
        expect16(AVB_CHAIN_KEY_CHECK_ADDRESS + 2, 0xAE6C))
        NOP(AVB_CHAIN_KEY_CHECK_ADDRESS, 2);

    if (expect16(AVB_KEY_TRUSTED_ADDRESS, 0xF8DC) &&
        expect16(AVB_KEY_TRUSTED_ADDRESS + 2, 0x3000))
        PATCH_MEM(AVB_KEY_TRUSTED_ADDRESS, 0x2301, 0xBF00);

    return true;
}

// Registered as allowed while security is on: the whole point of the spoof is
// that the device reports itself locked, so the command that turns it back off
// must not be subject to that same lock state.
FASTBOOT_CMD(bldr_spoof, "oem bldr_spoof", cmd_spoof_bootloader_lock, 1);

void board_early_init(void) {
    // These three are needed for the modified bootloader to boot and for
    // fastboot to stay reachable; they are not part of the lock-state toggle.
    if (expect16(GET_VFY_POLICY_ADDRESS, 0xB508))
        FORCE_RETURN(GET_VFY_POLICY_ADDRESS, 0);

    if (expect16(GET_DL_POLICY_ADDRESS, 0xB508))
        FORCE_RETURN(GET_DL_POLICY_ADDRESS, 0);

    if (expect_bl(FASTBOOT_GATE_ADDRESS, FASTBOOT_GATE_CALL_ADDRESS))
        PATCH_BRANCH(FASTBOOT_GATE_ADDRESS, (void *)FASTBOOT_HANDLER_ADDRESS);

    if (expect_bl(EARLY_RTC_CHECK_CALL_ADDRESS, RTC_RECOVERY_CHECK_ADDRESS))
        PATCH_CALL(EARLY_RTC_CHECK_CALL_ADDRESS, (void *)recovery_check_hook,
                   TARGET_THUMB);
}

void board_late_init(void) {
    fastboot_publish("recovery-key", is_recovery_key_enabled() ? "1" : "0");

    bool spoofed = spoof_lock_state();

    if (!spoofed)
        return;

    // On an unlocked bootloader these screens also introduce a five-second
    // delay. Hide them only while the spoof is active; with the spoof off the
    // stock warnings and their delay are left untouched.
    if (expect16(ORANGE_WARNING_ADDRESS, 0xB508))
        FORCE_RETURN(ORANGE_WARNING_ADDRESS, 0);

    if (expect16(DM_VERITY_WARNING_ADDRESS, 0x2802))
        FORCE_RETURN(DM_VERITY_WARNING_ADDRESS, 0);
}
