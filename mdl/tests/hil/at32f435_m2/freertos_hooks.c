/*
 * FreeRTOS application hooks required by this project's FreeRTOSConfig.h:
 * configUSE_MALLOC_FAILED_HOOK, configCHECK_FOR_STACK_OVERFLOW=2, and
 * configSUPPORT_STATIC_ALLOCATION=1 (the kernel's own idle/timer tasks
 * need their TCB+stack storage supplied statically when static
 * allocation is enabled at all, regardless of whether application code
 * uses static allocation for anything else).
 */
#include "FreeRTOS.h"
#include "task.h"

void vApplicationMallocFailedHook(void)
{
    /* A kernel-object allocation (task/queue/etc, from configTOTAL_HEAP_SIZE)
     * failed -- not a module fault, a host-side resource exhaustion.
     * M3 gives this a real recovery story; for now, stop where it's
     * obvious under a debugger rather than limping on with a NULL a
     * caller didn't check. */
    __asm volatile("bkpt #0");
    for (;;) {
    }
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;
    /* configCHECK_FOR_STACK_OVERFLOW=2's software canary check caught
     * an overflow (belt-and-suspenders alongside module_task.c's
     * hardware guard gap -- this hook also covers HOST-side task stack
     * overflows, which the MPU guard gap doesn't, since it only applies
     * to the module's own stack block). M3 formalizes fault recovery;
     * for now, stop visibly rather than continue on a corrupted stack. */
    __asm volatile("bkpt #0");
    for (;;) {
    }
}

static StaticTask_t s_idle_tcb;
static StackType_t  s_idle_stack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                    StackType_t **ppxIdleTaskStackBuffer,
                                    uint32_t *pulIdleTaskStackSize)
{
    *ppxIdleTaskTCBBuffer = &s_idle_tcb;
    *ppxIdleTaskStackBuffer = s_idle_stack;
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

static StaticTask_t s_timer_tcb;
static StackType_t  s_timer_stack[configTIMER_TASK_STACK_DEPTH];

void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer,
                                     StackType_t **ppxTimerTaskStackBuffer,
                                     uint32_t *pulTimerTaskStackSize)
{
    *ppxTimerTaskTCBBuffer = &s_timer_tcb;
    *ppxTimerTaskStackBuffer = s_timer_stack;
    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}
