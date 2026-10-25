#include "ep_device.h"
#include <string.h>

static void put32(uint8_t *out, uint32_t value) {
    unsigned i; for(i=0;i<4;i++)out[i]=(uint8_t)(value>>(8u*i));
}
static uint32_t get32(const uint8_t *in) {
    return (uint32_t)in[0]|((uint32_t)in[1]<<8)|((uint32_t)in[2]<<16)|((uint32_t)in[3]<<24);
}
void ep_device_init(ep_device *d, ep_store *s, ep_hal hal, uint16_t id, uint32_t now) {
    uint32_t generation;
    memset(d,0,sizeof(*d)); d->store=s; d->hal=hal; d->device=id; d->boot=1;
    d->last_tick=now; d->last_sample=now; (void)ep_store_load(s,&d->config,&generation);
}
bool ep_device_enable_reliable(ep_device *d) {
    if(d->reliable_enabled || d->activity_started || d->metrics.sampled ||
       d->metrics.attempts || d->metrics.tx_attempts || d->metrics.resets || d->count)return false;
    d->reliable_enabled=true; return true;
}
static void clear_pending(ep_device *d) {
    d->head_attempts=0; d->last_tx=0; d->head_ack_eligible=false;
}
static void restart(ep_device *d, uint32_t now) {
    uint32_t generation;
    d->activity_started=true; clear_pending(d);
    d->metrics.reset_dropped+=(uint32_t)d->count; d->count=0; d->head=0;
    d->metrics.resets++; d->boot++; d->seq=0; d->last_sample=now;
    memset(d->commands,0,sizeof(d->commands)); d->command_next=0;
    (void)ep_store_load(d->store,&d->config,&generation);
}
static void send_pending(ep_device *d, uint32_t now) {
    if(!d->count)return;
    if(d->reliable_enabled) {
        if(d->head_attempts && now-d->last_tx<EP_ACK_TIMEOUT_MS)return;
        if(d->head_attempts>=EP_MAX_RETRIES+1u) {
            d->head=(d->head+1u)%EP_QUEUE_CAP; d->count--;
            d->metrics.delivery_timeouts++; clear_pending(d); return;
        }
        if(d->head_attempts)d->metrics.retransmissions++;
        d->head_attempts++; d->last_tx=now;
        d->metrics.tx_attempts++; d->activity_started=true;
        /* A failed later attempt cannot revoke a previously sent frame's ACK. */
        if(d->hal.send(d->hal.ctx,&d->queue[d->head]))d->head_ack_eligible=true;
    } else {
        d->metrics.tx_attempts++; d->activity_started=true;
        if(d->hal.send(d->hal.ctx,&d->queue[d->head])) {
            d->head=(d->head+1u)%EP_QUEUE_CAP; d->count--; d->metrics.sent++;
        }
    }
}
void ep_device_tick(ep_device *d, uint32_t now) {
    uint32_t elapsed=now-d->last_tick;
    d->last_tick=now;
    if(elapsed>=EP_WATCHDOG_MS) { restart(d,now); return; }
    elapsed=now-d->last_sample;
    if(elapsed>=d->config.period_ms) {
        int32_t value=0; uint32_t periods=elapsed/d->config.period_ms;
        d->activity_started=true;
        d->metrics.skipped_periods+=periods-1u;
        d->last_sample+=periods*d->config.period_ms; d->metrics.attempts++;
        if(d->hal.read(d->hal.ctx,&value)) {
            ep_frame frame={0}; d->metrics.sampled++;
            frame.type=1; frame.device=d->device; frame.boot=d->boot; frame.seq=d->seq++;
            frame.len=9; put32(frame.payload,now); put32(frame.payload+4,(uint32_t)value);
            frame.payload[8]=(uint8_t)(value>=d->config.threshold_mc);
            if(d->count==EP_QUEUE_CAP)d->metrics.dropped++;
            else {
                d->queue[(d->head+d->count)%EP_QUEUE_CAP]=frame; d->count++;
                if(d->count>d->metrics.queue_peak)d->metrics.queue_peak=(uint32_t)d->count;
            }
        } else d->metrics.sensor_errors++;
    }
    send_pending(d,now);
}
bool ep_device_acknowledge(ep_device *d, const ep_frame *ack) {
    const ep_frame *head;
    if(!d->reliable_enabled)return false;
    if(!d->count || !d->head_attempts || !d->head_ack_eligible) {
        d->metrics.invalid_acks++; return false;
    }
    head=&d->queue[d->head];
    if(ack->type!=3 || ack->len!=0 || ack->device!=head->device ||
       ack->boot!=head->boot || ack->seq!=head->seq) {
        d->metrics.invalid_acks++; return false;
    }
    d->head=(d->head+1u)%EP_QUEUE_CAP; d->count--;
    d->metrics.sent++; d->metrics.acknowledged++; clear_pending(d); return true;
}
ep_command_result ep_device_configure(ep_device *d, uint32_t id, ep_config c, size_t cut_after) {
    size_t i;
    if(!ep_config_valid(&c))return EP_CMD_INVALID;
    for(i=0;i<EP_COMMAND_WINDOW;i++) {
        const ep_command *old=&d->commands[i];
        if(old->valid && old->id==id)
            return old->config.period_ms==c.period_ms && old->config.threshold_mc==c.threshold_mc ? EP_CMD_DUPLICATE : EP_CMD_CONFLICT;
    }
    if(!ep_store_save(d->store,&c,cut_after))return EP_CMD_STORAGE;
    d->config=c; d->last_sample=d->last_tick; d->metrics.commands_applied++;
    d->commands[d->command_next]=(ep_command){id,c,true};
    d->command_next=(d->command_next+1u)%EP_COMMAND_WINDOW; return EP_CMD_OK;
}
ep_command_result ep_device_receive(ep_device *d, const ep_frame *f) {
    ep_config c; uint32_t raw;
    if(f->type!=2 || f->device!=d->device || f->boot!=d->boot || f->len!=8)return EP_CMD_INVALID;
    c.period_ms=get32(f->payload); raw=get32(f->payload+4);
    /* Decode two's complement without implementation-defined unsigned-to-signed conversion. */
    c.threshold_mc=raw<=INT32_MAX ? (int32_t)raw : -1-(int32_t)(UINT32_MAX-raw);
    return ep_device_configure(d,f->seq,c,EP_STORE_COMPLETE);
}
