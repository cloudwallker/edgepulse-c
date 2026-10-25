#ifndef EP_STORE_H
#define EP_STORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define EP_SLOT_SIZE 24u
#define EP_STORE_COMPLETE ((size_t)-1)
typedef struct { uint32_t period_ms; int32_t threshold_mc; } ep_config;
/* Complete record commit, before RAM becomes visible. False means unconfirmed.
 * Callback and ctx must outlive the store; callbacks may not reenter it. */
typedef bool (*ep_store_commit_fn)(void *ctx, size_t target, const uint8_t record[EP_SLOT_SIZE]);
typedef struct {
    uint8_t slots[2][EP_SLOT_SIZE]; uint32_t writes;
    ep_store_commit_fn commit; void *commit_ctx;
} ep_store;
bool ep_config_valid(const ep_config *config);
void ep_store_init(ep_store *store);
/* False means no valid record; config receives defaults (100 ms, 30000 mC). */
bool ep_store_load(const ep_store *store, ep_config *config, uint32_t *generation);
/* cut_after counts bytes successfully written (0..24); COMPLETE writes all.
 * Erasing target slot is atomic in this RAM model, not a hardware guarantee.
 * A bound commit callback requires EP_STORE_COMPLETE; RAM cuts are rejected. */
bool ep_store_save(ep_store *store, const ep_config *config, size_t cut_after);
#endif
