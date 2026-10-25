#ifndef EP_DEVICE_H
#define EP_DEVICE_H
#include "ep_protocol.h"
#include "ep_store.h"
#define EP_QUEUE_CAP 16u
#define EP_COMMAND_WINDOW 8u
#define EP_WATCHDOG_MS 5000u
#define EP_ACK_TIMEOUT_MS 100u
#define EP_MAX_RETRIES 3u
typedef bool (*ep_sensor_fn)(void *ctx, int32_t *temperature_mc);
/* True means HAL acceptance; reliable completion still requires an ACK.
 * Calls must not re-enter the device. Dispatch ACKs only after tick returns. */
typedef bool (*ep_send_fn)(void *ctx, const ep_frame *frame);
typedef struct { ep_sensor_fn read; ep_send_fn send; void *ctx; } ep_hal;
typedef struct {
    uint32_t attempts, sampled, sensor_errors, sent, dropped, reset_dropped;
    uint32_t skipped_periods, resets, queue_peak, commands_applied;
    uint32_t tx_attempts, retransmissions, acknowledged, delivery_timeouts, invalid_acks;
} ep_metrics;
typedef struct { uint32_t id; ep_config config; bool valid; } ep_command;
typedef enum { EP_CMD_OK, EP_CMD_DUPLICATE, EP_CMD_CONFLICT, EP_CMD_INVALID, EP_CMD_STORAGE } ep_command_result;
typedef struct {
    ep_store *store; ep_hal hal; ep_config config; ep_metrics metrics;
    ep_frame queue[EP_QUEUE_CAP]; size_t head, count;
    ep_command commands[EP_COMMAND_WINDOW]; size_t command_next;
    uint16_t device; uint32_t boot, seq, last_tick, last_sample;
    uint32_t head_attempts, last_tx;
    bool reliable_enabled, activity_started, head_ack_eligible;
} ep_device;
/* Non-null arguments and callbacks required; serialized calls, no ISR safety. */
void ep_device_init(ep_device *device, ep_store *store, ep_hal hal, uint16_t id, uint32_t now);
/* Opt in before any sampling attempt, send, or watchdog reset; configuration
 * commands and earlier ticks without activity are allowed. Failure is inert. */
bool ep_device_enable_reliable(ep_device *device);
/* Monotonic modulo-2^32 milliseconds; adjacent calls must be < 2^31 ms apart. */
void ep_device_tick(ep_device *device, uint32_t now);
/* Type 3, len 0, exact outstanding device/boot/seq, after a true HAL send.
 * Serialized/non-reentrant: call only after tick/send has returned.
 * Invalid reliable ACKs increment invalid_acks, leaving the queue untouched. */
bool ep_device_acknowledge(ep_device *device, const ep_frame *ack);
ep_command_result ep_device_configure(ep_device *device, uint32_t command_id, ep_config config, size_t cut_after);
/* Type 2, matching device and boot, 8-byte LE period + signed threshold. */
ep_command_result ep_device_receive(ep_device *device, const ep_frame *frame);
#endif
