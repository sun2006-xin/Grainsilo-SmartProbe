#include "autolink_persistence.h"

#include <string.h>
#include <gs_proto.h>

static uint16_t record_crc(const al_persistent_record_t *record)
{
    return gs_crc16((const uint8_t *)record,
                    (uint16_t)offsetof(al_persistent_record_t, crc));
}

bool al_record_valid(const al_persistent_record_t *record)
{
    return record != NULL && record->magic == AL_RECORD_MAGIC &&
           record->version == AL_RECORD_VERSION &&
           record->payload_len <= AL_RECORD_PAYLOAD_MAX &&
           record->valid_marker == AL_RECORD_VALID &&
           record->crc == record_crc(record);
}

void al_journal_init(al_journal_t *journal, al_storage_port_t storage)
{
    memset(journal, 0, sizeof(*journal));
    journal->storage = storage;
}

bool al_journal_load(al_journal_t *journal, void *payload, size_t capacity,
                     size_t *payload_len)
{
    al_persistent_record_t records[2];
    bool valid[2] = {false, false};
    uint8_t selected;
    if (journal == NULL || payload == NULL || payload_len == NULL ||
        journal->storage.read_slot == NULL) return false;
    memset(records, 0, sizeof(records));
    valid[0] = journal->storage.read_slot(journal->storage.context, 0u,
                                          &records[0], sizeof(records[0])) &&
               al_record_valid(&records[0]);
    valid[1] = journal->storage.read_slot(journal->storage.context, 1u,
                                          &records[1], sizeof(records[1])) &&
               al_record_valid(&records[1]);
    if (!valid[0] && !valid[1]) return false;
    selected = valid[1] && (!valid[0] ||
               records[1].generation > records[0].generation) ? 1u : 0u;
    if (records[selected].payload_len > capacity) return false;
    memcpy(payload, records[selected].payload, records[selected].payload_len);
    *payload_len = records[selected].payload_len;
    journal->generation = records[selected].generation;
    journal->active_slot = selected;
    journal->loaded = true;
    return true;
}

bool al_journal_commit(al_journal_t *journal, const void *payload,
                       size_t payload_len)
{
    al_persistent_record_t record;
    al_persistent_record_t verify;
    uint8_t target;
    if (journal == NULL || payload == NULL ||
        payload_len > AL_RECORD_PAYLOAD_MAX ||
        journal->storage.write_slot == NULL ||
        journal->storage.read_slot == NULL) return false;
    memset(&record, 0, sizeof(record));
    record.magic = AL_RECORD_MAGIC;
    record.version = AL_RECORD_VERSION;
    record.payload_len = (uint16_t)payload_len;
    record.generation = journal->generation + 1u;
    memcpy(record.payload, payload, payload_len);
    record.valid_marker = AL_RECORD_VALID;
    record.crc = record_crc(&record);
    target = journal->loaded ? (uint8_t)(journal->active_slot ^ 1u) : 0u;
    if (!journal->storage.write_slot(journal->storage.context, target,
                                     &record, sizeof(record))) return false;
    memset(&verify, 0, sizeof(verify));
    if (!journal->storage.read_slot(journal->storage.context, target,
                                    &verify, sizeof(verify)) ||
        !al_record_valid(&verify) ||
        verify.generation != record.generation) return false;
    journal->generation = record.generation;
    journal->active_slot = target;
    journal->loaded = true;
    return true;
}
