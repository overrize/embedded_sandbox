#include "console.h"
#include "protocol.h"
#include "registry.h"
#include "host_api.h"
#include "supervisor.h"
#include "module_task.h"
#include "at32f435_437.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

/* ---- line assembly (USB RX task context) ---------------------------- */

static char     s_line[MDL_CONSOLE_LINE_MAX + 1];
static uint32_t s_line_pos;

static char     s_ready_line[MDL_CONSOLE_LINE_MAX + 1];
static volatile bool s_line_ready;

static TaskHandle_t s_supervisor_handle;

void mdl_console_set_supervisor_handle(void *h);
void mdl_console_set_supervisor_handle(void *h)
{
    s_supervisor_handle = (TaskHandle_t)h;
}

void mdl_console_puts(const char *s)
{
    mdl_transport_write((const uint8_t *)s, (uint32_t)strlen(s));
}

void mdl_console_rx_byte(uint8_t b)
{
    /*
     * Treat CR, LF and CRLF as exactly one line terminator.
     *
     * Without the CRLF case, a terminal that sends both bytes gets two
     * prompts for every Enter -- CR ends the line, then LF is seen as a
     * second (empty) line. It shows up immediately with tools/console.py
     * and with anything else configured to send CRLF; terminals that
     * send a bare CR never reveal it. Swallowing an LF only when it
     * directly follows a CR keeps a lone LF (Unix-style senders) working
     * as a terminator in its own right.
     */
    static bool s_last_was_cr;
    bool was_cr = s_last_was_cr;
    s_last_was_cr = (b == '\r');

    if (b == '\n' && was_cr) {
        return;
    }

    if (b == '\r' || b == '\n') {
        if (s_line_pos == 0) {
            mdl_console_puts("\r\nmdl> ");
            return;
        }
        s_line[s_line_pos] = '\0';
        /* Drop the line if the supervisor hasn't picked up the previous
         * one yet, rather than overwriting a line it is about to read.
         * Typing faster than the supervisor drains is not a real
         * scenario for a human; a script driving the console should use
         * the binary protocol instead. */
        if (!s_line_ready) {
            memcpy(s_ready_line, s_line, s_line_pos + 1);
            s_line_ready = true;
            if (s_supervisor_handle != NULL) {
                xTaskNotifyGive(s_supervisor_handle);
            }
        }
        s_line_pos = 0;
        mdl_console_puts("\r\n");
        return;
    }

    if (b == '\b' || b == 0x7F) { /* backspace / DEL */
        if (s_line_pos > 0) {
            s_line_pos--;
            mdl_console_puts("\b \b");
        }
        return;
    }

    if (b < 0x20 || b > 0x7E) {
        return; /* ignore other control bytes */
    }

    if (s_line_pos < MDL_CONSOLE_LINE_MAX) {
        s_line[s_line_pos++] = (char)b;
        /* Echo, so a terminal without local echo still shows typing.
         * mdl_transport_write() is mutex-protected (usb_cdc.c), which is
         * what makes echoing from this task safe while the supervisor
         * task may be writing a response. */
        uint8_t one = b;
        mdl_transport_write(&one, 1);
    }
}

bool mdl_console_take_line(char **out_line)
{
    if (!s_line_ready) {
        return false;
    }
    *out_line = s_ready_line;
    s_line_ready = false;
    return true;
}

/* ---- tiny formatting helpers ----------------------------------------
 * No printf: newlib's printf pulls in ~10KB and a malloc-backed FILE
 * buffer, for a console whose entire output vocabulary is decimal
 * integers, hex words, and fixed strings. */

static void put_u32(uint32_t v)
{
    char buf[11];
    int i = 10;
    buf[10] = '\0';
    if (v == 0) {
        mdl_console_puts("0");
        return;
    }
    while (v != 0 && i > 0) {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    mdl_console_puts(&buf[i]);
}

static void put_hex32(uint32_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    char buf[11];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        buf[2 + i] = digits[(v >> (28 - 4 * i)) & 0xFu];
    }
    buf[10] = '\0';
    mdl_console_puts(buf);
}

/* ---- argument parsing ------------------------------------------------ */

/* Splits `line` in place into up to max_argv tokens on runs of spaces.
 * Returns the token count. */
static int split_args(char *line, char **argv, int max_argv)
{
    int argc = 0;
    char *p = line;
    while (*p != '\0' && argc < max_argv) {
        while (*p == ' ') {
            *p++ = '\0';
        }
        if (*p == '\0') {
            break;
        }
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ') {
            p++;
        }
    }
    return argc;
}

/* Parses decimal, or hex when prefixed 0x. Returns false on any
 * non-digit, so a typo reports an error instead of silently reading
 * address 0. */
static bool parse_u32(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    if (s[0] == '\0') {
        return false;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        if (*s == '\0') {
            return false;
        }
        for (; *s != '\0'; s++) {
            char c = *s;
            uint32_t d;
            if (c >= '0' && c <= '9')      d = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
            else return false;
            v = (v << 4) | d;
        }
    } else {
        for (; *s != '\0'; s++) {
            if (*s < '0' || *s > '9') {
                return false;
            }
            v = v * 10u + (uint32_t)(*s - '0');
        }
    }
    *out = v;
    return true;
}

/* ---- commands -------------------------------------------------------- */

static const char *slot_state_str(mdl_slot_state_t s)
{
    switch (s) {
    case MDL_SLOT_EMPTY:   return "EMPTY";
    case MDL_SLOT_LOADED:  return "LOADED (ran, idle)";
    case MDL_SLOT_RUNNING: return "RUNNING";
    case MDL_SLOT_FAULTED: return "FAULTED";
    }
    return "?";
}

static void cmd_help(void)
{
    mdl_console_puts(
        "commands:\r\n"
        "  help              this list\r\n"
        "  status            which MDL is loaded, and the last fault\r\n"
        "  clk               system_core_clock, as configured\r\n"
        "  arena             arena region addresses\r\n"
        "  pins              gpio pins, and who owns each one\r\n"
        "  led <i> <0|1>     drive whitelisted output pin i (LEDs are active-low)\r\n"
        "  btn <i>           read whitelisted input pin i\r\n"
        "  mem <addr> [n]    dump n words (default 8) from addr\r\n"
        "  fault             fault record left by the previous run\r\n"
        "  ver               firmware build time + ABI version\r\n"
        "  unload            unload the MDL, reclaim its resources\r\n");

    /* A loaded module's own command is listed right next to the
     * built-ins, because from where the user sits there is no
     * difference: it is a command the device answers to now and did not
     * answer to a moment ago. That difference IS the feature. */
    if (g_mdl_slot.state != MDL_SLOT_EMPTY && g_mdl_slot.cmd_name[0] != 0) {
        mdl_console_puts("  ");
        mdl_console_puts(g_mdl_slot.cmd_name);
        mdl_console_puts("            <- from the loaded MDL\r\n");
    }

    mdl_console_puts(
        "\r\n"
        "MDLs are pushed over this same port as binary frames by\r\n"
        "tools/watch.py -- typing here does not interfere with that.\r\n");
}

/*
 * Supplied by the board layer (mdl/tests/hil/common/board_debug.c),
 * which is where the .noinit fault record and its decoder live. Weak
 * default so a build without a board layer -- M3, or any future host
 * that has a console but no AT32 board files -- still links, exactly
 * like mdl_transport_write() in protocol.c.
 */
__attribute__((weak)) void board_debug_print_fault(void (*out)(const char *))
{
    out("no fault reporter in this build\r\n");
}

/*
 * Strong override of host_api.c's weak host_log_sink(): a module's
 * log() output belongs on the same console a person is already looking
 * at, not down a semihosting channel that requires a debugger to exist.
 *
 * Runs in the module task, privilege already raised by host_log()'s
 * syscall gate. mdl_transport_write() is mutex-guarded, so sharing the
 * port with the supervisor's own output is safe; blocking on that mutex
 * here is fine because the module task is the lowest-priority thing in
 * the system anyway.
 */
/* Declared here rather than in host_api.h on purpose: that header is the
 * MODULE-facing ABI, and this is a host-internal seam. Same reason
 * usb_cdc.c declares board_usb_clock_init() locally. */
void host_log_sink(const char *msg);
void host_log_sink(const char *msg)
{
    mdl_console_puts("[mdl] ");
    mdl_console_puts(msg);
    mdl_console_puts("\r\n");
}

static void cmd_fault(void)
{
    /* Reads the PREVIOUS run: a fault resets the board on purpose now
     * (board_debug.c), and the record lives in .noinit so it crosses
     * that reset. So the thing to do after a module kills the device is
     * reconnect and type this. */
    board_debug_print_fault(mdl_console_puts);
}

/*
 * Supplied by the board layer when one is linked. Weak default so a
 * build without board files still links -- same pattern as
 * board_debug_print_fault() above.
 */
__attribute__((weak)) const char *board_build_id(void)
{
    return "(no board layer)";
}

__attribute__((weak)) int board_write_buffer_disabled(void)
{
    return 0;
}

static void cmd_ver(void)
{
    /* The point of this command: compare it against what build.ps1
     * printed. If they differ, the download did not take -- Ozone
     * caches the ELF from project load and will happily re-flash that
     * cached copy after a rebuild. */
    mdl_console_puts("build  : ");
    mdl_console_puts(board_build_id());
    mdl_console_puts("\r\nabi    : v");
    put_u32((uint32_t)HOST_API_ABI_VERSION);
    mdl_console_puts("   (an MDL packed for a different ABI is refused)\r\n");

    /* Reported because a benchmark taken on a bring-up build would be
     * silently wrong -- slower, i.e. wrong in our own favour. */
    mdl_console_puts("wbuf   : ");
    mdl_console_puts(board_write_buffer_disabled()
                      ? "DISABLED (bring-up; every store is slower -- do not benchmark)"
                      : "enabled (normal speed)");
    mdl_console_puts("\r\n");
}

static void cmd_status(void)
{
    mdl_console_puts("slot   : ");
    mdl_console_puts(slot_state_str(g_mdl_slot.state));
    if (g_mdl_slot.state != MDL_SLOT_EMPTY && g_mdl_slot.name[0] != 0) {
        mdl_console_puts("   ");
        mdl_console_puts(g_mdl_slot.name);
    }
    mdl_console_puts("\r\nentry  : ");
    put_hex32((uint32_t)(uintptr_t)g_mdl_slot.entry);

    mdl_console_puts("\r\nfault  : ");
    if (!g_mdl_last_fault.occurred) {
        mdl_console_puts("none on record");
    } else {
        mdl_console_puts("pc=");
        put_hex32(g_mdl_last_fault.pc);
        mdl_console_puts(" cfsr=");
        put_hex32(g_mdl_last_fault.cfsr);
        mdl_console_puts(" mmfar=");
        put_hex32(g_mdl_last_fault.mmfar);
        mdl_console_puts("\r\n         module text offset: ");
        if (g_mdl_last_fault.text_offset == 0xFFFFFFFFu) {
            mdl_console_puts("n/a (fault was outside module text)");
        } else {
            put_hex32(g_mdl_last_fault.text_offset);
            mdl_console_puts("  <- addr2line this into the module's own .so");
        }
    }
    mdl_console_puts("\r\n");
}

static void cmd_clk(void)
{
    mdl_console_puts("system_core_clock = ");
    put_u32(system_core_clock);
    mdl_console_puts(" Hz\r\n");
    /* 288000000 means board_clock_init() ran and the 24MHz crystal is
     * live. 48000000 means the part is still on the HICK reset default,
     * i.e. board_clock_init() was never called -- USB will not work. */
    if (system_core_clock == 288000000u) {
        mdl_console_puts("  (PLL from 24MHz HEXT: 24/3*144/4 -- at spec maximum)\r\n");
    } else if (system_core_clock == 48000000u) {
        mdl_console_puts("  (HICK reset default -- board_clock_init() did NOT run)\r\n");
    }
    return;
}

static void cmd_arena(void)
{
    mdl_console_puts("text : ");
    put_hex32((uint32_t)g_mdl_slot.text_lo);
    mdl_console_puts(" .. ");
    put_hex32((uint32_t)g_mdl_slot.text_hi);
    mdl_console_puts("\r\ndata : ");
    put_hex32((uint32_t)g_mdl_slot.data_lo);
    mdl_console_puts(" .. ");
    put_hex32((uint32_t)g_mdl_slot.data_hi);
    mdl_console_puts("\r\nheap : ");
    put_hex32((uint32_t)g_mdl_slot.heap_stack_lo);
    mdl_console_puts(" .. ");
    put_hex32((uint32_t)g_mdl_slot.heap_stack_hi);
    mdl_console_puts("\r\nguard: ");
    put_hex32((uint32_t)g_mdl_slot.guard_lo);
    mdl_console_puts(" .. ");
    put_hex32((uint32_t)g_mdl_slot.guard_hi);
    mdl_console_puts("  (no access, any privilege)\r\n");
}

static void cmd_pins(void)
{
    for (int i = 0; ; i++) {
        const char *name = host_gpio_name(i);
        if (name == NULL) {
            break;
        }
        mdl_console_puts("  ");
        put_u32((uint32_t)i);
        mdl_console_puts("  ");
        mdl_console_puts(name);

        /* Who has it. A module may claim only a pin with no host owner,
         * and may touch only a pin it declared -- so this column is the
         * answer to 'why was my module rejected'. */
        /* Order matters: the CLAIM is the current truth, the host's own
         * entry is only what would happen without one. Checking the host
         * first reported that the host owned LEDB at a moment when it had
         * already stopped driving it -- an owner column that contradicts
         * the hardware is worse than no column. */
        const char *owner = host_gpio_host_owner(i);
        bool claimed = (g_mdl_slot.state != MDL_SLOT_EMPTY) &&
                        ((g_mdl_slot.gpio_claimed & (1u << (unsigned)i)) != 0u);
        if (claimed) {
            mdl_console_puts("  [MDL");
            if (owner != NULL) {
                mdl_console_puts(", host yielded");
            }
            mdl_console_puts("]");
        } else if (owner != NULL) {
            mdl_console_puts("  [");
            mdl_console_puts(owner);
            mdl_console_puts("]");
        } else {
            mdl_console_puts("  [free]");
        }
        mdl_console_puts("\r\n");
    }
}

static void cmd_led(int argc, char **argv)
{
    uint32_t idx, level;
    if (argc < 3 || !parse_u32(argv[1], &idx) || !parse_u32(argv[2], &level)) {
        mdl_console_puts("usage: led <index> <0|1>\r\n");
        return;
    }
    if (host_gpio_direct_set((int)idx, (int)level) != 0) {
        mdl_console_puts("not a whitelisted pin (see `pins`)\r\n");
        return;
    }
    mdl_console_puts("ok\r\n");
}

static void cmd_btn(int argc, char **argv)
{
    uint32_t idx;
    if (argc < 2 || !parse_u32(argv[1], &idx)) {
        mdl_console_puts("usage: btn <index>\r\n");
        return;
    }
    int v = host_gpio_direct_get((int)idx);
    if (v < 0) {
        mdl_console_puts("not a whitelisted pin (see `pins`)\r\n");
        return;
    }
    put_u32((uint32_t)v);
    mdl_console_puts((v == 0) ? "  (pressed)\r\n" : "  (idle)\r\n");
}

static void cmd_mem(int argc, char **argv)
{
    uint32_t addr, count = 8;
    if (argc < 2 || !parse_u32(argv[1], &addr)) {
        mdl_console_puts("usage: mem <addr> [words]\r\n");
        return;
    }
    if (argc >= 3 && !parse_u32(argv[2], &count)) {
        mdl_console_puts("usage: mem <addr> [words]\r\n");
        return;
    }
    if (count == 0 || count > 64) {
        count = 8;
    }
    addr &= ~3u; /* word-align: an unaligned 32-bit read would fault */

    for (uint32_t i = 0; i < count; i++) {
        if ((i % 4u) == 0u) {
            if (i != 0) {
                mdl_console_puts("\r\n");
            }
            put_hex32(addr + i * 4u);
            mdl_console_puts(":");
        }
        mdl_console_puts(" ");
        put_hex32(*(volatile uint32_t *)(uintptr_t)(addr + i * 4u));
    }
    mdl_console_puts("\r\n");
}

void mdl_console_execute(char *line)
{
    char *argv[4];
    int argc = split_args(line, argv, 4);

    if (argc == 0) {
        mdl_console_puts("mdl> ");
        return;
    }

    if      (strcmp(argv[0], "help")   == 0) cmd_help();
    else if (strcmp(argv[0], "status") == 0) cmd_status();
    else if (strcmp(argv[0], "clk")    == 0) cmd_clk();
    else if (strcmp(argv[0], "arena")  == 0) cmd_arena();
    else if (strcmp(argv[0], "pins")   == 0) cmd_pins();
    else if (strcmp(argv[0], "led")    == 0) cmd_led(argc, argv);
    else if (strcmp(argv[0], "btn")    == 0) cmd_btn(argc, argv);
    else if (strcmp(argv[0], "mem")    == 0) cmd_mem(argc, argv);
    else if (strcmp(argv[0], "fault")  == 0) cmd_fault();
    else if (strcmp(argv[0], "ver")    == 0) cmd_ver();
    else if (strcmp(argv[0], "unload") == 0) {
        /* Reuses the binary protocol's own unload path rather than
         * duplicating reclaim logic: same code, same single-threaded
         * supervisor context, so a console unload and a watch.py unload
         * cannot diverge in behaviour. */
        mdl_supervisor_request_unload();
        mdl_console_puts("unload requested\r\n");
    } else if (g_mdl_slot.state != MDL_SLOT_EMPTY &&
                g_mdl_slot.cmd_name[0] != 0 &&
                strcmp(argv[0], g_mdl_slot.cmd_name) == 0) {
        /* Straight to the supervisor, which restarts the module's own
         * UNPRIVILEGED task at module_cmd() and waits for it. Calling
         * the module from here would run it at this task's privilege
         * and the sandbox would be worth nothing. */
        int ret = 0;
        mdl_cmd_result_t r = mdl_supervisor_run_module_command(argc, argv, &ret);
        switch (r) {
        case MDL_CMD_OK:
            mdl_console_puts("MDL returned ");
            put_u32((uint32_t)ret);
            mdl_console_puts("\r\n");
            break;
        case MDL_CMD_TIMEOUT:
            mdl_console_puts("MDL did not finish in time -- unloaded\r\n");
            break;
        case MDL_CMD_FAULTED:
            mdl_console_puts("MDL faulted -- unloaded (see `status`)\r\n");
            break;
        case MDL_CMD_START_FAILED:
            mdl_console_puts("could not start it (arguments too long?)\r\n");
            break;
        default:
            mdl_console_puts("MDL refused the call\r\n");
            break;
        }
    } else {
        mdl_console_puts("unknown command: ");
        mdl_console_puts(argv[0]);
        mdl_console_puts("  (try `help`)\r\n");
    }

    mdl_console_puts("mdl> ");
}

void mdl_console_greet(void)
{
    mdl_console_puts(
        "\r\n"
        "=== mdl console (AT32F435VCT7 / UYUP-RPI-A-2.4) ===\r\n"
        "type `help`. MDL pushes share this same port.\r\n"
        "mdl> ");
}
