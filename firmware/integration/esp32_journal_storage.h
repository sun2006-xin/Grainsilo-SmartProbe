/* GrainSilo ESP32 Preferences adapter for the frozen AutoLink journal.
 *
 * ESP32 Preferences cannot store the complete al_persistent_record_t as one
 * blob on the target (the current record is 2064 bytes).  The journal owner
 * still receives and validates one complete record; this adapter stores only
 * the fixed record header, the record's effective payload, and the CRC/valid
 * trailer.  Reads zero-fill the unused payload tail before returning the
 * original record shape, so the frozen journal's CRC and torn-record rules
 * remain authoritative.
 */
#ifndef GRAINSILO_ESP32_JOURNAL_STORAGE_H
#define GRAINSILO_ESP32_JOURNAL_STORAGE_H

#include <Preferences.h>
#include <autolink_persistence.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Keep a comfortable margin below the target NVS blob boundary.  Current
 * GrainSilo registry/node payloads are far smaller than this limit. */
#define GRAINSILO_NVS_JOURNAL_MAX_COMPACT 1900u
#define GRAINSILO_NVS_JOURNAL_HEADER_SIZE \
    offsetof(al_persistent_record_t, payload)
#define GRAINSILO_NVS_JOURNAL_TRAILER_SIZE \
    (sizeof(al_persistent_record_t) - offsetof(al_persistent_record_t, crc))

static size_t grainsilo_nvs_journal_compact_length(
    const al_persistent_record_t *record)
{
    if (record == NULL || record->payload_len > AL_RECORD_PAYLOAD_MAX)
        return 0u;
    return GRAINSILO_NVS_JOURNAL_HEADER_SIZE + record->payload_len +
           GRAINSILO_NVS_JOURNAL_TRAILER_SIZE;
}

static bool grainsilo_nvs_journal_read(Preferences &preferences, uint8_t slot,
                                       void *data, size_t length,
                                       const char *legacy_slot0,
                                       const char *legacy_slot1)
{
    static uint8_t compact[GRAINSILO_NVS_JOURNAL_MAX_COMPACT];
    al_persistent_record_t *record = (al_persistent_record_t *)data;
    const char *legacy = slot == 0u ? legacy_slot0 : legacy_slot1;
    size_t stored;
    size_t expected;

    if (slot > 1u || record == NULL ||
        length != sizeof(al_persistent_record_t) || legacy == NULL)
        return false;
    stored = preferences.getBytesLength(legacy);
    if (stored == length)
        return preferences.getBytes(legacy, record, length) == length;
    if (stored < GRAINSILO_NVS_JOURNAL_HEADER_SIZE +
                    GRAINSILO_NVS_JOURNAL_TRAILER_SIZE ||
        stored > sizeof(compact) ||
        preferences.getBytes(legacy, compact, stored) != stored)
        return false;
    if (stored < offsetof(al_persistent_record_t, payload_len) +
                 sizeof(record->payload_len)) return false;
    memcpy(record, compact, GRAINSILO_NVS_JOURNAL_HEADER_SIZE);
    expected = grainsilo_nvs_journal_compact_length(record);
    if (expected == 0u || expected != stored) return false;
    memset(record->payload, 0, sizeof(record->payload));
    memcpy(record->payload, compact + GRAINSILO_NVS_JOURNAL_HEADER_SIZE,
           record->payload_len);
    memcpy((uint8_t *)record + offsetof(al_persistent_record_t, crc),
           compact + GRAINSILO_NVS_JOURNAL_HEADER_SIZE + record->payload_len,
           GRAINSILO_NVS_JOURNAL_TRAILER_SIZE);
    return true;
}

static bool grainsilo_nvs_journal_write(Preferences &preferences,
                                        uint8_t slot, const void *data,
                                        size_t length,
                                        const char *legacy_slot0,
                                        const char *legacy_slot1)
{
    static uint8_t compact[GRAINSILO_NVS_JOURNAL_MAX_COMPACT];
    const al_persistent_record_t *record =
        (const al_persistent_record_t *)data;
    const char *legacy = slot == 0u ? legacy_slot0 : legacy_slot1;
    size_t compact_length;

    if (slot > 1u || record == NULL || legacy == NULL ||
        length != sizeof(al_persistent_record_t)) return false;
    compact_length = grainsilo_nvs_journal_compact_length(record);
    if (compact_length == 0u || compact_length > sizeof(compact)) return false;
    memcpy(compact, record, GRAINSILO_NVS_JOURNAL_HEADER_SIZE);
    memcpy(compact + GRAINSILO_NVS_JOURNAL_HEADER_SIZE, record->payload,
           record->payload_len);
    memcpy(compact + GRAINSILO_NVS_JOURNAL_HEADER_SIZE + record->payload_len,
           (const uint8_t *)record + offsetof(al_persistent_record_t, crc),
           GRAINSILO_NVS_JOURNAL_TRAILER_SIZE);
    return preferences.putBytes(legacy, compact, compact_length) ==
           compact_length;
}

#endif /* GRAINSILO_ESP32_JOURNAL_STORAGE_H */
