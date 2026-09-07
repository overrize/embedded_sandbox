# Test MDLs

Built and packed by `mdl/tools/watch.py <dir> --port COMx`, or by hand with
the flags in that file. Every one of them is packed for the CURRENT
`HOST_API_ABI_VERSION`; the loader refuses a mismatch, so after an ABI bump
they all have to be repacked.

## Positive

| MDL | What it demonstrates |
|---|---|
| `hello` | GOT relocation and a mutated global (`g_counter == 42`) |
| `sw3_blue` / `sw4_green` / `sw3_blink` | polling; the same button doing different things across MDLs, with no reflash |
| `sw3_irq` | the same behaviour as `sw3_blue` via interrupts -- `module_init` RETURNS and the MDL stays resident |
| `sw3_slow` | sleeps 400ms per event on purpose, so events outrun the handler and the coalescing path is exercised |
| `blink` | exports a console command (`blink [n]`) |
| `i2c_ok` | peripheral claims that do not conflict (I2C1 + TMR3 + LEDG) |
| `long_sleep` | sleeping is not hanging: one 6s sleep, then 5s of work with explicit `watchdog_feed()` |
| `pin_residue` | leaves an LED on, so `unload` returning the pin is observable |

## Negative -- these are SUPPOSED to be refused

The packer refuses the three resource ones now (B2), which is the point:
failing a build beats failing a deploy. They are therefore packed with
`--allow-conflicts`, so the DEVICE-side check stays exercised too -- that
check is the last line of defence for an image that did not come through
this packer, and an untested last line of defence is not one.

| MDL | Refused because |
|---|---|
| `conflict` | claims GPIO 4 = PA9, the DAP debug UART |
| `uart1_clash` | claims USART1 = PA9/PA10, same pins, reached by naming a peripheral instead |
| `self_clash` | claims GPIO(2) and USART2 -- different names, same PA3 |
| `fault_*` (6) | M3's fault-recovery suite: null deref, host RAM write, jump to host, peripheral read, stack overflow, infinite loop |

`fault_infinite_loop` is also the watchdog's negative test: it makes no host
call at all, so it must still be killed under the ABI v3 rules where an
ordinary host call no longer counts as a sign of life.
