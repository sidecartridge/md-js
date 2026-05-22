/**
 * File: js_worker.h
 * Description: MD/JS JavaScript Worker — RP2040 Core 1 runtime.
 *
 * Threading model:
 *   Core 0  owns: ROM emulator DMA/PIO, tprotocol parse, emul_start() loop.
 *   Core 1  owns: JerryScript init/eval/call/cleanup, blocking on FIFO.
 *
 *   Signalling: pico_multicore hardware FIFO (32-bit tagged words).
 *   Data:       JsWorkerMsgBlock in Core 0 BSS, protected by spin-lock 14.
 *
 * ROM-in-RAM layout (128 KB total at __rom_in_ram_start__):
 *   0x0000–0xEFFF  ROM bank data (ROM4 / ROM3 emulation)
 *   0xF000–0xF007  Random token + seed
 *   0xF008–0xF009  Async status word
 *   0xF00A–0xF00B  Worker-ready word
 *   0xF00C–0xF0FF  Reserved/shared words
 *   0xF100–0xF8FF  JS result buffer     (2 KB) — MD/JS
 *   0xF900–0xFFFF  Reserved
 */

#ifndef JS_WORKER_H
#define JS_WORKER_H

#include <stdbool.h>
#include <stdint.h>

#include "tprotocol.h"

/* ── Command IDs ────────────────────────────────────────────────────────── */
/* Start at 0x10 to leave APP_TERMINAL range (0x00–0x01) intact.            */
#define CMD_JS_PING         0x10  /* Ping — returns version JSON              */
#define CMD_JS_UPLOAD       0x11  /* Upload JS source (chunked)               */
#define CMD_JS_CALL         0x12  /* Call a named function with JSON args     */
#define CMD_JS_RESET        0x13  /* Wipe JS context and re-initialise        */
#define CMD_JS_CALL_ASYNC   0x14  /* Non-blocking call — ACKs before Core 1   */
#define CMD_JS_POLL         0x15  /* Poll async status (returns status JSON)   */

/* ── Async status word (readable by ST at ROM4_ADDR + JS_STATUS_OFFSET) ── */
/* ROM4 base = $FA0000; JS_STATUS_OFFSET = 0xF008 → ST address $FAF008.    */
/* RP2040 stores the status in the high byte of the bus word so an ST byte   */
/* read at even address $FAF008 sees the status value directly.              */
#define JS_STATUS_OFFSET    0xF008

/* Status values — shared between RP2040 (js_worker.h) and ST (mdjs.h). */
#define MDJS_STATUS_IDLE    0x00  /* No async call in progress                */
#define MDJS_STATUS_BUSY    0x01  /* Core 1 is executing                      */
#define MDJS_STATUS_DONE    0x02  /* Result ready at JS_RESULT_OFFSET         */
#define MDJS_STATUS_ERROR   0x03  /* Error string at JS_RESULT_OFFSET         */

/* ── Result buffer (readable by ST at ROM4_ADDR + JS_RESULT_OFFSET) ────── */
/* ROM4 base = $FA0000; JS_RESULT_OFFSET = 0xF100 → ST address $FAF100.    */
#define JS_RESULT_OFFSET   0xF100
#define JS_RESULT_MAX_SIZE 2048

/* ── Upload chunking ────────────────────────────────────────────────────── */
/* send_sync_write always sends token(4) + d3/d4/d5 longwords(12) before    */
/* streaming buffer bytes. MD/JS uses the low word of d3/d4/d5 for          */
/* chunk_index, total_chunks, and chunk_size.                               */
#define JS_WRITE_HDR_SIZE   16
#define JS_UPLOAD_HDR_SIZE  JS_WRITE_HDR_SIZE
#define JS_UPLOAD_CHUNK_MAX (MAX_PROTOCOL_PAYLOAD_SIZE - JS_UPLOAD_HDR_SIZE)

/* Maximum total assembled JS (8 chunks * max bytes per chunk) */
#define JS_SOURCE_MAX (JS_UPLOAD_CHUNK_MAX * 8)

/* ── CALL payload layout ────────────────────────────────────────────────── */
/* token(4) + d3/d4/d5(12) + func_name(NUL-terminated) + args_json(NUL).    */
#define JS_CALL_FUNC_NAME_MAX 64
#define JS_CALL_HDR_SIZE JS_WRITE_HDR_SIZE
#define JS_CALL_ARGS_MAX \
  (MAX_PROTOCOL_PAYLOAD_SIZE - JS_CALL_HDR_SIZE - JS_CALL_FUNC_NAME_MAX)

/* ── Inter-core FIFO message tags ───────────────────────────────────────── */
/* Upper 8 bits of the 32-bit FIFO word carry the message type.             */
#define FIFO_TAG_SHIFT 24
#define FIFO_TAG_MASK  0xFF000000u

#define FIFO_MSG_PING      0x01u  /* Core 0 → Core 1: ping                     */
#define FIFO_MSG_UPLOAD    0x02u  /* Core 0 → Core 1: eval assembled JS source */
#define FIFO_MSG_CALL      0x03u  /* Core 0 → Core 1: call named function      */
#define FIFO_MSG_RESET     0x04u  /* Core 0 → Core 1: wipe context             */
#define FIFO_MSG_FETCH_REQ 0x05u  /* Core 1 → Core 0: perform HTTP fetch       */
#define FIFO_MSG_DONE      0x80u  /* Core 1 → Core 0: result ready (success)   */
#define FIFO_MSG_ERROR     0x81u  /* Core 1 → Core 0: result ready (error)     */
#define FIFO_MSG_FETCH_OK  0x82u  /* Core 0 → Core 1: fetch succeeded          */
#define FIFO_MSG_FETCH_ERR 0x83u  /* Core 0 → Core 1: fetch failed             */

/* ── Spin-lock ──────────────────────────────────────────────────────────── */
/* User-available range is 0–15; SDK uses 16–31.                            */
#define JS_SPINLOCK_ID 14

/* ── JS execution timeout ───────────────────────────────────────────────── */
/* Core 0 waits at most this long for Core 1 to reply before resetting it.  */
#define JS_CALL_TIMEOUT_US 10000000  /* 10 seconds */

/* ── Shared message block ───────────────────────────────────────────────── */
/* Lives in Core 0 BSS. Core 1 reads/writes it under spin-lock 14.         */
typedef struct {
  /* Upload accumulation */
  uint8_t  js_source[JS_SOURCE_MAX];
  uint32_t js_source_len;
  uint16_t chunks_expected;
  uint16_t chunks_received;

  /* Call parameters */
  char call_func[JS_CALL_FUNC_NAME_MAX];
  char call_args_json[JS_CALL_ARGS_MAX];

  /* Result (written by Core 1, flushed to ROM-in-RAM) */
  char result_json[JS_RESULT_MAX_SIZE];
  bool result_is_error;

  /* fetch() request/response — Core 1 writes URL fields, Core 0 fills body */
  char fetch_url[256];          /* full URL e.g. "http://host/path"           */
  char fetch_body[4096];        /* response body (NUL-terminated)             */
  char fetch_status_text[32];   /* HTTP reason phrase e.g. "OK", "Not Found"  */
  uint16_t fetch_status;        /* HTTP status code (e.g. 200)                */
  bool fetch_ok;                /* true if request succeeded and status 2xx   */
  bool fetch_redirected;        /* true if the request followed a redirect    */
} JsWorkerMsgBlock;

/* ── Shared state (accessible to js_fetch.c) ────────────────────────────── */
#include "hardware/sync.h"
extern JsWorkerMsgBlock js_worker_msg;
extern spin_lock_t     *js_worker_spin_lock;

/* ── Public API (called from Core 0 / emul.c) ───────────────────────────── */

/**
 * @brief Initialise the JS worker and launch Core 1.
 * Call once from emul_start() before entering the main loop.
 */
void js_worker_init(void);

/**
 * @brief Process any pending JS commands from the ST.
 * Call from the emul_start() main loop instead of term_loop().
 */
void __not_in_flash_func(js_worker_loop)(void);

#endif  /* JS_WORKER_H */
