# MD/JS: JavaScript Worker for Atari ST

Microfirmware for the [SidecarTridge Multi-device](https://sidecartridge.com) by [Neil Rackett](https://x.com/neilrackett)

## Introduction

MD/JS turns the SidecarT into a persistent JavaScript Worker for the Atari ST.

- Upload JavaScript source from your ST program
- Call named functions with JSON arguments
- Read JSON result

## How it works

The RP2040 runs a full [JerryScript](https://jerryscript.net) ES.next runtime (48 KB heap) on Core 1, with Core 0 continuing to service the cartridge bus to avoid blocking.

```
Atari ST (68000)                         RP2040
────────────────                       ──────
mdjs_ping()           ──CMD 0x10──►  Core 0: tprotocol decode
mdjs_upload(src)      ──CMD 0x11──►       ↓  multicore FIFO
mdjs_call(f, a, r)    ──CMD 0x12──►  Core 1: JerryScript runtime
mdjs_reset()          ──CMD 0x13──►       ↓  jerry_eval / jerry_call
mdjs_call_async(f, a) ──CMD 0x14──►       ↓  result → ROM-in-RAM @ $FAF100
mdjs_poll()           ──CMD 0x15──►  Core 0: writes random token (unblocks ST)

mdjs_result(r)        ◄──────────   status byte @ $FAF008 (no bus transaction)
```

The result buffer is mapped into the ST's ROM4 address space at `$FAF100` and is directly readable with a plain `move` instruction. The async status byte lives at `$FAF008` — a zero-overhead read.

## Hardware requirements

- [SidecarTridge Multi-device](https://sidecartridge.com) (RP2040-based ROM cartridge emulator)
- Atari ST, STE, MegaST, or MegaSTE
- Raspberry Pi Debug Probe or Picoprobe for flashing/debugging (optional but recommended for development)

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

| Parameter              | Limit / address                      |
| ---------------------- | ------------------------------------ |
| JS source per upload   | Up to ~16 KB (8 chunks × 2102 bytes) |
| Function name          | 63 characters                        |
| Result / args JSON     | 2048 bytes                           |
| JerryScript heap       | 48 KB                                |
| Result buffer (ST)     | `$FAF100` (ROM4 + 0xF100)            |
| Async status byte (ST) | `$FAF008` (ROM4 + 0xF008)            |

## Repository structure

```
rp/src/           RP2040 firmware (C, Pico SDK + JerryScript)
  js_worker.c     Core 1 JerryScript worker + Core 0 dispatcher
  jerry_port.c    Minimal JerryScript port layer for RP2040
  emul.c          ROM emulator + firmware entry point

target/atarist/   Atari ST firmware and demo app
  src/main.s      ROM cartridge header and boot stub (68000 assembly)
  src/mdjs.h      ST-side C API header
  src/mdjs.c      ST-side client library (ping / upload / call / call_async / poll / reset)
  src/demo_gem.c  GEM demo app — uploads add(), calls it, shows result

lib/jerryscript/  JerryScript v3.0.0 (git submodule)
pico-sdk/         Raspberry Pi Pico SDK v2.2.0 (git submodule)
pico-extras/      Pico Extras sdk-2.2.0 (git submodule)
fatfs-sdk/        FatFS SD/SDIO driver (git submodule)
```

## Build prerequisites

### macOS

**1. ARM GNU Toolchain** (for RP2040 cross-compilation)

Download the macOS AArch64 package from the [Arm Developer website](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads). Look for the `arm-none-eabi` variant for your host (Apple Silicon: `aarch64-apple-darwin`, Intel: `x86_64-apple-darwin`). Install the `.pkg` file — it installs to `/Applications/ArmGNUToolchain/<version>/arm-none-eabi/`.

Tested with **15.2.rel1**. Version 14.x also works.

**2. atarist-toolkit-docker** (for Atari ST cross-compilation)

Install `stcmd` following the instructions at [github.com/sidecartridge/atarist-toolkit-docker](https://github.com/sidecartridge/atarist-toolkit-docker). This provides the `vasm`, `vlink`, and `m68k-atari-mint-gcc` toolchain via Docker.

**3. Other tools**

```bash
brew install cmake git python3
```

CMake 3.26 or later is required. Python 3 is used by the build script to generate the firmware header.

### Linux

Use your distribution's package manager to install `cmake`, `git`, `python3`. Download the ARM GNU Toolchain `.tar.xz` from the same Arm Developer page above and extract it somewhere permanent, e.g. `/opt/arm-gnu-toolchain/`.

## Building

**1. Clone and initialise submodules**

```bash
git clone <repo-url> md-js
cd md-js
git submodule update --init --recursive
```

**2. Run the build script**

```bash
PICO_TOOLCHAIN_PATH=/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin \
  ./build.sh pico_w debug <your-uuid>
```

Replace `<your-uuid>` with the UUID from `desc/app.json` (or generate one with `uuidgen`). Use `release` instead of `debug` to disable UART output.

Adjust `PICO_TOOLCHAIN_PATH` to match your installed version and host platform.

**3. Output**

A successful build produces the following in `dist/`:

| File                    | Description                                   |
| ----------------------- | --------------------------------------------- |
| `<UUID>-v<version>.uf2` | RP2040 firmware to flash to the SidecarTridge |
| `<UUID>.json`           | App descriptor with version and MD5           |
| `MDJSDEMO.PRG`          | GEM demo application for the Atari ST         |

## Flashing

With the SidecarTridge in BOOTSEL mode (hold BOOTSEL while plugging in USB), copy the UF2 file to the mass-storage volume that appears:

```bash
cp dist/<UUID>-v<version>.uf2 /Volumes/RPI-RP2/
```

Or use `picotool`:

```bash
picotool load dist/<UUID>-v<version>.uf2 --force
```

## Boot behaviour

On boot, the ROM cartridge header pings the RP2040. If the worker is running, the following message is printed to the ST console before the desktop loads:

```
MD/JS: JavaScript Worker is ready
```

If the SidecarTridge is not detected:

```
MD/JS: JavaScript Worker not detected
```

MD/JS then hands control back to GEM normally. It acts as a silent coprocessor — no app is launched automatically.

## Running the demo

1. Copy `MDJSDEMO.PRG` to the SD card at `/MDJS/MDJSDEMO.PRG`.
2. Boot the Atari ST with the SidecarTridge inserted.
3. Run `MDJSDEMO.PRG` from the GEM desktop.
4. A dialog appears: **MD/JS Demo — add(5,7) = 12**

## Verifying with UART (debug build)

Connect a debug probe to the SidecarTridge header (TX, RX, GND) and open a serial terminal at 115200 baud. On boot you should see:

```
MD/JS ready. PING=0x10 UPLOAD=0x11 CALL=0x12 RESET=0x13 CALL_ASYNC=0x14 POLL=0x15
Core 1: JerryScript initialized. Heap: 48 KB
```

Each command from the ST then appears as `Command ID: 0x10` etc.

## Troubleshooting

| Symptom                                        | Fix                                                                                          |
| ---------------------------------------------- | -------------------------------------------------------------------------------------------- |
| `arm-none-eabi-gcc not found`                  | Check `PICO_TOOLCHAIN_PATH` points to the `bin/` directory of your ARM GNU Toolchain install |
| Build fails at JerryScript with `gcc-ar` error | Delete `rp/build/` and rebuild — stale CMake cache from a different toolchain                |
| `stcmd` fails with "not a TTY"                 | Run with `pty=true` or use the `stcmd` wrapper script                                        |
| UF2 not found after build                      | The RP build failed — scroll up for the first compiler or linker error                       |
| ST shows "worker not detected"                 | Confirm the UF2 is flashed; check UART log for `MD/JS ready`                                 |
| JerryScript eval error in result               | The uploaded JS has a syntax error — the result buffer will contain the error string         |

## License

Source code is licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE) for the full text.
