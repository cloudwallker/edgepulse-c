#include "ep_store.h"
#include "ep_protocol.h"
#include <string.h>

/* Stable byte layout (all integers little endian):
 *  0..1  magic "EP"      2 version=1     3 reserved=0
 *  4..7  generation      8..11 period   12..15 signed threshold
 * 16..20 reserved=0     21..22 CRC16 over bytes 0..20
 * 23     commit=0xA5, programmed last.
 * This models an atomic erase followed by individual byte writes in RAM.
 * It does not claim hardware flash, filesystem or power-loss durability.
 */
#define EP_RECORD_CRC_OFFSET 21u
#define EP_RECORD_COMMIT_OFFSET 23u
#define EP_RECORD_COMMIT 0xa5u

static uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
           ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
    for (size_t i = 0u; i < 4u; ++i) {
        bytes[i] = (uint8_t)(value >> (8u * i));
    }
}

static int32_t read_i32(const uint8_t *bytes)
{
    const uint32_t raw = read_u32(bytes);
    /* Avoid the implementation-defined unsigned-to-signed conversion. */
    if (raw <= (uint32_t)INT32_MAX) { return (int32_t)raw; }
    return -1 - (int32_t)(UINT32_MAX - raw);
}

bool ep_config_valid(const ep_config *config)
{
    return config != NULL && config->period_ms >= 10u &&
           config->period_ms <= 60000u && config->threshold_mc >= -40000 &&
           config->threshold_mc <= 125000;
}

void ep_store_init(ep_store *store)
{
    if (store != NULL) {
        memset(store->slots, 0xff, sizeof(store->slots));
        store->writes = 0u;
        store->commit = NULL;
        store->commit_ctx = NULL;
    }
}

static bool decode_record(const uint8_t *record, ep_config *config,
                          uint32_t *generation)
{
    const uint16_t stored_crc = (uint16_t)((uint16_t)record[21] |
                                          ((uint16_t)record[22] << 8u));
    if (record[0] != 0x45u || record[1] != 0x50u || record[2] != 1u ||
        record[3] != 0u || record[EP_RECORD_COMMIT_OFFSET] != EP_RECORD_COMMIT) {
        return false;
    }
    for (size_t i = 16u; i < EP_RECORD_CRC_OFFSET; ++i) {
        if (record[i] != 0u) { return false; }
    }
    if (ep_crc16(record, EP_RECORD_CRC_OFFSET) != stored_crc) { return false; }
    config->period_ms = read_u32(record + 8u);
    config->threshold_mc = read_i32(record + 12u);
    if (!ep_config_valid(config)) { return false; }
    *generation = read_u32(record + 4u);
    return true;
}

static int newest_slot(const ep_store *store, ep_config *config,
                       uint32_t *generation)
{
    ep_config decoded[2];
    uint32_t generations[2] = {0u, 0u};
    const bool valid0 = decode_record(store->slots[0], &decoded[0], &generations[0]);
    const bool valid1 = decode_record(store->slots[1], &decoded[1], &generations[1]);
    int selected = -1;
    if (valid0 && valid1) {
        /* Consecutive committed writes differ by one, even across wrap.
         * Equal/ambiguous generations deterministically prefer slot zero. */
        const uint32_t distance = generations[1] - generations[0];
        selected = distance != 0u && distance < UINT32_C(0x80000000) ? 1 : 0;
    } else if (valid0) {
        selected = 0;
    } else if (valid1) {
        selected = 1;
    }
    if (selected >= 0) {
        *config = decoded[selected];
        *generation = generations[selected];
    }
    return selected;
}

bool ep_store_load(const ep_store *store, ep_config *config, uint32_t *generation)
{
    uint32_t selected_generation = 0u;
    bool valid;
    if (store == NULL || config == NULL) { return false; }
    config->period_ms = 100u;
    config->threshold_mc = 30000;
    valid = newest_slot(store, config, &selected_generation) >= 0;
    if (generation != NULL) { *generation = selected_generation; }
    return valid;
}

bool ep_store_save(ep_store *store, const ep_config *config, size_t cut_after)
{
    uint8_t record[EP_SLOT_SIZE] = {0u};
    ep_config previous;
    uint32_t generation = 0u;
    uint16_t crc;
    int active;
    size_t target;
    size_t bytes_to_write;
    if (store == NULL || !ep_config_valid(config) ||
        (cut_after != EP_STORE_COMPLETE && cut_after > EP_SLOT_SIZE) ||
        (store->commit != NULL && cut_after != EP_STORE_COMPLETE)) {
        return false;
    }
    active = newest_slot(store, &previous, &generation);
    target = active == 0 ? 1u : 0u;
    record[0] = 0x45u;
    record[1] = 0x50u;
    record[2] = 1u;
    write_u32(record + 4u, generation + 1u);
    write_u32(record + 8u, config->period_ms);
    write_u32(record + 12u, (uint32_t)config->threshold_mc);
    crc = ep_crc16(record, EP_RECORD_CRC_OFFSET);
    record[21] = (uint8_t)crc;
    record[22] = (uint8_t)(crc >> 8u);
    record[EP_RECORD_COMMIT_OFFSET] = EP_RECORD_COMMIT;

    /* Count accepted write attempts, including fault-injected interruptions. */
    ++store->writes;
    /* An external backend must confirm the whole record before RAM commits.
     * A failed disk sync may still have reached storage; the backend owns its
     * unhealthy/reopen policy. Never publish that uncertain result in RAM. */
    if (store->commit != NULL &&
        !store->commit(store->commit_ctx, target, record)) {
        return false;
    }
    memset(store->slots[target], 0xff, EP_SLOT_SIZE);
    bytes_to_write = cut_after == EP_STORE_COMPLETE ? EP_SLOT_SIZE : cut_after;
    for (size_t i = 0u; i < bytes_to_write; ++i) {
        store->slots[target][i] = record[i];
    }
    return bytes_to_write == EP_SLOT_SIZE;
}
