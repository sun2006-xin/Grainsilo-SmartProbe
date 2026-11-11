#ifndef GRAINSILO_REGISTRY_BOOT_POLICY_H
#define GRAINSILO_REGISTRY_BOOT_POLICY_H

#include <stdbool.h>

typedef enum {
    GS_REGISTRY_SLOT_NOT_FOUND = 0,
    GS_REGISTRY_SLOT_PRESENT,
    GS_REGISTRY_SLOT_READ_ERROR
} gs_registry_slot_state_t;

typedef enum {
    GS_REGISTRY_BOOT_RESTORED = 0,
    GS_REGISTRY_BOOT_EMPTY,
    GS_REGISTRY_BOOT_CORRUPT,
    GS_REGISTRY_BOOT_STORAGE_UNAVAILABLE
} gs_registry_boot_state_t;

/* A new network identity is safe only when NVS opened and both journal slots
 * are explicitly absent. A failed status read is not evidence of absence. */
static inline gs_registry_boot_state_t gs_registry_boot_classify(
    bool storage_open, bool registry_restored,
    gs_registry_slot_state_t slot0_state,
    gs_registry_slot_state_t slot1_state)
{
    if (registry_restored) return GS_REGISTRY_BOOT_RESTORED;
    if (!storage_open ||
        slot0_state == GS_REGISTRY_SLOT_READ_ERROR ||
        slot1_state == GS_REGISTRY_SLOT_READ_ERROR)
        return GS_REGISTRY_BOOT_STORAGE_UNAVAILABLE;
    if (slot0_state == GS_REGISTRY_SLOT_NOT_FOUND &&
        slot1_state == GS_REGISTRY_SLOT_NOT_FOUND)
        return GS_REGISTRY_BOOT_EMPTY;
    return GS_REGISTRY_BOOT_CORRUPT;
}

#endif
