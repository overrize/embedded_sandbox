#!/usr/bin/env python3
"""
console.py -- interactive terminal for the device's text console.

    python mdl/tools/console.py --port COM5

Exists because Windows ships no serial terminal, and the alternatives
(PuTTY, TeraTerm) are an extra install for something this small. It is a
dumb pipe in both directions: keystrokes go out one byte at a time, and
whatever comes back is printed. Deliberately no local echo -- the
firmware echoes every character itself (mdl/transport/console.c), so
echoing here too would double every keypress.

The same CDC port also carries the binary MDLC frames that tools/watch.py
pushes .mdl images over, and the firmware demultiplexes the two by
matching the frame magic byte by byte. But a serial port can only be open
once: run watch.py or this, not both. Ctrl+] to quit and hand the port
back.

Commands the device understands (type `help` to get this list from the
firmware itself, which is the authoritative copy):

    help              this list
    status            module slot state + last fault
    clk               system_core_clock, as configured
    arena             arena region addresses
    pins              gpio whitelist a module may touch
    led <i> <0|1>     drive whitelisted output pin i (LEDs are active-low)
    btn <i>           read whitelisted input pin i
    mem <addr> [n]    dump n words (default 8) from addr
    unload            unload the module, reclaim its resources
"""
import argparse
import sys
import threading
import time

try:
    import serial  # pyserial
except ImportError:
    print("needs pyserial:  pip install pyserial", file=sys.stderr)
    sys.exit(2)

QUIT = b"\x1d"  # Ctrl+]


def reader(ser, stop):
    """Serial -> stdout. Runs until the main thread sets `stop`."""
    while not stop.is_set():
        try:
            data = ser.read(ser.in_waiting or 1)
        except Exception:
            break
        if data:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
        else:
            time.sleep(0.01)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="e.g. COM5 or /dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200,
                    help="ignored by the device (CDC baud is virtual), "
                         "but pyserial insists on a number")
    args = ap.parse_args(argv)

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.1)
    except Exception as e:
        print(f"could not open {args.port}: {e}", file=sys.stderr)
        print("if tools/watch.py is running, stop it first -- a serial port "
              "can only be open once.", file=sys.stderr)
        return 1

    print(f"connected to {args.port}.  Ctrl+] to quit.\n")

    stop = threading.Event()
    t = threading.Thread(target=reader, args=(ser, stop), daemon=True)
    t.start()

    # No nudge byte is sent here on purpose. Opening the port makes the
    # host emit CDC SET_LINE_CODING, which the firmware uses as its
    # "a terminal connected" signal and answers with the banner (see
    # usb_cdc_take_host_open_event()). Sending a newline as well only
    # raced that banner and printed stray prompts ahead of it.

    try:
        if sys.platform == "win32":
            import msvcrt
            while True:
                if msvcrt.kbhit():
                    ch = msvcrt.getch()
                    if ch == QUIT:
                        break
                    # Windows reports Enter as \r; the firmware accepts
                    # either, but send \r for consistency with terminals.
                    ser.write(ch)
                else:
                    time.sleep(0.01)
        else:
            import termios, tty
            fd = sys.stdin.fileno()
            old = termios.tcgetattr(fd)
            try:
                tty.setraw(fd)
                while True:
                    ch = sys.stdin.buffer.read(1)
                    if not ch or ch == QUIT:
                        break
                    ser.write(ch)
            finally:
                termios.tcsetattr(fd, termios.TCSADRAIN, old)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        ser.close()
        print("\ndisconnected.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
