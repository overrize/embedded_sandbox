#include "board_clock.h"
#include "at32f435_437.h"

/*
 * WHY THE VENDOR'S PLL NUMBERS CANNOT BE COPIED
 *
 * Every AT32F435 example in the BSP (including usb_device/virtual_comport,
 * which this file's USB half is modelled on) configures 288MHz as:
 *
 *     crm_pll_config(CRM_PLL_SOURCE_HEXT, 144, 1, CRM_PLL_FR_4)
 *
 * and the table printed next to it is explicitly labelled
 * "pll source selected hick or hext(8mhz)". This board's crystal is
 * 24MHz (schematic: X2 24MHz + C11/C12 15pF into OSC_IN/OSC_OUT), so
 * those exact arguments would ask for
 *
 *     24MHz * 144 / (1 * 4) = 864MHz
 *
 * which is three times the part's maximum. It also violates the driver's
 * own documented input constraint before it ever gets there --
 * at32f435_437_crm.c's crm_pll_config() header states:
 *
 *     31  <= pll_ns <= 500
 *     1   <= pll_ms <= 15
 *     2MHz  <= pll_rcs_freq / pll_ms          <= 16MHz
 *     500MHz <= pll_rcs_freq * pll_ns / pll_ms <= 1200MHz
 *
 * With pll_ms = 1 the PLL reference input would be the raw 24MHz, past
 * the 16MHz ceiling. So pll_ms is not a free parameter here: it has to
 * bring 24MHz back inside 2..16MHz.
 *
 * WHAT THIS FILE USES, AND WHY IT IS THE SAFE CHOICE
 *
 *     pll_ms = 3   ->  reference  = 24MHz / 3 = 8MHz        (2..16MHz OK)
 *     pll_ns = 144 ->  VCO        = 8MHz * 144 = 1152MHz    (500..1200MHz OK)
 *     pll_fr = 4   ->  sclk       = 1152MHz / 4 = 288MHz    (part maximum)
 *
 * Note what that means: dividing by 3 first lands the PLL on exactly the
 * same 8MHz reference and the same 1152MHz VCO as the vendor's validated
 * 8MHz-crystal configuration. This is not a new operating point -- it is
 * the vendor's operating point, reached from a different crystal. Every
 * downstream divider (AHB, APB, and the /6 for USB) is therefore
 * unchanged from the example, and 288MHz is the datasheet maximum rather
 * than an overclock.
 *
 * If the crystal is ever changed, pll_ms is the only value that should
 * move: keep pll_rcs_freq / pll_ms == 8MHz and everything else follows.
 */

#define BOARD_PLL_MS 3u   /* 24MHz / 3 = 8MHz PLL reference */
#define BOARD_PLL_NS 144u /* 8MHz * 144 = 1152MHz VCO       */
                          /* CRM_PLL_FR_4 -> 1152 / 4 = 288MHz sclk */

void board_clock_init(void)
{
    crm_reset();

    crm_periph_clock_enable(CRM_PWC_PERIPH_CLOCK, TRUE);

    /* 288MHz needs the high LDO setting and 3 flash wait-state clocks --
     * both must be raised BEFORE the core actually runs at 288MHz, not
     * after, or the first flash fetch at speed fails. */
    pwc_ldo_output_voltage_set(PWC_LDO_OUTPUT_1V3);
    flash_clock_divider_set(FLASH_CLOCK_DIV_3);

    crm_clock_source_enable(CRM_CLOCK_SOURCE_HEXT, TRUE);
    while (crm_hext_stable_wait() == ERROR) {
        /* A hang here means the 24MHz crystal is not oscillating --
         * check X2 / C11 / C12 before suspecting anything in software. */
    }

    crm_pll_config(CRM_PLL_SOURCE_HEXT, BOARD_PLL_NS, BOARD_PLL_MS, CRM_PLL_FR_4);
    crm_clock_source_enable(CRM_CLOCK_SOURCE_PLL, TRUE);
    while (crm_flag_get(CRM_PLL_STABLE_FLAG) != SET) {
    }

    crm_ahb_div_set(CRM_AHB_DIV_1);   /* AHB  = 288MHz */
    crm_apb2_div_set(CRM_APB2_DIV_2); /* APB2 = 144MHz (bus maximum) */
    crm_apb1_div_set(CRM_APB1_DIV_2); /* APB1 = 144MHz (bus maximum) */

    /* Auto-step ramps the switch to 288MHz through intermediate
     * frequencies instead of stepping in one jump -- required by the
     * part when the target is this far above the current sclk. */
    crm_auto_step_mode_enable(TRUE);
    crm_sysclk_switch(CRM_SCLK_PLL);
    while (crm_sysclk_switch_status_get() != CRM_SCLK_PLL) {
    }
    crm_auto_step_mode_enable(FALSE);

    /* Recomputes system_core_clock from the registers just programmed.
     * It reads HEXT_VALUE for the PLL-from-HEXT case, so the 24000000 in
     * at32f435_437_conf.h has to be right or every consumer of
     * system_core_clock (SysTick reload, FreeRTOS configCPU_CLOCK_HZ,
     * UART baud divisors) is off by the same factor. */
    system_core_clock_update();
}

void board_usb_clock_init(void)
{
    /* Select the PLL (not HICK) as the OTGFS clock source.
     *
     * Strictly redundant today: CRM_USB_CLOCK_SOURCE_PLL is 0, and
     * board_clock_init()'s crm_reset() clears the whole misc1 register,
     * so hick_to_usb is already 0 by the time we get here. Stated
     * explicitly anyway -- the alternative is a 48MHz USB clock that
     * depends on a side effect of a reset call in a different function,
     * which is exactly the kind of thing that silently breaks when
     * someone reorders init later. The vendor example gets away with
     * omitting it for the same reason; we would rather not have to know
     * that to read this file. */
    crm_usb_clock_source_select(CRM_USB_CLOCK_SOURCE_PLL);

    /* OTGFS needs 48MHz. sclk is 288MHz -> /6. Kept as an explicit
     * assertion-by-construction rather than a switch on the measured
     * sclk (what the vendor example does): this board has exactly one
     * clock configuration, so a silent "default: do nothing" branch
     * would just hide a misconfiguration until USB failed to enumerate. */
    crm_usb_clock_div_set(CRM_USB_DIV_6);
}
