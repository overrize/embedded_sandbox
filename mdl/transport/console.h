#ifndef MDL_TRANSPORT_CONSOLE_H
#define MDL_TRANSPORT_CONSOLE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Human-facing text console, sharing the one USB CDC pipe with the
 * binary MDLC frame protocol (mdl/transport/protocol.h).
 *
 * WHY BOTH ON ONE PIPE
 * The board exposes exactly one CDC data interface. tools/watch.py needs
 * it to push binary .mdl images; a person with PuTTY / Windows Terminal
 * needs it to see what the firmware is doing and poke at it. Rather than
 * pick one, mdl_proto_rx_byte() acts as the demultiplexer: bytes that
 * begin the 4-byte MDLC magic are held and, if the whole magic arrives,
 * handed to the frame parser; anything else falls through to this file's
 * line editor. A binary frame and a typed command can therefore never be
 * confused for one another, and neither side needed a mode switch.
 *
 * ONE PRACTICAL CONSEQUENCE: a typed line whose bytes happen to spell
 * "MDLC" (capitals) enters frame-parsing mode and is not seen as text.
 * All console commands are lowercase, so this only bites someone
 * deliberately typing the magic.
 *
 * THREADING
 * mdl_console_rx_byte() runs in the USB RX task (see usb_cdc.c). It only
 * accumulates and echoes. A completed line is handed to the supervisor
 * task the same way a completed frame is -- flag + task notification --
 * so every command actually EXECUTES in supervisor context, single
 * threaded with load/unload/fault recovery. Nothing here mutates module
 * state from the USB task.
 */

#define MDL_CONSOLE_LINE_MAX 96

/* Called once alongside mdl_proto_set_supervisor_handle(), with the same
 * TaskHandle_t as a bare void* (this header stays FreeRTOS-type-free for
 * the same reason protocol.h does). */
void mdl_console_set_supervisor_handle(void *supervisor_task_handle);

/* Called per received byte from mdl_proto_rx_byte()'s text fall-through.
 * USB RX task context. Echoes, handles backspace, and on CR/LF marks a
 * line ready and notifies the supervisor. */
void mdl_console_rx_byte(uint8_t b);

/*
 * Supervisor-task context only. Returns true and consumes the pending
 * line if one is ready. The returned pointer is into an internal static
 * buffer, valid until the next completed line -- act on it before
 * returning to the wait loop, exactly like mdl_proto_take_frame().
 */
bool mdl_console_take_line(char **out_line);

/* Supervisor-task context only: run one command line and write its
 * output, then print a fresh prompt. */
void mdl_console_execute(char *line);

/* Prints the banner + first prompt. Call once from the supervisor task
 * at startup, after mdl_supervisor_init(). */
void mdl_console_greet(void);

/* Write a NUL-terminated string out over the transport. Safe from any
 * task (mdl_transport_write() serialises internally). */
void mdl_console_puts(const char *s);

/* Decimal, for host code that needs to report a number in an error --
 * there is no printf on this target and every caller writing its own
 * digit loop is how they end up subtly different. */
void console_put_u32(uint32_t v);

#endif /* MDL_TRANSPORT_CONSOLE_H */
