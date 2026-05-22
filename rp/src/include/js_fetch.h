/**
 * File: js_fetch.h
 * Description: Native fetch() for the JerryScript MD/JS worker.
 *
 * Registers a global fetch(url) function on the JerryScript context.
 * Call js_fetch_init() from Core 1 after jerry_init().
 *
 * The actual HTTP request is performed by Core 0 (which owns the lwIP async
 * context) via the FIFO_MSG_FETCH_REQ / FIFO_MSG_FETCH_OK/ERR protocol.
 */

#ifndef JS_FETCH_H
#define JS_FETCH_H

/**
 * Register the native fetch() function on the JerryScript global object.
 * Must be called from Core 1 after jerry_init().
 */
void js_fetch_init(void);

#endif /* JS_FETCH_H */
