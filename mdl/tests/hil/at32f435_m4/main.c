/*
 * M4 acceptance firmware: modules arrive over USB CDC at runtime --
 * this is the first milestone where main.c does NOT embed a module via
 * objcopy (M1-M3's build-time test shim). tools/packer.py + a serial
 * write (tools/watch.py automates this) is the only way a module gets
 * into this firmware from here on.
 *
 * VERIFIED ON REAL HARDWARE 2026-09-06 (AT32F435VCT7 on a
 * UYUP-RPI-A-2.4): enumerates as VID 2E3C / PID 5740, the console
 * answers over the CDC port, and `clk` reads back the full 288MHz. What
 * is still unproven here is the part this milestone exists for -- an
 * actual module arriving over the wire and running. Build and flash with
 * mdl/tests/hil/build.ps1; see mdl/tests/hil/ozone/README.md for the
 * board's two non-obvious prerequisites (DAP_CONF grounded before power,
 * and an A-to-C cable).
 */
#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS.h"
#include "task.h"
#include "at32f435_437.h"
#include "registry.h"
#include "sandbox.h"
#include "host_api.h"
#include "supervisor.h"
#include "console.h"
#include "usb_cdc.h"
#include "board_clock.h"
#include "board_debug.h"

void mdl_freertos_assert_failed(const char *file, int line)
{
    (void)file;
    (void)line;
    __asm volatile("bkpt #0");
    for (;;) {
    }
}

static void supervisor_task(void *pvParameters)
{
    (void)pvParameters;

    registry_init();
    /* Without this the arena bounds in g_mdl_slot stay zero and
     * mdl_load() would memcpy the module to address 0. It went unnoticed
     * until the console's `arena` command printed all zeros on the first
     * working hardware run. Bounds only, not MPU programming -- see
     * sandbox.h for why that half belongs to FreeRTOS on this target. */
    sandbox_bounds_init();
    host_api_init();
    mdl_supervisor_init();

    /* Before the greeting, so a restored MDL's own startup log lands after
     * the banner instead of interleaved with it. */
    mdl_supervisor_restore();

    mdl_console_greet();

    /* "USB CDC 和 loader 任务在任何情况下都要活着" -- this task IS both
     * the loader/supervisor AND (via protocol.c's frame handling in
     * mdl_supervisor_run()) the thing that answers USB requests, so
     * keeping it alive is the whole point of everything M3 built. */
    mdl_supervisor_run();
}

static void usb_task(void *pvParameters)
{
    usb_cdc_rx_task(pvParameters); /* never returns */
}

/*
 * Everything a person looking at the board (or at a freshly opened
 * terminal) needs in order to tell what state the thing is in. Three
 * separate questions, three separate answers:
 *
 *   LEDB (PD10), 1Hz blink   "the firmware is alive"
 *       Power, 288MHz clock, scheduler, task switching and GPIO all
 *       work. Independent of USB -- this is what distinguishes "the
 *       firmware died" from "USB is broken", which look identical from
 *       the PC side when nothing enumerates.
 *
 *   LEDG (PE15), solid on    "USB is enumerated and configured"
 *       The host has seen the device and set up the CDC endpoints. Off
 *       means the link is down: bad cable (this board needs A-to-C, see
 *       mdl/tests/hil/ozone/README.md), wrong D+/D- jumper pair, or the
 *       48MHz clock is wrong.
 *
 *   banner reprinted         "your terminal just connected"
 *       The boot banner goes out long before anyone opens a COM port, so
 *       opening one used to show a blank screen until you pressed Enter.
 *
 * Lower priority than the supervisor so host_api_init() -- which is what
 * configures PD10/PE15 as outputs in the first place -- has necessarily
 * run before the first toggle. Still above MDL_MODULE_TASK_PRIORITY, per
 * the rule that no module may outrank a host task.
 */
static void indicator_task(void *pvParameters)
{
    (void)pvParameters;

    bool was_up = false;

    for (;;) {
        /* Reprinting on connect is what makes an opened terminal show
         * something immediately. mdl_transport_write() is mutex-guarded,
         * so writing from this task alongside the supervisor is safe. */
        if (usb_cdc_take_host_open_event()) {
            mdl_console_greet();
        }

        /* Stand down on any pin a loaded module has DECLARED.
         *
         * Both indicators are a courtesy, not a requirement: the host
         * stays a working host without them. So a module that asks for
         * one gets it, and the host stops driving it rather than
         * fighting -- which is the difference between a handover and the
         * silent two-writer collision this arbitration exists to
         * prevent. was_up is reset on release so the LED is repainted
         * from the real link state rather than a stale edge. */
        bool green_mine = !host_gpio_yielded_to_module(1);
        bool blue_mine  = !host_gpio_yielded_to_module(0);

        if (green_mine) {
            bool up = usb_cdc_link_up();
            if (up != was_up) {
                host_gpio_direct_set(1, up ? 0 : 1); /* active-low: 0 = lit */
                was_up = up;
            }
        } else {
            was_up = false;
        }

        if (blue_mine) {
            host_gpio_direct_set(0, 0); /* LEDB on */
            vTaskDelay(pdMS_TO_TICKS(100));
            host_gpio_direct_set(0, 1); /* LEDB off */
            vTaskDelay(pdMS_TO_TICKS(400));
        } else {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

int main(void)
{
    /* Before everything: take BusFault/UsageFault in their own handlers
     * and disable the write buffer, so a faulting store is reported at
     * the store instead of surfacing as an imprecise HardFault hundreds
     * of instructions later. See board_debug.h. */
    board_debug_faults_init();

    /* 24MHz HEXT -> 288MHz sclk. MUST come first: usb_cdc_init() derives
     * the mandatory 48MHz USB clock as sclk/6, so on the 48MHz HICK
     * reset default it would hand OTGFS 8MHz and enumeration would fail
     * with nothing in software to point at. */
    board_clock_init();

    NVIC_SetPriority(MemoryManagement_IRQn,
                      configMAX_SYSCALL_INTERRUPT_PRIORITY >> (8 - configPRIO_BITS));

    usb_cdc_init();

    /* portPRIVILEGE_BIT is mandatory for host tasks -- see the long note
     * next to MDL_LOADER_TASK_PRIORITY in FreeRTOSConfig.h. Without it
     * these come up unprivileged, their stacks land in PRIVILEGED_DATA
     * (the FreeRTOS heap), and the first exception return to them faults
     * with CFSR.MUNSTKERR before any of their code runs. */
    xTaskCreate(supervisor_task, "supervisor", configMINIMAL_STACK_SIZE * 2,
                NULL, MDL_LOADER_TASK_PRIORITY | portPRIVILEGE_BIT, NULL);
    xTaskCreate(usb_task, "usb_cdc", configMINIMAL_STACK_SIZE * 2,
                NULL, MDL_USB_TASK_PRIORITY | portPRIVILEGE_BIT, NULL);
    xTaskCreate(indicator_task, "indicator", configMINIMAL_STACK_SIZE * 2,
                NULL, (MDL_MODULE_TASK_PRIORITY + 1) | portPRIVILEGE_BIT, NULL);

    vTaskStartScheduler();

    for (;;) {
        __asm volatile("bkpt #0");
    }
}
