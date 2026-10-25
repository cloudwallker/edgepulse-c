#include "ep_store.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
    return false; } } while (0)

/* Independent wire fixtures: CRC-CCITT-FALSE checked with Python crc_hqx. */
static const uint8_t record_max[EP_SLOT_SIZE] = {
    0x45u, 0x50u, 0x01u, 0x00u, 0xffu, 0xffu, 0xffu, 0xffu,
    0x7bu, 0x00u, 0x00u, 0x00u, 0x64u, 0x7du, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xbdu, 0x38u, 0xa5u
};
static const uint8_t record_zero[EP_SLOT_SIZE] = {
    0x45u, 0x50u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0xc8u, 0x01u, 0x00u, 0x00u, 0x18u, 0xfcu, 0xffu, 0xffu,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x69u, 0xa6u, 0xa5u
};
static const uint8_t record_bad_version[EP_SLOT_SIZE] = {
    0x45u, 0x50u, 0x02u, 0x00u, 0x07u, 0x00u, 0x00u, 0x00u,
    0x7bu, 0x00u, 0x00u, 0x00u, 0x64u, 0x7du, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x3cu, 0xb1u, 0xa5u
};
static const uint8_t record_bad_range[EP_SLOT_SIZE] = {
    0x45u, 0x50u, 0x01u, 0x00u, 0x07u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x64u, 0x7du, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xcfu, 0x66u, 0xa5u
};

static bool test_erased_store_defaults(void)
{
    ep_store store;
    ep_config config = {0u, 0};
    uint32_t generation = 99u;
    ep_store_init(&store);
    CHECK(!ep_store_load(&store, &config, &generation));
    CHECK(config.period_ms == 100u && config.threshold_mc == 30000);
    CHECK(generation == 0u && store.writes == 0u);
    for (size_t slot = 0u; slot < 2u; ++slot) {
        for (size_t byte = 0u; byte < EP_SLOT_SIZE; ++byte) {
            CHECK(store.slots[slot][byte] == 0xffu);
        }
    }
    return true;
}

static bool test_config_limits(void)
{
    static const ep_config valid[] = {{10u, -40000}, {60000u, 125000}};
    static const ep_config invalid[] = {
        {9u, 30000}, {60001u, 30000}, {100u, -40001},
        {100u, 125001}, {UINT32_MAX, INT32_MIN}
    };
    ep_store store;
    ep_store_init(&store);
    for (size_t i = 0u; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        ep_config loaded;
        CHECK(ep_config_valid(&valid[i]));
        CHECK(ep_store_save(&store, &valid[i], EP_STORE_COMPLETE));
        CHECK(ep_store_load(&store, &loaded, NULL));
        CHECK(loaded.period_ms == valid[i].period_ms);
        CHECK(loaded.threshold_mc == valid[i].threshold_mc);
    }
    for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        ep_store before = store;
        CHECK(!ep_config_valid(&invalid[i]));
        CHECK(!ep_store_save(&store, &invalid[i], EP_STORE_COMPLETE));
        CHECK(memcmp(store.slots, before.slots, sizeof(store.slots)) == 0);
        CHECK(store.writes == before.writes);
    }
    CHECK(!ep_config_valid(NULL));
    return true;
}

static bool test_first_save_all_cut_points(void)
{
    const ep_config wanted = {250u, -20000};
    for (size_t cut = 0u; cut <= EP_SLOT_SIZE; ++cut) {
        ep_store store;
        ep_config loaded;
        uint32_t generation = UINT32_MAX;
        ep_store_init(&store);
        CHECK(ep_store_save(&store, &wanted, cut) == (cut == EP_SLOT_SIZE));
        CHECK(store.writes == 1u);
        if (cut == EP_SLOT_SIZE) {
            CHECK(ep_store_load(&store, &loaded, &generation));
            CHECK(loaded.period_ms == 250u && loaded.threshold_mc == -20000);
            CHECK(generation == 1u);
        } else {
            CHECK(!ep_store_load(&store, &loaded, &generation));
            CHECK(loaded.period_ms == 100u && loaded.threshold_mc == 30000);
            CHECK(generation == 0u);
        }
    }
    return true;
}

static bool test_replacement_all_cut_points(void)
{
    ep_store base;
    const ep_config first = {100u, 31000};
    const ep_config old = {200u, 32000};
    const ep_config wanted = {300u, -30000};
    ep_store_init(&base);
    CHECK(ep_store_save(&base, &first, EP_STORE_COMPLETE));
    CHECK(ep_store_save(&base, &old, EP_STORE_COMPLETE));
    for (size_t cut = 0u; cut <= EP_SLOT_SIZE; ++cut) {
        ep_store store = base;
        ep_config loaded;
        uint32_t generation = 0u;
        CHECK(ep_store_save(&store, &wanted, cut) == (cut == EP_SLOT_SIZE));
        CHECK(store.writes == 3u);
        CHECK(memcmp(store.slots[1], base.slots[1], EP_SLOT_SIZE) == 0);
        CHECK(ep_store_load(&store, &loaded, &generation));
        if (cut == EP_SLOT_SIZE) {
            CHECK(loaded.period_ms == 300u && loaded.threshold_mc == -30000);
            CHECK(generation == 3u);
        } else {
            CHECK(loaded.period_ms == 200u && loaded.threshold_mc == 32000);
            CHECK(generation == 2u);
        }
    }
    return true;
}

static bool test_crc_corruption_fallback(void)
{
    ep_store store;
    ep_config loaded;
    uint32_t generation = 0u;
    ep_store_init(&store);
    memcpy(store.slots[0], record_max, EP_SLOT_SIZE);
    memcpy(store.slots[1], record_zero, EP_SLOT_SIZE);
    CHECK(ep_store_load(&store, &loaded, &generation));
    CHECK(generation == 0u && loaded.period_ms == 456u);
    store.slots[1][8] ^= 0x01u;
    CHECK(ep_store_load(&store, &loaded, &generation));
    CHECK(generation == UINT32_MAX && loaded.period_ms == 123u);
    store.slots[0][21] ^= 0x01u;
    CHECK(!ep_store_load(&store, &loaded, &generation));
    CHECK(generation == 0u && loaded.period_ms == 100u);
    return true;
}

static bool test_invalid_record_metadata(void)
{
    ep_store store;
    ep_config loaded;
    uint32_t generation = 99u;
    ep_store_init(&store);
    memcpy(store.slots[0], record_bad_version, EP_SLOT_SIZE);
    memcpy(store.slots[1], record_bad_range, EP_SLOT_SIZE);
    CHECK(!ep_store_load(&store, &loaded, &generation));
    CHECK(generation == 0u && loaded.period_ms == 100u);
    memcpy(store.slots[0], record_max, EP_SLOT_SIZE);
    store.slots[0][23] = 0xffu;
    CHECK(!ep_store_load(&store, &loaded, &generation));
    memcpy(store.slots[0], record_max, EP_SLOT_SIZE);
    store.slots[0][0] = 0u;
    CHECK(!ep_store_load(&store, &loaded, &generation));
    return true;
}

static bool test_generation_rollover(void)
{
    ep_store store;
    ep_config loaded;
    const ep_config wanted = {456u, -1000};
    uint32_t generation = 42u;
    ep_store_init(&store);
    memcpy(store.slots[0], record_max, EP_SLOT_SIZE);
    CHECK(ep_store_load(&store, &loaded, &generation));
    CHECK(generation == UINT32_MAX && loaded.period_ms == 123u);
    CHECK(ep_store_save(&store, &wanted, EP_STORE_COMPLETE));
    CHECK(ep_store_load(&store, &loaded, &generation));
    CHECK(generation == 0u && loaded.period_ms == 456u);
    CHECK(loaded.threshold_mc == -1000);
    /* Newest selection must not depend on physical slot order. */
    memcpy(store.slots[0], record_zero, EP_SLOT_SIZE);
    memcpy(store.slots[1], record_max, EP_SLOT_SIZE);
    CHECK(ep_store_load(&store, &loaded, &generation));
    CHECK(generation == 0u && loaded.period_ms == 456u);
    CHECK(ep_store_save(&store, &wanted, EP_STORE_COMPLETE));
    CHECK(ep_store_load(&store, &loaded, &generation));
    CHECK(generation == 1u);
    return true;
}

static bool test_invalid_calls_leave_store_unchanged(void)
{
    ep_store store;
    ep_store before;
    ep_config loaded;
    const ep_config wanted = {500u, 20000};
    ep_store_init(&store);
    CHECK(ep_store_save(&store, &wanted, EP_STORE_COMPLETE));
    before = store;
    CHECK(!ep_store_save(&store, &wanted, EP_SLOT_SIZE + 1u));
    CHECK(!ep_store_save(&store, NULL, EP_STORE_COMPLETE));
    CHECK(!ep_store_save(NULL, &wanted, EP_STORE_COMPLETE));
    CHECK(!ep_store_load(NULL, &loaded, NULL));
    CHECK(!ep_store_load(&store, NULL, NULL));
    CHECK(memcmp(store.slots, before.slots, sizeof(store.slots)) == 0);
    CHECK(store.writes == before.writes);
    ep_store_init(NULL);
    return true;
}

int main(void)
{
    typedef bool (*test_fn)(void);
    static const test_fn tests[] = {
        test_erased_store_defaults, test_config_limits,
        test_first_save_all_cut_points, test_replacement_all_cut_points,
        test_crc_corruption_fallback, test_invalid_record_metadata,
        test_generation_rollover, test_invalid_calls_leave_store_unchanged
    };
    unsigned passed = 0u;
    for (size_t i = 0u; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i]()) { ++passed; }
    }
    printf("store: %u/8 tests passed; 50 interruption points\n", passed);
    return passed == 8u ? 0 : 1;
}
