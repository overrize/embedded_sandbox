/*
 * QEMU acceptance test: load a deliberately-crashing module
 * (fault_null_deref), start it as a real unprivileged FreeRTOS-MPU
 * task, and confirm the whole M3 fault-recovery path actually executes
 * -- MemManage fault -> classified as a module fault -> faulted task's
 * PC patched to the trap loop -> supervisor woken -> module task
 * vTaskDelete()'d -> slot reclaimed back to MDL_SLOT_EMPTY -- observed
 * via semihosting output, since this sandbox has no debugger attached.
 *
 * Unlike the AT32 HIL builds' main.c (which runs mdl_supervisor_run()
 * forever), this one runs a BOUNDED watcher loop and calls
 * semihost_exit() with a real pass/fail code once the outcome is known
 * -- appropriate for an automated QEMU test, not for real firmware.
 */
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_device.h"
#include "registry.h"
#include "loader.h"
#include "module_task.h"
#include "host_api.h"
#include "supervisor.h"

extern const uint8_t _binary_module_mdl_start[];
extern const uint8_t _binary_module_mdl_end[];

void qemu_log(const char *msg);

static void semihost_exit(int code)
{
    register uint32_t r0 __asm__("r0") = 0x18; /* SYS_EXIT */
    register uint32_t r1 __asm__("r1") = (code == 0) ? 0x20026u : 0x20023u; /* ApplicationExit / RunTimeError */
    __asm__ volatile("bkpt 0xAB" : : "r"(r0), "r"(r1) : "memory");
}

/* noinline: required under QEMU -- see host_api_qemu.c's comment on the
 * same function for the (empirically confirmed) reason: inlined,
 * back-to-back `bkpt 0xAB` instructions crash QEMU's semihosting
 * emulation. */
__attribute__((noinline))
static void semihost_write0(const char *msg)
{
    register uint32_t r0 __asm__("r0") = 0x04;
    register const char *r1 __asm__("r1") = msg;
    __asm__ volatile("bkpt 0xAB" : : "r"(r0), "r"(r1) : "memory");
}

void mdl_freertos_assert_failed(const char *file, int line)
{
    (void)line; /* skip formatting this for now -- keep the handler as
                 * simple as possible while diagnosing an unrelated
                 * semihosting-call corruption bug */
    semihost_write0("FAIL: configASSERT tripped at ");
    semihost_write0(file);
    semihost_write0("\n");
    semihost_exit(1);
    for (;;) {
    }
}

static void watcher_task(void *pvParameters)
{
    (void)pvParameters;

    registry_init();
    host_api_init();

    /* Give supervisor_task (created before this one, see main()) a
     * moment to run mdl_supervisor_init() from ITS OWN context first --
     * it must be the task whose handle gets notified on a fault, and
     * task creation order alone doesn't guarantee it actually ran
     * before this task starts loading. A fixed short delay is a little
     * crude but entirely adequate for a one-shot QEMU test harness. */
    vTaskDelay(pdMS_TO_TICKS(5));

    extern uint8_t __mdl_text_start[], __mdl_text_end[];
    extern uint8_t __mdl_data_start[], __mdl_data_end[];
    extern uint8_t __mdl_heap_stack_start[], __mdl_heap_stack_end[];

    g_mdl_slot.text_lo       = (uintptr_t)__mdl_text_start;
    g_mdl_slot.text_hi       = (uintptr_t)__mdl_text_end;
    g_mdl_slot.data_lo       = (uintptr_t)__mdl_data_start;
    g_mdl_slot.data_hi       = (uintptr_t)__mdl_data_end;
    g_mdl_slot.heap_stack_lo = (uintptr_t)__mdl_heap_stack_start;
    g_mdl_slot.heap_stack_hi = (uintptr_t)__mdl_heap_stack_end;

    size_t image_len = (size_t)(_binary_module_mdl_end - _binary_module_mdl_start);
    qemu_log("loading module...\n");
    mdl_load_status_t st = mdl_load(&g_mdl_slot, _binary_module_mdl_start, image_len,
                                     HOST_API_ABI_VERSION, MDL_ARCH_ARMV7M);
    if (st != MDL_LOAD_OK) {
        qemu_log("FAIL: mdl_load() rejected the module\n");
        semihost_exit(1);
    }

    host_api_pool_reset(&g_mdl_slot);
    if (!mdl_start_module_task(&g_mdl_slot, &g_host_api)) {
        qemu_log("FAIL: mdl_start_module_task() failed\n");
        semihost_exit(1);
    }
    qemu_log("module task started, expecting it to fault...\n");

    /* Poll for the slot to come back to EMPTY (supervisor's reclaim
     * path) instead of calling mdl_supervisor_run() ourselves -- we
     * need a SEPARATE task to actually run the supervisor loop (it
     * never returns), so spin up a real supervisor task and just watch
     * its work from here. */
    for (int i = 0; i < 200; i++) { /* ~2s at 10ms polls -- generous vs.
                                      * MDL_WATCHDOG_POLL_MS=500 */
        vTaskDelay(pdMS_TO_TICKS(10));
        if (g_mdl_slot.state == MDL_SLOT_EMPTY) {
            qemu_log("PASS: fault caught, module reclaimed, system alive\n");
            semihost_exit(0);
        }
    }
    qemu_log("FAIL: timed out waiting for fault recovery\n");
    semihost_exit(1);
    for (;;) {
    }
}

static void supervisor_task(void *pvParameters)
{
    (void)pvParameters;
    qemu_log("supervisor_task started\n");
    mdl_supervisor_init(); /* captures THIS task's handle -- must run before
                             * watcher_task's mdl_start_module_task() so a
                             * fault has someone correct to wake */
    mdl_supervisor_run(); /* never returns -- watcher_task drives loading,
                            * this task is purely the recovery/watchdog loop */
}

int main(void)
{
    qemu_log("main() entered\n");
    NVIC_SetPriority(MemoryManagement_IRQn,
                      configMAX_SYSCALL_INTERRUPT_PRIORITY >> (8 - configPRIO_BITS));

    /* Created first, and at a higher priority than watcher_task (below)
     * so it actually runs and completes mdl_supervisor_init() during
     * watcher_task's own startup delay rather than merely being
     * schedulable before it. */
    /* portPRIVILEGE_BIT is mandatory for host tasks -- see the note next
     * to MDL_LOADER_TASK_PRIORITY in FreeRTOSConfig.h. Its absence here
     * is the prime suspect for this target's long-standing "MemManage
     * (CFSR.MUNSTKERR) on the second task switch" blocker: the same
     * omission reproduced on real AT32F435 hardware on 2026-09-06 with
     * exactly that CFSR value, and was fixed by adding this bit. The
     * stack-size bump to *8 above was an earlier attempt at that bug and
     * can probably go back to *2 once this is confirmed. */
    xTaskCreate(supervisor_task, "supervisor", configMINIMAL_STACK_SIZE * 8,
                NULL, MDL_LOADER_TASK_PRIORITY | portPRIVILEGE_BIT, NULL);
    xTaskCreate(watcher_task, "watcher", configMINIMAL_STACK_SIZE * 8,
                NULL, (MDL_LOADER_TASK_PRIORITY - 1) | portPRIVILEGE_BIT, NULL);

    qemu_log("about to start scheduler\n");
    vTaskStartScheduler();
    qemu_log("FAIL: vTaskStartScheduler() returned\n");

    for (;;) {
    }
}
