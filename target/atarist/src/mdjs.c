/**
 * File: mdjs.c
 * Description: MD/JS ST-side client library implementation.
 *
 * Wraps the SidecarTridge low-level command protocol (send_sync /
 * send_sync_write) for use from C code.
 *
 * The underlying assembly routines (send_sync_command_to_sidecart,
 * send_sync_write_command_to_sidecart) use a register-based ABI:
 *   D0.W  = command code
 *   D1.W  = payload size in bytes
 *   D3–D6 = payload words (for small payloads)
 *   A4    = buffer pointer (for write commands)
 *   D6.W  = write byte count (for write commands)
 * Return value: D0.W = 0 on success, non-zero on timeout/error.
 *
 * GCC m68k passes function arguments on the stack and returns in D0, so
 * we use inline __asm__ blocks to marshal registers explicitly.
 */

#include "mdjs.h"

#include <string.h>

/* Entry points provided by sidecart_stubs.S (GAS-assembled alongside this file) */
extern int send_sync_command_to_sidecart(void);
extern int send_sync_write_command_to_sidecart(void);

/* ── Helper: call send_sync for a small (register-payload) command ────────── */
/* Sets D0=cmd, D1=payload_size, D3=d3, D4=d4. Returns D0 (0=ok). */
static int call_send_sync(unsigned short cmd, unsigned short payload_size,
                           long d3, long d4)
{
    register int result __asm__("d0");
    register unsigned short r_cmd  __asm__("d0") = cmd;
    register unsigned short r_size __asm__("d1") = payload_size;
    register long r_d3             __asm__("d3") = d3;
    register long r_d4             __asm__("d4") = d4;

    __asm__ volatile (
        "jsr send_sync_command_to_sidecart"
        : "=d" (result)
        : "d" (r_cmd), "d" (r_size), "d" (r_d3), "d" (r_d4)
        : "d2", "d5", "d6", "d7", "a0", "a1", "a2", "a3", "cc", "memory"
    );
    return result;
}

/* ── Helper: call send_sync_write for a buffer payload ───────────────────── */
/* Sets D0=cmd, A4=buf, D6.W=byte_count, D3=chunk_idx, D4=total_chunks,     */
/* D5=chunk_size. Returns D0 (0=ok).                                         */
static int call_send_sync_write(unsigned short cmd,
                                 const char *buf, unsigned short byte_count,
                                 unsigned short chunk_idx,
                                 unsigned short total_chunks,
                                 unsigned short chunk_size)
{
    register int result      __asm__("d0");
    register unsigned short r_cmd   __asm__("d0") = cmd;
    register unsigned short r_count __asm__("d6") = byte_count;
    register long r_d3              __asm__("d3") = (long)chunk_idx;
    register long r_d4              __asm__("d4") = (long)total_chunks;
    register long r_d5              __asm__("d5") = (long)chunk_size;
    register const char *r_a4      __asm__("a4") = buf;

    __asm__ volatile (
        "jsr send_sync_write_command_to_sidecart"
        : "=d" (result)
        : "d" (r_cmd), "d" (r_count), "d" (r_d3), "d" (r_d4),
          "d" (r_d5), "a" (r_a4)
        : "d1", "d2", "d7", "a0", "a1", "a2", "a3", "cc", "memory"
    );
    return result;
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

/* ── Public API ─────────────────────────────────────────────────────────── */

int mdjs_ping(void)
{
    /* CMD_JS_PING: payload = random token (4 bytes). D1=4, D3=D4=0. */
    return call_send_sync(CMD_JS_PING, 4, 0L, 0L);
}

int mdjs_upload(const char *js_source)
{
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
    /* Payload buffer: func_name\0args_json\0
     * func capped at JS_CALL_FUNC_NAME_MAX-1 (63); args fill the rest. */
    char payload[JS_CALL_FUNC_NAME_MAX + JS_RESULT_SIZE];
    int fn_len  = (int)strlen(func);
    int arg_len = (int)strlen(args_json);

    if (fn_len  >= JS_CALL_FUNC_NAME_MAX)  fn_len  = JS_CALL_FUNC_NAME_MAX - 1;
    if (arg_len >= JS_RESULT_SIZE)         arg_len = JS_RESULT_SIZE - 1;

    memcpy(payload, func, (unsigned int)fn_len);
    payload[fn_len] = '\0';
    memcpy(payload + fn_len + 1, args_json, (unsigned int)arg_len);
    payload[fn_len + 1 + arg_len] = '\0';

    unsigned short body_len = (unsigned short)(fn_len + 1 + arg_len + 1);
    int err = call_send_sync_write(CMD_JS_CALL, payload, body_len, 0, 1, body_len);
    if (err != 0) return err;

    if (result && result_size > 0) {
        read_result(result, result_size);
    }
    return 0;
}

int mdjs_reset(void)
{
    return call_send_sync(CMD_JS_RESET, 4, 0L, 0L);
}

int mdjs_call_async(const char *func, const char *args_json)
{
    /* Bail immediately if a previous async call is still running */
    if (mdjs_status() == MDJS_STATUS_BUSY)
        return MDJS_STATUS_BUSY;

    /* Build payload identically to mdjs_call() */
    char payload[JS_CALL_FUNC_NAME_MAX + JS_RESULT_SIZE];
    int fn_len  = (int)strlen(func);
    int arg_len = (int)strlen(args_json);

    if (fn_len  >= JS_CALL_FUNC_NAME_MAX)  fn_len  = JS_CALL_FUNC_NAME_MAX - 1;
    if (arg_len >= JS_RESULT_SIZE)         arg_len = JS_RESULT_SIZE - 1;

    memcpy(payload, func, (unsigned int)fn_len);
    payload[fn_len] = '\0';
    memcpy(payload + fn_len + 1, args_json, (unsigned int)arg_len);
    payload[fn_len + 1 + arg_len] = '\0';

    unsigned short body_len = (unsigned short)(fn_len + 1 + arg_len + 1);
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
    int err = call_send_sync(CMD_JS_POLL, 4, 0L, 0L);
    if (err != 0) return err;
    return (int)mdjs_status();
}

