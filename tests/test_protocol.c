#include "ep_protocol.h"
#ifdef NDEBUG
#error "Tests require assertions; do not define NDEBUG"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(EXIT_FAILURE); } } while (0)

/* Hand-serialized little-endian fields; CRC independently checked with
 * Python binascii.crc_hqx(data[2:-2], 0xffff), not ep_encode(). */
static const uint8_t fixture[] = {
    0xa5,0x5a,0x01,0x02,0x34,0x12,0x04,0x03,0x02,0x01,
    0x44,0x33,0x22,0x11,0x03,0x00,0x10,0xa5,0x5a,0x91,0x88
};
static const uint8_t empty_fixture[] = {
    0xa5,0x5a,0x01,0x01,0,0,0,0,0,0,0,0,0,0,0,0,0x68,0x97
};

typedef struct { unsigned count; ep_frame last; } capture;
static void capture_frame(void *ctx, const ep_frame *frame)
{
    capture *cap = ctx;
    ++cap->count;
    cap->last = *frame;
}

/* Wrong polynomial, initialization or bit order must fail the known vector. */
static void test_crc(void)
{
    static const uint8_t check[] = "123456789";
    CHECK(ep_crc16(check, 9u) == 0x29b1u);
    CHECK(ep_crc16(check, 0u) == 0xffffu);
    CHECK(ep_crc16(fixture + 2u, sizeof(fixture) - 4u) == 0x8891u);
}

/* Wrong wire byte order, payload length or capacity handling must fail. */
static void test_encode(void)
{
    ep_frame frame = {0};
    uint8_t wire[EP_FRAME_MAX + 1u];
    uint8_t before[sizeof(wire)];
    frame.type = 2u; frame.device = 0x1234u; frame.boot = 0x01020304u;
    frame.seq = 0x11223344u; frame.len = 3u;
    memcpy(frame.payload, fixture + 16u, 3u);
    memset(wire, 0xacu, sizeof(wire));
    CHECK(ep_encode(&frame, wire, sizeof(wire)) == sizeof(fixture));
    CHECK(memcmp(wire, fixture, sizeof(fixture)) == 0);
    CHECK(wire[sizeof(fixture)] == 0xacu);
    memcpy(before, wire, sizeof(wire));
    CHECK(ep_encode(&frame, wire, sizeof(fixture) - 1u) == 0u);
    CHECK(memcmp(wire, before, sizeof(wire)) == 0);
    frame.len = EP_PAYLOAD_MAX + 1u;
    CHECK(ep_encode(&frame, wire, sizeof(wire)) == 0u);
    CHECK(memcmp(wire, before, sizeof(wire)) == 0);
    CHECK(ep_encode(NULL, wire, sizeof(wire)) == 0u);
    CHECK(ep_encode(&frame, NULL, sizeof(wire)) == 0u);
    memset(&frame, 0, sizeof(frame)); frame.type = 1u;
    CHECK(ep_encode(&frame, wire, sizeof(wire)) == sizeof(empty_fixture));
    CHECK(memcmp(wire, empty_fixture, sizeof(empty_fixture)) == 0);
}

static void check_fixture(const ep_frame *frame)
{
    CHECK(frame->type == 2u); CHECK(frame->device == 0x1234u);
    CHECK(frame->boot == 0x01020304u); CHECK(frame->seq == 0x11223344u);
    CHECK(frame->len == 3u); CHECK(memcmp(frame->payload, fixture + 16u, 3u) == 0);
}

/* Premature callbacks, dropped coalesced frames and bad decoding fail here. */
static void test_fragmentation(void)
{
    size_t split;
    for (split = 0u; split <= sizeof(fixture); ++split) {
        ep_parser parser;
        capture cap = {0};
        ep_parser_init(&parser, capture_frame, &cap);
        ep_parser_feed(&parser, fixture, split);
        CHECK(cap.count == (split == sizeof(fixture) ? 1u : 0u));
        ep_parser_feed(&parser, fixture + split, sizeof(fixture) - split);
        CHECK(cap.count == 1u); check_fixture(&cap.last);
        CHECK(parser.used == 0u); CHECK(parser.accepted == 1u);
        CHECK(parser.rejected == 0u); CHECK(parser.discarded == 0u);
    }
    {
        uint8_t wire[sizeof(fixture) * 3u];
        ep_parser parser;
        capture cap = {0};
        for (split = 0u; split < 3u; ++split) {
            memcpy(wire + split * sizeof(fixture), fixture, sizeof(fixture));
        }
        ep_parser_init(&parser, capture_frame, &cap);
        ep_parser_feed(&parser, wire, sizeof(wire));
        CHECK(cap.count == 3u); CHECK(parser.accepted == 3u);
        CHECK(parser.discarded == 0u); CHECK(parser.rejected == 0u);
        check_fixture(&cap.last);
    }
}

/* A rejected candidate must not consume a following valid frame. */
static void test_bad_candidates(void)
{
    unsigned bad;
    for (bad = 0u; bad < 3u; ++bad) {
        uint8_t wire[sizeof(empty_fixture)];
        ep_parser parser;
        capture cap = {0};
        memcpy(wire, empty_fixture, sizeof(wire));
        if (bad == 0u) wire[sizeof(wire) - 1u] ^= 1u;
        if (bad == 1u) wire[14] = 65u;
        if (bad == 2u) wire[2] = 2u;
        ep_parser_init(&parser, capture_frame, &cap);
        ep_parser_feed(&parser, wire, sizeof(wire));
        ep_parser_feed(&parser, fixture, sizeof(fixture));
        CHECK(cap.count == 1u); check_fixture(&cap.last);
        CHECK(parser.rejected == 1u); CHECK(parser.discarded == sizeof(wire));
        CHECK(parser.used == 0u);
    }
    {
        const uint8_t noise[] = {0u,0x5au,0xa5u,0xa5u};
        ep_parser parser;
        capture cap = {0};
        ep_parser_init(&parser, capture_frame, &cap);
        ep_parser_feed(&parser, noise, sizeof(noise));
        ep_parser_feed(&parser, fixture, sizeof(fixture));
        CHECK(cap.count == 1u); CHECK(parser.discarded == sizeof(noise));
        CHECK(parser.rejected == 0u); CHECK(parser.used == 0u);
    }
}

/* A bogus in-range length can hide a complete frame: idle flush must
 * resynchronize byte-by-byte instead of dropping the entire buffer. */
static void test_flush_recovers_nested_frame(void)
{
    uint8_t prefix[16] = {0xa5u,0x5au,1u,3u,0,0,0,0,0,0,0,0,0,0,64u,0};
    ep_parser parser;
    capture cap = {0};
    ep_parser_init(&parser, capture_frame, &cap);
    ep_parser_feed(&parser, prefix, sizeof(prefix));
    ep_parser_feed(&parser, fixture, sizeof(fixture));
    CHECK(cap.count == 0u);
    ep_parser_flush(&parser);
    CHECK(cap.count == 1u); check_fixture(&cap.last);
    CHECK(parser.used == 0u); CHECK(parser.accepted == 1u);
    CHECK(parser.rejected == 1u); CHECK(parser.discarded == sizeof(prefix));
    ep_parser_flush(&parser);
    CHECK(cap.count == 1u); CHECK(parser.rejected == 1u);
}

/* Truncated frames must not leak into the next receive session. */
static void test_all_truncations(void)
{
    size_t cut;
    for (cut = 1u; cut < sizeof(empty_fixture); ++cut) {
        ep_parser parser;
        capture cap = {0};
        ep_parser_init(&parser, capture_frame, &cap);
        ep_parser_feed(&parser, empty_fixture, cut);
        ep_parser_flush(&parser);
        CHECK(cap.count == 0u); CHECK(parser.used == 0u);
        CHECK(parser.discarded == cut);
        CHECK(parser.rejected == (cut >= 2u ? 1u : 0u));
        ep_parser_feed(&parser, fixture, sizeof(fixture));
        CHECK(cap.count == 1u); check_fixture(&cap.last);
    }
}

typedef struct { uint32_t count; } ledger;
static uint8_t sample_byte(uint32_t seq, size_t offset)
{
    return (uint8_t)((seq * 17u + (uint32_t)offset * 31u) & 0xffu);
}
static void verify_campaign_frame(void *ctx, const ep_frame *frame)
{
    ledger *book = ctx;
    size_t i;
    CHECK(frame->seq == book->count);
    CHECK(frame->type == 7u); CHECK(frame->device == 0xabcdu);
    CHECK(frame->boot == 0xf1234567u);
    CHECK(frame->len == book->count % 65u);
    for (i = 0u; i < frame->len; ++i) CHECK(frame->payload[i] == sample_byte(book->count, i));
    ++book->count;
}

static uint32_t next_random(uint32_t *state)
{
    *state ^= *state << 13u; *state ^= *state >> 17u; *state ^= *state << 5u;
    return *state;
}

/* Loss, duplicate delivery, sequence corruption or a 64-byte payload overrun
 * fails the independent ledger/canary, including every legal payload size. */
static void test_ten_thousand_frames(void)
{
    struct { uint8_t before[8]; ep_parser parser; uint8_t after[8]; } guarded;
    ledger book = {0};
    uint32_t seq, rng = 0x20261023u;
    memset(&guarded, 0xacu, sizeof(guarded));
    ep_parser_init(&guarded.parser, verify_campaign_frame, &book);
    for (seq = 0u; seq < 10000u; ++seq) {
        ep_frame frame = {0};
        uint8_t wire[EP_FRAME_MAX];
        size_t i, len, offset = 0u;
        frame.type = 7u; frame.device = 0xabcdu; frame.boot = 0xf1234567u;
        frame.seq = seq; frame.len = (uint16_t)(seq % 65u);
        for (i = 0u; i < frame.len; ++i) frame.payload[i] = sample_byte(seq, i);
        len = ep_encode(&frame, wire, sizeof(wire));
        CHECK(len == (size_t)frame.len + 18u);
        while (offset < len) {
            size_t chunk = (size_t)(next_random(&rng) % 23u) + 1u;
            if (chunk > len - offset) chunk = len - offset;
            ep_parser_feed(&guarded.parser, wire + offset, chunk);
            CHECK(guarded.parser.used <= EP_FRAME_MAX);
            offset += chunk;
        }
        for (i = 0u; i < sizeof(guarded.before); ++i) {
            CHECK(guarded.before[i] == 0xacu); CHECK(guarded.after[i] == 0xacu);
        }
    }
    ep_parser_flush(&guarded.parser);
    CHECK(book.count == 10000u); CHECK(guarded.parser.accepted == 10000u);
    CHECK(guarded.parser.rejected == 0u); CHECK(guarded.parser.discarded == 0u);
    CHECK(guarded.parser.used == 0u);
    printf("protocol: reconciled 10000 frames, seed=0x20261023, accepted=%lu rejected=%lu discarded=%lu\n",
           (unsigned long)guarded.parser.accepted, (unsigned long)guarded.parser.rejected,
           (unsigned long)guarded.parser.discarded);
}

/* Random hostile bytes may create arbitrary candidates, but may neither
 * overflow the bounded parser nor prevent reception after idle flush. */
static void test_hostile_stream(void)
{
    ep_parser parser;
    capture cap = {0};
    uint32_t rng = 0xbad5eedu;
    size_t i;
    ep_parser_init(&parser, NULL, NULL);
    for (i = 0u; i < 100000u; ++i) {
        uint8_t byte = (uint8_t)next_random(&rng);
        ep_parser_feed(&parser, &byte, 1u);
        CHECK(parser.used <= EP_FRAME_MAX);
    }
    ep_parser_flush(&parser);
    CHECK(parser.used == 0u);
    parser.emit = capture_frame; parser.ctx = &cap;
    ep_parser_feed(&parser, fixture, sizeof(fixture));
    CHECK(cap.count == 1u); check_fixture(&cap.last);
}

int main(void)
{
    test_crc(); test_encode(); test_fragmentation(); test_bad_candidates();
    test_flush_recovers_nested_frame(); test_all_truncations();
    test_ten_thousand_frames(); test_hostile_stream();
    puts("protocol: 8 test groups passed");
    return EXIT_SUCCESS;
}
