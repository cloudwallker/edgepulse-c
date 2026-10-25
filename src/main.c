#include "ep_device.h"
#include "ep_file_store.h"
#include "ep_receiver.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <stdlib.h>
#include <windows.h>
#endif

#define SIM_ACK_CAP 8u
typedef struct { ep_frame frame; uint32_t due; bool corrupt, duplicate; } queued_ack;

typedef struct {
    ep_parser wire;
    ep_parser ack_wire;
    ep_receiver receiver;
    ep_device *device;
    queued_ack acks[SIM_ACK_CAP];
    size_t ack_head, ack_count;
    ep_frame expected;
    uint32_t sensor_rng, link_rng, wire_received, checksum;
    uint32_t last_boot, last_seq;
    uint32_t now, fault_phase, wire_transmitted, data_lost, data_corrupted;
    uint32_t ack_generated, ack_lost, ack_corrupted_delivered;
    bool online, expecting, valid, have_last, reliable;
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

static bool deliver_telemetry(void *ctx, const ep_frame *frame) {
    simulation *sim = ctx;
    const ep_frame *expected = &sim->expected;
    bool equal = sim->expecting && frame->device == expected->device &&
        frame->boot == expected->boot && frame->seq == expected->seq &&
        frame->len == 9 && memcmp(frame->payload, expected->payload, 9) == 0;
    if (!equal) sim->valid = false;
    return equal;
}

static void queue_ack(simulation *sim, const ep_frame *ack) {
    uint32_t ticket = ++sim->ack_generated + sim->fault_phase;
    queued_ack *pending;
    if (ticket % 5u == 1u) { sim->ack_lost++; return; }
    if (sim->ack_count == SIM_ACK_CAP) { sim->valid = false; return; }
    pending = &sim->acks[(sim->ack_head + sim->ack_count) % SIM_ACK_CAP];
    pending->frame = *ack;
    pending->due = sim->now + 2u;
    pending->corrupt = ticket % 13u == 2u;
    pending->duplicate = !pending->corrupt && ticket % 8u == 0u;
    sim->ack_count++;
}

static void receive_telemetry(void *ctx, const ep_frame *frame) {
    simulation *sim = ctx;
    const ep_frame *expected = &sim->expected;
    if (sim->reliable) {
        ep_frame ack;
        ep_receive_result result;
        sim->wire_received++;
        result = ep_receiver_receive(&sim->receiver, frame, &ack);
        if (result == EP_RX_NEW || result == EP_RX_DUPLICATE) queue_ack(sim, &ack);
        else sim->valid = false;
        return;
    }
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

static void receive_ack(void *ctx, const ep_frame *frame) {
    simulation *sim = ctx;
    /* Duplicate or delayed ACKs are expected; only the exact live head can pop. */
    (void)ep_device_acknowledge(sim->device, frame);
}

static void deliver_due_acks(simulation *sim) {
    while (sim->ack_count && (uint32_t)(sim->now - sim->acks[sim->ack_head].due) < UINT32_C(0x80000000)) {
        queued_ack pending = sim->acks[sim->ack_head];
        uint8_t bytes[EP_FRAME_MAX];
        size_t length;
        sim->ack_head = (sim->ack_head + 1u) % SIM_ACK_CAP;
        sim->ack_count--;
        if (!sim->online) { sim->ack_lost++; continue; }
        length = ep_encode(&pending.frame, bytes, sizeof(bytes));
        if (!length) { sim->valid = false; continue; }
        if (pending.corrupt) { bytes[length - 1u] ^= 1u; sim->ack_corrupted_delivered++; }
        fragmented_feed(&sim->ack_wire, bytes, length, &sim->link_rng);
        if (pending.duplicate) fragmented_feed(&sim->ack_wire, bytes, length, &sim->link_rng);
        ep_parser_flush(&sim->ack_wire); /* Explicit simulated packet-idle boundary. */
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
    if (sim->reliable) {
        uint32_t ticket = ++sim->wire_transmitted + sim->fault_phase;
        if (ticket % 7u == 1u) { sim->data_lost++; sim->expecting = false; return true; }
        if (ticket % 11u == 2u) { bytes[length - 1u] ^= 1u; sim->data_corrupted++; }
        fragmented_feed(&sim->wire, bytes, length, &sim->link_rng);
        if (ticket % 11u != 2u && ticket % 9u == 0u)
            fragmented_feed(&sim->wire, bytes, length, &sim->link_rng);
        ep_parser_flush(&sim->wire);
        sim->expecting = false;
        return true; /* Link acceptance deliberately does not promise delivery. */
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

static bool send_configuration(ep_device *device, ep_config config, uint32_t *rng, uint32_t *duplicates) {
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
    put32(frame.payload, config.period_ms);
    put32(frame.payload + 4, (uint32_t)config.threshold_mc);
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
    fputs("Usage: edgepulse [--demo] [--seed N] [--ticks N] [--json] [--store FILE] [--reliable]\n"
          "       edgepulse config show FILE\n"
          "       edgepulse config set FILE PERIOD_MS THRESHOLD_MC\n"
          "  --seed   1..4294967295 (default 20261023)\n"
          "  --ticks  10..1000000 one-millisecond scheduler steps (default 12000)\n"
          "  --store  load an existing configuration file for this demo\n"
          "  --reliable  require ACKs; retry at 100 ms, up to three retries\n"
          "  config   period 10..60000 ms; threshold -40000..125000 mC\n"
          "  --json   print only the machine-readable summary\n"
          "  --help   show this help\n", stream);
}

static bool parse_threshold(const char *text, int32_t *out) {
    uint32_t magnitude;
    const bool negative = text[0] == '-';
    if (!parse_number(text + (negative ? 1 : 0), 0, negative ? 40000u : 125000u, &magnitude)) return false;
    *out = negative ? -(int32_t)magnitude : (int32_t)magnitude;
    return true;
}

static int storage_error(ep_file_status status) {
    fprintf(stderr, "Configuration storage error: %s.\n", ep_file_status_string(status));
    return 1;
}

static int configuration_cli(int argc, char **argv) {
    ep_store store;
    ep_file_store file;
    ep_config config;
    uint32_t generation;
    ep_file_status status;
    bool set;
    if (argc < 3) { usage(stderr); return 2; }
    set = strcmp(argv[2], "set") == 0;
    if ((!set && strcmp(argv[2], "show") != 0) || argc != (set ? 6 : 4) || !argv[3][0]) {
        usage(stderr); return 2;
    }
    if (set && (!parse_number(argv[4], 10, 60000, &config.period_ms) ||
                !parse_threshold(argv[5], &config.threshold_mc))) {
        fputs("Invalid configuration value.\n", stderr); return 2;
    }
    status = ep_file_store_open(&file, &store, argv[3], set);
    if (status != EP_FILE_OK) return storage_error(status);
    if (set && !ep_store_save(&store, &config, EP_STORE_COMPLETE)) {
        status = file.status;
        ep_file_store_close(&file);
        return storage_error(status);
    }
    if (!ep_store_load(&store, &config, &generation)) {
        ep_file_store_close(&file);
        return storage_error(EP_FILE_FORMAT);
    }
    ep_file_store_close(&file);
    printf("{\"period_ms\":%" PRIu32 ",\"threshold_mc\":%" PRId32
           ",\"generation\":%" PRIu32 ",\"persistent\":true}\n",
           config.period_ms, config.threshold_mc, generation);
    return 0;
}

static int run_cli(int argc, char **argv) {
    uint32_t seed = 20261023, ticks = 12000, now = 0, step, duplicates = 0;
    bool json_only = false, reliable = false, command_ok, accounting_ok;
    simulation sim = {0};
    ep_device device;
    ep_store store;
    ep_file_store file;
    const char *store_path = NULL;
    ep_hal hal = {sensor_read, link_send, &sim};
    const ep_metrics *metrics = &device.metrics;
    int i;
    if (argc > 1 && strcmp(argv[1], "config") == 0) return configuration_cli(argc, argv);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) { usage(stdout); return 0; }
        if (strcmp(argv[i], "--demo") == 0) continue;
        if (strcmp(argv[i], "--json") == 0) { json_only = true; continue; }
        if (strcmp(argv[i], "--reliable") == 0) {
            if (reliable) { fputs("Duplicate reliability option.\n", stderr); return 2; }
            reliable = true;
            continue;
        }
        if (strcmp(argv[i], "--store") == 0) {
            if (store_path != NULL || ++i == argc || !argv[i][0] || strncmp(argv[i], "--", 2) == 0) {
                fputs("Invalid, missing or duplicate storage option.\n", stderr); return 2;
            }
            store_path = argv[i];
            continue;
        }
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
    sim.reliable = reliable;
    sim.fault_phase = seed % 97u;
    sim.device = &device;
    ep_parser_init(&sim.wire, receive_telemetry, &sim);
    ep_parser_init(&sim.ack_wire, receive_ack, &sim);
    if (store_path != NULL) {
        ep_file_status status = ep_file_store_open(&file, &store, store_path, false);
        if (status != EP_FILE_OK) return storage_error(status);
    } else ep_store_init(&store);
    ep_device_init(&device, &store, hal, 7, now);
    ep_receiver_init(&sim.receiver, device.device, device.boot, deliver_telemetry, &sim);
    if (reliable && !ep_device_enable_reliable(&device)) {
        if (store_path != NULL) ep_file_store_close(&file);
        fputs("Unable to enable reliable delivery.\n", stderr); return 1;
    }
    command_ok = send_configuration(&device, store_path != NULL ? device.config : (ep_config){10, 30000},
                                    &sim.link_rng, &duplicates);
    if (!command_ok) {
        if (store_path != NULL) {
            ep_file_status status = file.status;
            ep_file_store_close(&file);
            return storage_error(status);
        }
        fputs("Configuration command failed.\n", stderr); return 1;
    }
    for (step = 1; step <= ticks; step++) {
        sim.online = step < ticks / 4u || step >= (ticks * 3u) / 4u;
        /* A single scheduler stall models watchdog detection on the next call. */
        if (step == ticks / 2u) now += EP_WATCHDOG_MS;
        now++;
        sim.now = now;
        ep_device_tick(&device, now);
        if (reliable) {
            if (sim.receiver.boot != device.boot) ep_receiver_begin_session(&sim.receiver, device.boot);
            /* Dispatch after tick/send returns; callbacks never reenter the device. */
            deliver_due_acks(&sim);
        }
    }
    accounting_ok = command_ok && sim.valid && sim.wire.used == 0 && sim.ack_wire.used == 0 &&
        metrics->sampled == metrics->sent + metrics->dropped + metrics->reset_dropped + metrics->delivery_timeouts + device.count &&
        metrics->attempts == metrics->sampled + metrics->sensor_errors &&
        metrics->commands_applied == 1 && store.writes == 1 && metrics->resets == 1;
    if (reliable) {
        accounting_ok = accounting_ok && metrics->sent == metrics->acknowledged &&
            metrics->acknowledged <= sim.receiver.metrics.accepted &&
            sim.wire_received == sim.receiver.metrics.accepted + sim.receiver.metrics.duplicates &&
            sim.ack_wire.accepted == metrics->acknowledged + metrics->invalid_acks &&
            sim.receiver.metrics.conflicts == 0 && sim.receiver.metrics.stale == 0 &&
            sim.receiver.metrics.invalid == 0 && sim.receiver.metrics.delivery_failures == 0;
    } else accounting_ok = accounting_ok && sim.wire.rejected == 0 && metrics->sent == sim.wire_received;
    if (!json_only) {
        puts("EdgePulse C | deterministic host simulation");
        puts("Scenario: framed config + duplicate, sensor faults, link outage, watchdog stall, reconnect.");
        puts("One scheduler step = 1 ms; the injected stall adds 5000 ms. No real hardware is used.");
        if (reliable) puts("Reliable mode: data/ACK loss, CRC corruption, duplication and deferred ACKs; expiry means unconfirmed delivery.");
    }
    printf("{\"seed\":%" PRIu32 ",\"ticks\":%" PRIu32 ",\"simulated_ms\":%" PRIu32
        ",\"attempts\":%" PRIu32 ",\"sampled\":%" PRIu32 ",\"sent\":%" PRIu32
        ",\"dropped\":%" PRIu32 ",\"reset_dropped\":%" PRIu32 ",\"pending\":%zu"
        ",\"sensor_errors\":%" PRIu32 ",\"skipped\":%" PRIu32 ",\"resets\":%" PRIu32
        ",\"queue_peak\":%" PRIu32 ",\"commands_applied\":%" PRIu32 ",\"duplicate_commands\":%" PRIu32
        ",\"store_writes\":%" PRIu32 ",\"wire_received\":%" PRIu32 ",\"wire_rejected\":%" PRIu32
        ",\"telemetry_checksum\":%" PRIu32 ",\"accounting_ok\":%s"
        ",\"period_ms\":%" PRIu32 ",\"threshold_mc\":%" PRId32 ",\"persistent\":%s",
        seed, ticks, now, metrics->attempts, metrics->sampled, metrics->sent,
        metrics->dropped, metrics->reset_dropped, device.count, metrics->sensor_errors,
        metrics->skipped_periods, metrics->resets, metrics->queue_peak,
        metrics->commands_applied, duplicates, store.writes, sim.wire_received,
        sim.wire.rejected, sim.checksum, accounting_ok ? "true" : "false",
        device.config.period_ms, device.config.threshold_mc, store_path != NULL ? "true" : "false");
    if (reliable) {
        printf(",\"reliable\":true,\"tx_attempts\":%" PRIu32 ",\"retransmissions\":%" PRIu32
            ",\"acknowledged\":%" PRIu32 ",\"delivery_timeouts\":%" PRIu32 ",\"invalid_acks\":%" PRIu32
            ",\"wire_transmitted\":%" PRIu32 ",\"receiver_unique\":%" PRIu32 ",\"receiver_duplicates\":%" PRIu32
            ",\"receiver_rejected\":%" PRIu32 ",\"data_lost\":%" PRIu32 ",\"data_corrupted\":%" PRIu32
            ",\"ack_lost\":%" PRIu32 ",\"ack_received\":%" PRIu32 ",\"ack_rejected\":%" PRIu32
            ",\"ack_corrupted_delivered\":%" PRIu32 ",\"pending_acks\":%zu",
            metrics->tx_attempts, metrics->retransmissions, metrics->acknowledged, metrics->delivery_timeouts,
            metrics->invalid_acks, sim.wire_transmitted, sim.receiver.metrics.accepted, sim.receiver.metrics.duplicates,
            sim.receiver.metrics.invalid + sim.receiver.metrics.conflicts + sim.receiver.metrics.stale + sim.receiver.metrics.delivery_failures,
            sim.data_lost, sim.data_corrupted, sim.ack_lost, sim.ack_wire.accepted, sim.ack_wire.rejected,
            sim.ack_corrupted_delivered, sim.ack_count);
    }
    puts("}");
    if (store_path != NULL) ep_file_store_close(&file);
    return accounting_ok ? 0 : 1;
}

#ifdef _WIN32
/* Windows CRT's narrow argv may lose characters outside the active code page.
 * Convert the native wide command line to UTF-8 for the portable CLI/backend. */
int wmain(int argc, wchar_t **wide_argv) {
    char **argv = calloc((size_t)argc + 1u, sizeof(*argv));
    int i, result = 1;
    if (argv == NULL) { fputs("Unable to read command line.\n", stderr); return 1; }
    for (i = 0; i < argc; i++) {
        int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1, NULL, 0, NULL, NULL);
        if (length <= 0 || (argv[i] = malloc((size_t)length)) == NULL ||
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1, argv[i], length, NULL, NULL) == 0) {
            fputs("Unable to read command line.\n", stderr);
            goto cleanup;
        }
    }
    result = run_cli(argc, argv);
cleanup:
    for (i = 0; i < argc; i++) free(argv[i]);
    free(argv);
    return result;
}
#else
int main(int argc, char **argv) { return run_cli(argc, argv); }
#endif
