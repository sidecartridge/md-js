# MD/JS: JavaScript Worker for Atari ST

Microfirmware for the [SidecarTridge Multi-device](https://sidecartridge.com) by [Neil Rackett](https://x.com/neilrackett)

## Introduction

MD/JS turns your SidecarT into a persistent JavaScript Worker for the Atari ST, enabling you to:

- Upload JavaScript source from your ST
- Call named functions with JSON arguments
- Read JSON results

To see this in action, simply install the microfirmware, open the cartridge icon on your ST's desktop (lower case `c` drive) and run `MDJSDEMO.PRG`.

In addition, the MD/JS Code example is [available to download](https://github.com/neilrackett/md-js/releases), showing a more developed GEM client.

Instructions for integrating MD/JS into your own ST apps are below.

## What's next?

- [ ] Make MD/JS Code editable
- [ ] Add text-only HTTP fetch implementation

## Hardware requirements

- [SidecarTridge Multi-device](https://sidecartridge.com) (RP2040-based ROM cartridge emulator)
- Atari ST, STE, MegaST, or MegaSTE
- Raspberry Pi Debug Probe or Picoprobe for flashing/debugging (optional but recommended for development)

## Installation

1. Download the latest files from the [releases page](https://github.com/neilrackett/md-js/releases).
2. Copy the `.uf2` and `.json` files to the `/apps` folder of your SidecarT's microSD card.
3. On the Booster screen, press ESC for the app list and select the MD/JS app.
4. To return to Booster, turn on your ST while holding the SELECT button on your SidecarT.

You'll know if the microfirmware is working because you'll see "MD/JS: JavaScript Worker is ready" on your ST's screen as it boots.

## How it works

The RP2040 runs a full [JerryScript](https://jerryscript.net) ES.next runtime (48 KB heap) on Core 1, with Core 0 continuing to service the cartridge bus to avoid blocking.

```
Atari ST (68000)                     RP2040
────────────────                     ──────
mdjs_ping()           ──CMD 0x10──►  Core 0: tprotocol decode
mdjs_upload(src)      ──CMD 0x11──►       ↓  multicore FIFO
mdjs_call(f, a, r)    ──CMD 0x12──►  Core 1: JerryScript runtime
mdjs_reset()          ──CMD 0x13──►       ↓  jerry_parse / jerry_run / jerry_call
mdjs_call_async(f, a) ──CMD 0x14──►       ↓  result → ROM-in-RAM @ $FAF100
mdjs_poll()           ──CMD 0x15──►  Core 0: writes random token (unblocks ST)

mdjs_result(r)        ◄────────────  Status byte @ $FAF008 (no bus transaction)
```

The result buffer is mapped into the ST's ROM4 address space at `$FAF100` and is directly readable with `move.b` instructions — the RP2040 pre-swaps bytes so character reads come out correctly. Use the C API (`mdjs_result()`) if you'd rather not handle that yourself. The async status byte lives at `$FAF008` — a zero-overhead read.

## ST-side API

Include `mdjs.h` and link against `mdjs.c` and `sidecart_stubs.S` in your ST project.

```c
#include "mdjs.h"

/* Check the worker is present */
if (mdjs_ping() != 0) {
  /* No SidecarTridge / worker not running */
}

/* Upload JavaScript source (evaluated immediately) */
mdjs_upload("function greet(name) { return 'Hello, ' + name + '!'; }");

/* Call a function — args as a JSON array, result as a JSON value */
char result[256];
mdjs_call("greet", "[\"World\"]", result, sizeof(result));
/* result == "\"Hello, World!\"" */

/* Clear the JS context and start fresh */
mdjs_reset();
```

All functions return `0` on success, non-zero on timeout or error. Results are NUL-terminated strings in the caller-supplied buffer. `mdjs_upload` handles chunking automatically — just pass the full source string.

### Async calls

`mdjs_call` blocks the 68000 until the RP2040 finishes executing the JavaScript. For long-running functions you can use the non-blocking variant instead:

```c
/* Submit the call and return immediately */
int err = mdjs_call_async("heavyCalc", "[1000]");
if (err != 0) { /* busy or protocol error */ }

/* Do other work while JS runs on Core 1 */
while (mdjs_status() == MDJS_STATUS_BUSY) {
    do_other_work();
}

if (mdjs_status() == MDJS_STATUS_DONE) {
    char result[256];
    mdjs_result(result, sizeof(result));
    /* result now contains the JSON return value */
}
```

`mdjs_status()` is a zero-overhead single byte read from `MDJS_STATUS_ADDR` (`$FAF008`) — no bus transaction. Only one async call can be in flight at a time; submitting a second returns `MDJS_STATUS_BUSY` immediately.

## API limits

| Parameter              | Limit / address                              |
| ---------------------- | -------------------------------------------- |
| JS source per upload   | Up to ~16 KB (8 chunks × 2096 bytes)         |
| Function name          | 63 characters                                |
| Result JSON            | 2048 bytes                                   |
| Args JSON              | 2031 bytes (max, with 63-char function name) |
| JerryScript heap       | 48 KB                                        |
| Result buffer (ST)     | `$FAF100` (ROM4 + 0xF100)                    |
| Async status byte (ST) | `$FAF008` (ROM4 + 0xF008)                    |

## Building

To build or monitor the microfirmware, the following `make` targets are available:

```bash
# Production build
make

# Debug build
make debug

# Monitor debug build over UART
make uart
```

If you'd like more information about coding for the SidecarT, [the docs are here](https://docs.sidecartridge.com/sidecartridge-multidevice/programming/).

## License

Source code is licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE) for the full text.
