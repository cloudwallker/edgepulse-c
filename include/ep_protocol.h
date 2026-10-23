#ifndef EP_PROTOCOL_H
#define EP_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>
#define EP_PAYLOAD_MAX 64u
#define EP_FRAME_MAX 82u
typedef struct { uint8_t type; uint16_t device; uint32_t boot, seq; uint16_t len; uint8_t payload[EP_PAYLOAD_MAX]; } ep_frame;
/* Called synchronously with a temporary frame. Copy it to retain it. The
 * callback must not re-enter or modify this parser; parser use is serialized. */
typedef void (*ep_frame_fn)(void *ctx, const ep_frame *frame);
/* accepted: complete valid frames, even with a NULL callback.
 * rejected: A5 5A candidates rejected for version, length, CRC, or incomplete
 * data at flush. Overlapping sync candidates are counted separately.
 * discarded: bytes removed that were not part of accepted frames.
 * Counters wrap modulo 2^32; bytes still pending are tracked by used.
 * Initialize before use; do not modify the parser fields between calls. */
typedef struct { uint8_t bytes[EP_FRAME_MAX]; size_t used; uint32_t accepted, rejected, discarded; ep_frame_fn emit; void *ctx; } ep_parser;
/* CRC16-CCITT-FALSE: polynomial 0x1021, init 0xffff, no reflection/xorout.
 * bytes must point to at least len bytes (NULL is valid only when len == 0). */
uint16_t ep_crc16(const uint8_t *bytes, size_t len);
/* Returns wire length, or 0 for NULL arguments, invalid len or short capacity.
 * No output is modified on failure. Input/output storage must not overlap. */
size_t ep_encode(const ep_frame *frame, uint8_t *out, size_t capacity);
void ep_parser_init(ep_parser *parser, ep_frame_fn emit, void *ctx);
/* Buffer pointers must be valid for len bytes; must not alias parser storage. */
void ep_parser_feed(ep_parser *parser, const uint8_t *bytes, size_t len);
/* Invoke at a caller-defined idle/session boundary. Accept complete valid
 * frames hidden behind a truncated candidate, discard incomplete remainders.
 * Do not call between fragments of a frame that is still arriving. */
void ep_parser_flush(ep_parser *parser);
#endif
