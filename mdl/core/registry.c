#include "registry.h"
#include "console.h"
#include "persist.h"
#include "host_events.h"
#include "arch_if.h"
#include <stddef.h>

module_t g_mdl_slot;
mdl_fault_info_t g_mdl_last_fault;

/*
 * Weak no-op default for the arch layer's fault path.
 *
 * mdl/arch/arm_cm4/fault_arm.c calls mdl_supervisor_wake_from_isr() after
 * classifying a fault as module-internal. On the RTOS targets (M2+)
 * mdl/core/supervisor.c provides the real, strong definition and this one
 * is discarded by the linker. On the bare-metal targets (M0/M1) there is
 * no supervisor task to wake -- and no module task either, so the branch
 * that calls it is unreachable -- but the reference still has to resolve
 * at link time. Defining it weakly here, in the same file the arch layer
 * already talks to for fault classification, keeps fault_arm.c free of
 * any build-configuration #ifdef.
 */
__attribute__((weak)) void mdl_supervisor_wake_from_isr(void)
{
}

void registry_init(void)
{
    g_mdl_slot.state = MDL_SLOT_EMPTY;
    g_mdl_slot.entry = NULL;
    g_mdl_slot.task_handle = NULL;
    g_mdl_slot.last_active_tick = 0;
    g_mdl_last_fault.occurred = false;
}

bool mdl_record_fault(uint32_t pc, uint32_t lr, uint32_t mmfar, uint32_t cfsr)
{
    g_mdl_last_fault.occurred = true;
    g_mdl_last_fault.pc = pc;
    g_mdl_last_fault.lr = lr;
    g_mdl_last_fault.mmfar = mmfar;
    g_mdl_last_fault.cfsr = cfsr;

    bool is_module_fault = g_mdl_slot.state != MDL_SLOT_EMPTY &&
                            arch_pc_in_range(pc, g_mdl_slot.text_lo, g_mdl_slot.text_hi);

    g_mdl_last_fault.text_offset = is_module_fault ? (pc - g_mdl_slot.text_lo) : 0xFFFFFFFFu;

    if (is_module_fault) {
        g_mdl_slot.state = MDL_SLOT_FAULTED;
    }

    /* Classification only -- deciding what to DO about it (patch the
     * faulted task's saved PC to a trap, wake the loader task, or reset
     * for an unrecoverable host-side fault) is
     * mdl/arch/arm_cm4/fault_arm.c's job: patching an exception stack
     * frame is ARM-specific stack-layout knowledge this function
     * shouldn't need, and it stays the caller's decision either way. */
    return is_module_fault;
}

/*
 * Weak no-op event layer, so a target without one still links.
 *
 * mdl/host/host_events.c provides the real implementation and any build
 * with a board does. But module_task.c and supervisor.c call these
 * unconditionally now, and M0-M3 predate the event layer entirely -- the
 * exact shape of build rot this project has been bitten by twice (see
 * maintain.md's build discipline note). Same weak-default fix as
 * mdl_supervisor_wake_from_isr() above and mdl_transport_write() in
 * protocol.c: no #ifdefs, no fake source files per target.
 *
 * On such a target no event is ever posted, so take() returning false
 * forever is exactly right -- the resident loop simply waits.
 */
__attribute__((weak)) bool mdl_events_take(mdl_event_t *out)
{
    (void)out;
    return false;
}

__attribute__((weak)) bool mdl_events_post_console(void)
{
    return false;
}

__attribute__((weak)) bool mdl_events_arm_gpio(int pin, uint8_t edge)
{
    (void)pin;
    (void)edge;
    return false;
}

__attribute__((weak)) void mdl_events_get_stats(mdl_events_stats_t *out)
{
    out->delivered = 0;
    out->lost = 0;
    out->coalesced = 0;
    out->declared_hz = 0;
    out->peak_hz = 0;
}

__attribute__((weak)) void mdl_events_disarm_all(void)
{
}

__attribute__((weak)) void mdl_events_reset(uint16_t depth, uint16_t rate_hz,
                                             void *module_task_handle)
{
    (void)depth;
    (void)rate_hz;
    (void)module_task_handle;
}

/*
 * Weak persistence hooks. mdl/tests/hil/common/board_persist.c provides
 * the real ones; a target with no flash store keeps these and simply
 * never has anything saved. Same pattern as the event-layer stubs above.
 */
__attribute__((weak)) const void *board_persist_image(uint32_t *out_len)
{
    (void)out_len;
    return NULL;
}

__attribute__((weak)) bool board_persist_save(const void *image, uint32_t len)
{
    (void)image;
    (void)len;
    return false;
}

__attribute__((weak)) bool board_persist_forget(void)
{
    return false;
}

/*
 * Weak no-op console, for a target that has no text console at all.
 *
 * supervisor.c calls these unconditionally -- it prints why a saved MDL
 * was rejected at boot, and drains typed lines in its main loop. The QEMU
 * target has neither a CDC port nor a console; it reports through
 * semihosting. Rather than #ifdef the supervisor, the console is optional
 * the same way the transport and the event layer already are.
 *
 * take_line() returning false forever means the loop simply never has a
 * line to run, which is exactly right where nobody can type.
 */
__attribute__((weak)) void console_put_u32(uint32_t v)
{
    (void)v;
}

__attribute__((weak)) void mdl_console_puts(const char *s)
{
    (void)s;
}

__attribute__((weak)) void mdl_console_set_supervisor_handle(void *h)
{
    (void)h;
}

__attribute__((weak)) bool mdl_console_take_line(char **out_line)
{
    (void)out_line;
    return false;
}

__attribute__((weak)) void mdl_console_execute(char *line)
{
    (void)line;
}

/*
 * Weak pin release. The real one (mdl/host/host_api.c) returns each pin
 * to its default state on unload; a target whose host layer has no pin
 * table has nothing to hand back.
 */
__attribute__((weak)) uint32_t host_gpio_release_claims(uint32_t claimed)
{
    (void)claimed;
    return 0;
}

/* Weak I2C claim/release. The real ones are in mdl/host/host_i2c.c; a
 * target with no I2C hardware wired up simply never brings one up, and a
 * claim it cannot honour is reported at load rather than silently
 * granted. */
__attribute__((weak)) bool host_i2c_claim(int instance)
{
    (void)instance;
    return false;
}

__attribute__((weak)) void host_i2c_release(int instance)
{
    (void)instance;
}

/*
 * Weak I2C transfer implementations, for a target with no I2C driver
 * linked in (M2/M3, and the QEMU target).
 *
 * -1 is the vtable's 'refused' code, which is the truthful answer here:
 * the MDL could not have declared a bus this build can honour, so any
 * call it makes was never going to be allowed. Returning 0 would be far
 * worse -- an MDL would read zeroes from a sensor that is not there and
 * treat them as data.
 */
__attribute__((weak)) int host_i2c_write_impl(int bus, int addr7,
                                               const void *data, uint32_t len)
{
    (void)bus; (void)addr7; (void)data; (void)len;
    return -1;
}

__attribute__((weak)) int host_i2c_read_impl(int bus, int addr7,
                                              void *data, uint32_t len)
{
    (void)bus; (void)addr7; (void)data; (void)len;
    return -1;
}

__attribute__((weak)) int host_i2c_write_read_impl(int bus, int addr7,
                                                    const void *tx, uint32_t txlen,
                                                    void *rx, uint32_t rxlen)
{
    (void)bus; (void)addr7; (void)tx; (void)txlen; (void)rx; (void)rxlen;
    return -1;
}

/* Weak UART, for a target with no serial driver linked. -1 is the
 * vtable's 'refused' code and is the honest answer: a build with no
 * driver could not have honoured the claim either. Returning 0 from
 * read() would be worse -- an MDL would treat 'no driver' as 'no data
 * yet' and wait forever for bytes nothing is receiving. */
__attribute__((weak)) bool host_uart_claim(int instance)
{
    (void)instance;
    return false;
}

__attribute__((weak)) void host_uart_release(int instance)
{
    (void)instance;
}

__attribute__((weak)) uint32_t host_uart_dropped(int instance)
{
    (void)instance;
    return 0u;
}

__attribute__((weak)) int host_uart_config_impl(int bus, uint32_t baud)
{
    (void)bus; (void)baud;
    return -1;
}

__attribute__((weak)) int host_uart_write_impl(int bus, const void *data, uint32_t len)
{
    (void)bus; (void)data; (void)len;
    return -1;
}

__attribute__((weak)) int host_uart_loopback_impl(int bus, int enable)
{
    (void)bus; (void)enable;
    return -1;
}

__attribute__((weak)) int host_uart_read_impl(int bus, void *data, uint32_t maxlen)
{
    (void)bus; (void)data; (void)maxlen;
    return -1;
}

__attribute__((weak)) void mdl_events_post_uart(uint8_t instance, uint16_t waiting)
{
    (void)instance; (void)waiting;
}

/* Weak ADC, for a target with no analog front end linked in. -2 rather
 * than 0 on read: 0 counts is a valid measurement of a grounded pin, so
 * a build with no driver must not be able to masquerade as one. */
__attribute__((weak)) bool host_adc_claim(int channel)
{
    (void)channel;
    return false;
}

__attribute__((weak)) void host_adc_release(int channel)
{
    (void)channel;
}

__attribute__((weak)) int host_adc_read_impl(int channel)
{
    (void)channel;
    return -1;
}

__attribute__((weak)) int host_adc_read_mv_impl(int channel)
{
    (void)channel;
    return -1;
}

__attribute__((weak)) int host_adc_describe(int index, int *channel, char *port,
                                             int *pin, bool *confirmed)
{
    (void)index; (void)channel; (void)port; (void)pin; (void)confirmed;
    return -1;
}
