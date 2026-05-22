/**
 * File: js_fetch.c
 * Description: Native fetch() implementation for the MD/JS JerryScript worker.
 *
 * Exposes a minimal fetch(url) to JavaScript. HTTP only, text/json response.
 * The HTTP request runs on Core 0 (which owns the lwIP async context); Core 1
 * signals it via FIFO and blocks until done.
 *
 * JS usage:
 *   const response = await fetch("http://host/path");
 *   if (response.ok) {
 *     const text = await response.text();
 *     const obj  = await response.json();
 *   }
 */

#include "js_fetch.h"
#include "js_worker.h"

#include <string.h>

#include "jerryscript.h"
#include "hardware/sync.h"
#include "pico/multicore.h"

/* Use the shared message block and spin-lock exported from js_worker.c */
#define s_msg       js_worker_msg
#define s_spin_lock js_worker_spin_lock

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static jerry_value_t make_resolved_promise(jerry_value_t value) {
  jerry_value_t p = jerry_promise();
  jerry_value_t resolve_result = jerry_promise_resolve(p, value);
  jerry_value_free(resolve_result);
  return p;
}

static jerry_value_t make_rejected_promise(const char *msg) {
  jerry_value_t err = jerry_string_sz(msg);
  jerry_value_t p   = jerry_promise();
  jerry_value_t rej = jerry_promise_reject(p, err);
  jerry_value_free(rej);
  jerry_value_free(err);
  return p;
}

/* ── Response method: text() ─────────────────────────────────────────────── */

static jerry_value_t response_text_handler(
    const jerry_call_info_t *call_info,
    const jerry_value_t args[], jerry_length_t argc) {
  (void)args; (void)argc;

  /* Retrieve body string stored as internal property on 'this' */
  jerry_value_t this_val  = call_info->this_value;
  jerry_value_t body_key  = jerry_string_sz("_body");
  jerry_value_t body_val  = jerry_object_get(this_val, body_key);
  jerry_value_free(body_key);

  jerry_value_t p = make_resolved_promise(body_val);
  jerry_value_free(body_val);
  return p;
}

/* ── Response method: json() ─────────────────────────────────────────────── */

static jerry_value_t response_json_handler(
    const jerry_call_info_t *call_info,
    const jerry_value_t args[], jerry_length_t argc) {
  (void)args; (void)argc;

  jerry_value_t this_val  = call_info->this_value;
  jerry_value_t body_key  = jerry_string_sz("_body");
  jerry_value_t body_str  = jerry_object_get(this_val, body_key);
  jerry_value_free(body_key);

  /* Convert jerry string → C buffer → jerry_json_parse */
  jerry_size_t len = jerry_string_size(body_str, JERRY_ENCODING_UTF8);
  jerry_char_t *buf = (jerry_char_t *)alloca(len + 1);
  jerry_string_to_buffer(body_str, JERRY_ENCODING_UTF8, buf, len);
  buf[len] = '\0';
  jerry_value_free(body_str);

  jerry_value_t parsed = jerry_json_parse(buf, len);
  jerry_value_t p;
  if (jerry_value_is_exception(parsed)) {
    jerry_value_free(parsed);
    p = make_rejected_promise("JSON parse error");
  } else {
    p = make_resolved_promise(parsed);
    jerry_value_free(parsed);
  }
  return p;
}

/* ── Build JS Response object ────────────────────────────────────────────── */

static jerry_value_t build_response(bool ok, uint16_t status,
                                    const char *status_text, const char *url,
                                    bool redirected, jerry_value_t v_body) {
  jerry_value_t obj = jerry_object();

  jerry_value_t v_ok         = jerry_boolean(ok);
  jerry_value_t v_status     = jerry_number((double)status);
  jerry_value_t v_status_txt = jerry_string_sz(status_text ? status_text : "");
  jerry_value_t v_url        = jerry_string_sz(url ? url : "");
  jerry_value_t v_redirected = jerry_boolean(redirected);
  jerry_value_t v_type       = jerry_string_sz("basic");
  jerry_value_t v_body_used  = jerry_boolean(false);

  jerry_object_set_sz(obj, "ok",          v_ok);
  jerry_object_set_sz(obj, "status",      v_status);
  jerry_object_set_sz(obj, "statusText",  v_status_txt);
  jerry_object_set_sz(obj, "url",         v_url);
  jerry_object_set_sz(obj, "redirected",  v_redirected);
  jerry_object_set_sz(obj, "type",        v_type);
  jerry_object_set_sz(obj, "bodyUsed",    v_body_used);
  jerry_object_set_sz(obj, "_body",       v_body);

  jerry_value_free(v_ok);
  jerry_value_free(v_status);
  jerry_value_free(v_status_txt);
  jerry_value_free(v_url);
  jerry_value_free(v_redirected);
  jerry_value_free(v_type);
  jerry_value_free(v_body_used);

  jerry_value_t text_fn = jerry_function_external(response_text_handler);
  jerry_value_t json_fn = jerry_function_external(response_json_handler);
  jerry_object_set_sz(obj, "text", text_fn);
  jerry_object_set_sz(obj, "json", json_fn);
  jerry_value_free(text_fn);
  jerry_value_free(json_fn);

  return obj;
}

/* ── Native fetch() handler (runs on Core 1) ─────────────────────────────── */

static jerry_value_t fetch_handler(
    const jerry_call_info_t *call_info,
    const jerry_value_t args[], jerry_length_t argc) {
  (void)call_info;

  if (argc < 1 || !jerry_value_is_string(args[0])) {
    return make_rejected_promise("fetch: URL must be a string");
  }

  /* Extract URL */
  jerry_size_t url_len = jerry_string_size(args[0], JERRY_ENCODING_UTF8);
  if (url_len >= sizeof(s_msg.fetch_url)) {
    return make_rejected_promise("fetch: URL too long");
  }
  uint32_t save = spin_lock_blocking(s_spin_lock);
  jerry_string_to_buffer(args[0], JERRY_ENCODING_UTF8,
                         (jerry_char_t *)s_msg.fetch_url, url_len);
  s_msg.fetch_url[url_len] = '\0';
  spin_unlock(s_spin_lock, save);

  /* Ask Core 0 to perform the HTTP request */
  multicore_fifo_push_blocking((uint32_t)FIFO_MSG_FETCH_REQ << FIFO_TAG_SHIFT);

  /* Block until Core 0 replies */
  uint32_t resp = multicore_fifo_pop_blocking();
  uint8_t  tag  = (uint8_t)((resp & FIFO_TAG_MASK) >> FIFO_TAG_SHIFT);

  if (tag != FIFO_MSG_FETCH_OK) {
    jerry_value_t empty    = jerry_string_sz("");
    jerry_value_t err_resp = build_response(false, 0, "", s_msg.fetch_url,
                                            false, empty);
    jerry_value_free(empty);
    jerry_value_t p = make_resolved_promise(err_resp);
    jerry_value_free(err_resp);
    return p;
  }

  save = spin_lock_blocking(s_spin_lock);
  bool     ok          = s_msg.fetch_ok;
  uint16_t status      = s_msg.fetch_status;
  bool     redirected  = s_msg.fetch_redirected;
  char     url_snap[sizeof(s_msg.fetch_url)];
  char     st_snap[sizeof(s_msg.fetch_status_text)];
  memcpy(url_snap, s_msg.fetch_url,         sizeof(url_snap));
  memcpy(st_snap,  s_msg.fetch_status_text, sizeof(st_snap));
  jerry_size_t blen = (jerry_size_t)strnlen(s_msg.fetch_body,
                                            sizeof(s_msg.fetch_body) - 1);
  /* Build the jerry string directly from s_msg.fetch_body while under the
     spin-lock so no other core can overwrite it before jerry copies the data. */
  jerry_value_t v_body = jerry_string((const jerry_char_t *)s_msg.fetch_body,
                                      (jerry_size_t)blen,
                                      JERRY_ENCODING_UTF8);
  spin_unlock(s_spin_lock, save);

  jerry_value_t response_obj = build_response(ok, status, st_snap, url_snap,
                                              redirected, v_body);
  jerry_value_free(v_body);

  jerry_value_t p = make_resolved_promise(response_obj);
  jerry_value_free(response_obj);
  return p;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void js_fetch_init(void) {
  jerry_value_t global = jerry_current_realm();
  jerry_value_t fn     = jerry_function_external(fetch_handler);
  jerry_value_t key    = jerry_string_sz("fetch");
  jerry_object_set(global, key, fn);
  jerry_value_free(key);
  jerry_value_free(fn);
  jerry_value_free(global);
}
