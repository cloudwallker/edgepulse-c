#include "ep_protocol.h"
#include <string.h>

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u));
}

static uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
           ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
    bytes[2] = (uint8_t)(value >> 16u);
    bytes[3] = (uint8_t)(value >> 24u);
}

uint16_t ep_crc16(const uint8_t *bytes, size_t len)
{
    uint16_t crc = 0xffffu;
    size_t i;
    for (i = 0u; i < len; ++i) {
        unsigned bit;
        crc ^= (uint16_t)((uint16_t)bytes[i] << 8u);
        for (bit = 0u; bit < 8u; ++bit) {
            crc = (uint16_t)((crc & 0x8000u) != 0u ?
                  ((uint32_t)crc << 1u) ^ 0x1021u : (uint32_t)crc << 1u);
        }
    }
    return crc;
}

size_t ep_encode(const ep_frame *frame, uint8_t *out, size_t capacity)
{
    size_t total;
    if (frame == NULL || out == NULL || frame->len > EP_PAYLOAD_MAX) return 0u;
    total = (size_t)frame->len + 18u;
    if (capacity < total) return 0u;
    out[0] = 0xa5u; out[1] = 0x5au; out[2] = 1u; out[3] = frame->type;
    write_u16(out + 4u, frame->device);
    write_u32(out + 6u, frame->boot);
    write_u32(out + 10u, frame->seq);
    write_u16(out + 14u, frame->len);
    memcpy(out + 16u, frame->payload, frame->len);
    write_u16(out + total - 2u, ep_crc16(out + 2u, total - 4u));
    return total;
}

void ep_parser_init(ep_parser *parser, ep_frame_fn emit, void *ctx)
{
    memset(parser, 0, sizeof(*parser));
    parser->emit = emit;
    parser->ctx = ctx;
}

static void consume(ep_parser *parser, size_t count, int discard)
{
    parser->used -= count;
    memmove(parser->bytes, parser->bytes + count, parser->used);
    if (discard != 0) parser->discarded += (uint32_t)count;
}

/* A failed candidate consumes only its first byte, so a valid sync sequence
 * anywhere inside damaged data is reconsidered. The bounded maximum frame
 * makes this rescan's memory and per-byte work bounded as well. */
static void scan(ep_parser *parser, int flush)
{
    while (parser->used != 0u) {
        uint16_t payload_len;
        size_t total;
        ep_frame frame;
        if (parser->bytes[0] != 0xa5u) {
            consume(parser, 1u, 1);
            continue;
        }
        if (parser->used < 2u) {
            if (flush != 0) consume(parser, 1u, 1);
            return;
        }
        if (parser->bytes[1] != 0x5au) {
            consume(parser, 1u, 1);
            continue;
        }
        if (parser->used < 3u) {
            if (flush == 0) return;
            ++parser->rejected;
            consume(parser, 1u, 1);
            continue;
        }
        if (parser->bytes[2] != 1u) {
            ++parser->rejected;
            consume(parser, 1u, 1);
            continue;
        }
        if (parser->used < 16u) {
            if (flush == 0) return;
            ++parser->rejected;
            consume(parser, 1u, 1);
            continue;
        }
        payload_len = read_u16(parser->bytes + 14u);
        if (payload_len > EP_PAYLOAD_MAX) {
            ++parser->rejected;
            consume(parser, 1u, 1);
            continue;
        }
        total = (size_t)payload_len + 18u;
        if (parser->used < total) {
            if (flush == 0) return;
            ++parser->rejected;
            consume(parser, 1u, 1);
            continue;
        }
        if (ep_crc16(parser->bytes + 2u, total - 4u) !=
            read_u16(parser->bytes + total - 2u)) {
            ++parser->rejected;
            consume(parser, 1u, 1);
            continue;
        }
        memset(&frame, 0, sizeof(frame));
        frame.type = parser->bytes[3];
        frame.device = read_u16(parser->bytes + 4u);
        frame.boot = read_u32(parser->bytes + 6u);
        frame.seq = read_u32(parser->bytes + 10u);
        frame.len = payload_len;
        memcpy(frame.payload, parser->bytes + 16u, payload_len);
        consume(parser, total, 0);
        ++parser->accepted;
        if (parser->emit != NULL) parser->emit(parser->ctx, &frame);
    }
}

void ep_parser_feed(ep_parser *parser, const uint8_t *bytes, size_t len)
{
    size_t i;
    for (i = 0u; i < len; ++i) {
        /* scan always accepts/rejects a candidate once all 82 bytes arrive;
         * therefore used is strictly smaller than EP_FRAME_MAX here. */
        parser->bytes[parser->used++] = bytes[i];
        scan(parser, 0);
    }
}

void ep_parser_flush(ep_parser *parser)
{
    scan(parser, 1);
}
