#ifndef EP_DEVICE_H
#define EP_DEVICE_H
#include "ep_protocol.h"
#include "ep_store.h"
#define EP_QUEUE_CAP 16u
#define EP_COMMAND_WINDOW 8u
#define EP_WATCHDOG_MS 5000u
typedef bool (*ep_sensor_fn)(void *ctx, int32_t *temperature_mc);
/* True transfers ownership to the simulated link; no end-to-end ACK guarantee. */
typedef bool (*ep_send_fn)(void *ctx, const ep_frame *frame);
typedef struct { ep_sensor_fn read; ep_send_fn send; void *ctx; } ep_hal;
typedef struct { uint32_t attempts, sampled, sensor_errors, sent, dropped, reset_dropped, skipped_periods, resets, queue_peak, commands_applied; } ep_metrics;
typedef struct { uint32_t id; ep_config config; bool valid; } ep_command;
typedef enum { EP_CMD_OK, EP_CMD_DUPLICATE, EP_CMD_CONFLICT, EP_CMD_INVALID, EP_CMD_STORAGE } ep_command_result;
typedef struct {
    ep_store *store; ep_hal hal; ep_config config; ep_metrics metrics;
    ep_frame queue[EP_QUEUE_CAP]; size_t head, count;
    ep_command commands[EP_COMMAND_WINDOW]; size_t command_next;
    uint16_t device; uint32_t boot, seq, last_tick, last_sample;
} ep_device;
/* Non-null arguments and callbacks required; serialized calls, no ISR safety. */
void ep_device_init(ep_device *device, ep_store *store, ep_hal hal, uint16_t id, uint32_t now);
/* Monotonic modulo-2^32 milliseconds; adjacent calls must be < 2^31 ms apart. */
void ep_device_tick(ep_device *device, uint32_t now);
ep_command_result ep_device_configure(ep_device *device, uint32_t command_id, ep_config config, size_t cut_after);
/* Type 2, matching device and boot, 8-byte LE period + signed threshold. */
ep_command_result ep_device_receive(ep_device *device, const ep_frame *frame);
#endif
