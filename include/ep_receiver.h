#ifndef EP_RECEIVER_H
#define EP_RECEIVER_H

#include "ep_protocol.h"
#include <stdbool.h>

#define EP_RECEIVER_WINDOW 16u

typedef bool (*ep_deliver_fn)(void *ctx, const ep_frame *frame);

typedef enum {
    EP_RX_NEW,
    EP_RX_DUPLICATE,
    EP_RX_INVALID,
    EP_RX_CONFLICT,
    EP_RX_STALE,
    EP_RX_DELIVERY_FAILED
} ep_receive_result;

typedef struct {
    uint32_t accepted, duplicates, invalid, conflicts, stale, delivery_failures;
} ep_receiver_metrics;

typedef struct {
    uint16_t device;
    uint32_t boot;
    ep_receiver_metrics metrics;
    ep_frame history[EP_RECEIVER_WINDOW];
    size_t count, next;
    uint32_t highwater;
    bool has_highwater;
    ep_deliver_fn deliver;
    void *ctx;
} ep_receiver;

/* All pointers and the callback must be non-NULL. Calls are serialized and
 * non-reentrant; the callback must not modify or re-enter this receiver.
 * A true callback return means application acceptance, not durable storage.
 * Side effects performed before returning false may repeat on retry.
 * Initialize before use; do not modify receiver fields between calls. */
void ep_receiver_init(ep_receiver *receiver, uint16_t device, uint32_t boot,
                      ep_deliver_fn deliver, void *ctx);

/* Trusted session control must explicitly select the boot identity. Clears
 * volatile history/highwater, retaining device, callback and cumulative metrics.
 * Reusing a boot identity or resetting history can permit repeated delivery. */
void ep_receiver_begin_session(ep_receiver *receiver, uint32_t boot);

/* Input and output frames must not alias each other or receiver storage.
 * New sequences advance by unsigned distance 0 < distance < 2^31; cached
 * duplicates may be acknowledged behind highwater. Rejections leave ack intact.
 * Metrics wrap modulo 2^32. No authentication or exactly-once guarantee. */
ep_receive_result ep_receiver_receive(ep_receiver *receiver,
                                      const ep_frame *frame, ep_frame *ack);

#endif
