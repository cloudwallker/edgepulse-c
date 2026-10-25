#include "ep_device.h"
#ifdef NDEBUG
#error "Tests require assertions; do not define NDEBUG"
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    bool online, sensor_fails;
    int32_t temperature;
    uint32_t calls;
    ep_frame first, last;
} fake;

static bool sensor(void *ctx, int32_t *value) {
    fake *f=ctx;
    *value=f->temperature;
    return !f->sensor_fails;
}
static bool send_frame(void *ctx, const ep_frame *frame) {
    fake *f=ctx;
    if(f->calls==0)f->first=*frame;
    f->last=*frame; f->calls++;
    return f->online;
}
static void setup(ep_device *d, ep_store *s, fake *f, uint32_t now, bool reliable) {
    ep_hal hal={sensor,send_frame,f};
    memset(f,0,sizeof(*f)); f->online=true; f->temperature=31000;
    ep_store_init(s); ep_device_init(d,s,hal,7,now);
    if(reliable)assert(ep_device_enable_reliable(d));
}
static void accounting(const ep_device *d) {
    assert(d->metrics.sampled==d->metrics.sent+d->metrics.dropped+
           d->metrics.reset_dropped+d->metrics.delivery_timeouts+(uint32_t)d->count);
    assert(d->metrics.attempts==d->metrics.sampled+d->metrics.sensor_errors);
}
static ep_frame ack_for(const ep_frame *frame) {
    ep_frame ack={0};
    ack.type=3; ack.device=frame->device; ack.boot=frame->boot; ack.seq=frame->seq;
    return ack;
}
static void same_frame(const ep_frame *left, const ep_frame *right) {
    assert(left->type==right->type && left->device==right->device);
    assert(left->boot==right->boot && left->seq==right->seq && left->len==right->len);
    assert(memcmp(left->payload,right->payload,left->len)==0);
}
static void reject_ack(ep_device *d, const ep_frame *ack) {
    ep_device expected;
    memcpy(&expected,d,sizeof(expected)); expected.metrics.invalid_acks++;
    assert(!ep_device_acknowledge(d,ack));
    assert(memcmp(&expected,d,sizeof(expected))==0);
    accounting(d);
}
static void reject_enable(ep_device *d) {
    ep_device expected;
    memcpy(&expected,d,sizeof(expected));
    assert(!ep_device_enable_reliable(d));
    assert(memcmp(&expected,d,sizeof(expected))==0);
}

/* Catches a mode switch after activity, including empty legacy queues/reset. */
static void enabling(void) {
    ep_device d; ep_store s; fake f;
    ep_config c={1000,28000};
    setup(&d,&s,&f,0,true); reject_enable(&d);
    setup(&d,&s,&f,0,false);
    assert(ep_device_configure(&d,42,c,EP_STORE_COMPLETE)==EP_CMD_OK);
    ep_device_tick(&d,999);
    assert(ep_device_enable_reliable(&d));
    ep_device_tick(&d,1000);
    assert(d.count==1 && d.metrics.sent==0 && d.metrics.commands_applied==1);
    setup(&d,&s,&f,0,false); ep_device_tick(&d,100);
    assert(d.count==0); reject_enable(&d);
    setup(&d,&s,&f,0,false); f.sensor_fails=true; ep_device_tick(&d,100);
    assert(d.metrics.attempts==1 && d.metrics.sampled==0); reject_enable(&d);
    setup(&d,&s,&f,0,false); ep_device_tick(&d,5000);
    assert(d.metrics.resets==1 && d.metrics.sampled==0); reject_enable(&d);
    setup(&d,&s,&f,0,false); f.online=false; ep_device_tick(&d,100);
    assert(d.count==1); reject_enable(&d);
}

/* Catches enqueue-on-send popping and missing successful ACK accounting. */
static void retained_head(void) {
    ep_device d; ep_store s; fake f; ep_frame ack;
    setup(&d,&s,&f,0,true); ep_device_tick(&d,100);
    assert(d.count==1 && d.metrics.sent==0 && d.metrics.acknowledged==0);
    assert(d.metrics.tx_attempts==1 && d.metrics.retransmissions==0);
    ep_device_tick(&d,101); assert(f.calls==1 && d.count==1);
    ack=ack_for(&f.last); assert(ep_device_acknowledge(&d,&ack));
    assert(d.count==0 && d.metrics.sent==1 && d.metrics.acknowledged==1);
    reject_ack(&d,&ack); accounting(&d);
}

/* Catches omitted ACK validation for every header field/type/length. */
static void wrong_ack(void) {
    ep_device d; ep_store s; fake f; ep_frame ack, bad;
    const uint8_t types[]={0,1,2,4,255};
    const uint16_t lengths[]={1,9,64,65,UINT16_MAX};
    size_t i;
    setup(&d,&s,&f,0,true); ep_device_tick(&d,100); ack=ack_for(&f.last);
    for(i=0;i<sizeof(types)/sizeof(types[0]);i++) {
        bad=ack; bad.type=types[i]; reject_ack(&d,&bad);
    }
    for(i=0;i<sizeof(lengths)/sizeof(lengths[0]);i++) {
        bad=ack; bad.len=lengths[i]; reject_ack(&d,&bad);
    }
    bad=ack; bad.device++; reject_ack(&d,&bad);
    bad=ack; bad.boot++; reject_ack(&d,&bad);
    bad=ack; bad.seq++; reject_ack(&d,&bad);
    assert(d.metrics.invalid_acks==13 && d.count==1);
    assert(ep_device_acknowledge(&d,&ack)); accounting(&d);
}

/* Catches ACK acceptance before a successful HAL call and false-send spinning. */
static void no_send_eligibility(void) {
    ep_device d; ep_store s; fake f; ep_frame ack;
    setup(&d,&s,&f,0,true); f.online=false; ep_device_tick(&d,100);
    ack=ack_for(&f.last); reject_ack(&d,&ack);
    ep_device_tick(&d,199); assert(f.calls==1 && d.metrics.tx_attempts==1);
    ep_device_tick(&d,200); assert(f.calls==2); reject_ack(&d,&ack);
    f.online=true; ep_device_tick(&d,300);
    assert(f.calls==3 && d.metrics.retransmissions==2);
    assert(ep_device_acknowledge(&d,&ack));
    assert(d.count==2 && d.metrics.sent==1 && d.metrics.acknowledged==1);
    accounting(&d);
}

/* Catches changed payload on retry, early retry, and an extra fifth HAL call. */
static void retry_payload(void) {
    ep_device d; ep_store s; fake f; ep_frame ack; uint32_t i;
    setup(&d,&s,&f,0,true); ep_device_tick(&d,100);
    f.sensor_fails=true; f.temperature=-12000;
    for(i=200;i<=400;i+=100) {
        uint32_t before=f.calls;
        ep_device_tick(&d,i-1u); assert(f.calls==before);
        ep_device_tick(&d,i); assert(f.calls==before+1u);
        same_frame(&f.first,&f.last); accounting(&d);
    }
    assert(f.calls==4 && d.count==1 && d.metrics.retransmissions==3);
    ep_device_tick(&d,499); assert(f.calls==4 && d.count==1);
    ep_device_tick(&d,500);
    assert(f.calls==4 && d.count==0 && d.metrics.delivery_timeouts==1);
    assert(d.metrics.sent==0 && d.metrics.acknowledged==0 && d.metrics.tx_attempts==4);
    ack=ack_for(&f.first); reject_ack(&d,&ack); accounting(&d);
}

/* Catches clearing eligibility on a failed retry or carrying it to a new head. */
static void late_ack(void) {
    ep_device d; ep_store s; fake f; ep_frame old, next;
    setup(&d,&s,&f,0,true); ep_device_tick(&d,100); old=ack_for(&f.last);
    f.online=false; ep_device_tick(&d,200);
    assert(d.metrics.tx_attempts==2 && d.count==2);
    assert(ep_device_acknowledge(&d,&old));
    assert(d.count==1 && d.metrics.acknowledged==1);
    reject_ack(&d,&old);
    next=old; next.seq=1; reject_ack(&d,&next);
    ep_device_tick(&d,201); assert(f.calls==3); reject_ack(&d,&next);
    f.online=true; ep_device_tick(&d,300); assert(f.calls==3);
    ep_device_tick(&d,301); assert(f.calls==4);
    reject_ack(&d,&old); assert(ep_device_acknowledge(&d,&next));
    assert(d.metrics.acknowledged==2 && d.metrics.retransmissions==2);
    accounting(&d);
}

/* Catches dropping at the fourth send instead of its next deadline. */
static void fourth_call_late_ack(void) {
    ep_device d; ep_store s; fake f; ep_frame ack; uint32_t now;
    setup(&d,&s,&f,0,true); ep_device_tick(&d,100); ack=ack_for(&f.last);
    f.online=false; f.sensor_fails=true;
    for(now=200;now<=400;now+=100)ep_device_tick(&d,now);
    ep_device_tick(&d,499);
    assert(f.calls==4 && d.count==1 && d.metrics.delivery_timeouts==0);
    assert(ep_device_acknowledge(&d,&ack)); ep_device_tick(&d,500);
    assert(d.count==0 && f.calls==4 && d.metrics.delivery_timeouts==0);
    assert(d.metrics.acknowledged==1); accounting(&d);
}

/* Catches failed HAL calls not consuming the budget or two heads sent per tick. */
static void expiration_next_head(void) {
    ep_device d; ep_store s; fake f; ep_frame old, next; uint32_t now;
    setup(&d,&s,&f,0,true); ep_device_tick(&d,100); f.online=false;
    for(now=200;now<=400;now+=100)ep_device_tick(&d,now);
    assert(f.calls==4 && d.count==4 && d.metrics.retransmissions==3);
    old=ack_for(&f.last); ep_device_tick(&d,500);
    assert(f.calls==4 && d.count==4 && d.metrics.delivery_timeouts==1);
    reject_ack(&d,&old); next=old; next.seq=1; reject_ack(&d,&next);
    ep_device_tick(&d,501);
    assert(f.calls==5 && f.last.seq==1 && d.metrics.retransmissions==3);
    reject_ack(&d,&old); reject_ack(&d,&next);
    f.online=true; ep_device_tick(&d,601);
    assert(f.calls==6 && d.metrics.retransmissions==4);
    assert(ep_device_acknowledge(&d,&next));
    assert(d.count==4 && d.metrics.acknowledged==1); accounting(&d);
}

/* Catches deadline comparison using absolute timestamps across uint32 wrap. */
static void deadline_wrap(void) {
    ep_device d; ep_store s; fake f; uint32_t now;
    setup(&d,&s,&f,UINT32_MAX-150u,true);
    ep_device_tick(&d,UINT32_MAX-50u); assert(f.calls==1 && d.count==1);
    f.sensor_fails=true;
    for(now=49;now<=249;now+=100) {
        uint32_t before=f.calls;
        ep_device_tick(&d,now-1u); assert(f.calls==before);
        ep_device_tick(&d,now); assert(f.calls==before+1u);
    }
    ep_device_tick(&d,348); assert(d.count==1);
    ep_device_tick(&d,349);
    assert(d.count==0 && f.calls==4 && d.metrics.delivery_timeouts==1);
    assert(d.metrics.resets==0); accounting(&d);
}

/* Catches pending attempts/ACKs surviving a watchdog or mode being disabled. */
static void watchdog_reset(void) {
    ep_device d; ep_store s; fake f; ep_frame old, next;
    setup(&d,&s,&f,0,true); ep_device_tick(&d,100); old=ack_for(&f.last);
    ep_device_tick(&d,200); ep_device_tick(&d,5200);
    assert(d.boot==2 && d.count==0 && d.metrics.resets==1);
    assert(d.metrics.reset_dropped==2 && d.metrics.delivery_timeouts==0 && f.calls==2);
    reject_ack(&d,&old); reject_enable(&d);
    f.online=false; ep_device_tick(&d,5300);
    assert(d.count==1 && f.calls==3 && f.last.seq==0 && f.last.boot==2);
    assert(d.metrics.retransmissions==1);
    reject_ack(&d,&old); next=ack_for(&f.last);
    reject_ack(&d,&next); f.online=true; ep_device_tick(&d,5400);
    assert(ep_device_acknowledge(&d,&next)); accounting(&d);
}

/* Catches overflow accounting mixing expiry with overload or reset drops. */
static void overload(void) {
    ep_device d; ep_store s; fake f; ep_config c={10,30000}; uint32_t now;
    setup(&d,&s,&f,0,false);
    assert(ep_device_configure(&d,1,c,EP_STORE_COMPLETE)==EP_CMD_OK);
    assert(ep_device_enable_reliable(&d)); f.online=false;
    for(now=10;now<=2000;now+=10) {
        uint32_t before=f.calls;
        ep_device_tick(&d,now); assert(f.calls-before<=1u); accounting(&d);
    }
    assert(d.metrics.sampled==200 && d.metrics.dropped==180);
    assert(d.count==16 && d.metrics.queue_peak==16 && d.metrics.delivery_timeouts==4);
    assert(f.calls==20 && d.metrics.tx_attempts==20 && d.metrics.retransmissions==15);
    assert(d.metrics.reset_dropped==0 && d.metrics.sent==0);
}

/* Catches stale ACK matching after a sequence boundary. */
static void sequence_wrap(void) {
    ep_device d; ep_store s; fake f; ep_frame old, next;
    setup(&d,&s,&f,0,true); d.seq=UINT32_MAX; ep_device_tick(&d,100);
    assert(f.last.seq==UINT32_MAX); old=ack_for(&f.last);
    assert(ep_device_acknowledge(&d,&old)); ep_device_tick(&d,200);
    assert(f.last.seq==0 && d.seq==1); next=ack_for(&f.last);
    reject_ack(&d,&old); assert(ep_device_acknowledge(&d,&next));
    assert(d.metrics.sent==2 && d.metrics.acknowledged==2); accounting(&d);
}

/* Catches accidental stop-and-wait/backoff/ACK effects in default mode. */
static void legacy(void) {
    ep_device d, before; ep_store s; fake f; ep_frame ack;
    setup(&d,&s,&f,0,false); f.online=false; ep_device_tick(&d,100);
    ep_device_tick(&d,101); assert(f.calls==2 && d.count==1 && d.metrics.tx_attempts==2);
    ack=ack_for(&f.last); memcpy(&before,&d,sizeof(before));
    assert(!ep_device_acknowledge(&d,&ack)); assert(memcmp(&before,&d,sizeof(d))==0);
    f.online=true; ep_device_tick(&d,102);
    assert(f.calls==3 && d.count==0 && d.metrics.sent==1 && d.metrics.tx_attempts==3);
    assert(d.metrics.retransmissions==0 && d.metrics.acknowledged==0);
    assert(d.metrics.delivery_timeouts==0 && d.metrics.invalid_acks==0); accounting(&d);
}

int main(int argc, char **argv) {
    const struct { const char *name; void (*run)(void); } tests[]={
        {"enabling",enabling},{"retained_head",retained_head},{"wrong_ack",wrong_ack},
        {"no_send_eligibility",no_send_eligibility},{"retry_payload",retry_payload},
        {"late_ack",late_ack},{"fourth_call_late_ack",fourth_call_late_ack},
        {"expiration_next_head",expiration_next_head},{"deadline_wrap",deadline_wrap},
        {"watchdog_reset",watchdog_reset},{"overload",overload},
        {"sequence_wrap",sequence_wrap},{"legacy",legacy}
    };
    size_t i, ran=0;
    assert(argc==1 || argc==2);
    for(i=0;i<sizeof(tests)/sizeof(tests[0]);i++) {
        if(argc==1 || strcmp(argv[1],tests[i].name)==0) { tests[i].run(); ran++; }
    }
    assert(ran>0);
    printf("reliable sender: PASS (%u groups)\n",(unsigned)ran); return 0;
}
