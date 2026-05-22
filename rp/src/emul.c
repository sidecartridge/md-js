/**
 * File: emul.c
 * Description: MD/JS runtime bootstrap (ROM emulation + JS worker loop).
 */

#include "emul.h"

#include <stdint.h>

#include "debug.h"
#include "js_worker.h"
#include "mdjs_protocol.h"
#include "memfunc.h"
#include "pico/stdlib.h"
#include "romemul.h"
#include "target_firmware.h"

#if !MDJS_NO_NETWORK
#include "network.h"
#endif

#define SLEEP_LOOP_MS 10

void emul_start(void) {
  /* Copy the ST-side cartridge program into ROM-in-RAM. */
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);

  /* Serve cartridge bus requests using the protocol-only DMA lookup handler. */
  init_romemul(NULL, mdjs_dma_irq_handler_lookup, false);

#if !MDJS_NO_NETWORK
  /* Initialise WiFi in STA mode and connect using credentials from config. */
  network_wifiInit(WIFI_MODE_STA);
  network_wifiStaConnect();
#endif

  /* Launch Core 1 JerryScript worker. */
  js_worker_init();

  DPRINTF(
      "MD/JS ready. PING=0x10 UPLOAD=0x11 CALL=0x12 RESET=0x13 "
      "CALL_ASYNC=0x14 POLL=0x15\n");

  while (true) {
    js_worker_loop();
    sleep_ms(SLEEP_LOOP_MS);
  }
}
