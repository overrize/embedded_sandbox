#ifndef MDL_MODULE_TASK_H
#define MDL_MODULE_TASK_H

#include "registry.h"

struct host_api;

/*
 * How the module's 8K heap+stack arena sub-region (registry.h's
 * heap_stack_lo/heap_stack_hi) is sliced, fixed for v1:
 *
 *   [heap_stack_lo,        heap_stack_lo+4K) : alloc() pool (host_api.c)
 *   [heap_stack_lo+4K,     heap_stack_lo+6K) : unmapped guard, 2K
 *   [heap_stack_lo+6K,     heap_stack_lo+8K) : FreeRTOS task stack, 2K
 *
 * Why the guard isn't its own xRegions[] entry: xTaskCreateRestricted()
 * gives a task only 3 configurable MPU regions (text/data/heap -- see
 * mdl/third_party/freertos_mpu_port/README.md's region-budget note) but
 * the task's OWN stack is a 4th region the PORT manages automatically
 * (portSTACK_REGION, index 4, reprogrammed by the port on every context
 * switch from puxStackBuffer/usStackDepth below) -- it costs none of the
 * 3 configurable slots. Sizing the stack's dedicated block (2K) smaller
 * than the arena slice leaves the 2K in between simply unmapped by any
 * region, which is a hardware guard for free: the stack grows DOWN from
 * heap_stack_hi, so an overflow past the 2K stack block lands in that
 * unmapped gap and faults immediately, before ever reaching the heap
 * pool. (configCHECK_FOR_STACK_OVERFLOW=2 in FreeRTOSConfig.h is a
 * software second line of defense, not a substitute for this.)
 *
 * All three offsets/sizes below satisfy ARMv7-M's power-of-two-size,
 * self-aligned-base MPU rule given mdl/linker/mdl_arena.ld's existing
 * layout (verified: heap_stack_lo sits at a 24K offset from the 64K-
 * aligned arena start, so +0/+4K/+6K are 4K/2K/2K-aligned respectively)
 * -- mdl_arena.ld itself needed NO changes for this.
 */
#define MDL_HEAP_SIZE  (4 * 1024)
#define MDL_GUARD_SIZE (2 * 1024)
#define MDL_STACK_SIZE (2 * 1024)

/*
 * Scratch carved off the bottom of the module's heap region, used only
 * to hand a console command's argv to the module [ABI v2].
 *
 * It has to live in module-readable memory: the console's line buffer is
 * host .bss, and an unprivileged module reading it takes a MemManage
 * fault. Taking it from the heap region rather than adding a fifth arena
 * region is deliberate -- all three configurable MPU regions are already
 * spent (text/data/heap), so a fourth would not fit.
 */
#define MDL_ARGBLOCK_SIZE 128u
#define MDL_CMD_MAX_ARGS  8

/*
 * Starts *m (already loaded via mdl_load()) as an unprivileged,
 * MPU-restricted FreeRTOS task via xTaskCreateRestricted(): xRegions[]
 * gets exactly the module's text (RX), data (RW+XN), and heap (RW+XN)
 * sub-regions -- nothing else is reachable, no peripheral or host
 * memory region is ever granted. r9 (the module's GOT base) is loaded
 * once at task entry by the trampoline this function installs as
 * pvTaskCode; ordinary Cortex-M context switching (which saves/restores
 * r4-r11, r9 included, as part of every task's exception frame) keeps
 * it correct across preemption for free -- no bespoke r9 save/restore
 * code needed anywhere.
 *
 * Priority is always MDL_MODULE_TASK_PRIORITY (FreeRTOSConfig.h) --
 * the lowest in the system, per the spec's hard requirement that no
 * module task can ever outrank a system or communication task.
 *
 * Returns pdPASS/pdFAIL (xTaskCreateRestricted()'s own return values).
 */
int mdl_start_module_task(module_t *m, const struct host_api *host);

/*
 * Queue a console command for the MDL's resident task [ABI v4].
 *
 * Not a call and not a task restart: the MDL has ONE task, which may
 * already be blocked waiting for a hardware event, so a command is
 * delivered through the same queue events use. argv is copied into the
 * MDL's own arg block first -- the console's line buffer is host memory
 * an unprivileged MDL cannot read.
 *
 * Returns 0 if there is nothing to deliver to, or the arguments do not
 * fit. The reply comes back through the slot; see
 * mdl_supervisor_run_module_command().
 */
int mdl_post_module_command(module_t *m, int argc, const char *const *argv);

#endif /* MDL_MODULE_TASK_H */
