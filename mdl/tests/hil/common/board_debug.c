#include "board_debug.h"
#include "board_buildid.h"
#include "at32f435_437.h"

/*
 * In .noinit (see the linker script) so the record survives the reset
 * that board_fault_record() performs at the end -- without that, the
 * only run that knows what went wrong is also the run that can no
 * longer tell anyone. Startup does not zero this, so `magic` is the
 * only thing that distinguishes a real record from power-on garbage;
 * never trust any other field without checking it first.
 */
volatile board_fault_record_t g_board_fault __attribute__((section(".noinit")));

void board_debug_faults_init(void)
{
    /* Take BusFault / MemManage / UsageFault in their own handlers rather
     * than letting them escalate to HardFault. Escalation is lossy: HFSR
     * only tells you FORCED=1, and by then the interesting question
     * ("which fault, at what address") is answerable only from CFSR --
     * which survives, but the dedicated handler is where you actually
     * want to be stopped.
     *
     * MEMFAULTENA is also set by arch_setup_regions() (mdl/arch/arm_cm4/
     * mpu_armv7m.c) and by FreeRTOS's prvSetupMPU(). Setting it a third
     * time here is deliberate: this runs before either of those, and a
     * fault during clock or USB init would otherwise still escalate. */
    SCB->SHCSR |= SCB_SHCSR_BUSFAULTENA_Msk |
                  SCB_SHCSR_MEMFAULTENA_Msk |
                  SCB_SHCSR_USGFAULTENA_Msk;

#if BOARD_DEBUG_PRECISE_BUS_FAULTS
    /* ACTLR.DISDEFWBUF (bit 1), Cortex-M3/M4 specific. Disables the
     * store write buffer so a faulting store is reported AT the store.
     * Note this is ACTLR (0xE000E008), not CCR -- a common mix-up,
     * because CCR bit 1 is USERSETMPEND and setting that does nothing
     * for fault precision. */
    SCnSCB->ACTLR |= (1u << 1);
#endif

    __DSB();
    __ISB();
}

/*
 * Strong override of the weak hook in mdl/arch/arm_cm4/fault_arm.c.
 *
 * MemManage is handled there, not here (that file owns it so a module
 * fault can be recovered rather than stopping the board), and its two
 * unrecoverable paths reset immediately. This is what makes those two
 * resets leave evidence: same .noinit record, same `fault` command, so a
 * host-side MemManage reads out exactly like a BusFault or UsageFault
 * does. Fills the record only -- the caller does the resetting.
 *
 * exc_return is not recoverable here: fault_arm.c's naked handler passes
 * only the frame pointer, so it is stored as all-ones to mean 'unknown'
 * rather than a plausible-looking lie.
 */
void arch_fault_persist(const char *which, uint32_t *stacked, uint32_t cfsr, uint32_t mmfar);
void arch_fault_persist(const char *which, uint32_t *stacked, uint32_t cfsr, uint32_t mmfar)
{
    g_board_fault.which      = which;
    g_board_fault.cfsr       = cfsr;
    g_board_fault.hfsr       = SCB->HFSR;
    g_board_fault.shcsr      = SCB->SHCSR;
    g_board_fault.exc_return = 0xFFFFFFFFu;
    g_board_fault.bfar       = (cfsr & SCB_CFSR_BFARVALID_Msk) ? SCB->BFAR : 0xFFFFFFFFu;
    g_board_fault.mmfar      = mmfar; /* captured by the caller pre-clear */

    if (stacked != 0) {
        g_board_fault.r0   = stacked[0];
        g_board_fault.r1   = stacked[1];
        g_board_fault.r2   = stacked[2];
        g_board_fault.r3   = stacked[3];
        g_board_fault.r12  = stacked[4];
        g_board_fault.lr   = stacked[5];
        g_board_fault.pc   = stacked[6];
        g_board_fault.xpsr = stacked[7];
    } else {
        g_board_fault.pc = 0xFFFFFFFFu;
        g_board_fault.lr = 0xFFFFFFFFu;
    }

    g_board_fault.magic = BOARD_FAULT_MAGIC;
    __DSB();
}
/* Strong override of console.c's weak default. Lives here rather than in
 * board_buildid.c so that file stays a single string and nothing else. */
const char *board_build_id(void);
const char *board_build_id(void)
{
    return mdl_build_id;
}

/* ---- post-mortem readout ------------------------------------------- */

static void fault_hex32(void (*out)(const char *), uint32_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    char buf[11];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        buf[2 + i] = digits[(v >> ((7 - i) * 4)) & 0xFu];
    }
    buf[10] = 0;
    out(buf);
}

static void fault_line(void (*out)(const char *), const char *label, uint32_t v)
{
    out(label);
    fault_hex32(out, v);
    out("\r\n");
}

void board_debug_print_fault(void (*out)(const char *))
{
    if (g_board_fault.magic != BOARD_FAULT_MAGIC) {
        out("no fault on record (clean boot)\r\n");
        return;
    }

    out("LAST RUN FAULTED -- record survived the reset\r\n");
    out("  which : ");
    out(g_board_fault.which ? g_board_fault.which : "(null)");
    out("\r\n");

    fault_line(out, "  pc    : ", g_board_fault.pc);
    fault_line(out, "  lr    : ", g_board_fault.lr);
    fault_line(out, "  xpsr  : ", g_board_fault.xpsr);
    fault_line(out, "  cfsr  : ", g_board_fault.cfsr);
    fault_line(out, "  hfsr  : ", g_board_fault.hfsr);
    fault_line(out, "  bfar  : ", g_board_fault.bfar);
    fault_line(out, "  mmfar : ", g_board_fault.mmfar);
    fault_line(out, "  excret: ", g_board_fault.exc_return);

    /* The handful of CFSR bits that actually change what you go and look
     * at next, spelled out -- decoding this by hand from the raw word is
     * exactly the step that gets got wrong at 2am. */
    uint32_t c = g_board_fault.cfsr;
    if (c & (1u << 0))  out("  MMFSR.IACCVIOL   instruction fetch from a no-execute region\r\n");
    if (c & (1u << 1))  out("  MMFSR.DACCVIOL   data access denied by the MPU (see mmfar)\r\n");
    if (c & (1u << 3))  out("  MMFSR.MUNSTKERR  fault while UNstacking on exception return\r\n");
    if (c & (1u << 4))  out("  MMFSR.MSTKERR    fault while stacking on exception entry\r\n");
    if (c & (1u << 8))  out("  BFSR.IBUSERR     instruction bus error\r\n");
    if (c & (1u << 9))  out("  BFSR.PRECISERR   precise data bus error (see bfar)\r\n");
    if (c & (1u << 10)) out("  BFSR.IMPRECISERR imprecise -- pc is NOT the culprit\r\n");
    if (c & (1u << 16)) out("  UFSR.UNDEFINSTR  undefined instruction\r\n");
    if (c & (1u << 17)) out("  UFSR.INVSTATE    branched to an address with the Thumb bit clear\r\n");
    if (c & (1u << 18)) out("  UFSR.INVPC       bad EXC_RETURN / integrity check failed\r\n");
    if (c & (1u << 24)) out("  UFSR.UNALIGNED   unaligned access\r\n");

    /* Only the BusFault/UsageFault/HardFault path knows EXC_RETURN; the
     * MemManage path (fault_arm.c) does not pass it, and inventing a
     * reading from all-ones would confidently name the wrong stack. */
    if (g_board_fault.exc_return != 0xFFFFFFFFu) {
        if (g_board_fault.exc_return & (1u << 2)) {
            out("  came from a task stack (PSP)\r\n");
        } else {
            out("  came from the main stack (MSP) -- handler or pre-scheduler\r\n");
        }
    } else {
        out("  (EXC_RETURN not captured on this path -- use xpsr IPSR instead)\r\n");
    }

    /* IPSR is the bottom 9 bits of the stacked xPSR: 0 = thread mode,
     * anything else is the exception number that was executing. */
    uint32_t ipsr = g_board_fault.xpsr & 0x1FFu;
    if (ipsr != 0u) {
        out("  faulted inside exception number ");
        fault_hex32(out, ipsr);
        out(" (14 = PendSV, i.e. a context switch)\r\n");
    }
}

/*
 * Common recorder. `stacked` points at the hardware-stacked frame:
 *   r0 r1 r2 r3 r12 lr pc xpsr
 */
static void board_fault_record(uint32_t *stacked, const char *which, uint32_t exc_return)
    __attribute__((noreturn, noinline));

static void board_fault_record(uint32_t *stacked, const char *which, uint32_t exc_return)
{
    uint32_t cfsr = SCB->CFSR;

    g_board_fault.which      = which;
    g_board_fault.cfsr       = cfsr;
    g_board_fault.hfsr       = SCB->HFSR;
    g_board_fault.shcsr      = SCB->SHCSR;
    g_board_fault.exc_return = exc_return;
    g_board_fault.bfar       = (cfsr & SCB_CFSR_BFARVALID_Msk) ? SCB->BFAR : 0xFFFFFFFFu;
    g_board_fault.mmfar      = (cfsr & SCB_CFSR_MMARVALID_Msk) ? SCB->MMFAR : 0xFFFFFFFFu;

    /* CFSR.MSTKERR / BFSR.STKERR mean exception entry's own stacking
     * failed, so `stacked` was never written and must not be read. */
    if ((cfsr & (SCB_CFSR_MSTKERR_Msk | SCB_CFSR_STKERR_Msk)) == 0u) {
        g_board_fault.r0   = stacked[0];
        g_board_fault.r1   = stacked[1];
        g_board_fault.r2   = stacked[2];
        g_board_fault.r3   = stacked[3];
        g_board_fault.r12  = stacked[4];
        g_board_fault.lr   = stacked[5];
        g_board_fault.pc   = stacked[6];
        g_board_fault.xpsr = stacked[7];
    } else {
        g_board_fault.pc = 0xFFFFFFFFu;
    }

    g_board_fault.magic = BOARD_FAULT_MAGIC;

    __DSB(); /* record is in .noinit and must be in RAM before the reset */

    /*
     * Two very different situations, and the old code did the wrong
     * thing in one of them.
     *
     * DEBUGGER ATTACHED: halt, so the whole machine state -- not just
     * the fields captured above -- is available to inspect.
     *
     * NO DEBUGGER: `bkpt` is NOT quietly ignored here. With C_DEBUGEN
     * clear it escalates, and escalating from inside a fault handler
     * puts the core in LOCKUP: the CPU stops, USB stops answering, the
     * host drops the port, and the record just written is unreachable.
     * That is precisely what made a faulting module look like "the
     * device vanished" with nothing to go on. Reset instead: the record
     * is in .noinit, so the very next boot can print it (`fault` on the
     * console). A deliberate reset also beats parking in a loop, which
     * left USB dead and required a power cycle by hand.
     */
    if (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) {
        __asm volatile("bkpt #0");
    }
    NVIC_SystemReset();

    for (;;) {
    }
}

/*
 * One C trampoline per exception. The naked asm below only recovers the
 * frame pointer and EXC_RETURN into r0/r1 and tail-calls these; the
 * exception's name is then an ordinary C string literal. Doing it this
 * way rather than materialising a string symbol inside the asm avoids
 * two real hazards: the symbol has no C-level reference, so
 * -fdata-sections + --gc-sections would drop it, and a `ldr rN, =sym`
 * literal pool inside a naked function has nowhere guaranteed to land.
 *
 * `used` because nothing in C calls these -- only the asm branch does,
 * which the compiler cannot see.
 */
__attribute__((used)) static void board_fault_c_hard(uint32_t *sp, uint32_t exc)
{
    board_fault_record(sp, "HardFault (check HFSR.FORCED: escalated?)", exc);
}

__attribute__((used)) static void board_fault_c_bus(uint32_t *sp, uint32_t exc)
{
    board_fault_record(sp, "BusFault", exc);
}

__attribute__((used)) static void board_fault_c_usage(uint32_t *sp, uint32_t exc)
{
    board_fault_record(sp, "UsageFault", exc);
}

/* Naked so the hardware-stacked frame is found from the real EXC_RETURN
 * still in lr, before any compiler prologue touches the stack.
 * EXC_RETURN bit 2: 0 = MSP was in use, 1 = PSP. */
#define BOARD_FAULT_HANDLER(name, target)          \
    __attribute__((naked)) void name(void);        \
    __attribute__((naked)) void name(void)         \
    {                                              \
        __asm volatile (                           \
            "tst   lr, #4        \n"               \
            "ite   eq            \n"               \
            "mrseq r0, msp       \n"               \
            "mrsne r0, psp       \n"               \
            "mov   r1, lr        \n"               \
            "b     " #target "   \n"               \
        );                                         \
    }

/* MemManage_Handler is NOT defined here -- mdl/arch/arm_cm4/fault_arm.c
 * owns it, because a MemManage fault inside a loaded module is a
 * recoverable event the supervisor handles, not a bring-up stop. */
BOARD_FAULT_HANDLER(HardFault_Handler,  board_fault_c_hard)
BOARD_FAULT_HANDLER(BusFault_Handler,   board_fault_c_bus)
BOARD_FAULT_HANDLER(UsageFault_Handler, board_fault_c_usage)
