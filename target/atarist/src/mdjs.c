/**
 * File: mdjs.c
 * Description: MD/JS ST-side client library implementation.
 *
 * Wraps the SidecarTridge low-level command protocol (send_sync /
 * send_sync_write) for use from C code.
 *
 * Register marshalling is implemented in dedicated assembly wrappers
 * (sidecart_stubs.S) to keep C code ABI-safe and avoid inline asm
 * constraint pitfalls on m68k.
 */

#include "mdjs.h"

#include <string.h>

/* C ABI entry points provided by sidecart_stubs.S */
extern int mdjs_send_sync_command(int cmd, int payload_size, long d3, long d4);
extern int mdjs_send_sync_write_command(int cmd, const char *buf, int byte_count,
                                        int chunk_idx, int total_chunks,
                                        int chunk_size);

/* ── Helper: call send_sync for a small (register-payload) command ────────── */
static int call_send_sync(unsigned short cmd, unsigned short payload_size,
                           long d3, long d4)
{
    return mdjs_send_sync_command((int)cmd, (int)payload_size, d3, d4);
}

/* ── Helper: call send_sync_write for a buffer payload ───────────────────── */
static int call_send_sync_write(unsigned short cmd,
                                 const char *buf, unsigned short byte_count,
                                 unsigned short chunk_idx,
                                 unsigned short total_chunks,
                                 unsigned short chunk_size)
{
    return mdjs_send_sync_write_command((int)cmd, buf, (int)byte_count,
                                        (int)chunk_idx, (int)total_chunks,
                                        (int)chunk_size);
}

/* Build CALL payload as func_name\0args_json\0 and return byte length. */
static unsigned short build_call_payload(const char *func, const char *args_json,
                                         char *payload)
{
    int fn_len  = (int)strlen(func);
    int arg_len = (int)strlen(args_json);
    int max_args_len;

    if (fn_len >= JS_CALL_FUNC_NAME_MAX) {
        fn_len = JS_CALL_FUNC_NAME_MAX - 1;
    }

    /* Keep the write body within both the transport and RP-side args buffer. */
    max_args_len = JS_CALL_ARGS_MAX - 1;
    if (max_args_len > JS_UPLOAD_CHUNK_MAX - (fn_len + 2)) {
        max_args_len = JS_UPLOAD_CHUNK_MAX - (fn_len + 2);
    }
    if (max_args_len < 0) {
        max_args_len = 0;
    }
    if (arg_len > max_args_len) {
        arg_len = max_args_len;
    }

    memcpy(payload, func, (unsigned int)fn_len);
    payload[fn_len] = '\0';
    memcpy(payload + fn_len + 1, args_json, (unsigned int)arg_len);
    payload[fn_len + 1 + arg_len] = '\0';

    return (unsigned short)(fn_len + 1 + arg_len + 1);
}

/* ── Read result from MDJS_RESULT_ADDR into a C buffer ─────────────────────── */
/* The RP2040 stores the result with 16-bit byte-swap. We un-swap here.      */
static void read_result(char *buf, int buf_size)
{
    volatile unsigned short *src = (volatile unsigned short *)MDJS_RESULT_ADDR;
    unsigned char *dst = (unsigned char *)buf;
    int max = buf_size - 1;
    int i = 0;

    while (i < max) {
        unsigned short w = *src++;
        /* The RP2040 swapped the bytes (big-endian), so high byte is first */
        unsigned char hi = (unsigned char)(w >> 8);
        unsigned char lo = (unsigned char)(w & 0xFF);
        if (hi == 0) break;
        dst[i++] = hi;
        if (lo == 0) break;
        if (i < max) dst[i++] = lo;
    }
    dst[i] = '\0';
}

/* ── Inter-command settle delay ───────────────────────────────────────────── */
/* Back-to-back protocol commands can race the cartridge bus settle path on
 * a freshly-booted SidecarTridge, causing one or more commands to be lost
 * by the RP's protocol parser. A short delay between commands eliminates
 * the race. ~6ms on an 8 MHz 68000 — imperceptible to humans, with margin.
 *
 * The settle is invoked at the start of each command-emitting function
 * except mdjs_ping (typically the first call after fresh boot, no preceding
 * command to race with) and mdjs_poll (called rapidly in a polling loop;
 * the prior mdjs_call_async will have already settled).
 *
 * Tuning history: 50ms reliable → 25ms reliable → 12ms reliable → 6ms
 * reliable. Stopped here as a sensible floor with safety margin. */
static void mdjs_settle(void)
{
    volatile long i;
    for (i = 0; i < 25000L; i++) { }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int mdjs_ping(void)
{
    /* send_sync adds the 4-byte random token internally. */
    return call_send_sync(CMD_JS_PING, 0, 0L, 0L);
}

int mdjs_upload(const char *js_source)
{
    if (!js_source) {
        return 1;
    }

    mdjs_settle();

    int total        = (int)strlen(js_source);
    int offset       = 0;
    int chunk_idx    = 0;
    int total_chunks = (total + JS_UPLOAD_CHUNK_MAX - 1) / JS_UPLOAD_CHUNK_MAX;
    if (total_chunks == 0) total_chunks = 1;

    do {
        int remaining  = total - offset;
        int chunk_size = (remaining > JS_UPLOAD_CHUNK_MAX)
                         ? JS_UPLOAD_CHUNK_MAX : remaining;

        int err = call_send_sync_write(
            CMD_JS_UPLOAD,
            js_source + offset,
            (unsigned short)chunk_size,
            (unsigned short)chunk_idx,
            (unsigned short)total_chunks,
            (unsigned short)chunk_size);

        if (err != 0) return err;

        offset    += chunk_size;
        chunk_idx += 1;
    } while (offset < total);
    return 0;
}

int mdjs_call(const char *func, const char *args_json,
              char *result, int result_size)
{
    if (!func || !args_json) {
        return 1;
    }

    mdjs_settle();

    char payload[JS_UPLOAD_CHUNK_MAX];
    unsigned short body_len = build_call_payload(func, args_json, payload);
    int err = call_send_sync_write(CMD_JS_CALL, payload, body_len, 0, 1, body_len);
    if (err != 0) return err;

    if (result && result_size > 0) {
        read_result(result, result_size);
    }
    return 0;
}

int mdjs_reset(void)
{
    mdjs_settle();
    return call_send_sync(CMD_JS_RESET, 0, 0L, 0L);
}

int mdjs_call_async(const char *func, const char *args_json)
{
    if (!func || !args_json) {
        return 1;
    }

    /* Bail immediately if a previous async call is still running */
    if (mdjs_status() == MDJS_STATUS_BUSY)
        return MDJS_STATUS_BUSY;

    mdjs_settle();

    char payload[JS_UPLOAD_CHUNK_MAX];
    unsigned short body_len = build_call_payload(func, args_json, payload);
    return call_send_sync_write(CMD_JS_CALL_ASYNC, payload, body_len,
                                0, 1, body_len);
}

int mdjs_result(char *result, int result_size)
{
    if (!result || result_size <= 0)
        return 1;

    read_result(result, result_size);
    return 0;
}

unsigned char mdjs_status(void)
{
    return *MDJS_STATUS_ADDR;
}

int mdjs_poll(void)
{
    int err = call_send_sync(CMD_JS_POLL, 0, 0L, 0L);
    if (err != 0) return err;
    return (int)mdjs_status();
}
