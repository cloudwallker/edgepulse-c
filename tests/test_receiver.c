#include "ep_receiver.h"
#ifdef NDEBUG
#error "Tests require assertions; do not define NDEBUG"
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    bool fail;
    uint32_t calls, delivered;
    ep_frame last;
} sink;

static bool deliver(void *ctx, const ep_frame *frame) {
    sink *s = ctx;
    s->calls++;
    if (s->fail) return false;
    s->delivered++;
    s->last = *frame;
    return true;
}

static ep_frame telemetry(uint32_t seq) {
    ep_frame frame = {0};
    frame.type = 1;
    frame.device = 7;
    frame.boot = 3;
    frame.seq = seq;
    frame.len = 9;
    frame.payload[0] = 0x18;
    frame.payload[1] = 0x79;
    frame.payload[4] = 0x64;
    frame.payload[8] = 1;
    return frame;
}

static void setup(ep_receiver *r, sink *s) {
    memset(s, 0, sizeof(*s));
    ep_receiver_init(r, 7, 3, deliver, s);
}

static void assert_ack(const ep_frame *ack, const ep_frame *frame) {
    assert(ack->type == 3 && ack->len == 0);
    assert(ack->device == frame->device);
    assert(ack->boot == frame->boot && ack->seq == frame->seq);
}

static void expect_rejection(ep_receiver *r, const ep_frame *frame,
                             ep_receive_result result) {
    ep_frame ack;
    unsigned char before[sizeof(ack)];
    memset(&ack, 0xa5, sizeof(ack));
    memcpy(before, &ack, sizeof(ack));
    assert(ep_receiver_receive(r, frame, &ack) == result);
    assert(memcmp(before, &ack, sizeof(ack)) == 0);
}

/* Catches ACKing before delivery, wrong ACK identity, or rejecting an arbitrary
 * first sequence. Both allowed alarm values must pass validation. */
static void test_first_delivery_and_ack(void) {
    ep_receiver r;
    sink s;
    ep_frame frame = telemetry(42), ack;
    setup(&r, &s);
    assert(r.metrics.accepted == 0 && r.metrics.duplicates == 0);
    assert(r.metrics.invalid == 0 && r.metrics.conflicts == 0);
    assert(r.metrics.stale == 0 && r.metrics.delivery_failures == 0);
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    assert_ack(&ack, &frame);
    assert(s.calls == 1 && s.delivered == 1);
    assert(s.last.seq == 42 && s.last.device == 7 && s.last.boot == 3);
    assert(s.last.type == 1 && s.last.len == 9);
    assert(memcmp(s.last.payload, frame.payload, 9) == 0);
    frame.seq = 43;
    frame.payload[8] = 0;
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    assert_ack(&ack, &frame);
    assert(r.metrics.accepted == 2 && s.delivered == 2);
}

/* Catches repeated sink calls and comparing struct padding or unused payload. */
static void test_duplicates_ignore_unused_storage(void) {
    ep_receiver r;
    sink s;
    ep_frame first = telemetry(10), later = telemetry(20), same, ack;
    setup(&r, &s);
    assert(ep_receiver_receive(&r, &first, &ack) == EP_RX_NEW);
    assert(ep_receiver_receive(&r, &later, &ack) == EP_RX_NEW);
    memset(&same, 0x5a, sizeof(same));
    same.type = 1;
    same.device = 7;
    same.boot = 3;
    same.seq = 10;
    same.len = 9;
    memcpy(same.payload, first.payload, 9);
    assert(ep_receiver_receive(&r, &same, &ack) == EP_RX_DUPLICATE);
    assert_ack(&ack, &same);
    assert(ep_receiver_receive(&r, &later, &ack) == EP_RX_DUPLICATE);
    assert_ack(&ack, &later);
    assert(r.metrics.accepted == 2 && r.metrics.duplicates == 2);
    assert(s.calls == 2 && s.delivered == 2);
}

/* Catches checking only part of telemetry or overwriting history on conflict. */
static void test_payload_conflicts(void) {
    ep_receiver r;
    sink s;
    ep_frame original = telemetry(10), changed, ack;
    size_t i;
    setup(&r, &s);
    assert(ep_receiver_receive(&r, &original, &ack) == EP_RX_NEW);
    for (i = 0; i < 9; i++) {
        changed = original;
        changed.payload[i] ^= 1;
        expect_rejection(&r, &changed, EP_RX_CONFLICT);
    }
    assert(ep_receiver_receive(&r, &original, &ack) == EP_RX_DUPLICATE);
    assert_ack(&ack, &original);
    assert(r.metrics.conflicts == 9 && r.metrics.duplicates == 1);
    assert(r.metrics.accepted == 1 && s.calls == 1);
}

/* Catches omitting any telemetry/session validation or accepting malformed
 * cached frames as duplicates. Length 65535 must not read past the payload. */
static void test_invalid_frames(void) {
    static const uint8_t types[] = {0, 2, 3, 255};
    static const uint16_t lengths[] = {0, 8, 10, UINT16_MAX};
    ep_receiver r;
    sink s;
    ep_frame frame, ack;
    size_t i;
    setup(&r, &s);
    for (i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        frame = telemetry(1000);
        frame.type = types[i];
        expect_rejection(&r, &frame, EP_RX_INVALID);
    }
    for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        frame = telemetry(1000);
        frame.len = lengths[i];
        expect_rejection(&r, &frame, EP_RX_INVALID);
    }
    frame = telemetry(1000);
    frame.payload[8] = 2;
    expect_rejection(&r, &frame, EP_RX_INVALID);
    frame.payload[8] = 255;
    expect_rejection(&r, &frame, EP_RX_INVALID);
    frame = telemetry(1000);
    frame.device = 8;
    expect_rejection(&r, &frame, EP_RX_INVALID);
    frame = telemetry(1000);
    frame.boot = 4;
    expect_rejection(&r, &frame, EP_RX_INVALID);
    assert(r.metrics.invalid == 12 && s.calls == 0);
    frame = telemetry(0);
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    frame.len = 8;
    expect_rejection(&r, &frame, EP_RX_INVALID);
    assert(r.metrics.invalid == 13 && r.metrics.accepted == 1);
    assert(r.metrics.duplicates == 0 && s.calls == 1);
}

/* Catches treating every uncached sequence as new and moving highwater backward. */
static void test_gaps_and_out_of_order(void) {
    ep_receiver r;
    sink s;
    ep_frame frame = telemetry(10), ack;
    setup(&r, &s);
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    frame.seq = 30;
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    frame.seq = 20;
    expect_rejection(&r, &frame, EP_RX_STALE);
    frame.seq = 9;
    expect_rejection(&r, &frame, EP_RX_STALE);
    frame.seq = 10;
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_DUPLICATE);
    frame.seq = 29;
    expect_rejection(&r, &frame, EP_RX_STALE);
    frame.seq = 31;
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    assert(r.metrics.stale == 3 && r.metrics.accepted == 3);
    assert(r.metrics.duplicates == 1 && s.calls == 3);
}

/* Catches accepting exact half-range ambiguity and rejecting the largest
 * unambiguous positive serial distance. */
static void test_half_range_boundaries(void) {
    ep_receiver r;
    sink s;
    ep_frame frame = telemetry(7), ack;
    setup(&r, &s);
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    frame.seq = UINT32_C(0x80000007);
    expect_rejection(&r, &frame, EP_RX_STALE);
    frame.seq = UINT32_C(0x80000008);
    expect_rejection(&r, &frame, EP_RX_STALE);
    frame.seq = UINT32_C(0x80000006);
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    frame.seq = UINT32_C(0x80000007);
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    frame.seq = 7;
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_DUPLICATE);
    assert(r.metrics.accepted == 3 && r.metrics.stale == 2);
    assert(r.metrics.duplicates == 1 && s.calls == 3);
}

/* Catches ordinary integer ordering in place of unsigned serial arithmetic. */
static void test_sequence_wrap(void) {
    ep_receiver r;
    sink s;
    ep_frame frame = telemetry(UINT32_MAX - 1u), ack;
    setup(&r, &s);
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    frame.seq = UINT32_MAX;
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    frame.seq = 0;
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    frame.seq = 3;
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    frame.seq = 2;
    expect_rejection(&r, &frame, EP_RX_STALE);
    frame.seq = UINT32_MAX - 2u;
    expect_rejection(&r, &frame, EP_RX_STALE);
    frame.seq = UINT32_MAX;
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_DUPLICATE);
    assert_ack(&ack, &frame);
    assert(r.metrics.accepted == 4 && r.metrics.stale == 2);
    assert(s.calls == 4 && r.metrics.duplicates == 1);
}

/* Catches duplicate hits refreshing the FIFO and redelivery after eviction. */
static void test_window_eviction(void) {
    ep_receiver r;
    sink s;
    ep_frame frame, ack;
    uint32_t i;
    setup(&r, &s);
    for (i = 0; i < 16; i++) {
        frame = telemetry(i);
        assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    }
    frame = telemetry(0);
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_DUPLICATE);
    frame.seq = 16;
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    frame.seq = 0;
    expect_rejection(&r, &frame, EP_RX_STALE);
    frame.payload[0] ^= 1;
    expect_rejection(&r, &frame, EP_RX_STALE);
    frame = telemetry(1);
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_DUPLICATE);
    for (i = 17; i < 33; i++) {
        frame = telemetry(i);
        assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    }
    frame = telemetry(16);
    expect_rejection(&r, &frame, EP_RX_STALE);
    frame.seq = 17;
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_DUPLICATE);
    assert_ack(&ack, &frame);
    assert(r.metrics.accepted == 33 && r.metrics.duplicates == 3);
    assert(r.metrics.stale == 3 && s.calls == 33);
}

/* Catches caching failed delivery, ACKing failure, or skipping the retry sink. */
static void test_first_delivery_failure_is_retryable(void) {
    ep_receiver r;
    sink s;
    ep_frame frame = telemetry(100), ack;
    setup(&r, &s);
    s.fail = true;
    expect_rejection(&r, &frame, EP_RX_DELIVERY_FAILED);
    expect_rejection(&r, &frame, EP_RX_DELIVERY_FAILED);
    assert(r.metrics.delivery_failures == 2 && r.metrics.accepted == 0);
    assert(s.calls == 2 && s.delivered == 0);
    s.fail = false;
    frame.payload[0] ^= 1;
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    assert_ack(&ack, &frame);
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_DUPLICATE);
    assert(r.metrics.accepted == 1 && r.metrics.duplicates == 1);
    assert(s.calls == 3 && s.delivered == 1);
}

/* Catches advancing highwater when the sink rejects a new sequence. */
static void test_failure_preserves_highwater(void) {
    ep_receiver r;
    sink s;
    ep_frame frame = telemetry(100), ack;
    setup(&r, &s);
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    s.fail = true;
    frame.seq = 300;
    expect_rejection(&r, &frame, EP_RX_DELIVERY_FAILED);
    s.fail = false;
    frame.seq = 200;
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    frame.seq = 300;
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    assert_ack(&ack, &frame);
    assert(r.metrics.accepted == 3 && r.metrics.delivery_failures == 1);
    assert(s.calls == 4 && s.delivered == 3);
}

/* Catches evicting a successfully delivered frame before the sink accepts its
 * replacement; cached retries must remain ACKable even while the sink fails. */
static void test_failure_preserves_full_window(void) {
    ep_receiver r;
    sink s;
    ep_frame frame, ack;
    uint32_t i;
    setup(&r, &s);
    for (i = 0; i < 16; i++) {
        frame = telemetry(i);
        assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    }
    s.fail = true;
    frame = telemetry(16);
    expect_rejection(&r, &frame, EP_RX_DELIVERY_FAILED);
    frame.seq = 0;
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_DUPLICATE);
    assert_ack(&ack, &frame);
    assert(s.calls == 17 && r.metrics.delivery_failures == 1);
    s.fail = false;
    frame.seq = 16;
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    frame.seq = 0;
    expect_rejection(&r, &frame, EP_RX_STALE);
    assert(r.metrics.accepted == 17 && s.calls == 18 && s.delivered == 17);
}

/* Catches automatic boot switching, stale history surviving trusted session
 * control, or discarding cumulative rejection/delivery metrics on a new boot. */
static void test_session_isolation_and_cumulative_metrics(void) {
    ep_receiver r;
    sink s;
    ep_frame frame = telemetry(400), ack;
    setup(&r, &s);
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_DUPLICATE);
    frame.payload[0] ^= 1;
    expect_rejection(&r, &frame, EP_RX_CONFLICT);
    frame = telemetry(399);
    expect_rejection(&r, &frame, EP_RX_STALE);
    frame = telemetry(500);
    frame.boot = 4;
    expect_rejection(&r, &frame, EP_RX_INVALID);
    assert(r.boot == 3);
    frame.boot = 3;
    s.fail = true;
    expect_rejection(&r, &frame, EP_RX_DELIVERY_FAILED);
    s.fail = false;
    ep_receiver_begin_session(&r, 4);
    assert(r.boot == 4 && r.device == 7);
    assert(r.metrics.accepted == 1 && r.metrics.duplicates == 1);
    assert(r.metrics.conflicts == 1 && r.metrics.stale == 1);
    assert(r.metrics.invalid == 1 && r.metrics.delivery_failures == 1);
    frame = telemetry(400);
    expect_rejection(&r, &frame, EP_RX_INVALID);
    frame = telemetry(0);
    frame.boot = 4;
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    assert_ack(&ack, &frame);
    frame.seq = 400;
    frame.payload[0] ^= 1;
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_DUPLICATE);
    assert_ack(&ack, &frame);
    assert(r.metrics.accepted == 3 && r.metrics.duplicates == 2);
    assert(r.metrics.invalid == 2 && s.calls == 4 && s.delivered == 3);
}

/* Catches skipping explicit session resets when the caller selects the same
 * boot, and leaving a nonzero FIFO cursor/count after clearing full history. */
static void test_same_boot_explicit_reset(void) {
    ep_receiver r;
    sink s;
    ep_frame frame, ack;
    uint32_t i;
    setup(&r, &s);
    for (i = 50; i < 70; i++) {
        frame = telemetry(i);
        assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    }
    ep_receiver_begin_session(&r, 3);
    frame = telemetry(1);
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    frame = telemetry(60);
    frame.payload[0] ^= 1;
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_NEW);
    assert(ep_receiver_receive(&r, &frame, &ack) == EP_RX_DUPLICATE);
    assert_ack(&ack, &frame);
    assert(r.metrics.accepted == 22 && r.metrics.duplicates == 1);
    assert(s.calls == 22 && s.delivered == 22);
}

int main(void) {
    test_first_delivery_and_ack();
    test_duplicates_ignore_unused_storage();
    test_payload_conflicts();
    test_invalid_frames();
    test_gaps_and_out_of_order();
    test_half_range_boundaries();
    test_sequence_wrap();
    test_window_eviction();
    test_first_delivery_failure_is_retryable();
    test_failure_preserves_highwater();
    test_failure_preserves_full_window();
    test_session_isolation_and_cumulative_metrics();
    test_same_boot_explicit_reset();
    puts("receiver: PASS (13 groups: delivery, duplicate storage, payload conflicts, invalid frames, out of order, half range, wrap, eviction, failed first delivery, failed highwater, failed eviction, session isolation, explicit reset)");
    return 0;
}
