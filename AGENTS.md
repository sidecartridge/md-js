# AGENTS.md — MD/JS Developer & LLM Playbook

Quick reference for humans and LLMs working on this repo. Read this before touching anything.

## What this project is

MD/JS is a microfirmware app for the SidecarTridge Multi-device (RP2040-based ROM cartridge for Atari ST). It exposes a JavaScript Worker: the ST uploads JS source via the cartridge bus, calls named functions with JSON args, and reads back JSON results from a shared memory region at `$FAF100`.

- **Core 0** — ROM emulator + tprotocol command decoder + `js_worker_loop()`
- **Core 1** — JerryScript v3.0.0 runtime (32 KB heap, `JERRY_EXTERNAL_CONTEXT=ON`; reduced from 48 KB to free RAM for lwIP/CYW43 when network is enabled)
- **ST side** — `mdjs.c` / `mdjs.h` C library + `sidecart_stubs.S` (GAS-syntax wrappers for the bus protocol)

## Environment setup

### Required tools

| Tool                           | Version             | Notes                                                                                             |
| ------------------------------ | ------------------- | ------------------------------------------------------------------------------------------------- |
| ARM GNU Toolchain              | 15.2.rel1 (or 14.x) | Download from developer.arm.com — the Homebrew `arm-none-eabi-gcc` **lacks newlib** and will fail |
| CMake                          | 3.26+               | `brew install cmake`                                                                              |
| atarist-toolkit-docker / stcmd | latest              | For 68000 cross-compile (`vasm`, `vlink`, `m68k-atari-mint-gcc`)                                  |
| Python 3                       | any recent          | Used by build script for firmware.py                                                              |

### Key environment variables

```bash
# Required — point at the ARM GNU Toolchain bin/ dir (not Homebrew arm-none-eabi)
export PICO_TOOLCHAIN_PATH=/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin
```

The SDK paths (`PICO_SDK_PATH`, `PICO_EXTRAS_PATH`, `FATFS_SDK_PATH`) are set automatically by `rp/build.sh` — you do not need to export them.

## Build

```bash
# First time — fetch all submodules
git submodule update --init --recursive

# Build everything
PICO_TOOLCHAIN_PATH=/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin \
  ./build.sh pico_w debug <UUID>

# Output in dist/:
#   <UUID>-v<version>.uf2   ← flash this to the SidecarTridge
#   <UUID>.json             ← app descriptor (md5 auto-filled)
#   MDJSCODE.PRG            ← standalone GEM demo for the ST
```

The build script: copies `version.txt` → builds ST target (vasm + m68k-atari-mint-gcc) → generates `target_firmware.h` → builds RP2040 target (CMake + ARM GCC) → assembles dist/.

## Key files

```
rp/src/
  CMakeLists.txt        Add sources, link libraries, JerryScript config here
  emul.c                Firmware entry point — calls js_worker_init() + js_worker_loop()
  js_worker.c           Core 0 dispatcher + Core 1 JerryScript worker
  js_worker.h / include/js_worker.h
                        Command IDs (0x10–0x15), JS_STATUS_OFFSET, MDJS_STATUS_* constants,
                        message block struct, API
  jerry_port.c          Minimal JerryScript port layer (context, log, fatal, stubs); JERRY_HEAP_BYTES = 32 KB
  js_fetch.c            Native fetch() implementation (Core 1 side); build_response(), fetch_handler()
  include/js_fetch.h    Public API: js_fetch_init() — call after jerry_init()
  term.c / term.h       tprotocol buffer management; exposes term_consume_protocol()
  include/tprotocol.h   TransmissionProtocol struct, MAX_PROTOCOL_PAYLOAD_SIZE (2112)

target/atarist/src/
  mdjs.h / mdjs.c       ST-side C library — include these in ST projects
  sidecart_stubs.S      GAS-syntax translation of send_sync / send_sync_write
  main.s                ROM cartridge header + boot stub (vasm devpac syntax)

examples/mdjscode/
  main.c                MD/JS Code — GEM editor/runner for MDJSCODE.PRG
  textedit.c / .h       Scrollable text editor widget (reusable)
  math.js               Example: basic arithmetic functions
  fetch.js              Example: async fetch() + response.json()
  Makefile              Standalone build; run as: STCMD_NO_TTY=1 make examples from repo root
                        (or: ST_WORKING_FOLDER=<repo-root> stcmd make -C examples/mdjscode)
```

## JerryScript integration — known pitfalls

These caused build failures during initial development:

1. **LTO sets `CMAKE_AR` to host `gcc-ar`** — breaks cross-compilation. Fixed with `set(ENABLE_LTO OFF CACHE BOOL "" FORCE)` before `add_subdirectory(jerryscript)`.

2. **`JERRY_CMDLINE=ON` by default** — tries to build a host CLI executable, fails with arm-none-eabi. Fixed with `JERRY_CMDLINE/TEST/SNAPSHOT=OFF`, `JERRY_PORT=OFF`, `JERRY_EXT=OFF`.

3. **`JERRY_PORT=OFF` removes the port library** — but `jerry-core` still requires `jerry_port_*` symbols. Implemented in `rp/src/jerry_port.c`.

4. **JerryScript 3.0.0 API renames** vs older docs:
   - `jerry_get_value_from_error(val, free)` → `jerry_exception_value(val, free)`
   - `jerry_string_to_char_buffer(str, buf, sz)` → `jerry_string_to_buffer(str, JERRY_ENCODING_UTF8, buf, sz)`

5. **GCC 15 `-Wno-unterminated-string-initialization`** — JerryScript date helper has `char[7][3]` arrays initialised with 4-char string literals. Suppressed only for `jerry-core` via `target_compile_options`.

6. **Homebrew arm-none-eabi-gcc has no newlib sysroot** — download the official ARM GNU Toolchain from developer.arm.com and set `PICO_TOOLCHAIN_PATH` to its `bin/` dir.

## Inter-core protocol

Commands flow ST → Core 0 (tprotocol decode) → FIFO push → Core 1 (JerryScript) → result in ROM-in-RAM → Core 0 writes random token → ST unblocks.

- Shared data: `JsWorkerMsgBlock s_msg` in `js_worker.c`, protected by spin-lock 14 (`JS_SPINLOCK_ID`).
- Result buffer: `__rom_in_ram_start__ + JS_RESULT_OFFSET (0xF100)`, readable by ST at `$FAF100`.
- Async status word: `__rom_in_ram_start__ + JS_STATUS_OFFSET (0xF008)`, readable by ST at `$FAF008` as a single byte (no bus transaction needed).
- Timeout: `JS_CALL_TIMEOUT_US` (10 seconds — raised from 5s to accommodate HTTP round-trips). On timeout, Core 0 writes `{"error":"timeout"}` and restarts Core 1 with `multicore_reset_core1()`. Both sync and async paths use the shared `js_write_timeout_error()` helper.
- Core 1 calls `jerry_cleanup()` then `jerry_init()` at startup — this handles both first boot and post-timeout restart.

### Async call flow (`CMD_JS_CALL_ASYNC = 0x14`)

The Immediate-ACK pattern: Core 0 ACKs the ST _before_ waiting for Core 1, so the 68000 is unblocked immediately.

```
ST                        Core 0                   Core 1
│  CMD_JS_CALL_ASYNC       │                         │
│──────────────────────────►                         │
│                          │ *s_status_mem = BUSY     │
│                          │ FIFO_MSG_CALL ──────────►│
│  ACK (token written)     │                         │ JS executing...
│◄──────────────────────────│                         │
│  (does other work)       │                         │ JS finishes
│  *$FAF008 == BUSY         │                         │ result → ROM-in-RAM
│  *$FAF008 == DONE ◄────────────────────────────────  │ *s_status_mem = DONE
│  read MDJS_RESULT_ADDR      │                         │ FIFO push
│                          │ js_drain_async_fifo()    │
│                          │ s_async_pending = false  │
```

- `js_drain_async_fifo()` is called at the top of every `js_worker_loop()` iteration — non-blocking, never holds the spin-lock unless a timeout fires.
- Core 1 writes the status byte _inside_ `core1_flush_result()` while holding the spin-lock, so the ST cannot see `DONE` before the result bytes are committed.
- Only one async call can be in flight at a time. Sending `CMD_JS_CALL` or `CMD_JS_CALL_ASYNC` while `s_async_pending` is true returns `{"error":"busy"}` immediately.

### ROM-in-RAM layout

```
Offset    ST address    Purpose
────────────────────────────────────────────────────────────
0xF000    $FAF000       Random token (4 B)
0xF004    $FAF004       Token seed (4 B)
0xF008    $FAF008       Async status word (uint16_t) — low byte = status
0xF00A–0xF03F           Free
0xF040    $FAF040       TERM shared variables (indices 0–15)
0xF100    $FAF100       JS result buffer (2048 B)
```

## Native fetch() implementation

`fetch()` is exposed to JS as a global. HTTP only (no HTTPS), text/JSON responses, body capped at 4 KB.

### Threading model

`http_client_request_sync()` must run on Core 0 (owns the lwIP async context). Core 1 cannot call it directly. Protocol:

1. JS calls `await fetch("http://...")` → `fetch_handler()` runs on Core 1
2. Core 1 writes URL to `s_msg.fetch_url` under spin-lock, pushes `FIFO_MSG_FETCH_REQ`
3. Core 0's `js_wait_for_core1()` loop sees `FIFO_MSG_FETCH_REQ`, calls `js_handle_fetch_request()`, pushes `FIFO_MSG_FETCH_OK/ERR`
4. Core 1's `fetch_handler()` unblocks, reads body from `s_msg.fetch_body`, returns a resolved Promise
5. `jerry_call()` returns the outer async function's pending Promise
6. `jerry_run_jobs()` drains microtasks — the `await` continuation runs, the outer Promise settles
7. `jerry_promise_result()` extracts the settled value for JSON serialisation

**Key invariant:** `fetch_handler` is called from within `jerry_call()`, so Core 0 is already in `js_wait_for_core1()` and will service the FIFO_MSG_FETCH_REQ. There is no deadlock.

### Response object properties

`ok`, `status`, `statusText`, `url`, `redirected`, `type` (always `"basic"`), `bodyUsed` (always `false`), `text()`, `json()`

### Enabling/disabling network

Controlled by `MDJS_NO_NETWORK` in `rp/src/CMakeLists.txt` (0 = enabled, 1 = disabled). When enabled:
- `emul.c` calls `network_wifiInit(WIFI_MODE_STA)` + `network_wifiStaConnect()` before `js_worker_init()`
- WiFi credentials are read automatically from flash config (set via SidecarT config tool)
- `js_fetch_init()` is called after `jerry_init()` in both `core1_entry()` and `core1_handle_reset()`

### Known limitations

- HTTP only — `https://` URLs return `{ok: false}`
- Response body capped at 4 KB (`fetch_body[4096]` in `JsWorkerMsgBlock`)
- `statusText` is always `"OK"` or `""` — httpc doesn't return the reason phrase
- `redirected` is always `false` — httpc follows redirects silently without notifying the caller
- No `headers` support — httpc callback doesn't capture response headers
- No request options (method, headers, body) — GET only

## ST-side assembly linkage

`vasm` produces aout-format objects incompatible with `m68k-atari-mint-gcc`'s linker. The send_sync protocol routines are therefore re-implemented in GAS syntax in `sidecart_stubs.S` and compiled alongside the C code. Do not try to link vasm `.o` files with GCC.

## Guardrails — do not modify

- `fatfs-sdk/` — third-party SD library
- `pico-sdk/` — Raspberry Pi Pico SDK
- `pico-extras/` — Pico Extras
- `lib/jerryscript/` — JerryScript source (pinned to v3.0.0)

## Troubleshooting quick reference

| Symptom                                         | Cause                                   | Fix                                                                                  |
| ----------------------------------------------- | --------------------------------------- | ------------------------------------------------------------------------------------ |
| `gcc-ar: no such file` at link                  | LTO enabled, sets CMAKE_AR to host tool | `ENABLE_LTO OFF` in CMakeLists                                                       |
| `undefined reference to jerry_port_*`           | JERRY_PORT=OFF removes the port lib     | Symbols provided by `jerry_port.c`                                                   |
| `stdlib.h not found`                            | Homebrew arm-none-eabi lacks newlib     | Use official ARM GNU Toolchain                                                       |
| `-Wunterminated-string-initialization` error    | GCC 15 + JerryScript 3.0 date code      | `target_compile_options(jerry-core PRIVATE -Wno-unterminated-string-initialization)` |
| `stcmd` image not found                         | STCMD_IMAGE_TAG mismatch                | Check `stcmd` version vs app version                                                 |
| Vasm warnings about overflow / trailing garbage | Version strings passed as `-D` macros   | Harmless, ignore                                                                     |
| ST shows "not detected"                         | PING command timed out                  | Check UF2 is flashed; check UART for `MD/JS ready`                                   |

## Bus protocol byte-order quirk (IMPORTANT)

The cartridge protocol stores transmitted bytes in **byte-pair-swapped** order relative to what the m68k sent. This is a consequence of the PIO/DMA path:

- m68k transmits a 16-bit word with the **high byte at the lower address** (big-endian).
- PIO captures the 16-bit value the m68k put on the address bus.
- `store_payload_16_asm` does `strh` to the protocol buffer — ARM little-endian writes **low byte at the lower address**.
- Net effect: every adjacent byte pair appears swapped when read sequentially as a byte stream.

**Where this matters:**
- **UPLOAD body** (`core1_handle_upload`): source bytes must be un-swapped before `jerry_parse` sees them. Done in [js_worker.c](rp/src/js_worker.c) via `COPY_AND_CHANGE_ENDIANESS_BLOCK16(data, s_msg.js_source + ..., even_size)` in the CMD_JS_UPLOAD handler.
- **Result buffer flush** (`core1_flush_result`): result bytes are pre-swapped on write so the ST sees big-endian byte order through `move.b` / `move.w`. **Always writes the full 2048 bytes** (with NUL-padded source) so stale tails from previous longer results don't bleed through.
- **Token / longword fields**: NOT affected — `TPROTO_GET_RANDOM_TOKEN` swaps the two 16-bit halves internally; `TPROTO_GET_PAYLOAD_PARAM32` reads two adjacent words as (low, high) which compensates.
- **CALL func_name / args_json**: currently NOT swapped in `js_parse_call_payload` because attempts to add the swap repeatedly hung the firmware in cold-start scenarios. The CALL path works empirically — investigate before re-adding any swap there. (Theory: payload alignment, or the stale TransmissionProtocol buffer interacts with the swap helper differently than the inline UPLOAD swap.)
- **CMD_JS_POLL**: writes a short status JSON to the result buffer but does NOT zero-pad — bytes beyond the JSON length may contain stale data from previous flushes. Users who read the buffer after `mdjs_poll()` will see truncated stale tails. `mdjs_status()` reads `$FAF008` directly and is unaffected.

**Single-character ASCII results:** the byte-swap produces `[0x00, digit]` at offsets 0..1 of `s_result_mem`. The PIO outputs uint16 `0x_digit_00` to the bus. m68k reads byte at even address (UDS) = high byte of word = the digit. So `"7\0"` reads back as `'7'` correctly via byte reads.

## ST-side C ABI quirks

`m68k-atari-mint-gcc` uses **32-bit `int`** by default (no `-mshort`). Stack args are pushed in 4-byte slots; for small int values, the **low 16 bits sit at offset +2** of the 4-byte slot (big-endian). Stub wrappers in [sidecart_stubs.S](target/atarist/src/sidecart_stubs.S) must read full longwords for `int` args, not 16-bit words:

```asm
; CORRECT — reads full 32-bit int, callee uses d0.w
move.l 48(sp),d0

; WRONG — reads the HIGH 16 bits of a 32-bit int, always 0 for small values
move.w 48(sp),d0
```

Any new C-callable assembly wrapper must follow the same pattern. Symptom of getting this wrong: every command is sent as 0x0000 and the RP logs `js_worker_loop: ignoring command 0x0000`.

## JerryScript memory model

Two changes that are easy to undo and re-break:

1. **Static context buffer** ([jerry_port.c](rp/src/jerry_port.c)) — JerryScript's context + heap lives in a static BSS buffer, NOT malloc. Calling `malloc()` from Core 1 is not safe on pico-sdk (newlib's lazy init races with Core 0). Buffer size: **32 KB heap** (reduced from 48 KB to free ~16 KB for lwIP/CYW43 when `MDJS_NO_NETWORK=0`) + 8 KB context headroom. The 8 KB headroom is generous; `sizeof(jerry_context_t)` can grow with build flags, and a NULL `current_context_p` from a too-small buffer causes a silent fault in `jerry_init()`.

2. **`__StackLimit` capped at end of RAM** ([memmap_rp.ld](rp/src/memmap_rp.ld)) — originally `ORIGIN(RAM) + LENGTH(RAM) + LENGTH(ROM_IN_RAM)`, which let `_sbrk` grow the libc heap into ROM_IN_RAM (the bus-shared cartridge emulation buffer). Now `ORIGIN(RAM) + LENGTH(RAM)` only. If you raise this back, malloc can corrupt the ST-visible memory.

## Core 1 stdio is dangerous

Newlib's `printf` / `fputs` are **NOT dual-core safe** on pico-sdk. Concurrent stdio from both cores corrupts buffers and can deadlock Core 1 inside libc. Symptoms: garbled interleaved UART output, then Core 1 hangs.

**Rules:**
- Core 1 must NEVER call `DPRINTF` / `printf` / `puts` / `fprintf`.
- For Core 1 diagnostics, use the shared-state pattern in [js_worker.c](rp/src/js_worker.c): Core 1 writes a phase byte (`s_core1_phase`) or stages a NUL-terminated string in `s_core1_diag_msg` + bumps `s_core1_diag_seq`. Core 0 polls these in `js_worker_loop` and prints transitions.
- `jerry_port_log` is the one allowed exception, but it's wired to write to a stderr that won't actually emit (JerryScript only logs on errors during script parsing, and we control the script source).

## Debug diagnostics still in tree

The following are intentionally kept enabled for debug builds; remove for release if noise becomes a problem:

- `js_dispatch: cmd=0xNNNN payload=N` — every protocol command dispatched
- `Core 1 phase -> N` — Core 1 init/loop state machine transitions
- `Core 1 diag -> N` — UPLOAD parse/run outcome, CALL FUNC_FOUND/MISS, call result OK/ERR
- `Core 1 msg: ...` — upload source hex + ASCII view; call result string

Constants: `C1_PHASE_*` in [js_worker.c](rp/src/js_worker.c) define the diag byte values.

## Demo applications — TWO separate implementations

Confusing trap: there are two demos and they share neither source nor build path.

1. **Cartridge-embedded demo** ([target/atarist/src/main.s](target/atarist/src/main.s) — labels `mdjsdemo_cart_run` / `mdjsdemo_ram_entry`) — 68k assembly. Runs when the user double-clicks the cartridge `c:` drive icon. Self-contained: includes its own AES wrappers, random-number generation (XBIOS Supexec reading `$4BA` 200 Hz counter), and call payload builder. **This is what we used for protocol debugging.**

2. **Standalone `MDJSCODE.PRG`** ([examples/mdjscode/main.c](examples/mdjscode/main.c) + [mdjs.c](target/atarist/src/mdjs.c) + [sidecart_stubs.S](target/atarist/src/sidecart_stubs.S)) — C-based GEM program built from `examples/mdjscode/`. For users to drop in their AUTO folder or run from desktop. Distinct filename from the cartridge-embedded `MDJSDEMO.PRG` so the two don't get confused.

**If you change protocol behavior, you must update BOTH.** The cartridge demo uses the inline assembly path; the standalone PRG uses the C `mdjs.c` + stubs path. They send the same commands but via different code.

## RESOLVED: first-call cold-bus flake (was: back-to-back command race)

Earlier we had a "first call after RP boot returns empty result" cosmetic bug
that defied every fix we tried at the firmware/result-buffer level. Turned
out the actual cause was **rate-limiting at the protocol-parser side**:
back-to-back protocol commands on a freshly-booted SidecarTridge can race
the cartridge bus settle path, causing one or more commands to be missed
by the RP's parser. On first run only, PING+UPLOAD would get swallowed and
only the CALL would reach the dispatcher — so the call ran against an empty
JS context, returned `function 'add' not found`, and the ST saw nothing
useful in the result buffer.

**Diagnosis:** with the standalone demo stripped to PING-only, three sequential
runs produced exactly three `cmd=0x0010` dispatches and zero anomalies. With
the full PING+UPLOAD+CALL demo, the first run consistently showed only a
single `cmd=0x0012` (the CALL) reaching the parser. The full demo run
sending three commands rapidly was the difference. Adding ~50ms between
commands on the ST side completely eliminated the anomaly across all runs.

**Fix in tree:**
- [mdjs.c](target/atarist/src/mdjs.c) `mdjs_settle()` — ~6ms busy-loop called
  at the start of every command-emitting function except `mdjs_ping`. Users
  get this transparently; no API change.
- [main.s](target/atarist/src/main.s) `mdjsdemo_settle` — equivalent ~6ms
  busy-loop invoked before UPLOAD and before CALL in the cartridge-embedded
  demo (it can't use the C library).

**Tuning was empirical, halving until failure.** Started at 50ms, halved
repeatedly to 25ms → 12ms → 6ms, all reliable. Stopped at 6ms as a sensible
floor with safety margin — going lower wasn't worth chasing diminishing
returns. If you shorten it further, retest thoroughly across multiple cold
boots and run counts.

**`mdjs_poll` deliberately omitted** from the settle path because it's
expected to be called rapidly in a polling loop. The async-call user should
have already settled in `mdjs_call_async`, and the bus is warm by the time
poll loops start.

If a future change removes the settle entirely, expect first-run flakiness
to return immediately. Don't touch unless you have a clear replacement.

## RP2040-side build quirks

- `make debug` (top-level) builds with `DEBUG_MODE=1` → `_DEBUG=1` → enables UART stdio on GP0/GP1. USB CDC is intentionally disabled ([CMakeLists.txt:241](rp/src/CMakeLists.txt#L241)) — UART output requires a debug probe or USB-to-UART adapter. There is no `/dev/tty.usbmodem*` PTY from the RP itself, only from the debug probe's CDC bridge.
- `make uart` attaches `screen` to the first matching serial device at 115200.
- The build script auto-bumps `version.txt` patch number on every invocation. To suppress, set `SKIP_VERSION_BUMP=1`.

## Where to look when something breaks

| Symptom on ST                           | Where to look                                                                                              |
| --------------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| Boot message wrong / missing            | [main.s](target/atarist/src/main.s) `start_rom_code`, polls `$FAF00A` for magic `$4A`                      |
| Demo always says "worker not detected"  | `mdjs_ping()` timing out — for standalone PRG, check [sidecart_stubs.S](target/atarist/src/sidecart_stubs.S) parameter offsets (32-bit int!); ensure `examples/mdjscode/` was built and `MDJSCODE.PRG` deployed |
| `function 'X' not found` for valid name | Byte-swap issue on CALL path, or odd-length JS source was truncated (check `core1_handle_upload` even_size fix), or upload parse error — check UART for `Core 1 diag` and `Core 1 msg` |
| `{}` returned from async function       | Promise not settled — `jerry_run_jobs()` must be called after `jerry_call()` when result `jerry_value_is_promise()`; see `core1_handle_call` in js_worker.c |
| fetch() always returns `{ok:false}`     | WiFi not connected yet, or `https://` URL used, or timeout — check UART for `js_fetch:` lines |
| Garbled chars in result                 | `core1_flush_result` not pre-swapping; check `COPY_AND_CHANGE_ENDIANESS_BLOCK16` is called                 |
| Bombs / crash on demo exit              | Stack-resident demo code returning to corrupt SR/USP — check `mdjsdemo_cart_run`                            |
| `ignoring command 0x0000` in UART       | Either bus noise (harmless) or C ABI int-size bug in stubs (broken)                                        |
