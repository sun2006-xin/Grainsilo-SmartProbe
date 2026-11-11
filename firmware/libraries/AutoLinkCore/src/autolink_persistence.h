#ifndef AUTOLINK_PERSISTENCE_H
#define AUTOLINK_PERSISTENCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AL_RECORD_MAGIC 0x414C5632u
#define AL_RECORD_VERSION 1u
#define AL_RECORD_VALID 0xA55Au
#define AL_RECORD_PAYLOAD_MAX 2048u

typedef struct {
    void *context;
    bool (*read_slot)(void *context, uint8_t slot, void *data, size_t len);
    bool (*write_slot)(void *context, uint8_t slot, const void *data, size_t len);
} al_storage_port_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t payload_len;
    uint32_t generation;
    uint8_t payload[AL_RECORD_PAYLOAD_MAX];
    uint16_t crc;
    uint16_t valid_marker;
} al_persistent_record_t;

typedef struct {
    al_storage_port_t storage;
    uint32_t generation;
    uint8_t active_slot;
    bool loaded;
} al_journal_t;

void al_journal_init(al_journal_t *journal, al_storage_port_t storage);
bool al_journal_load(al_journal_t *journal, void *payload, size_t capacity,
                     size_t *payload_len);
bool al_journal_commit(al_journal_t *journal, const void *payload,
                       size_t payload_len);
bool al_record_valid(const al_persistent_record_t *record);

#ifdef __cplusplus
}
#endif
#endif
