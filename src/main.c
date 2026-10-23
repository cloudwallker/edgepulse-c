#include "ep_device.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    ep_parser wire;
    ep_frame expected;
    uint32_t sensor_rng, link_rng, wire_received, checksum;
    uint32_t last_boot, last_seq;
    bool online, expecting, valid, have_last;
} simulation;

typedef struct {
    ep_device *device;
    uint32_t accepted, duplicates;
    bool valid;
} command_receiver;

static uint32_t random_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void put32(uint8_t *out, uint32_t value) {
    unsigned i;
    for (i = 0; i < 4; i++) out[i] = (uint8_t)(value >> (8u * i));
}

static bool sensor_read(void *ctx, int32_t *temperature) {
    simulation *sim = ctx;
    uint32_t value = random_next(&sim->sensor_rng);
    if (value % 31u == 0) return false;
    *temperature = (int32_t)(value % 55001u) - 10000;
    return true;
}

static void receive_telemetry(void *ctx, const ep_frame *frame) {
    simulation *sim = ctx;
    const ep_frame *expected = &sim->expected;
    if (!sim->expecting || frame->type != 1 || frame->len != 9 ||
        frame->device != expected->device || frame->boot != expected->boot ||
        frame->seq != expected->seq || frame->len != expected->len ||
        memcmp(frame->payload, expected->payload, expected->len) != 0) {
        sim->valid = false;
    }
    if (sim->have_last && (frame->boot < sim->last_boot ||
        (frame->boot == sim->last_boot && frame->seq <= sim->last_seq))) {
        sim->valid = false;
    }
    sim->last_boot = frame->boot;
    sim->last_seq = frame->seq;
    sim->have_last = true;
    sim->expecting = false;
    sim->wire_received++;
}

static void fragmented_feed(ep_parser *parser, const uint8_t *bytes, size_t length, uint32_t *rng) {
    size_t offset = 0;
    while (offset < length) {
        size_t chunk = 1u + random_next(rng) % 11u;
        if (chunk > length - offset) chunk = length - offset;
        ep_parser_feed(parser, bytes + offset, chunk);
        offset += chunk;
    }
}

static bool link_send(void *ctx, const ep_frame *frame) {
    simulation *sim = ctx;
    uint8_t bytes[EP_FRAME_MAX];
    size_t length, i;
    uint32_t before;
    if (!sim->online) return false;
    length = ep_encode(frame, bytes, sizeof(bytes));
    if (!length) {
        sim->valid = false;
        return false;
    }
    sim->expected = *frame;
    sim->expecting = true;
    before = sim->wire_received;
    for (i = 0; i < length; i++) {
        sim->checksum ^= bytes[i];
        sim->checksum *= UINT32_C(16777619);
    }
    fragmented_feed(&sim->wire, bytes, length, &sim->link_rng);
    if (sim->expecting || sim->wire_received != before + 1u) sim->valid = false;
    return true;
}

static void receive_command(void *ctx, const ep_frame *frame) {
    command_receiver *receiver = ctx;
    ep_command_result result = ep_device_receive(receiver->device, frame);
    if (result == EP_CMD_OK) receiver->accepted++;
    else if (result == EP_CMD_DUPLICATE) receiver->duplicates++;
    else receiver->valid = false;
}

static bool send_configuration(ep_device *device, uint32_t *rng, uint32_t *duplicates) {
    uint8_t bytes[EP_FRAME_MAX];
    ep_frame frame = {0};
    ep_parser parser;
    command_receiver receiver = {device, 0, 0, true};
    size_t length;
    frame.type = 2;
    frame.device = device->device;
    frame.boot = device->boot;
    frame.seq = 42;
    frame.len = 8;
    put32(frame.payload, 10);
    put32(frame.payload + 4, 30000);
    length = ep_encode(&frame, bytes, sizeof(bytes));
    if (!length) return false;
    ep_parser_init(&parser, receive_command, &receiver);
    fragmented_feed(&parser, bytes, length, rng);
    fragmented_feed(&parser, bytes, length, rng);
    *duplicates = receiver.duplicates;
    return receiver.valid && receiver.accepted == 1 && receiver.duplicates == 1 &&
        parser.accepted == 2 && parser.rejected == 0 && parser.used == 0;
}

static bool parse_number(const char *text, uint32_t minimum, uint32_t maximum, uint32_t *out) {
    uint32_t value = 0;
    const unsigned char *cursor = (const unsigned char *)text;
    if (!*cursor) return false;
    while (*cursor) {
        uint32_t digit;
        if (*cursor < '0' || *cursor > '9') return false;
        digit = (uint32_t)(*cursor - '0');
        if (value > (maximum - digit) / 10u) return false;
        value = value * 10u + digit;
        cursor++;
    }
    if (value < minimum || value > maximum) return false;
    *out = value;
    return true;
}

static void usage(FILE *stream) {
    fputs("Usage: edgepulse [--demo] [--seed N] [--ticks N] [--json]\n"
          "  --seed   1..4294967295 (default 20261023)\n"
          "  --ticks  10..1000000 one-millisecond scheduler steps (default 12000)\n"
          "  --json   print only the machine-readable summary\n"
          "  --help   show this help\n", stream);
}

int main(int argc, char **argv) {
    uint32_t seed = 20261023, ticks = 12000, now = 0, step, duplicates = 0;
    bool json_only = false, command_ok, accounting_ok;
    simulation sim = {0};
    ep_device device;
    ep_store store;
    ep_hal hal = {sensor_read, link_send, &sim};
    const ep_metrics *metrics = &device.metrics;
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) { usage(stdout); return 0; }
        if (strcmp(argv[i], "--demo") == 0) continue;
        if (strcmp(argv[i], "--json") == 0) { json_only = true; continue; }
        if (strcmp(argv[i], "--seed") == 0 || strcmp(argv[i], "--ticks") == 0) {
            bool is_seed = strcmp(argv[i], "--seed") == 0;
            if (++i == argc || !parse_number(argv[i], is_seed ? 1u : 10u,
                    is_seed ? UINT32_MAX : 1000000u, is_seed ? &seed : &ticks)) {
                fputs("Invalid or missing numeric option.\n", stderr);
                usage(stderr);
                return 2;
            }
        } else {
            fputs("Unknown argument.\n", stderr);
            usage(stderr);
            return 2;
        }
    }
    sim.sensor_rng = seed;
    sim.link_rng = seed;
    sim.checksum = UINT32_C(2166136261);
    sim.valid = true;
    ep_parser_init(&sim.wire, receive_telemetry, &sim);
    ep_store_init(&store);
    ep_device_init(&device, &store, hal, 7, now);
    command_ok = send_configuration(&device, &sim.link_rng, &duplicates);
    for (step = 1; step <= ticks; step++) {
        sim.online = step < ticks / 4u || step >= (ticks * 3u) / 4u;
        /* A single scheduler stall models watchdog detection on the next call. */
        if (step == ticks / 2u) now += EP_WATCHDOG_MS;
        now++;
        ep_device_tick(&device, now);
    }
    accounting_ok = command_ok && sim.valid && sim.wire.used == 0 &&
        sim.wire.rejected == 0 && metrics->sent == sim.wire_received &&
        metrics->sampled == metrics->sent + metrics->dropped + metrics->reset_dropped + device.count &&
        metrics->attempts == metrics->sampled + metrics->sensor_errors &&
        metrics->commands_applied == 1 && store.writes == 1 && metrics->resets == 1;
    if (!json_only) {
        puts("EdgePulse C | deterministic host simulation");
        puts("Scenario: framed config + duplicate, sensor faults, link outage, watchdog stall, reconnect.");
        puts("One scheduler step = 1 ms; the injected stall adds 5000 ms. No real hardware is used.");
    }
    printf("{\"seed\":%" PRIu32 ",\"ticks\":%" PRIu32 ",\"simulated_ms\":%" PRIu32
        ",\"attempts\":%" PRIu32 ",\"sampled\":%" PRIu32 ",\"sent\":%" PRIu32
        ",\"dropped\":%" PRIu32 ",\"reset_dropped\":%" PRIu32 ",\"pending\":%zu"
        ",\"sensor_errors\":%" PRIu32 ",\"skipped\":%" PRIu32 ",\"resets\":%" PRIu32
        ",\"queue_peak\":%" PRIu32 ",\"commands_applied\":%" PRIu32 ",\"duplicate_commands\":%" PRIu32
        ",\"store_writes\":%" PRIu32 ",\"wire_received\":%" PRIu32 ",\"wire_rejected\":%" PRIu32
        ",\"telemetry_checksum\":%" PRIu32 ",\"accounting_ok\":%s}\n",
        seed, ticks, now, metrics->attempts, metrics->sampled, metrics->sent,
        metrics->dropped, metrics->reset_dropped, device.count, metrics->sensor_errors,
        metrics->skipped_periods, metrics->resets, metrics->queue_peak,
        metrics->commands_applied, duplicates, store.writes, sim.wire_received,
        sim.wire.rejected, sim.checksum, accounting_ok ? "true" : "false");
    return accounting_ok ? 0 : 1;
}
