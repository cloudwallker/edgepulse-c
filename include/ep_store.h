#ifndef EP_STORE_H
#define EP_STORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define EP_SLOT_SIZE 24u
#define EP_STORE_COMPLETE ((size_t)-1)
typedef struct { uint32_t period_ms; int32_t threshold_mc; } ep_config;
typedef struct { uint8_t slots[2][EP_SLOT_SIZE]; uint32_t writes; } ep_store;
bool ep_config_valid(const ep_config *config);
void ep_store_init(ep_store *store);
/* False means no valid record; config receives defaults (100 ms, 30000 mC). */
bool ep_store_load(const ep_store *store, ep_config *config, uint32_t *generation);
/* cut_after counts bytes successfully written (0..24); COMPLETE writes all.
 * Erasing target slot is atomic in this host model, not a hardware guarantee. */
bool ep_store_save(ep_store *store, const ep_config *config, size_t cut_after);
#endif
