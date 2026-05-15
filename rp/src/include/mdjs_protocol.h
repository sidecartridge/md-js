/**
 * File: mdjs_protocol.h
 * Description: Minimal tprotocol bridge for MD/JS command ingestion.
 */

#ifndef MDJS_PROTOCOL_H
#define MDJS_PROTOCOL_H

#include <stdbool.h>

#include "pico.h"
#include "tprotocol.h"

/* Shared ROM-in-RAM offsets used by the ST protocol helpers. */
#define MDJS_RANDOM_TOKEN_OFFSET      0xF000
#define MDJS_RANDOM_TOKEN_SEED_OFFSET (MDJS_RANDOM_TOKEN_OFFSET + 4)
#define MDJS_READY_OFFSET             0xF00A
#define MDJS_READY_MAGIC              0x4A

/**
 * @brief DMA lookup IRQ callback used by ROM emulation.
 *
 * Feeds 16-bit words into tprotocol_parse() and publishes completed commands.
 */
void __not_in_flash_func(mdjs_dma_irq_handler_lookup)(void);

/**
 * @brief Atomically consume the latest decoded protocol command.
 *
 * @param out Destination for the decoded command snapshot.
 * @return true if a command was available, false otherwise.
 */
bool __not_in_flash_func(mdjs_consume_protocol)(TransmissionProtocol *out);

#endif  /* MDJS_PROTOCOL_H */
