#ifndef MDL_HOST_EVENTS_H
#define MDL_HOST_EVENTS_H

#include <stdint.h>
#include <stdbool.h>
#include "mdl_format.h" /* mdl_event_t, mdl_edge_t */

/*
 * Event delivery for MDLs [ABI v4].
 *
 * The shape of this file is decided by one fact: an ISR runs in handler
 * mode, which on ARMv7-M is always privileged. Calling MDL code from an
 * interrupt would therefore run it privileged, with whatever MPU regions
 * the interrupted task happened to have -- the sandbox would be worth
 * nothing. So the interrupt half is host code (small, flashed, privileged)
 * and does exactly two things: capture the event, wake the MDL's task. The
 * MDL's own handler runs later, unprivileged, in that task.
 *
 * The cost is honest and unavoidable: an event reaches the MDL after a
 * task wake and a context switch, not at interrupt latency. Work with a
 * deadline tighter than that cannot be done by an MDL at all, and no ABI
 * change would fix it -- it has to be host code.
 */

/* MDL_EVT_QUEUE_MAX lives in mdl_format.h -- it is a contract term the
 * packer checks against, not a private detail of this file. */

/* Arm/disarm. Called from the loader (privileged, module not yet running)
 * and from reclaim_module(). Disarm is not optional: an interrupt still
 * pointing at an unloaded MDL is a wake for a task that no longer exists. */
bool mdl_events_arm_gpio(int pin, uint8_t edge);
void mdl_events_disarm_all(void);

/* Called by the loader once the MDL's declared budget is known. */
void mdl_events_reset(uint16_t depth, uint16_t rate_hz, void *module_task_handle);

/*
 * Take the oldest event, or false if none. Runs in the module task
 * through the syscall gate (module_task.c), so `out` points into module
 * memory and the copy happens while privileged.
 */
bool mdl_events_take(mdl_event_t *out);

/* Queue a console command for the resident task, so commands and hardware
 * events arrive through one path. An MDL cannot both block waiting for an
 * event and be restarted to run a command, so there is one queue. */
bool mdl_events_post_console(void);

/* For `status`: what was lost, and whether the declared rate held. */
typedef struct {
    uint32_t delivered;
    uint32_t lost;        /* could not even be merged -- real loss */
    uint32_t coalesced;   /* merged into an earlier event; not loss */
    uint16_t declared_hz;
    uint32_t peak_hz;     /* highest rate seen in any 1s window */
} mdl_events_stats_t;

void mdl_events_get_stats(mdl_events_stats_t *out);

#endif /* MDL_HOST_EVENTS_H */
