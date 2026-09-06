#ifndef MDL_BOARD_CLOCK_H
#define MDL_BOARD_CLOCK_H

/*
 * Clock bring-up for the UYUP-RPI-A-2.4 board (AT32F435VCT7, 24MHz HEXT).
 *
 * Shared by every target under mdl/tests/hil that needs more than the
 * reset default. SystemInit() (vendor, runs from the startup file before main)
 * leaves the part on HICK with misc1_bit.hick_to_sclk=1 -> 48MHz, using
 * no external crystal at all. That is enough for the M0/M1 arena and
 * loader tests, but not for USB, and not for any performance claim.
 */

/*
 * 24MHz HEXT -> 288MHz sclk (the AT32F435's rated maximum; this is NOT
 * an overclock), AHB 288MHz, APB1/APB2 144MHz.
 *
 * Blocks until HEXT and the PLL are both stable, then updates
 * system_core_clock. Call once, first thing in main(), before anything
 * that derives a timebase (SysTick, FreeRTOS, USB).
 */
void board_clock_init(void);

/*
 * Point the OTGFS peripheral clock at 48MHz, derived from the 288MHz PLL
 * (288/6). Call after board_clock_init() and before usbd_init().
 *
 * Deliberately NOT the HICK+ACC crystal-less path the vendor example
 * offers as an alternative: this board has a real 24MHz crystal, so the
 * PLL-derived clock is both available and more accurate than SOF-trimmed
 * HICK.
 */
void board_usb_clock_init(void);

#endif /* MDL_BOARD_CLOCK_H */
