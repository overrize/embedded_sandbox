#include "module_task.h"
#include "host_events.h"
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
/*
 * THE TRAMPOLINE RUNS UNPRIVILEGED, SO IT CANNOT TOUCH g_mdl_slot DIRECTLY.
 *
 * mdl_start_module_task() below deliberately does NOT set portPRIVILEGE_BIT,
 * so module_task_trampoline() -- host code though it is -- executes at the
 * same privilege level as the module itself. g_mdl_slot lives in ordinary
 * host .bss (mdl/core/registry.c), which is outside every region granted
 * to this task: FreeRTOS-MPU's prvSetupMPU() gives unprivileged tasks
 * flash (read-only) plus their own stack plus xRegions[], and nothing
 * else in SRAM. Reading or writing g_mdl_slot from here therefore takes a
 * MemManage fault on the very first access -- reliably, not intermittently.
 *
 * The two helpers below are the fix, and they reuse the exact mechanism
 * mdl/host/host_api.c already uses for the vtable: put the function in
 * the `freertos_system_calls` linker section (the only place from which
 * FreeRTOS-MPU's xPortRaisePrivilege() actually elevates -- it checks the
 * caller's return address against __syscalls_flash_start__/_end__, see
 * the M2 linker script), raise privilege, touch host state, drop back.
 *
 * Not solved by granting a fourth MPU region over g_mdl_slot: all three
 * of portNUM_CONFIGURABLE_REGIONS are already spent on text/data/heap.
 */
#define MDL_SYSCALL_GATE __attribute__((section("freertos_system_calls")))

/* Real declarations, matching mdl/third_party/freertos_mpu_port's
 * mpu_wrappers.c -- not exposed by any public FreeRTOS header. */
extern BaseType_t xPortRaisePrivilege(void);
extern void vPortResetPrivilege(BaseType_t xRunningPrivileged);

typedef struct {
    void *entry;
    void *got_base;
} module_entry_t;

static void module_task_read_entry(module_entry_t *out) MDL_SYSCALL_GATE;
static void module_task_read_entry(module_entry_t *out)
{
    BaseType_t was_priv = xPortRaisePrivilege();
    out->entry    = g_mdl_slot.entry;
    out->got_base = (void *)g_mdl_slot.data_lo;
    vPortResetPrivilege(was_priv);
}

/* argc/argv for a pending module_cmd() invocation. Host memory, so the
 * unprivileged trampoline reads them through the gate like everything
 * else it needs from the host side. */
static int s_cmd_argc;
static const char *const *s_cmd_argv;

static void module_task_read_cmd(module_entry_t *out, int *argc,
                                  const char *const **argv) MDL_SYSCALL_GATE;
static void module_task_read_cmd(module_entry_t *out, int *argc,
                                  const char *const **argv)
{
    BaseType_t was_priv = xPortRaisePrivilege();
    out->entry    = g_mdl_slot.cmd_entry;
    out->got_base = (void *)g_mdl_slot.data_lo;
    *argc         = s_cmd_argc;
    *argv         = s_cmd_argv;
    vPortResetPrivilege(was_priv);
}

static void module_task_finish_cmd(int ret) MDL_SYSCALL_GATE;
static void module_task_finish_cmd(int ret)
{
    BaseType_t was_priv = xPortRaisePrivilege();
    g_mdl_slot.cmd_ret     = ret;
    g_mdl_slot.cmd_pending = 0u; /* the task lives on -- only the command ended */
    vPortResetPrivilege(was_priv);
}

static void module_task_mark_finished(void) MDL_SYSCALL_GATE;
static void module_task_mark_finished(void)
{
    BaseType_t was_priv = xPortRaisePrivilege();
    g_mdl_slot.state = MDL_SLOT_LOADED; /* ran once; loader may reload */
    g_mdl_slot.task_handle = NULL;      /* about to be stale -- don't leave a
                                          * dangling handle supervisor.c could
                                          * mistakenly vTaskDelete() again on
                                          * some later, unrelated fault */
    vPortResetPrivilege(was_priv);
}

/* Everything the resident phase needs from the host side, read once. */
typedef struct {
    void *cmd_entry;
    void *evt_entry;
    void *got_base;
} module_ctx_t;

static void module_task_read_ctx(module_ctx_t *out) MDL_SYSCALL_GATE;
static void module_task_read_ctx(module_ctx_t *out)
{
    BaseType_t was_priv = xPortRaisePrivilege();
    out->cmd_entry = g_mdl_slot.cmd_entry;
    out->evt_entry = g_mdl_slot.evt_entry;
    out->got_base  = (void *)g_mdl_slot.data_lo;
    vPortResetPrivilege(was_priv);
}

static bool module_task_take_event(mdl_event_t *out) MDL_SYSCALL_GATE;
static bool module_task_take_event(mdl_event_t *out)
{
    BaseType_t was_priv = xPortRaisePrivilege();
    bool got = mdl_events_take(out);
    vPortResetPrivilege(was_priv);
    return got;
}

/*
 * Block until something arrives.
 *
 * Parked across the wait, or the watchdog would kill the MDL for the
 * crime of having nothing to do -- which is the normal state of an
 * event-driven module and the whole reason MDL_PARKED_FOREVER exists.
 * Waking counts as progress, so the idle timer restarts from here.
 */
static void module_task_wait(void) MDL_SYSCALL_GATE;
static void module_task_wait(void)
{
    BaseType_t was_priv = xPortRaisePrivilege();
    g_mdl_slot.parked_until_tick = MDL_PARKED_FOREVER;
    vPortResetPrivilege(was_priv);

    /* MPU_ulTaskNotifyTake via mpu_wrappers -- a legal SVC from
     * unprivileged code, unlike touching g_mdl_slot directly. */
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

    was_priv = xPortRaisePrivilege();
    g_mdl_slot.parked_until_tick = 0;
    g_mdl_slot.last_active_tick  = (uint32_t)xTaskGetTickCount();
    vPortResetPrivilege(was_priv);
}

/*
 * The MDL's one task, for its whole life.
 *
 * module_init() runs first, exactly as before. What changed in ABI v4 is
 * what happens after it returns: if the MDL exports module_event() or
 * module_cmd(), this task stays and drains a single queue carrying both
 * hardware events and console commands.
 *
 * One task and one queue is not tidiness, it is forced. The previous
 * design ran a console command by RESTARTING the module task at
 * module_cmd(), which cannot coexist with a task already blocked waiting
 * for an event -- there is only one task to restart. Routing commands
 * through the same queue removes the conflict instead of arbitrating it.
 */
static void module_task_trampoline(void *pvParameters)
{
    const void *host = pvParameters;

    module_entry_t me;
    module_task_read_entry(&me);
    (void)arch_call_privileged(me.entry, me.got_base, host);

    module_ctx_t ctx;
    module_task_read_ctx(&ctx);

    /* Nothing to stay resident for: v1 behaviour, unchanged. */
    if (ctx.cmd_entry == NULL && ctx.evt_entry == NULL) {
        module_task_mark_finished();
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        mdl_event_t evt;
        while (module_task_take_event(&evt)) {
            if (evt.source == (uint8_t)MDL_EVT_CONSOLE) {
                module_entry_t unused;
                int argc = 0;
                const char *const *argv = NULL;
                module_task_read_cmd(&unused, &argc, &argv);
                if (ctx.cmd_entry != NULL && argc > 0) {
                    int ret = arch_call_module3(ctx.cmd_entry, ctx.got_base,
                                                 host, argc, argv);
                    module_task_finish_cmd(ret);
                } else {
                    module_task_finish_cmd(-1);
                }
            } else if (ctx.evt_entry != NULL) {
                /* evt is on this task's own stack, i.e. module memory --
                 * the MDL can read it. A pointer into the host's queue
                 * could not be dereferenced here at all. */
                (void)arch_call_module2(ctx.evt_entry, ctx.got_base, host, &evt);
            }
        }
        module_task_wait();
    }
}

/*
 * ulParameters must be RAW ARMv7-M RASR BITS, not the tskMPU_REGION_*
 * flags from task.h. This looks like a free choice and is not.
 *
 * task.h defines tskMPU_REGION_READ_ONLY/READ_WRITE/EXECUTE_NEVER as
 * ( 1UL << 0/1/2 ) -- abstract flags that the ARMv8-M ports translate
 * inside vPortStoreTaskMPUSettings(). The ARMv7-M port does NOT
 * translate: it ORs ulParameters straight into RASR, whose low bits
 * mean something else entirely (bit 0 ENABLE, bits [5:1] SIZE, bits
 * [26:24] AP). Passing the abstract flags here therefore did two
 * things at once, and this cost a full hardware debugging session:
 *
 *   - AP came out 000 = no access AT ANY PRIVILEGE. Not merely
 *     'unprivileged denied' -- privileged handler-mode code is denied
 *     too, which is why the failure surfaced inside PendSV rather than
 *     in the module.
 *   - SIZE got ORed with 0b011. The 8K data region became a 64K one,
 *     and since the MPU ignores base-address bits below the region
 *     size, it silently expanded to cover 0x20020000..0x2002FFFF --
 *     swallowing the module task's own stack. Being region 6 against
 *     the stack's region 4, and higher region numbers winning on
 *     overlap, it beat the stack region that was set up correctly.
 *
 * Net effect: the very first context switch into a module task took a
 * MemManage DACCVIOL restoring that task's registers, on the load of
 * ANY module -- an 8-byte `return 7;` included.
 *
 * The TEX/S/C/B term is not optional either. vPortStoreTaskMPUSettings()
 * adds configTEX_S_C_B_SRAM for the stack region it builds itself but
 * not for these, so leaving it out gives TEX=S=C=B=0 -- Strongly
 * Ordered memory, on which unaligned accesses fault. Matching the
 * port's own choice for SRAM keeps module memory ordinary Normal
 * memory.
 */
#define MDL_MPU_SRAM_TEX_S_C_B \
(((configTEX_S_C_B_SRAM & portMPU_RASR_TEX_S_C_B_MASK)) << portMPU_RASR_TEX_S_C_B_LOCATION)

/*
 * Same shape as module_task_trampoline(), for a console-invoked
 * module_cmd() [ABI v2]. Unprivileged, like the module itself; every
 * host-side value it needs arrives through a syscall gate.
 */
static void module_cmd_trampoline(void *pvParameters)
{
    const void *host = pvParameters;

    module_entry_t me;
    int argc = 0;
    const char *const *argv = NULL;
    module_task_read_cmd(&me, &argc, &argv);

    int ret = arch_call_module3(me.entry, me.got_base, host, argc, argv);

    module_task_finish_cmd(ret);
    vTaskDelete(NULL);
}

/*
 * Region setup is identical for both entry points -- the module's MPU
 * view of itself does not depend on which of its functions is running --
 * so it lives here rather than being written twice and drifting.
 * See the long note inside about why ulParameters must be raw RASR bits.
 */
static void fill_task_def(TaskParameters_t *td, module_t *m,
                           const struct host_api *host, void *code)
{
    uintptr_t heap_base  = m->heap_stack_lo;
    uintptr_t stack_base = m->heap_stack_lo + MDL_HEAP_SIZE + MDL_GUARD_SIZE;

    td->pvTaskCode     = (TaskFunction_t)code;
    td->pcName         = "module";
    td->usStackDepth   = MDL_STACK_SIZE / sizeof(StackType_t);
    td->pvParameters   = (void *)host;
    td->uxPriority     = MDL_MODULE_TASK_PRIORITY; /* no portPRIVILEGE_BIT -> unprivileged */
    td->puxStackBuffer = (StackType_t *)stack_base;

    td->xRegions[0].pvBaseAddress   = (void *)m->text_lo;
    td->xRegions[0].ulLengthInBytes = (uint32_t)(m->text_hi - m->text_lo);
    td->xRegions[0].ulParameters    = portMPU_REGION_READ_ONLY |
                                       MDL_MPU_SRAM_TEX_S_C_B;

    td->xRegions[1].pvBaseAddress   = (void *)m->data_lo;
    td->xRegions[1].ulLengthInBytes = (uint32_t)(m->data_hi - m->data_lo);
    td->xRegions[1].ulParameters    = portMPU_REGION_READ_WRITE |
                                       portMPU_REGION_EXECUTE_NEVER |
                                       MDL_MPU_SRAM_TEX_S_C_B;

    td->xRegions[2].pvBaseAddress   = (void *)heap_base;
    td->xRegions[2].ulLengthInBytes = MDL_HEAP_SIZE;
    td->xRegions[2].ulParameters    = portMPU_REGION_READ_WRITE |
                                       portMPU_REGION_EXECUTE_NEVER |
                                       MDL_MPU_SRAM_TEX_S_C_B;
}

/*
 * Copy argv into the MDL's own arg block and queue a console event.
 *
 * The strings the console parsed live in host .bss, which an
 * unprivileged MDL cannot read at all -- handing those pointers over
 * would fault inside the MDL, far from the mistake. Refuses rather than
 * truncates: a silently shortened argument is worse than a rejected one.
 */
int mdl_post_module_command(module_t *m, int argc, const char *const *argv)
{
    if (m->cmd_entry == NULL || m->state != MDL_SLOT_RUNNING) {
        return 0;
    }
    if (argc <= 0 || argc > MDL_CMD_MAX_ARGS) {
        return 0;
    }

    char **out_argv = (char **)m->heap_stack_lo;
    char *w   = (char *)m->heap_stack_lo + (size_t)MDL_CMD_MAX_ARGS * sizeof(char *);
    char *end = (char *)m->heap_stack_lo + MDL_ARGBLOCK_SIZE;

    for (int i = 0; i < argc; i++) {
        const char *src = argv[i];
        out_argv[i] = w;
        while (*src != 0) {
            if (w >= end - 1) {
                return 0;
            }
            *w++ = *src++;
        }
        *w++ = 0;
    }

    s_cmd_argc = argc;
    s_cmd_argv = (const char *const *)out_argv;
    m->cmd_pending = 1u;
    return mdl_events_post_console() ? 1 : 0;
}

int mdl_start_module_task(module_t *m, const struct host_api *host)
{
    TaskParameters_t task_def = { 0 };
    fill_task_def(&task_def, m, host, (void *)module_task_trampoline);

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
