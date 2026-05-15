/**
 * File: js_worker.c
 * Description: MD/JS JavaScript Worker — Core 1 JerryScript runtime.
 *
 * Core 0 calls js_worker_init() once and js_worker_loop() from its main loop.
 * Core 1 runs core1_entry(), waiting on the multicore FIFO for work tags and
 * executing JerryScript operations (eval / call / reset / ping).
 *
 * Signalling:  32-bit FIFO words, tag in upper 8 bits (FIFO_MSG_*).
 * Data:        JsWorkerMsgBlock in BSS, protected by spin-lock JS_SPINLOCK_ID.
 * Result path: Core 1 writes result_json → ROM-in-RAM @ JS_RESULT_OFFSET
 *              (with 16-bit byte-swap so the ST can read it big-endian).
 *              Core 0 then writes the random token to unblock the ST.
 */

#include "js_worker.h"

#include <alloca.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "constants.h"
#include "debug.h"
#include "mdjs_protocol.h"
#include "memfunc.h"
#include "tprotocol.h"

#include "jerryscript.h"

#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/time.h"

/* ── Linker symbol for ROM-in-RAM base (defined in memmap_rp.ld) ────────── */
extern unsigned int __rom_in_ram_start__;

/* ── Shared state ────────────────────────────────────────────────────────── */
static JsWorkerMsgBlock s_msg;
static spin_lock_t     *s_spin_lock;

/* Cached addresses (set in js_worker_init, read-only thereafter) */
static uint32_t s_rom_base;
static uint32_t s_token_addr;
static uint32_t s_token_seed_addr;
static volatile char     *s_result_mem;
static volatile uint16_t *s_status_mem;   /* async status word at JS_STATUS_OFFSET */
static volatile uint16_t *s_ready_mem;    /* worker-ready byte at MDJS_READY_OFFSET */

/* Async call state — Core 0 only, no lock needed */
static bool     s_async_pending;
static uint64_t s_async_start_us;
static TransmissionProtocol s_loop_proto;

/* Drain spurious commands that the PIO/protocol parser may pick up during
 * cold-start before everything is fully initialised. Set true at the end of
 * js_worker_init(); until then, every consumed command is logged and ignored. */
static volatile bool s_dispatch_armed = false;
static char s_core1_result_json[JS_RESULT_MAX_SIZE];
static char s_core1_call_func[JS_CALL_FUNC_NAME_MAX];
static char s_core1_call_args_json[JS_CALL_ARGS_MAX];

#define MDJS_BUS_BYTE_WORD(value) ((uint16_t)((uint16_t)(value) << 8))
#define MDJS_BUS_WORD_BYTE(word)  ((uint8_t)(((uint16_t)(word) >> 8) & 0xFFu))

/* Ready flag: write the magic to BOTH bytes of the bus word so the ST sees the
 * same byte regardless of which half of the 16-bit word is exposed at the even
 * address. Removes ambiguity around bus byte-ordering for a single-byte poll. */
#define MDJS_READY_WORD ((uint16_t)((MDJS_READY_MAGIC << 8) | MDJS_READY_MAGIC))

/* ── Forward declarations ────────────────────────────────────────────────── */
static void core1_entry(void);
static void core1_handle_ping(void);
static void core1_handle_upload(void);
static void core1_handle_call(void);
static void core1_handle_reset(void);
static void core1_flush_result(void);
static void js_dispatch_command(const TransmissionProtocol *proto);
static void js_send_response(uint32_t random_token);
static void js_write_error_response(const char *message);
static void js_write_busy_error(void);
static void js_write_timeout_error(void);
static bool js_parse_call_payload(const uint8_t *payload, size_t payload_size);
static void js_copy_exception_string(jerry_value_t exception_value,
                                     char *dst,
                                     size_t dst_size);

/* ────────────────────────────────────────────────────────────────────────── */
/* Core 1 — JerryScript runtime                                              */
/* ────────────────────────────────────────────────────────────────────────── */

/* Core 1 must NOT call DPRINTF / printf — newlib stdio is not dual-core safe
 * on pico-sdk and concurrent prints from both cores deadlock Core 1 inside
 * libc. Instead, Core 1 writes a single-byte phase into s_core1_phase and
 * Core 0 prints transitions from js_worker_loop. */
static volatile uint8_t s_core1_phase = 0;
#define C1_PHASE_ENTRY            1
#define C1_PHASE_PRE_CLEANUP      2
#define C1_PHASE_PRE_INIT         3
#define C1_PHASE_POST_INIT        4
#define C1_PHASE_READY            5
#define C1_PHASE_LOOPING          6
#define C1_PHASE_UPLOAD_PARSE_ERR 10
#define C1_PHASE_UPLOAD_RUN_ERR   11
#define C1_PHASE_UPLOAD_OK        12
#define C1_PHASE_CALL_FUNC_FOUND  13
#define C1_PHASE_CALL_FUNC_MISS   14
#define C1_PHASE_CALL_RESULT_OK   15
#define C1_PHASE_CALL_RESULT_ERR  16

static volatile uint8_t s_core1_diag = 0;

/* Larger diagnostic ring — Core 1 stages a NUL-terminated message and bumps
 * s_core1_diag_seq; Core 0 prints it from js_worker_loop. */
static volatile uint32_t s_core1_diag_seq = 0;
static char s_core1_diag_msg[160];

static void core1_entry(void) {
  s_core1_phase = C1_PHASE_ENTRY;

  /* Only cleanup if we've initialised before (i.e. on post-timeout restart).
   * jerry_cleanup() on a never-initialised context can fault in some builds. */
  static bool s_core1_initialised = false;
  if (s_core1_initialised) {
    s_core1_phase = C1_PHASE_PRE_CLEANUP;
    jerry_cleanup();
  }
  *s_ready_mem = 0;
  s_core1_phase = C1_PHASE_PRE_INIT;
  jerry_init(JERRY_INIT_EMPTY);
  s_core1_phase = C1_PHASE_POST_INIT;
  s_core1_initialised = true;
  *s_ready_mem = MDJS_READY_WORD;
  s_core1_phase = C1_PHASE_READY;

  while (true) {
    s_core1_phase = C1_PHASE_LOOPING;
    uint32_t msg = multicore_fifo_pop_blocking();
    uint8_t  tag = (uint8_t)((msg & FIFO_TAG_MASK) >> FIFO_TAG_SHIFT);

    switch (tag) {
      case FIFO_MSG_PING:   core1_handle_ping();   break;
      case FIFO_MSG_UPLOAD: core1_handle_upload(); break;
      case FIFO_MSG_CALL:   core1_handle_call();   break;
      case FIFO_MSG_RESET:  core1_handle_reset();  break;
      default:
        DPRINTF("Core 1: unknown FIFO tag 0x%02X\n", tag);
        multicore_fifo_push_blocking((uint32_t)FIFO_MSG_ERROR << FIFO_TAG_SHIFT);
        break;
    }
  }
}

static void core1_handle_ping(void) {
  uint32_t save = spin_lock_blocking(s_spin_lock);
  snprintf(s_msg.result_json, JS_RESULT_MAX_SIZE,
           "{\"version\":\"MD/JS/1.0\",\"jerry\":\"%d.%d.%d\"}",
           JERRY_API_MAJOR_VERSION,
           JERRY_API_MINOR_VERSION,
           JERRY_API_PATCH_VERSION);
  s_msg.result_is_error = false;
  spin_unlock(s_spin_lock, save);

  core1_flush_result();
  multicore_fifo_push_blocking((uint32_t)FIFO_MSG_DONE << FIFO_TAG_SHIFT);
}

static void core1_handle_upload(void) {
  /* Copy source under lock, then parse+run without holding it.
   * We use jerry_parse + jerry_run (not jerry_eval) so function declarations
   * in the uploaded source register as globals — jerry_eval's indirect-eval
   * semantics don't always persist function bindings in JerryScript 3.0. */
  uint32_t save = spin_lock_blocking(s_spin_lock);
  uint32_t src_len = s_msg.js_source_len;
  spin_unlock(s_spin_lock, save);

  jerry_value_t parsed = jerry_parse(
      (const jerry_char_t *)s_msg.js_source, src_len, NULL);

  bool is_err = jerry_value_is_exception(parsed);
  jerry_value_t result;

  /* Build a combined diagnostic line: src_len, hex of first 16 bytes,
   * printable view of first 80 bytes, and parse-error text if any. */
  {
    char err_text[80];
    err_text[0] = '\0';
    if (is_err) {
      js_copy_exception_string(parsed, err_text, sizeof(err_text));
    }
    int n = snprintf(s_core1_diag_msg, sizeof(s_core1_diag_msg),
                     "upload len=%lu hex=", (unsigned long)src_len);
    size_t hex_n = src_len < 16u ? src_len : 16u;
    for (size_t i = 0; (i < hex_n) && (n < (int)sizeof(s_core1_diag_msg) - 4);
         i++) {
      n += snprintf(s_core1_diag_msg + n,
                    sizeof(s_core1_diag_msg) - (size_t)n,
                    "%02X", s_msg.js_source[i]);
    }
    if (n < (int)sizeof(s_core1_diag_msg) - 2) {
      s_core1_diag_msg[n++] = ' ';
      s_core1_diag_msg[n++] = '\'';
    }
    size_t snap = src_len < 60u ? src_len : 60u;
    for (size_t i = 0; (i < snap) && (n < (int)sizeof(s_core1_diag_msg) - 4);
         i++) {
      char c = (char)s_msg.js_source[i];
      s_core1_diag_msg[n++] = (c >= 0x20 && c <= 0x7E) ? c : '.';
    }
    if (n < (int)sizeof(s_core1_diag_msg) - 2) {
      s_core1_diag_msg[n++] = '\'';
    }
    if (is_err && (n < (int)sizeof(s_core1_diag_msg) - 8)) {
      n += snprintf(s_core1_diag_msg + n,
                    sizeof(s_core1_diag_msg) - (size_t)n,
                    " err=%s", err_text);
    }
    if (n >= (int)sizeof(s_core1_diag_msg)) {
      n = (int)sizeof(s_core1_diag_msg) - 1;
    }
    s_core1_diag_msg[n] = '\0';
    s_core1_diag_seq++;
  }

  if (is_err) {
    s_core1_diag = C1_PHASE_UPLOAD_PARSE_ERR;
    result = parsed;
  } else {
    result = jerry_run(parsed);
    jerry_value_free(parsed);
    is_err = jerry_value_is_exception(result);
    s_core1_diag = is_err ? C1_PHASE_UPLOAD_RUN_ERR : C1_PHASE_UPLOAD_OK;
  }

  if (is_err) {
    js_copy_exception_string(result, s_core1_result_json,
                             sizeof(s_core1_result_json));
  } else {
    snprintf(s_core1_result_json, sizeof(s_core1_result_json), "{\"ok\":true}");
  }
  jerry_value_free(result);

  save = spin_lock_blocking(s_spin_lock);
  memcpy(s_msg.result_json, s_core1_result_json, JS_RESULT_MAX_SIZE);
  s_msg.result_is_error = is_err;
  spin_unlock(s_spin_lock, save);

  core1_flush_result();
  multicore_fifo_push_blocking(
      (uint32_t)(is_err ? FIFO_MSG_ERROR : FIFO_MSG_DONE) << FIFO_TAG_SHIFT);
}

/* Maximum argv entries — caps alloca size to ~256 bytes on Core 1's 2 KB stack. */
#define JS_CALL_MAX_ARGS 32

static void core1_handle_call(void) {
  /* Copy call parameters under lock, then do all JS work without holding it. */
  uint32_t save = spin_lock_blocking(s_spin_lock);
  memcpy(s_core1_call_func, s_msg.call_func, JS_CALL_FUNC_NAME_MAX);
  memcpy(s_core1_call_args_json, s_msg.call_args_json, JS_CALL_ARGS_MAX);
  spin_unlock(s_spin_lock, save);

  bool result_is_error;

  jerry_value_t global    = jerry_current_realm();
  jerry_value_t fname_str = jerry_string_sz(s_core1_call_func);
  jerry_value_t func_val  = jerry_object_get(global, fname_str);
  jerry_value_free(fname_str);
  jerry_value_free(global);

  if (!jerry_value_is_function(func_val)) {
    s_core1_diag = C1_PHASE_CALL_FUNC_MISS;
    snprintf(s_core1_result_json, sizeof(s_core1_result_json),
             "{\"error\":\"function '%s' not found\"}", s_core1_call_func);
    jerry_value_free(func_val);
    result_is_error = true;
    goto write_result;
  }
  s_core1_diag = C1_PHASE_CALL_FUNC_FOUND;

  jerry_value_t args_val = jerry_json_parse(
      (const jerry_char_t *)s_core1_call_args_json,
      strlen(s_core1_call_args_json));

  if (jerry_value_is_exception(args_val)) {
    snprintf(s_core1_result_json, sizeof(s_core1_result_json),
             "{\"error\":\"invalid args JSON\"}");
    jerry_value_free(args_val);
    jerry_value_free(func_val);
    result_is_error = true;
    goto write_result;
  }

  jerry_value_t *argv = NULL;
  jerry_length_t  argc = 0;

  if (jerry_value_is_array(args_val)) {
    argc = jerry_array_length(args_val);
    if (argc > JS_CALL_MAX_ARGS) argc = JS_CALL_MAX_ARGS;
    if (argc > 0) {
      argv = (jerry_value_t *)alloca(sizeof(jerry_value_t) * argc);
      for (jerry_length_t i = 0; i < argc; i++) {
        argv[i] = jerry_object_get_index(args_val, i);
      }
    }
  }

  jerry_value_t undefined_val = jerry_undefined();
  jerry_value_t ret = jerry_call(func_val, undefined_val, argv, argc);
  jerry_value_free(undefined_val);

  for (jerry_length_t i = 0; i < argc; i++) {
    jerry_value_free(argv[i]);
  }
  jerry_value_free(args_val);
  jerry_value_free(func_val);

  if (jerry_value_is_exception(ret)) {
    js_copy_exception_string(ret, s_core1_result_json,
                             sizeof(s_core1_result_json));
    result_is_error = true;
  } else {
    jerry_value_t json_str = jerry_json_stringify(ret);
    jerry_value_free(ret);
    if (jerry_value_is_exception(json_str) || !jerry_value_is_string(json_str)) {
      jerry_value_free(json_str);
      snprintf(s_core1_result_json, sizeof(s_core1_result_json),
               "{\"error\":\"result not JSON-serialisable\"}");
      result_is_error = true;
    } else {
      jerry_size_t sz = jerry_string_to_buffer(
          json_str, JERRY_ENCODING_UTF8,
          (jerry_char_t *)s_core1_result_json, JS_RESULT_MAX_SIZE - 1);
      s_core1_result_json[sz] = '\0';
      jerry_value_free(json_str);
      result_is_error = false;
    }
  }

write_result:
  /* Snapshot the result for Core 0 to print. */
  {
    int n = snprintf(s_core1_diag_msg, sizeof(s_core1_diag_msg),
                     "call result is_err=%d len=%lu '",
                     (int)result_is_error,
                     (unsigned long)strnlen(s_core1_result_json,
                                            sizeof(s_core1_result_json)));
    size_t srclen = strnlen(s_core1_result_json, sizeof(s_core1_result_json));
    size_t snap = srclen < 60u ? srclen : 60u;
    for (size_t i = 0; (i < snap) && (n < (int)sizeof(s_core1_diag_msg) - 2);
         i++) {
      char c = s_core1_result_json[i];
      s_core1_diag_msg[n++] = (c >= 0x20 && c <= 0x7E) ? c : '.';
    }
    if (n < (int)sizeof(s_core1_diag_msg) - 1) {
      s_core1_diag_msg[n++] = '\'';
    }
    s_core1_diag_msg[n] = '\0';
    s_core1_diag_seq++;
    s_core1_diag = result_is_error ? C1_PHASE_CALL_RESULT_ERR
                                    : C1_PHASE_CALL_RESULT_OK;
  }

  save = spin_lock_blocking(s_spin_lock);
  memcpy(s_msg.result_json, s_core1_result_json, JS_RESULT_MAX_SIZE);
  s_msg.result_is_error = result_is_error;
  spin_unlock(s_spin_lock, save);

  core1_flush_result();
  multicore_fifo_push_blocking(
      (uint32_t)(s_msg.result_is_error ? FIFO_MSG_ERROR : FIFO_MSG_DONE)
      << FIFO_TAG_SHIFT);
}

static void core1_handle_reset(void) {
  *s_ready_mem = 0;
  jerry_cleanup();
  jerry_init(JERRY_INIT_EMPTY);
  *s_ready_mem = MDJS_READY_WORD;

  uint32_t save = spin_lock_blocking(s_spin_lock);
  s_msg.js_source_len    = 0;
  s_msg.chunks_expected  = 0;
  s_msg.chunks_received  = 0;
  snprintf(s_msg.result_json, JS_RESULT_MAX_SIZE,
           "{\"ok\":true,\"reset\":true}");
  s_msg.result_is_error = false;
  spin_unlock(s_spin_lock, save);

  core1_flush_result();
  multicore_fifo_push_blocking((uint32_t)FIFO_MSG_DONE << FIFO_TAG_SHIFT);
}

/**
 * Copy result_json to the ROM-in-RAM result area with 16-bit byte-swap so
 * the Atari ST sees the bytes in big-endian order when reading through the
 * cartridge ROM address space.
 */
/* Bumped by Core 1 at the end of every result flush. Core 0 reads it
 * before dispatching a Core-1 op and waits for it to change before
 * unblocking the ST — guards against a flush that happens but isn't yet
 * visible at the bus when the token is written. */
static volatile uint32_t s_flush_seq = 0;

static void core1_flush_result(void) {
  uint32_t save = spin_lock_blocking(s_spin_lock);
  size_t len = strnlen(s_msg.result_json, JS_RESULT_MAX_SIZE - 1);
  /* NUL-pad the source out to the full result buffer, then write the entire
   * thing. This wipes any stale bytes left in s_result_mem from previous
   * longer results — regardless of length difference between calls. */
  memset(s_msg.result_json + len, 0, JS_RESULT_MAX_SIZE - len);
  COPY_AND_CHANGE_ENDIANESS_BLOCK16(s_msg.result_json,
                                    (void *)s_result_mem,
                                    JS_RESULT_MAX_SIZE);
  /* Update async status so the ST can see DONE/ERROR before the FIFO push. */
  *s_status_mem = MDJS_BUS_BYTE_WORD(s_msg.result_is_error
                                      ? MDJS_STATUS_ERROR
                                      : MDJS_STATUS_DONE);
  __dmb();
  s_flush_seq++;
  spin_unlock(s_spin_lock, save);
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Core 0 — command dispatcher                                                */
/* ────────────────────────────────────────────────────────────────────────── */

/**
 * Write the random token back to shared memory to unblock the ST, and
 * seed the next token value.
 */
static void js_send_response(uint32_t random_token) {
  TPROTO_SET_RANDOM_TOKEN(s_token_addr, random_token);
  uint32_t new_seed = rand();
  TPROTO_SET_RANDOM_TOKEN(s_token_seed_addr, new_seed);
}

/**
 * Wait for Core 1 to finish, with timeout recovery.
 * Returns the FIFO message tag, or FIFO_MSG_ERROR on timeout.
 *
 * pre_seq is the value of s_flush_seq captured by the caller BEFORE pushing
 * a work request to Core 1. After the FIFO reply arrives, this function
 * waits briefly for s_flush_seq to advance past pre_seq — guarding against
 * a result flush whose final bus writes haven't propagated yet.
 */
static uint8_t js_wait_for_core1(uint32_t pre_seq) {
  uint32_t resp = 0;
  bool     ok   = multicore_fifo_pop_timeout_us(JS_CALL_TIMEOUT_US, &resp);
  if (!ok) {
    DPRINTF("js_worker: Core 1 timeout — resetting\n");
    uint32_t save = spin_lock_blocking(s_spin_lock);
    js_write_timeout_error();
    spin_unlock(s_spin_lock, save);

    /* Reset Core 1. jerry_cleanup() is called at the top of core1_entry()
     * before re-init, so the old heap is always freed on restart. */
    *s_ready_mem = 0;
    multicore_reset_core1();
    multicore_launch_core1(core1_entry);
    return FIFO_MSG_ERROR;
  }

  /* Belt-and-braces: spin briefly until Core 1's flush counter has advanced
   * past the pre-call snapshot. Bounded to ~1 ms total (10000 iterations of
   * ~100 ns each on a 125 MHz Cortex-M0+). */
  for (int i = 0; i < 10000; i++) {
    if (s_flush_seq != pre_seq) break;
    __asm volatile("nop");
  }
  __dmb();
  return (uint8_t)((resp & FIFO_TAG_MASK) >> FIFO_TAG_SHIFT);
}

static void js_copy_exception_string(jerry_value_t exception_value,
                                     char *dst,
                                     size_t dst_size) {
  jerry_value_t err_val = jerry_exception_value(exception_value, true);
  jerry_value_t err_str = jerry_value_to_string(err_val);
  jerry_value_free(err_val);

  if (jerry_value_is_exception(err_str)) {
    snprintf(dst, dst_size, "{\"error\":\"exception\"}");
    jerry_value_free(err_str);
    return;
  }

  jerry_size_t sz = jerry_string_to_buffer(
      err_str, JERRY_ENCODING_UTF8, (jerry_char_t *)dst, dst_size - 1);
  dst[sz] = '\0';
  jerry_value_free(err_str);
}

/* Write an error JSON literal to the result buffer and update the status word.
 * Must be called while holding s_spin_lock. */
static void js_write_timeout_error(void) {
  snprintf(s_msg.result_json, JS_RESULT_MAX_SIZE, "{\"error\":\"timeout\"}");
  s_msg.result_is_error = true;
  size_t len      = strnlen(s_msg.result_json, JS_RESULT_MAX_SIZE - 1) + 1;
  size_t copy_len = (len + 1u) & ~1u;
  COPY_AND_CHANGE_ENDIANESS_BLOCK16(s_msg.result_json, (void *)s_result_mem, copy_len);
  *s_status_mem = MDJS_BUS_BYTE_WORD(MDJS_STATUS_ERROR);
}

static void __not_in_flash_func(js_write_error_response)(const char *message) {
  uint32_t save = spin_lock_blocking(s_spin_lock);
  snprintf(s_msg.result_json, JS_RESULT_MAX_SIZE, "{\"error\":\"%s\"}", message);
  s_msg.result_is_error = true;
  size_t len = strnlen(s_msg.result_json, JS_RESULT_MAX_SIZE - 1) + 1;
  size_t copy_len = (len + 1u) & ~1u;
  COPY_AND_CHANGE_ENDIANESS_BLOCK16(s_msg.result_json, (void *)s_result_mem,
                                    copy_len);
  *s_status_mem = MDJS_BUS_BYTE_WORD(MDJS_STATUS_ERROR);
  spin_unlock(s_spin_lock, save);
}

/* Write the "busy" error JSON to the result buffer (sizeof includes NUL). */
static void __not_in_flash_func(js_write_busy_error)(void) {
  js_write_error_response("busy");
}

/* Parse a CALL payload (func_name\0args_json\0) into s_msg under spin-lock. */
static bool js_parse_call_payload(const uint8_t *payload, size_t payload_size) {
  if (payload_size == 0u) {
    return false;
  }

  /* Un-swap protocol bytes using a manual byte-by-byte loop to avoid the
   * uint16_t macro path that has been observed to wedge on cold-start
   * garbage. Bounded to 256 bytes — function names + small args fit easily. */
  static uint8_t swapped[256];
  size_t copy_len = payload_size < sizeof(swapped) ? payload_size : sizeof(swapped);
  size_t even_len = copy_len & ~(size_t)1u;
  for (size_t i = 0; i < even_len; i += 2) {
    swapped[i] = payload[i + 1];
    swapped[i + 1] = payload[i];
  }

  const char *func_name = (const char *)swapped;
  const char *fn_end = memchr(func_name, '\0', even_len);
  if ((fn_end == NULL) || (fn_end == func_name)) {
    return false;
  }

  size_t fn_len = (size_t)(fn_end - func_name);
  if (fn_len >= JS_CALL_FUNC_NAME_MAX) {
    return false;
  }

  const char *args_json = fn_end + 1;
  size_t args_size = even_len - fn_len - 1u;
  const char *args_end = memchr(args_json, '\0', args_size);
  if (args_end == NULL) {
    return false;
  }

  size_t args_len = (size_t)(args_end - args_json);
  if (args_len >= JS_CALL_ARGS_MAX) {
    return false;
  }

  uint32_t save = spin_lock_blocking(s_spin_lock);
  memcpy(s_msg.call_func, func_name, fn_len);
  s_msg.call_func[fn_len] = '\0';
  memcpy(s_msg.call_args_json, args_json, args_len);
  s_msg.call_args_json[args_len] = '\0';
  spin_unlock(s_spin_lock, save);
  return true;
}

/**
 * Drain the FIFO response from an in-progress async call, if any.
 * Called at the top of js_worker_loop() — never blocks.
 */
static void __not_in_flash_func(js_drain_async_fifo)(void) {
  if (!s_async_pending) return;

  /* Async timeout: write error result and reset Core 1 */
  if (time_us_64() - s_async_start_us > (uint64_t)JS_CALL_TIMEOUT_US) {
    DPRINTF("js_worker: async timeout — resetting Core 1\n");
    uint32_t save = spin_lock_blocking(s_spin_lock);
    js_write_timeout_error();
    spin_unlock(s_spin_lock, save);
    s_async_pending = false;
    *s_ready_mem = 0;
    multicore_reset_core1();
    multicore_launch_core1(core1_entry);
    return;
  }

  /* Non-blocking: only drain if Core 1 has already pushed a response */
  if (!multicore_fifo_rvalid()) return;
  multicore_fifo_pop_blocking(); /* discard tag — status already written by Core 1 */
  s_async_pending = false;
}

static void js_dispatch_command(const TransmissionProtocol *proto) {
  DPRINTF("js_dispatch: cmd=0x%04X payload=%u\n",
          proto->command_id, (unsigned)proto->payload_size);
  if (proto->payload_size < 4u) {
    DPRINTF("js_dispatch_command: short payload for 0x%04X\n",
            proto->command_id);
    return;
  }

  uint32_t  random_token = TPROTO_GET_RANDOM_TOKEN(proto->payload);
  uint16_t *payload_ptr  = (uint16_t *)proto->payload;

  switch (proto->command_id) {

    /* ── CMD_JS_PING ──────────────────────────────────────────────────── */
    case CMD_JS_PING: {
      uint32_t pre_seq = s_flush_seq;
      multicore_fifo_push_blocking((uint32_t)FIFO_MSG_PING << FIFO_TAG_SHIFT);
      js_wait_for_core1(pre_seq);
      js_send_response(random_token);
      break;
    }

    /* ── CMD_JS_UPLOAD ────────────────────────────────────────────────── */
    case CMD_JS_UPLOAD: {
      if (proto->payload_size < JS_UPLOAD_HDR_SIZE) {
        js_write_error_response("bad upload payload");
        js_send_response(random_token);
        break;
      }

      /* Skip the 4-byte random token */
      TPROTO_NEXT32_PAYLOAD_PTR(payload_ptr);

      uint32_t chunk_idx32 = TPROTO_GET_PAYLOAD_PARAM32(payload_ptr);
      TPROTO_NEXT32_PAYLOAD_PTR(payload_ptr);
      uint32_t total_chunks32 = TPROTO_GET_PAYLOAD_PARAM32(payload_ptr);
      TPROTO_NEXT32_PAYLOAD_PTR(payload_ptr);
      uint32_t chunk_size32 = TPROTO_GET_PAYLOAD_PARAM32(payload_ptr);
      TPROTO_NEXT32_PAYLOAD_PTR(payload_ptr);
      const uint8_t *data   = (const uint8_t *)payload_ptr;
      uint16_t data_size = proto->payload_size - JS_UPLOAD_HDR_SIZE;

      if ((chunk_idx32 > UINT16_MAX) || (total_chunks32 > UINT16_MAX) ||
          (chunk_size32 > UINT16_MAX)) {
        js_write_error_response("bad upload header");
        js_send_response(random_token);
        break;
      }

      uint16_t chunk_idx = (uint16_t)chunk_idx32;
      uint16_t total_chunks = (uint16_t)total_chunks32;
      uint16_t chunk_size = (uint16_t)chunk_size32;

      if ((total_chunks == 0u) || (chunk_idx >= total_chunks) ||
          (chunk_size > data_size) || (chunk_size > JS_UPLOAD_CHUNK_MAX)) {
        js_write_error_response("bad upload header");
        js_send_response(random_token);
        break;
      }

      uint32_t save = spin_lock_blocking(s_spin_lock);
      if (chunk_idx == 0) {
        s_msg.js_source_len   = 0;
        s_msg.chunks_received = 0;
        s_msg.chunks_expected = total_chunks;
      }
      uint32_t avail = JS_SOURCE_MAX - s_msg.js_source_len;
      if (chunk_size > avail) {
        spin_unlock(s_spin_lock, save);
        js_write_error_response("source too large");
        js_send_response(random_token);
        break;
      }
      /* The protocol parser stores each 16-bit bus word with little-endian
       * layout (ARM strh), but the m68k transmitted the bytes in big-endian
       * order (byte0 in high half of the word). Un-swap pairs while copying
       * so JerryScript sees the original source text. Demo sources are even
       * length; odd-length chunks aren't supported yet — round down. */
      {
        size_t even_size = (size_t)chunk_size & ~(size_t)1u;
        COPY_AND_CHANGE_ENDIANESS_BLOCK16(
            data, s_msg.js_source + s_msg.js_source_len, even_size);
      }
      s_msg.js_source_len += chunk_size;
      s_msg.chunks_received++;
      bool last_chunk = (s_msg.chunks_received >= s_msg.chunks_expected);
      spin_unlock(s_spin_lock, save);

      if (last_chunk) {
        /* All chunks received — tell Core 1 to eval */
        uint32_t pre_seq = s_flush_seq;
        multicore_fifo_push_blocking(
            (uint32_t)FIFO_MSG_UPLOAD << FIFO_TAG_SHIFT);
        js_wait_for_core1(pre_seq);
      } else {
        /* Intermediate chunk ACK — write a partial-OK into result memory */
        const char ack[] = "{\"ok\":true,\"partial\":true}";
        size_t     ack_len = ((sizeof(ack) + 1u) & ~1u);
        COPY_AND_CHANGE_ENDIANESS_BLOCK16((void *)ack,
                                          (void *)s_result_mem,
                                          ack_len);
      }
      js_send_response(random_token);
      break;
    }

    /* ── CMD_JS_CALL ──────────────────────────────────────────────────── */
    case CMD_JS_CALL: {
      /* Reject sync call while an async one is in flight */
      if (s_async_pending) {
        js_write_busy_error();
        js_send_response(random_token);
        break;
      }

      if (proto->payload_size <= JS_CALL_HDR_SIZE) {
        js_write_error_response("bad call payload");
        js_send_response(random_token);
        break;
      }

      const uint8_t *call_payload = proto->payload + JS_CALL_HDR_SIZE;
      size_t call_payload_size = proto->payload_size - JS_CALL_HDR_SIZE;
      if (!js_parse_call_payload(call_payload, call_payload_size)) {
        js_write_error_response("bad call payload");
        js_send_response(random_token);
        break;
      }

      uint32_t pre_seq = s_flush_seq;
      multicore_fifo_push_blocking((uint32_t)FIFO_MSG_CALL << FIFO_TAG_SHIFT);
      js_wait_for_core1(pre_seq);
      js_send_response(random_token);
      break;
    }

    /* ── CMD_JS_RESET ─────────────────────────────────────────────────── */
    case CMD_JS_RESET: {
      uint32_t pre_seq = s_flush_seq;
      multicore_fifo_push_blocking((uint32_t)FIFO_MSG_RESET << FIFO_TAG_SHIFT);
      js_wait_for_core1(pre_seq);
      js_send_response(random_token);
      break;
    }

    /* ── CMD_JS_CALL_ASYNC ───────────────────────────────────────────── */
    case CMD_JS_CALL_ASYNC: {
      /* Reject if a previous async call is still in flight */
      if (s_async_pending) {
        js_write_busy_error();
        js_send_response(random_token);
        break;
      }

      if (proto->payload_size <= JS_CALL_HDR_SIZE) {
        js_write_error_response("bad call payload");
        js_send_response(random_token);
        break;
      }

      const uint8_t *call_payload = proto->payload + JS_CALL_HDR_SIZE;
      size_t call_payload_size = proto->payload_size - JS_CALL_HDR_SIZE;
      if (!js_parse_call_payload(call_payload, call_payload_size)) {
        js_write_error_response("bad call payload");
        js_send_response(random_token);
        break;
      }

      *s_status_mem      = MDJS_BUS_BYTE_WORD(MDJS_STATUS_BUSY);
      s_async_pending    = true;
      s_async_start_us   = time_us_64();

      multicore_fifo_push_blocking((uint32_t)FIFO_MSG_CALL << FIFO_TAG_SHIFT);
      /* ACK the ST immediately — Core 1 will update status when done */
      js_send_response(random_token);
      break;
    }

    /* ── CMD_JS_POLL ─────────────────────────────────────────────────── */
    case CMD_JS_POLL: {
      /* Write current async status as JSON into the result buffer */
      char status_json[32];
      snprintf(status_json, sizeof(status_json),
               "{\"status\":%u}", (unsigned)MDJS_BUS_WORD_BYTE(*s_status_mem));
      size_t slen = ((strnlen(status_json, sizeof(status_json) - 1) + 2u) & ~1u);
      COPY_AND_CHANGE_ENDIANESS_BLOCK16((void *)status_json,
                                        (void *)s_result_mem,
                                        slen);
      js_send_response(random_token);
      break;
    }

    default:
      DPRINTF("js_dispatch_command: unexpected command 0x%04X\n",
              proto->command_id);
      break;
  }
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Public API                                                                 */
/* ────────────────────────────────────────────────────────────────────────── */

void js_worker_init(void) {
  s_rom_base         = (uint32_t)&__rom_in_ram_start__;
  s_token_addr       = s_rom_base + MDJS_RANDOM_TOKEN_OFFSET;
  s_token_seed_addr  = s_rom_base + MDJS_RANDOM_TOKEN_SEED_OFFSET;
  s_result_mem       = (volatile char    *)(s_rom_base + JS_RESULT_OFFSET);
  s_status_mem       = (volatile uint16_t *)(s_rom_base + JS_STATUS_OFFSET);
  s_ready_mem        = (volatile uint16_t *)(s_rom_base + MDJS_READY_OFFSET);
  *s_status_mem      = MDJS_BUS_BYTE_WORD(MDJS_STATUS_IDLE);
  *s_ready_mem       = 0;
  s_async_pending    = false;
  s_async_start_us   = 0;

  /* Initialise spin-lock */
  s_spin_lock = spin_lock_instance(JS_SPINLOCK_ID);
  uint32_t save = spin_lock_blocking(s_spin_lock);
  memset(&s_msg, 0, sizeof(s_msg));
  spin_unlock(s_spin_lock, save);

  /* Seed the random token in shared memory */
  srand((unsigned int)time(NULL));
  uint32_t seed = rand();
  TPROTO_SET_RANDOM_TOKEN(s_token_seed_addr, seed);

  DPRINTF("js_worker_init: launching Core 1\n");
  multicore_launch_core1(core1_entry);
  DPRINTF("js_worker_init: Core 1 launched, polling phase...\n");

  /* Poll Core 1's phase byte for up to 5 seconds and print transitions.
   * This tells us which step in jerry_init hangs without Core 1 calling
   * printf (which deadlocks on newlib's non-dual-core-safe stdio). */
  uint8_t last_phase = 0;
  for (int i = 0; i < 500; i++) {
    uint8_t phase = s_core1_phase;
    if (phase != last_phase) {
      DPRINTF("Core 1 phase -> %u\n", (unsigned)phase);
      last_phase = phase;
      if (phase >= C1_PHASE_READY) break;
    }
    sleep_ms(10);
  }
  if (last_phase < C1_PHASE_READY) {
    DPRINTF("Core 1 STUCK at phase %u\n", (unsigned)last_phase);
  }

  DPRINTF("MD/JS ready. PING=0x%02X UPLOAD=0x%02X CALL=0x%02X RESET=0x%02X"
          " CALL_ASYNC=0x%02X POLL=0x%02X\n",
          CMD_JS_PING, CMD_JS_UPLOAD, CMD_JS_CALL, CMD_JS_RESET,
          CMD_JS_CALL_ASYNC, CMD_JS_POLL);

  /* Drain anything the protocol parser has already latched. Don't reset the
   * parser state from Core 0 — it touches shared variables that the DMA IRQ
   * also writes, and can wedge in-flight frames. */
  TransmissionProtocol drain;
  for (int i = 0; i < 50; i++) {
    while (mdjs_consume_protocol(&drain)) {
      DPRINTF("js_worker_init: draining startup cmd 0x%04X payload=%u\n",
              drain.command_id, (unsigned)drain.payload_size);
    }
    sleep_ms(1);
  }
  s_dispatch_armed = true;
}

void __not_in_flash_func(js_worker_loop)(void) {
  js_drain_async_fifo();

  if (mdjs_consume_protocol(&s_loop_proto)) {
    if (!s_dispatch_armed) {
      DPRINTF("js_worker_loop: dropping pre-arm cmd 0x%04X payload=%u\n",
              s_loop_proto.command_id,
              (unsigned)s_loop_proto.payload_size);
    } else if (s_loop_proto.command_id >= CMD_JS_PING &&
               s_loop_proto.command_id <= CMD_JS_POLL) {
      js_dispatch_command(&s_loop_proto);
    } else {
      DPRINTF("js_worker_loop: ignoring command 0x%04X\n",
              s_loop_proto.command_id);
    }
  }

  /* Drain Core 1's diagnostic byte (set by upload/call handlers). */
  static uint8_t last_diag = 0;
  uint8_t diag = s_core1_diag;
  if (diag != last_diag) {
    DPRINTF("Core 1 diag -> %u\n", (unsigned)diag);
    last_diag = diag;
  }

  /* Drain Core 1's diagnostic message ring. */
  static uint32_t last_diag_seq = 0;
  uint32_t seq = s_core1_diag_seq;
  if (seq != last_diag_seq) {
    DPRINTF("Core 1 msg: %s\n", s_core1_diag_msg);
    last_diag_seq = seq;
  }
}
