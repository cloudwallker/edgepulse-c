#include "ep_device.h"
#ifdef NDEBUG
#error "Tests require assertions; do not define NDEBUG"
#endif
#include <assert.h>
#include <stdio.h>
typedef struct { bool online, fail; uint32_t received, boot, seq; } fake;
static bool sensor(void *ctx, int32_t *value) { fake *f=ctx; *value=31000; return !f->fail; }
static bool send_frame(void *ctx, const ep_frame *frame) { fake *f=ctx; if(!f->online)return false; f->received++; f->boot=frame->boot; f->seq=frame->seq; assert(frame->payload[8]==1); return true; }
static void setup(ep_device *d, ep_store *s, fake *f, uint32_t now) { ep_hal hal={sensor,send_frame,f}; ep_store_init(s); ep_device_init(d,s,hal,7,now); }
static void accounting(const ep_device *d) { assert(d->metrics.sampled==d->metrics.sent+d->metrics.dropped+d->metrics.reset_dropped+d->count); assert(d->metrics.attempts==d->metrics.sampled+d->metrics.sensor_errors); }
static bool reject_commit(void *ctx, size_t target, const uint8_t record[EP_SLOT_SIZE]) { (void)ctx; (void)target; (void)record; return false; }
int main(void) {
    ep_device d; ep_store s; fake f={false,false,0,0,0}; uint32_t i;
    setup(&d,&s,&f,0);
    for(i=100;i<=2000;i+=100)ep_device_tick(&d,i);
    assert(d.metrics.sampled==20 && d.count==16 && d.metrics.dropped==4 && d.metrics.queue_peak==16); accounting(&d);
    f.online=true; for(i=2001;i<=2016;i++)ep_device_tick(&d,i);
    assert(d.count==0 && f.received==16 && f.seq==15); accounting(&d);
    f.fail=true; ep_device_tick(&d,2100); assert(d.metrics.sensor_errors==1 && d.metrics.sampled==20); accounting(&d);
    f.fail=false; f.online=false; ep_device_tick(&d,2200);
    assert(d.count==1); ep_device_tick(&d,7200);
    assert(d.boot==2 && d.count==0 && d.metrics.resets==1 && d.metrics.reset_dropped==1); accounting(&d);
    f.online=true; ep_device_tick(&d,7300); assert(f.boot==2 && f.seq==0);
    setup(&d,&s,&f,UINT32_MAX-50u); ep_device_tick(&d,49);
    assert(d.metrics.sampled==1 && d.metrics.resets==0);
    ep_device_tick(&d,449); assert(d.metrics.sampled==2 && d.metrics.skipped_periods==3); accounting(&d);
    { ep_config c={50,30000}, bad={0,30000}; uint32_t writes;
      assert(ep_device_configure(&d,10,bad,EP_STORE_COMPLETE)==EP_CMD_INVALID);
      assert(ep_device_configure(&d,10,c,5)==EP_CMD_STORAGE && d.config.period_ms==100);
      assert(ep_device_configure(&d,10,c,EP_STORE_COMPLETE)==EP_CMD_OK); writes=s.writes;
      assert(ep_device_configure(&d,10,c,EP_STORE_COMPLETE)==EP_CMD_DUPLICATE && s.writes==writes);
      c.period_ms=60; assert(ep_device_configure(&d,10,c,EP_STORE_COMPLETE)==EP_CMD_CONFLICT);
      assert(d.config.period_ms==50); ep_device_tick(&d,5449);
      assert(d.boot==2 && d.config.period_ms==50);
      assert(ep_device_configure(&d,10,c,EP_STORE_COMPLETE)==EP_CMD_OK);
      for(i=11;i<=18;i++)assert(ep_device_configure(&d,i,c,EP_STORE_COMPLETE)==EP_CMD_OK);
      c.period_ms=70; assert(ep_device_configure(&d,10,c,EP_STORE_COMPLETE)==EP_CMD_OK);
    }
    { ep_frame cmd={0}; cmd.type=2; cmd.device=7; cmd.boot=d.boot; cmd.seq=99; cmd.len=8;
      cmd.payload[0]=100; cmd.payload[4]=0x30; cmd.payload[5]=0x75;
      assert(ep_device_receive(&d,&cmd)==EP_CMD_OK && d.config.period_ms==100);
      assert(ep_device_receive(&d,&cmd)==EP_CMD_DUPLICATE);
      cmd.boot--; assert(ep_device_receive(&d,&cmd)==EP_CMD_INVALID); cmd.boot++;
      cmd.len=7; assert(ep_device_receive(&d,&cmd)==EP_CMD_INVALID); cmd.len=8;
      cmd.device=8; assert(ep_device_receive(&d,&cmd)==EP_CMD_INVALID); cmd.device=7;
      cmd.type=1; assert(ep_device_receive(&d,&cmd)==EP_CMD_INVALID);
    }
    setup(&d,&s,&f,0);
    { ep_config c={50,28000};
      s.commit=reject_commit;
      assert(ep_device_configure(&d,42,c,EP_STORE_COMPLETE)==EP_CMD_STORAGE);
      assert(d.config.period_ms==100 && d.config.threshold_mc==30000 && d.metrics.commands_applied==0);
      s.commit=NULL;
      assert(ep_device_configure(&d,42,c,EP_STORE_COMPLETE)==EP_CMD_OK);
      assert(ep_device_configure(&d,42,c,EP_STORE_COMPLETE)==EP_CMD_DUPLICATE);
      assert(d.metrics.commands_applied==1);
    }
    puts("device: PASS (queue, accounting, sensor, watchdog, wrap, dedup, wire command, failed commit)"); return 0;
}
