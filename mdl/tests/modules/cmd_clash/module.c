/*
 * S4 negative case: two resident MDLs, one command name.
 *
 * This module exists to be REFUSED. It declares the command name
 * `uart_echo` already advertises, and deliberately claims nothing else --
 * no pins, no bus, no ADC channel. That isolation is the whole point: if
 * it also wanted PB10, the loader would refuse it for the pin and the
 * command-name check would never run, so a pass would prove nothing.
 *
 * Expected, with uart_echo resident:
 *
 *   -> RESP_ERROR  command name "uart" already answered by uart_echo
 *
 * Why this is worth a module of its own. S4's refusal path has been
 * written, compiled and reasoned about since 2026-09-14, and has never
 * executed on hardware, because none of the three test modules on the
 * board share a cmd_name. It has sat on the claim board marked 完成
 * rather than 已验证 for exactly that reason -- this project's own rule is
 * that compiling is not finishing.
 *
 * The bug it guards against is silent, which is what makes it worth
 * testing rather than assuming. find_slot_by_command() returns the FIRST
 * match, so before S4 both loads succeeded and typing the command was a
 * coin flip that reported nothing unusual either way.
 *
 * Note the check lives in the supervisor, not the loader: a command name
 * is not a resource in the res[] sense. Nothing physical collides -- what
 * collides is the console's ability to say which module it means.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();
MDL_MODULE_COMMAND("uart");          /* the same name uart_echo answers to */
MDL_MODULE_RESOURCES();              /* nothing, so the refusal has one cause */

int module_init(const host_api_t *host)
{
    /* Reached only if the refusal did not happen. Say so plainly: a test
     * whose failure looks like success is worse than no test, and this
     * module's whole job is to not run. */
    host->log("cmd_clash: loaded, so the duplicate-name check did NOT run");
    return 0;
}

int module_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return -1;
}
