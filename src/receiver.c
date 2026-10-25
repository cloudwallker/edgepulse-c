#include "ep_receiver.h"
#include <string.h>

void ep_receiver_init(ep_receiver *receiver, uint16_t device, uint32_t boot,
                      ep_deliver_fn deliver, void *ctx) {
    memset(receiver, 0, sizeof(*receiver));
    receiver->device = device;
    receiver->boot = boot;
    receiver->deliver = deliver;
    receiver->ctx = ctx;
}

void ep_receiver_begin_session(ep_receiver *receiver, uint32_t boot) {
    receiver->boot = boot;
    memset(receiver->history, 0, sizeof(receiver->history));
    receiver->count = 0;
    receiver->next = 0;
    receiver->highwater = 0;
    receiver->has_highwater = false;
}

static void write_ack(const ep_frame *frame, ep_frame *ack) {
    ep_frame response = {0};
    response.type = 3;
    response.device = frame->device;
    response.boot = frame->boot;
    response.seq = frame->seq;
    *ack = response;
}

ep_receive_result ep_receiver_receive(ep_receiver *receiver,
                                      const ep_frame *frame, ep_frame *ack) {
    size_t i;
    if (frame->type != 1 || frame->len != 9 || frame->payload[8] > 1 ||
        frame->device != receiver->device || frame->boot != receiver->boot) {
        receiver->metrics.invalid++;
        return EP_RX_INVALID;
    }
    for (i = 0; i < receiver->count; i++) {
        const ep_frame *cached = &receiver->history[i];
        if (cached->seq != frame->seq) continue;
        if (cached->len != frame->len ||
            memcmp(cached->payload, frame->payload, frame->len) != 0) {
            receiver->metrics.conflicts++;
            return EP_RX_CONFLICT;
        }
        receiver->metrics.duplicates++;
        write_ack(frame, ack);
        return EP_RX_DUPLICATE;
    }
    if (receiver->has_highwater) {
        uint32_t distance = frame->seq - receiver->highwater;
        if (distance == 0 || distance >= UINT32_C(0x80000000)) {
            receiver->metrics.stale++;
            return EP_RX_STALE;
        }
    }
    if (!receiver->deliver(receiver->ctx, frame)) {
        receiver->metrics.delivery_failures++;
        return EP_RX_DELIVERY_FAILED;
    }
    receiver->history[receiver->next] = *frame;
    receiver->next = (receiver->next + 1u) % EP_RECEIVER_WINDOW;
    if (receiver->count < EP_RECEIVER_WINDOW) receiver->count++;
    receiver->highwater = frame->seq;
    receiver->has_highwater = true;
    receiver->metrics.accepted++;
    write_ack(frame, ack);
    return EP_RX_NEW;
}
