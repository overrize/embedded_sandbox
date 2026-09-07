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
    g_mdl_slot.state       = MDL_SLOT_LOADED;
    g_mdl_slot.task_handle = NULL;
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

static void module_task_trampoline(void *pvParameters)
{
    /* pvParameters is &g_host_api, which lives in flash -- readable by an
     * unprivileged task through the port's own unprivileged-flash region,
     * so this one really can be dereferenced directly. */
    const void *host = pvParameters;

    module_entry_t me;
    module_task_read_entry(&me);

    /* arch_call_privileged()'s name is a holdover from M1, where the
     * caller (the loader, running from main()) really was privileged.
     * The mechanism itself -- load r9, blx, restore r9 -- doesn't touch
     * CONTROL or care what privilege level it runs at, so it's exactly
     * as correct called from this now-UNPRIVILEGED task. Not renamed to
     * avoid churning M1's already-verified arch_if.h contract for a
     * cosmetic reason. */
    (void)arch_call_privileged(me.entry, me.got_base, host);

    /* module_init() returned -- v1's module lifecycle ends here (no
     * "long-running task body" support yet; a module that wants to keep
     * doing work after init would need module_init() to itself loop,
     * which host_api.h's delay_ms() documents as fine to call from the
     * task body). */
    module_task_mark_finished();

    /* vTaskDelete() resolves to MPU_vTaskDelete() here (mpu_wrappers.h
     * remaps it for any file that isn't the kernel itself), so this is a
     * legal SVC from unprivileged code -- unlike the direct g_mdl_slot
     * writes above, it needs no gate of its own. */
    vTaskDelete(NULL);
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

int mdl_start_module_cmd_task(module_t *m, const struct host_api *host,
                               int argc, const char *const *argv)
{
    if (m->cmd_entry == NULL || m->state != MDL_SLOT_LOADED) {
        return 0;
    }
    if (argc <= 0 || argc > MDL_CMD_MAX_ARGS) {
        return 0;
    }

    /* Marshal argv into the module's own arg block. The strings the
     * console parsed live in host .bss, which the module cannot read at
     * all -- handing those pointers over would fault inside the module
     * on first use, far from the actual mistake. */
    char **out_argv = (char **)m->heap_stack_lo;
    char *w   = (char *)m->heap_stack_lo + (size_t)MDL_CMD_MAX_ARGS * sizeof(char *);
    char *end = (char *)m->heap_stack_lo + MDL_ARGBLOCK_SIZE;

    for (int i = 0; i < argc; i++) {
        const char *src = argv[i];
        out_argv[i] = w;
        while (*src != '\0') {
            if (w >= end - 1) {
                return 0; /* arguments do not fit -- refuse rather than truncate */
            }
            *w++ = *src++;
        }
        *w++ = '\0';
    }

    TaskParameters_t task_def = { 0 };
    fill_task_def(&task_def, m, host, (void *)module_cmd_trampoline);

    s_cmd_argc = argc;
    s_cmd_argv = (const char *const *)out_argv;

    TaskHandle_t created = NULL;
    if (xTaskCreateRestricted(&task_def, &created) != pdPASS) {
        return 0;
    }

    m->task_handle      = created;
    m->last_active_tick = (uint32_t)xTaskGetTickCount();
    m->state            = MDL_SLOT_RUNNING;
    return 1;
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
