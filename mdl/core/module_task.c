#include "module_task.h"
#include "arch_if.h"
#include "FreeRTOS.h"
#include "task.h"

/*
 * v1: exactly one module slot (g_mdl_slot), so the trampoline reads it
 * directly rather than threading extra context through pvParameters --
 * pvParameters carries just the host_api_t* module_init() itself needs.
 * A multi-module future would need a small per-task context struct
 * instead; not needed yet, not built speculatively.
 */
static void module_task_trampoline(void *pvParameters)
{
    const void *host = pvParameters;

    /* arch_call_privileged()'s name is a holdover from M1, where the
     * caller (the loader, running from main()) really was privileged.
     * The mechanism itself -- load r9, blx, restore r9 -- doesn't touch
     * CONTROL or care what privilege level it runs at, so it's exactly
     * as correct called from this now-UNPRIVILEGED task. Not renamed to
     * avoid churning M1's already-verified arch_if.h contract for a
     * cosmetic reason. */
    void *got_base = (void *)g_mdl_slot.data_lo;
    (void)arch_call_privileged(g_mdl_slot.entry, got_base, host);

    /* module_init() returned -- v1's module lifecycle ends here (no
     * "long-running task body" support yet; a module that wants to keep
     * doing work after init would need module_init() to itself loop,
     * which host_api.h's delay_ms() documents as fine to call from the
     * task body). vTaskDelete(NULL) is host-side code calling a real
     * FreeRTOS API directly -- allowed; it's module_init() that must
     * never do this, and it can't: modules don't link against tasks.h
     * at all. */
    g_mdl_slot.state = MDL_SLOT_LOADED; /* ran once; loader may reload */
    g_mdl_slot.task_handle = NULL; /* about to be stale -- don't leave a
                                     * dangling handle mdl_supervisor.c
                                     * could mistakenly vTaskDelete()
                                     * again on some later, unrelated
                                     * fault */
    vTaskDelete(NULL);
}

int mdl_start_module_task(module_t *m, const struct host_api *host)
{
    uintptr_t heap_base  = m->heap_stack_lo;
    uintptr_t stack_base = m->heap_stack_lo + MDL_HEAP_SIZE + MDL_GUARD_SIZE;

    TaskParameters_t task_def = { 0 };
    task_def.pvTaskCode     = module_task_trampoline;
    task_def.pcName         = "module";
    task_def.usStackDepth   = MDL_STACK_SIZE / sizeof(StackType_t);
    task_def.pvParameters   = (void *)host;
    task_def.uxPriority     = MDL_MODULE_TASK_PRIORITY; /* portPRIVILEGE_BIT NOT set -> unprivileged */
    task_def.puxStackBuffer = (StackType_t *)stack_base;

    task_def.xRegions[0].pvBaseAddress   = (void *)m->text_lo;
    task_def.xRegions[0].ulLengthInBytes = (uint32_t)(m->text_hi - m->text_lo);
    task_def.xRegions[0].ulParameters    = tskMPU_REGION_READ_ONLY;

    task_def.xRegions[1].pvBaseAddress   = (void *)m->data_lo;
    task_def.xRegions[1].ulLengthInBytes = (uint32_t)(m->data_hi - m->data_lo);
    task_def.xRegions[1].ulParameters    = tskMPU_REGION_READ_WRITE | tskMPU_REGION_EXECUTE_NEVER;

    task_def.xRegions[2].pvBaseAddress   = (void *)heap_base;
    task_def.xRegions[2].ulLengthInBytes = MDL_HEAP_SIZE;
    task_def.xRegions[2].ulParameters    = tskMPU_REGION_READ_WRITE | tskMPU_REGION_EXECUTE_NEVER;

    TaskHandle_t created = NULL;
    BaseType_t ok = xTaskCreateRestricted(&task_def, &created);
    if (ok != pdPASS) {
        return 0;
    }

    /* RUNNING starts counting from here (task exists, scheduler will
     * pick it up), not from the trampoline's first line -- avoids a
     * window where the supervisor could see a stale/uninitialized
     * last_active_tick if it happened to check before the new task got
     * its first timeslice. */
    m->task_handle = created;
    m->last_active_tick = (uint32_t)xTaskGetTickCount();
    m->state = MDL_SLOT_RUNNING;
    return 1;
}
