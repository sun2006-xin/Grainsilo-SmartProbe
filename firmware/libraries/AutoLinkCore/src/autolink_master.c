/* GrainSilo changes: sampling policy, READ_INFO scheduling, offline probing and sensor-error liveness. */
#include "autolink_master.h"
#include "generated/autolink_semantics.h"

#include <string.h>

#define AL_MASTER_OFFLINE_PROBE_INTERVAL_MS UINT32_C(1000)

static void put_u64be(uint8_t *p, uint64_t value)
{
    int i;
    for (i = 7; i >= 0; --i) {
        p[i] = (uint8_t)value;
        value >>= 8;
    }
}

static uint64_t get_u64be(const uint8_t *p)
{
    uint64_t value = 0u;
    uint8_t i;
    for (i = 0u; i < 8u; ++i) value = (value << 8) | p[i];
    return value;
}

static uint32_t bytes_signature(const uint8_t *p, uint16_t length)
{
    uint32_t value = 2166136261u;
    uint16_t i;
    for (i = 0u; i < length; ++i) value = (value ^ p[i]) * 16777619u;
    return value;
}

static bool frame_equal(const gs_frame_t *left, const gs_frame_t *right)
{
    return left != NULL && right != NULL &&
           left->ver == right->ver && left->addr == right->addr &&
           left->seq == right->seq &&
           left->func == right->func && left->len == right->len &&
           left->len <= GS_DATA_MAX_LEN &&
           memcmp(left->data, right->data, left->len) == 0;
}

static bool cache_descriptors(al_master_node_t *node,
                              const uint8_t *data, uint16_t length)
{
    uint8_t count;
    uint8_t i;
    if (length < 4u) return false;
    count = data[1];
    if (count > GS_CODEC_CHANNEL_CAPACITY ||
        length != (uint16_t)(4u + (uint16_t)count * 8u)) return false;
    for (i = 0u; i < count; ++i) {
        uint8_t j;
        if (gs_parse_chdesc(data + 4u + (uint16_t)i * 8u, 8u,
                            &node->descriptors[i]) != GS_PARSE_OK) return false;
        for (j = 0u; j < i; ++j)
            if (node->descriptors[j].ch_id == node->descriptors[i].ch_id)
                return false;
    }
    node->sensor_type = data[0];
    node->descriptor_count = count;
    node->schema_rev = gs_get_u16be(data + 2u);
    node->descriptor_signature = bytes_signature(data + 4u,
                                                   (uint16_t)(length - 4u));
    return true;
}

static bool sample_is_trusted(const al_master_node_t *node,
                              const uint8_t *data, uint16_t length)
{
    gs_sample_t sample;
    uint8_t i;
    if (node == NULL || node->peer_proto_ver != GS_PROTO_VER ||
        node->descriptor_count == 0u ||
        gs_parse_sample_v22(data, (uint8_t)length, &sample) != GS_PARSE_OK ||
        sample.sensor_type != node->sensor_type ||
        sample.schema_rev != node->schema_rev ||
        sample.channel_count != node->descriptor_count) return false;
    for (i = 0u; i < sample.channel_count; ++i) {
        uint8_t j;
        bool matched = false;
        for (j = 0u; j < node->descriptor_count; ++j) {
            if (sample.channels[i].id == node->descriptors[j].ch_id) {
                matched = sample.channels[i].value_len ==
                          al_data_type_width(node->descriptors[j].data_type);
                break;
            }
        }
        if (!matched) return false;
    }
    return true;
}

static al_master_event_t event(al_master_event_kind_t kind)
{
    al_master_event_t result;
    memset(&result, 0, sizeof(result));
    result.kind = kind;
    result.node_index = -1;
    return result;
}

void al_master_init(al_master_t *master, uint64_t network_id)
{
    memset(master, 0, sizeof(*master));
    master->network_id = network_id;
    master->transaction.lease_node_index = -1;
    master->transaction.sleep_recovery_node_index = -1;
    master->offline_probe_node_index = -1;
    master->current_bus_baud = GS_BAUD_DEFAULT;
    master->bus_baud_known = true;
    master->persist_node_index = -1;
    master->work_sample_interval_ms = AL_MASTER_WORK_POLL_INTERVAL_MS;
}

bool al_master_set_sample_interval_ms(al_master_t *master,
                                      uint32_t interval_ms,
                                      al_master_time_ms_t now_ms)
{
    if (master == NULL ||
        interval_ms < AL_MASTER_SAMPLE_INTERVAL_MIN_MS ||
        interval_ms > AL_MASTER_SAMPLE_INTERVAL_MAX_MS) return false;
    if (master->work_sample_interval_ms != interval_ms) {
        master->work_sample_interval_ms = interval_ms;
        master->work_next_poll_ms = now_ms;
    }
    return true;
}

bool al_master_platform_status(const al_master_t *master,
                               al_master_platform_status_t *status)
{
    if (master == NULL || status == NULL) return false;
    status->active_baud = master->current_bus_baud;
    return true;
}

static bool stage_node_persistence(al_master_t *master, int16_t index)
{
    size_t validated_index;
    if (master->persist_kind != AL_MASTER_PERSIST_NONE || index < 0)
        return false;
    /* The signed form carries the public -1 sentinel. Validate first, then
     * convert once into the uint8_t node-count domain. */
    validated_index = (size_t)index;
    if (validated_index > master->node_count ||
        validated_index >= AL_MASTER_NODE_CAPACITY)
        return false;
    master->persist_kind = AL_MASTER_PERSIST_NODE;
    master->persist_node_index = index;
    master->persist_node_count = master->node_count;
    if (validated_index < master->node_count)
        master->persist_before_node = master->nodes[validated_index];
    else
        memset(&master->persist_before_node, 0,
               sizeof(master->persist_before_node));
    return true;
}

int16_t al_master_find_uid(const al_master_t *master,
                           const uint8_t uid[GS_UID_LEN])
{
    uint8_t i;
    for (i = 0u; i < master->node_count; ++i)
        if (memcmp(master->nodes[i].uid, uid, GS_UID_LEN) == 0) return (int16_t)i;
    return -1;
}

int16_t al_master_find_addr(const al_master_t *master, uint8_t addr)
{
    uint8_t i;
    for (i = 0u; i < master->node_count; ++i)
        if (master->nodes[i].addr == addr) return (int16_t)i;
    return -1;
}

uint8_t al_master_export_records(const al_master_t *master,
                                 al_master_node_record_t *records,
                                 uint8_t capacity)
{
    uint8_t count;
    uint8_t i;
    if (master == NULL || records == NULL) return 0u;
    count = master->node_count < capacity ? master->node_count : capacity;
    for (i = 0u; i < count; ++i) {
        const al_master_node_t *node = &master->nodes[i];
        al_master_node_record_t *record = &records[i];
        memset(record, 0, sizeof(*record));
        memcpy(record->uid, node->uid, GS_UID_LEN);
        record->addr = node->addr;
        record->network_id = node->network_id;
        record->sleep_state = (uint8_t)node->sleep_state;
        record->sleep_duration_ms = node->sleep_duration_ms;
        record->schema_rev = node->schema_rev;
        record->descriptor_signature = node->descriptor_signature;
        record->peer_proto_ver = node->peer_proto_ver;
        record->sensor_type = node->sensor_type;
        record->capabilities = node->capabilities;
        record->descriptor_count = node->descriptor_count;
        memcpy(record->descriptors, node->descriptors,
               sizeof(record->descriptors));
    }
    return count;
}

al_master_registry_export_result_t al_master_export_registry(
    const al_master_t *master, al_master_registry_snapshot_t *snapshot,
    al_master_node_record_t *records, uint8_t capacity)
{
    al_master_registry_snapshot_t value;
    uint8_t count;
    if (master == NULL || snapshot == NULL ||
        (master->node_count != 0u && records == NULL))
        return AL_MASTER_REGISTRY_EXPORT_INVALID_ARGUMENT;
    if (capacity < master->node_count)
        return AL_MASTER_REGISTRY_EXPORT_INSUFFICIENT_CAPACITY;

    memset(&value, 0, sizeof(value));
    value.network_id = master->network_id;
    value.current_bus_baud = master->current_bus_baud;
    value.bus_baud_known = master->bus_baud_known;
    value.node_count = master->node_count;
    count = master->node_count == 0u ? 0u :
            al_master_export_records(master, records, capacity);
    if (count != master->node_count)
        return AL_MASTER_REGISTRY_EXPORT_INVALID_ARGUMENT;
    *snapshot = value;
    return AL_MASTER_REGISTRY_EXPORT_OK;
}

bool al_master_restore_records(al_master_t *master,
                               const al_master_node_record_t *records,
                               uint8_t count)
{
    uint8_t i;
    if (master == NULL || (records == NULL && count != 0u) ||
        count > AL_MASTER_NODE_CAPACITY) return false;
    master->node_count = count;
    for (i = 0u; i < count; ++i) {
        const al_master_node_record_t *record = &records[i];
        al_master_node_t *node = &master->nodes[i];
        if (record->descriptor_count > GS_CODEC_CHANNEL_CAPACITY) return false;
        memset(node, 0, sizeof(*node));
        memcpy(node->uid, record->uid, GS_UID_LEN);
        node->addr = record->addr;
        node->network_id = record->network_id;
        node->online = true;
        if (record->sleep_state > AL_MASTER_SLEEP_RECOVERY_VERIFY) return false;
        node->sleep_state = record->sleep_state == AL_MASTER_SLEEP_NONE ?
                            AL_MASTER_SLEEP_NONE :
                            AL_MASTER_SLEEP_RECOVERY_VERIFY;
        node->sleep_lease = node->sleep_state != AL_MASTER_SLEEP_NONE;
        node->sleep_lease_confirmed = false;
        node->sleep_duration_ms = record->sleep_duration_ms;
        node->schema_rev = record->schema_rev;
        node->descriptor_signature = record->descriptor_signature;
        node->peer_proto_ver = record->peer_proto_ver;
        node->sensor_type = record->sensor_type;
        node->capabilities = record->capabilities;
        node->descriptor_count = record->descriptor_count;
        memcpy(node->descriptors, record->descriptors,
               sizeof(node->descriptors));
    }
    return true;
}

bool al_master_take_registry_dirty(al_master_t *master)
{
    bool dirty;
    if (master == NULL) return false;
    dirty = master->registry_dirty;
    master->registry_dirty = false;
    return dirty;
}

bool al_master_on_persist_result(al_master_t *master, bool success)
{
    bool first_sleep_epoch_write = false;
    if (master == NULL || master->persist_kind == AL_MASTER_PERSIST_NONE)
        return false;
    if (!success) {
        if (master->persist_kind == AL_MASTER_PERSIST_NODE &&
            master->persist_node_index >= 0) {
            first_sleep_epoch_write =
                master->persist_node_index ==
                    master->transaction.lease_node_index &&
                master->persist_before_node.sleep_state ==
                    AL_MASTER_SLEEP_NONE &&
                master->nodes[master->persist_node_index].sleep_state ==
                    AL_MASTER_SLEEP_UNCERTAIN;
            if (!first_sleep_epoch_write) {
                master->node_count = master->persist_node_count;
                if (master->persist_node_index < master->persist_node_count) {
                    master->nodes[master->persist_node_index] =
                        master->persist_before_node;
                    /* Persistence rollback cannot restore an in-flight flag
                     * after its transaction has already terminated. */
                    master->nodes[master->persist_node_index].
                        sleep_recovery_probe_in_flight = false;
                    if (master->nodes[master->persist_node_index].sleep_state ==
                        AL_MASTER_SLEEP_RECOVERY_VERIFY) {
                        master->nodes[master->persist_node_index].
                            sleep_recovery_schedule_armed = false;
                        master->nodes[master->persist_node_index].
                            sleep_recovery_last_result =
                                AL_MASTER_SLEEP_VERIFY_EARLY_WAKE;
                    }
                }
            }
        } else if (master->persist_kind == AL_MASTER_PERSIST_BAUD) {
            master->current_bus_baud = master->persist_before_baud;
            master->bus_baud_known = false;
            master->baud.recovery_cursor = 0u;
            master->baud.recovery_active = true;
            master->baud.state = AL_MASTER_BAUD_RECOVERY;
        } else if (master->persist_kind == AL_MASTER_PERSIST_REGISTRY) {
            master->persistence_fault = true;
        }
    } else {
        master->persistence_fault = false;
        if (master->persist_kind == AL_MASTER_PERSIST_BAUD) {
            master->bus_baud_known = true;
            if (master->baud.state == AL_MASTER_BAUD_RECOVERY_PERSIST) {
                master->baud.recovery_active = false;
                master->baud.state = AL_MASTER_BAUD_COMPLETE;
                master->baud_effect_ready = false;
            } else {
                master->baud_effect_ready = true;
            }
        } else if (master->persist_kind == AL_MASTER_PERSIST_NODE &&
                   master->persist_node_index ==
                       master->transaction.lease_node_index &&
                   master->transaction.sleep_epoch_established)
            master->transaction.sleep_epoch_persisted = true;
    }
    master->persist_kind = AL_MASTER_PERSIST_NONE;
    master->persist_node_index = -1;
    return true;
}

bool al_master_stage_registry_persistence(al_master_t *master)
{
    if (master == NULL || master->persist_kind != AL_MASTER_PERSIST_NONE)
        return false;
    master->persist_kind = AL_MASTER_PERSIST_REGISTRY;
    master->registry_dirty = true;
    return true;
}

void al_master_mark_bus_baud_unknown(al_master_t *master)
{
    if (master == NULL) return;
    master->bus_baud_known = false;
    master->baud.recovery_cursor = 0u;
    master->baud.recovery_active = true;
    master->baud.state = AL_MASTER_BAUD_RECOVERY;
}

bool al_master_restore_bus_baud(al_master_t *master, uint32_t baud)
{
    if (master == NULL || !al_is_supported_baud(baud)) {
        al_master_mark_bus_baud_unknown(master);
        return false;
    }
    master->current_bus_baud = baud;
    master->bus_baud_known = true;
    master->baud.recovery_active = false;
    master->baud.recovery_cursor = 0u;
    master->baud.state = AL_MASTER_BAUD_IDLE;
    return true;
}

static int16_t upsert(al_master_t *master, const uint8_t uid[GS_UID_LEN],
                      uint8_t addr, uint64_t network_id)
{
    int16_t index = al_master_find_uid(master, uid);
    if (index < 0) {
        if (master->node_count >= AL_MASTER_NODE_CAPACITY) return -1;
        index = master->node_count++;
        memset(&master->nodes[index], 0, sizeof(master->nodes[index]));
        memcpy(master->nodes[index].uid, uid, GS_UID_LEN);
    }
    master->nodes[index].addr = addr;
    master->nodes[index].network_id = network_id;
    master->nodes[index].online = true;
    master->nodes[index].missed = 0u;
    return index;
}

static bool request_length_valid(const al_request_contract_t *contract,
                                 const gs_frame_t *request)
{
    if (contract->length_kind == AL_LEN_FIXED)
        return request->len == contract->length;
    if (contract->length_kind == AL_LEN_RANGE)
        return request->len <= GS_DATA_MAX_LEN;
    return request->len >= 1u &&
           al_config_request_length(request->data[0]) == request->len;
}

bool al_master_begin(al_master_t *master, const gs_frame_t *request,
                     uint8_t old_addr, uint32_t timeout_ms)
{
    const al_request_contract_t *contract;
    uint8_t op;
    if (master == NULL || request == NULL || master->persistence_fault ||
        master->transaction.state != AL_MASTER_IDLE ||
        master->transaction.awaiting_tx_complete) return false;
    op = request->func == GS_FUNC_NET_MGMT && request->len >= 9u ?
         request->data[8] : 0u;
    contract = al_find_request_contract(request->func, op);
    if (contract == NULL || !request_length_valid(contract, request)) return false;

    memset(&master->transaction, 0, sizeof(master->transaction));
    master->transaction.request = *request;
    master->transaction.op = op;
    master->transaction.old_addr = old_addr;
    master->transaction.timeout_ms = timeout_ms;
    master->transaction.lease_node_index = -1;
    master->transaction.sleep_recovery_node_index = -1;
    master->transaction.after_tx_state = contract->txn == AL_TXN_COLLECT ?
                                         AL_MASTER_COLLECT :
                                         (contract->txn == AL_TXN_BROADCAST_NOACK ?
                                          AL_MASTER_IDLE :
                                          AL_MASTER_WAIT_RESPONSE);
    master->transaction.awaiting_tx_complete = true;
    if (request->func == GS_FUNC_SLEEP && request->len == 4u) {
        master->transaction.sleep_duration_ms = gs_get_u32be(request->data);
        master->transaction.lease_node_index =
            al_master_find_addr(master, request->addr);
    }
    return true;
}

bool al_master_tx_authorized(const al_master_t *master,
                             const gs_frame_t *frame)
{
    return master != NULL && master->transaction.awaiting_tx_complete &&
           frame_equal(&master->transaction.request, frame);
}

bool al_master_on_tx_complete(al_master_t *master,
                              al_master_time_ms_t request_end_ms)
{
    al_master_transaction_t *transaction;
    if (master == NULL) return false;
    transaction = &master->transaction;
    if (!transaction->awaiting_tx_complete) return false;
    transaction->awaiting_tx_complete = false;
    transaction->state = transaction->after_tx_state;
    transaction->deadline_ms = al_master_time_after(
        request_end_ms, transaction->timeout_ms);
    if (transaction->sleep_duration_ms != 0u &&
        transaction->lease_node_index >= 0) {
        al_master_node_t *node = &master->nodes[transaction->lease_node_index];
        if (!transaction->sleep_epoch_established) {
            if (!stage_node_persistence(master,
                    transaction->lease_node_index)) return false;
            transaction->sleep_epoch_established = true;
            node->sleep_state = AL_MASTER_SLEEP_UNCERTAIN;
            node->sleep_lease = true;
            node->sleep_lease_confirmed = false;
            node->sleep_first_tx_ms = request_end_ms;
            node->sleep_duration_ms = transaction->sleep_duration_ms;
            node->expected_wake_ms = al_master_time_after(
                request_end_ms,
                (uint64_t)GS_TIMING_SLEEP_ARMED_WINDOW_MS +
                transaction->sleep_duration_ms);
            master->registry_dirty = true;
        } else if (!transaction->sleep_epoch_persisted) {
            if (!stage_node_persistence(master,
                    transaction->lease_node_index)) return false;
            master->registry_dirty = true;
        }
    }
    if (master->baud.state == AL_MASTER_BAUD_PREPARE_TX) {
        master->baud.node_cursor = 0u;
        master->baud.state = AL_MASTER_BAUD_QUERY_READY;
    } else if (master->baud.state == AL_MASTER_BAUD_COMMIT_TX) {
        master->baud.commit_copy++;
        if (master->baud.commit_copy < GS_TRANSACTION_COMMIT_COPIES) {
            master->baud.deadline_ms = al_master_time_after(
                request_end_ms, GS_TIMING_COMMIT_COPY_GAP_MS);
            master->baud.state = AL_MASTER_BAUD_COMMIT_READY;
        } else {
            master->baud.deadline_ms = al_master_time_after(
                request_end_ms, GS_TIMING_MASTER_SILENCE_MS);
            master->baud.state = AL_MASTER_BAUD_SILENCE;
        }
    }
    return true;
}

bool al_master_on_tx_complete_us(al_master_t *master,
                                 al_time_us_t request_crc_stop_us)
{
    return al_master_on_tx_complete(master, request_crc_stop_us / 1000u);
}

bool al_master_on_tx_complete_timestamp(
    al_master_t *master, const al_transport_timestamp_t *timestamp)
{
    if (timestamp == NULL ||
        timestamp->event != AL_BUS_EVENT_TX_CRC_STOP) return false;
    return al_master_on_tx_complete_us(master, timestamp->at_us);
}

static bool response_address_valid(const al_master_transaction_t *transaction,
                                   const gs_frame_t *response, bool exception)
{
    const gs_frame_t *request = &transaction->request;
    if (request->func == GS_FUNC_ASSIGN_ADDR)
        return response->addr == GS_ADDR_UNASSIGNED;
    if (request->func == GS_FUNC_READDRESS)
        return response->addr == (exception ? transaction->old_addr :
                                  request->data[6]);
    if (request->func == GS_FUNC_DISCOVER)
        return response->addr == GS_ADDR_UNASSIGNED;
    if (request->func == GS_FUNC_RECONCILE || request->func == GS_FUNC_RECOVER)
        return response->addr == response->data[7] &&
               ((response->addr >= GS_ADDR_MIN && response->addr <= GS_ADDR_MAX) ||
                (request->func == GS_FUNC_RECOVER &&
                 response->addr == GS_ADDR_UNASSIGNED));
    return response->addr == request->addr;
}

static bool confirm_recovered_baud(al_master_t *master,
                                   al_master_event_t *result)
{
    if (master->baud.state != AL_MASTER_BAUD_VERIFY_WAIT ||
        !master->baud.recovery_active ||
        master->persist_kind != AL_MASTER_PERSIST_NONE)
        return false;
    master->persist_before_baud = master->current_bus_baud;
    master->current_bus_baud = master->baud.target_baud;
    master->bus_baud_known = true;
    master->persist_kind = AL_MASTER_PERSIST_BAUD;
    master->registry_dirty = true;
    master->baud.state = AL_MASTER_BAUD_RECOVERY_PERSIST;
    if (master->baud.current_node < master->node_count) {
        master->nodes[master->baud.current_node].online = true;
        master->nodes[master->baud.current_node].missed = 0u;
    }
    result->kind = AL_MASTER_EVENT_PERSIST_REQUIRED;
    result->persist_registry = true;
    return true;
}

static bool collect_payload_valid(uint8_t request_func,
                                  const gs_frame_t *response)
{
    uint8_t type;
    uint8_t length;
    if (response->func != GS_FUNC_REG_REQ || response->len == 0u) return false;
    type = request_func == GS_FUNC_DISCOVER ? GS_REPORT_REGISTER :
           (request_func == GS_FUNC_RECONCILE ? GS_REPORT_RECONCILE :
            GS_REPORT_RECOVER);
    length = request_func == GS_FUNC_DISCOVER ? 7u :
             (request_func == GS_FUNC_RECONCILE ? 8u : 16u);
    return response->data[0] == type && response->len == length;
}

static void transaction_finish(al_master_transaction_t *transaction)
{
    transaction->state = AL_MASTER_IDLE;
    transaction->awaiting_tx_complete = false;
}

static uint32_t sleep_recovery_backoff_ms(uint8_t attempts)
{
    uint32_t delay = AL_MASTER_SLEEP_VERIFY_BASE_MS;
    uint8_t i;
    for (i = 1u; i < attempts && delay < AL_MASTER_SLEEP_VERIFY_MAX_MS; ++i) {
        if (delay > AL_MASTER_SLEEP_VERIFY_MAX_MS / 2u) {
            delay = AL_MASTER_SLEEP_VERIFY_MAX_MS;
            break;
        }
        delay *= 2u;
    }
    return delay > AL_MASTER_SLEEP_VERIFY_MAX_MS ?
           AL_MASTER_SLEEP_VERIFY_MAX_MS : delay;
}

static void offline_probe_failed(al_master_t *master, int16_t index,
                                 al_master_time_ms_t now_ms)
{
    if (master == NULL || index < 0 ||
        index >= (int16_t)AL_MASTER_NODE_CAPACITY) return;
    if (master->offline_probe_failures[index] < UINT8_MAX)
        master->offline_probe_failures[index]++;
    master->offline_probe_next_ms[index] = al_master_time_after(
        now_ms, AL_MASTER_OFFLINE_PROBE_INTERVAL_MS);
}

static void offline_probe_succeeded(al_master_t *master, int16_t index,
                                    al_master_time_ms_t now_ms)
{
    if (master == NULL || index < 0 ||
        index >= (int16_t)AL_MASTER_NODE_CAPACITY) return;
    master->offline_probe_failures[index] = 0u;
    master->offline_probe_next_ms[index] = 0u;
    master->work_next_poll_ms = now_ms;
}

static void sleep_recovery_probe_failed(al_master_node_t *node,
                                        al_master_time_ms_t now_ms)
{
    if (node == NULL) return;
    node->sleep_recovery_probe_in_flight = false;
    if (node->sleep_recovery_probe_attempts < UINT8_MAX)
        node->sleep_recovery_probe_attempts++;
    node->sleep_recovery_last_result = AL_MASTER_SLEEP_VERIFY_TIMEOUT;
    node->sleep_recovery_next_probe_ms = al_master_time_after(
        now_ms, sleep_recovery_backoff_ms(node->sleep_recovery_probe_attempts));
    node->sleep_recovery_schedule_armed = true;
}

static void sleep_recovery_probe_succeeded(al_master_node_t *node)
{
    if (node == NULL) return;
    node->sleep_recovery_probe_in_flight = false;
    node->sleep_recovery_schedule_armed = false;
    node->sleep_recovery_probe_attempts = 0u;
    node->sleep_recovery_last_result = AL_MASTER_SLEEP_VERIFY_EARLY_WAKE;
}

al_master_event_t al_master_receive_at(al_master_t *master,
                                       const gs_frame_t *response,
                                       al_master_time_ms_t response_end_ms)
{
    al_master_transaction_t *transaction;
    al_master_event_t result = event(AL_MASTER_EVENT_REJECTED);
    const gs_frame_t *request;
    bool collect;
    bool exception;
    if (master == NULL || response == NULL) return result;
    transaction = &master->transaction;
    collect = transaction->state == AL_MASTER_COLLECT;
    if (transaction->awaiting_tx_complete ||
        (!collect && transaction->state != AL_MASTER_WAIT_RESPONSE))
        return result;
    if (al_master_time_reached(response_end_ms, transaction->deadline_ms))
        return al_master_tick(master, response_end_ms);
    request = &transaction->request;
    exception = response->func ==
                (uint8_t)(GS_FUNC_ERR_BASE | request->func);
    if (response->seq != request->seq ||
        !al_response_func_allowed(request->func, transaction->op,
                                  response->func)) return result;

    if (exception) {
        if (response->len != 1u ||
            !al_error_allowed(request->func, response->data[0]) ||
            !response_address_valid(transaction, response, true)) return result;
        result.kind = AL_MASTER_EVENT_ACCEPTED;
        result.wire_error = response->data[0];
        /* Any validated error response to the current READ_SAMPLE proves
         * this registered node is reachable, but it is not a new sample. */
        if (request->func == GS_FUNC_READ_SAMPLE) {
            int16_t index = al_master_find_addr(master, response->addr);
            if (index >= 0) {
                master->nodes[index].online = true;
                master->nodes[index].missed = 0u;
                result.node_index = index;
            }
        }
        if (master->offline_probe_node_index >= 0 &&
            request->func == GS_FUNC_READ_ADDR) {
            offline_probe_failed(master, master->offline_probe_node_index,
                                 response_end_ms);
            master->offline_probe_node_index = -1;
        }
        if (transaction->sleep_recovery_node_index >= 0)
            sleep_recovery_probe_failed(
                &master->nodes[transaction->sleep_recovery_node_index],
                response_end_ms);
        if (master->baud.state == AL_MASTER_BAUD_QUERY_WAIT) {
            master->baud.state = AL_MASTER_BAUD_IDLE;
            result.kind = AL_MASTER_EVENT_BAUD_ABORTED;
        } else if (master->baud.state == AL_MASTER_BAUD_VERIFY_WAIT) {
            if (master->baud.recovery_active) {
                /* A fully validated exception proves the candidate UART baud
                 * just as strongly as a normal READ_ADDR payload.  The Wire
                 * error remains observable, while the baud lifecycle owns the
                 * same durable confirmation transition. */
                if (!confirm_recovered_baud(master, &result))
                    result = event(AL_MASTER_EVENT_REJECTED);
            } else {
                master->baud.recovery_cursor = 0u;
                master->baud.recovery_active = true;
                master->baud.state = AL_MASTER_BAUD_RECOVERY;
            }
        }
        transaction_finish(transaction);
        return result;
    }

    if (collect) {
        uint64_t net;
        int16_t prior_index;
        if (!collect_payload_valid(request->func, response) ||
            !response_address_valid(transaction, response, false)) return result;
        net = request->func == GS_FUNC_RECOVER ?
              get_u64be(response->data + 8u) : master->network_id;
        prior_index = al_master_find_uid(master, response->data + 1u);
        if (prior_index < 0) prior_index = master->node_count;
        if (!stage_node_persistence(master, prior_index)) return result;
        result.node_index = upsert(master, response->data + 1u,
                                   request->func == GS_FUNC_DISCOVER ?
                                   GS_ADDR_UNASSIGNED : response->data[7], net);
        if (result.node_index < 0) return result;
        result.kind = AL_MASTER_EVENT_REGISTRY_CHANGED;
        result.persist_registry = true;
        return result;
    }

    if (!al_response_payload_allowed(request->func, transaction->op,
                                     request->data, request->len,
                                     response->addr,
                                     response->data, response->len) ||
        !response_address_valid(transaction, response, false)) return result;
    if (request->func == GS_FUNC_ASSIGN_ADDR ||
        request->func == GS_FUNC_READDRESS) {
        int16_t prior_index;
        if (response->len != 15u ||
            memcmp(response->data, request->data, GS_UID_LEN) != 0 ||
            response->data[6] != request->data[6] ||
            memcmp(response->data + 7u, request->data + 7u, 8u) != 0)
            return result;
        prior_index = al_master_find_uid(master, response->data);
        if (prior_index < 0) prior_index = master->node_count;
        if (!stage_node_persistence(master, prior_index)) return result;
        result.node_index = upsert(master, response->data,
                                   response->data[6],
                                   get_u64be(response->data + 7u));
        if (result.node_index < 0) return result;
        result.kind = AL_MASTER_EVENT_REGISTRY_CHANGED;
        result.persist_registry = true;
    } else {
        bool node_persist = false;
        al_master_node_t parsed_info;
        result.kind = AL_MASTER_EVENT_ACCEPTED;
        if (request->func == GS_FUNC_READ_SAMPLE)
            result.payload_kind = AL_MASTER_PAYLOAD_SAMPLE;
        result.node_index = al_master_find_addr(master, response->addr);
        if (result.node_index >= 0) {
            al_master_node_t *node = &master->nodes[result.node_index];
            parsed_info = *node;
            if (request->func == GS_FUNC_READ_INFO &&
                request->data[0] == GS_INFO_SUB_CHDESC &&
                !cache_descriptors(&parsed_info, response->data,
                                   response->len))
                return event(AL_MASTER_EVENT_REJECTED);
            node_persist = (request->func == GS_FUNC_SLEEP &&
                            transaction->sleep_duration_ms != 0u) ||
                           node->sleep_lease ||
                           request->func == GS_FUNC_READ_INFO;
            if (node_persist &&
                !stage_node_persistence(master, result.node_index))
                return event(AL_MASTER_EVENT_REJECTED);
            node->online = true;
            node->missed = 0u;
            if (master->offline_probe_node_index == result.node_index &&
                request->func == GS_FUNC_READ_ADDR) {
                offline_probe_succeeded(master, result.node_index,
                                        response_end_ms);
                master->offline_probe_node_index = -1;
            }
            if (request->func == GS_FUNC_READ_SAMPLE) {
                node->sample_trusted = sample_is_trusted(
                    node, response->data, response->len);
                result.payload_trusted = node->sample_trusted;
            }
            if (request->func == GS_FUNC_SLEEP &&
                transaction->sleep_duration_ms != 0u) {
                node->sleep_lease = true;
                node->sleep_lease_confirmed = true;
                node->sleep_state = AL_MASTER_SLEEP_CONFIRMED;
                result.persist_registry = true;
            } else if (node->sleep_lease) {
                sleep_recovery_probe_succeeded(node);
                node->sleep_lease = false;
                node->sleep_lease_confirmed = false;
                node->sleep_state = AL_MASTER_SLEEP_NONE;
                node->sleep_duration_ms = 0u;
                result.persist_registry = true;
            }
            if (request->func == GS_FUNC_READ_INFO) {
                if (request->data[0] == GS_INFO_SUB_BASE) {
                    node->sensor_type = response->data[8];
                    node->capabilities = response->data[10];
                    node->peer_proto_ver = gs_get_u16be(response->data + 11u);
                    node->schema_rev = gs_get_u16be(response->data + 13u);
                    node->descriptor_count = 0u;
                    node->descriptor_signature = 0u;
                    memset(node->descriptors, 0, sizeof(node->descriptors));
                    node->sample_trusted = false;
                    result.persist_registry = true;
                } else {
                    node->sensor_type = parsed_info.sensor_type;
                    node->descriptor_count = parsed_info.descriptor_count;
                    node->schema_rev = parsed_info.schema_rev;
                    node->descriptor_signature =
                        parsed_info.descriptor_signature;
                    memcpy(node->descriptors, parsed_info.descriptors,
                           sizeof(node->descriptors));
                    node->sample_trusted = false;
                    result.persist_registry = true;
                }
            }
        }
    }
    if (master->baud.state == AL_MASTER_BAUD_QUERY_WAIT) {
        if (response->len == 2u && response->data[0] == GS_QUERY_READY &&
            response->data[1] == 0u) {
            master->baud.node_cursor = (uint8_t)(master->baud.current_node + 1u);
            master->baud.state = AL_MASTER_BAUD_QUERY_READY;
        } else {
            master->baud.state = AL_MASTER_BAUD_IDLE;
            result.kind = AL_MASTER_EVENT_BAUD_ABORTED;
        }
    } else if (master->baud.state == AL_MASTER_BAUD_VERIFY_WAIT) {
        if (master->baud.recovery_active) {
            if (!confirm_recovered_baud(master, &result)) {
                transaction_finish(transaction);
                return event(AL_MASTER_EVENT_REJECTED);
            }
        } else {
            master->baud.node_cursor =
                (uint8_t)(master->baud.current_node + 1u);
            master->baud.state = AL_MASTER_BAUD_VERIFY_READY;
        }
    }
    transaction_finish(transaction);
    return result;
}

al_master_event_t al_master_receive_at_us(al_master_t *master,
                                          const gs_frame_t *response,
                                          al_time_us_t response_crc_stop_us)
{
    return al_master_receive_at(master, response,
                                response_crc_stop_us / 1000u);
}

al_master_event_t al_master_receive_at_timestamp(
    al_master_t *master, const gs_frame_t *response,
    const al_transport_timestamp_t *timestamp)
{
    if (timestamp == NULL ||
        timestamp->event != AL_BUS_EVENT_RX_CRC_STOP)
        return event(AL_MASTER_EVENT_REJECTED);
    return al_master_receive_at_us(master, response, timestamp->at_us);
}

al_master_event_t al_master_tick(al_master_t *master,
                                 al_master_time_ms_t now_ms)
{
    al_master_event_t result = event(AL_MASTER_EVENT_NONE);
    int16_t index;
    uint8_t i;
    if (master == NULL || master->persistence_fault) return result;
    if (master->persist_kind != AL_MASTER_PERSIST_NONE) return result;
    for (i = 0u; i < master->node_count; ++i) {
        al_master_node_t *node = &master->nodes[i];
        if (node->sleep_state != AL_MASTER_SLEEP_RECOVERY_VERIFY &&
            node->sleep_lease &&
            al_master_time_reached(now_ms, al_master_time_after(
                node->expected_wake_ms, GS_TIMING_GRACE_PERIOD_MS))) {
            if (!stage_node_persistence(master, (int16_t)i)) return result;
            node->sleep_lease = false;
            node->sleep_lease_confirmed = false;
            node->sleep_state = AL_MASTER_SLEEP_NONE;
            node->sleep_duration_ms = 0u;
            result.kind = AL_MASTER_EVENT_PERSIST_REQUIRED;
            result.persist_registry = true;
            return result;
        }
    }
    if (master->baud.state == AL_MASTER_BAUD_SILENCE &&
        master->baud_effect_ready) {
        master->baud_effect_ready = false;
        master->baud.node_cursor = 0u;
        master->baud.state = AL_MASTER_BAUD_VERIFY_READY;
        result.kind = AL_MASTER_EVENT_SET_BAUD;
        result.effect_value = master->current_bus_baud;
        return result;
    }
    if (master->baud.state == AL_MASTER_BAUD_SILENCE &&
        al_master_time_reached(now_ms, master->baud.deadline_ms)) {
        master->persist_before_baud = master->current_bus_baud;
        master->current_bus_baud = master->baud.target_baud;
        master->persist_kind = AL_MASTER_PERSIST_BAUD;
        master->registry_dirty = true;
        result.kind = AL_MASTER_EVENT_PERSIST_REQUIRED;
        result.persist_registry = true;
        return result;
    }
    if (master->baud.state == AL_MASTER_BAUD_RECOVERY) {
        result.kind = AL_MASTER_EVENT_BAUD_RECOVERY;
        result.effect_value = al_supported_baud_at(master->baud.recovery_cursor);
        return result;
    }
    if (master->baud.state == AL_MASTER_BAUD_COMPLETE) {
        master->baud.state = AL_MASTER_BAUD_IDLE;
        result.kind = AL_MASTER_EVENT_BAUD_COMPLETE;
        result.effect_value = master->baud.target_baud;
        return result;
    }
    if (master->baud.state == AL_MASTER_BAUD_FAILED) {
        /* An unknown bus cannot safely fall through to ordinary work after a
         * proofless registry scan.  Report the completed failed pass, then
         * re-arm the generated registry from its first candidate. */
        if (!master->bus_baud_known) {
            master->baud.recovery_cursor = 0u;
            master->baud.recovery_active = true;
            master->baud.state = AL_MASTER_BAUD_RECOVERY;
        } else {
            master->baud.state = AL_MASTER_BAUD_IDLE;
        }
        result.kind = AL_MASTER_EVENT_BAUD_ABORTED;
        return result;
    }
    if (master->transaction.awaiting_tx_complete ||
        master->transaction.state == AL_MASTER_IDLE ||
        !al_master_time_reached(now_ms, master->transaction.deadline_ms))
        return result;
    if (master->transaction.state == AL_MASTER_WAIT_RESPONSE &&
        master->transaction.retry_count < GS_TRANSACTION_RETRY_MAX) {
        master->transaction.retry_count++;
        master->transaction.awaiting_tx_complete = true;
        result.kind = AL_MASTER_EVENT_RETRANSMIT;
        result.tx_frame = master->transaction.request;
        return result;
    }
    index = al_master_find_addr(master, master->transaction.request.addr);
    if (master->transaction.sleep_recovery_node_index >= 0) {
        index = master->transaction.sleep_recovery_node_index;
        sleep_recovery_probe_failed(&master->nodes[index], now_ms);
        result.kind = AL_MASTER_EVENT_TIMEOUT;
        result.node_index = index;
        transaction_finish(&master->transaction);
        return result;
    }
    if (index >= 0 && master->transaction.state == AL_MASTER_WAIT_RESPONSE &&
        !master->nodes[index].sleep_lease &&
        !(master->baud.state == AL_MASTER_BAUD_VERIFY_WAIT &&
          master->baud.recovery_active)) {
        if (master->nodes[index].missed < 255u) master->nodes[index].missed++;
        if (master->nodes[index].missed >= GS_TRANSACTION_OFFLINE_THRESHOLD)
            master->nodes[index].online = false;
    }
    if (index >= 0 && master->offline_probe_node_index == index &&
        master->transaction.request.func == GS_FUNC_READ_ADDR) {
        offline_probe_failed(master, index, now_ms);
        master->offline_probe_node_index = -1;
    }
    if (master->baud.state == AL_MASTER_BAUD_QUERY_WAIT) {
        master->baud.state = AL_MASTER_BAUD_IDLE;
        result.kind = AL_MASTER_EVENT_BAUD_ABORTED;
    } else if (master->baud.state == AL_MASTER_BAUD_VERIFY_WAIT) {
        if (master->baud.recovery_active) {
            master->baud.node_cursor =
                (uint8_t)(master->baud.current_node + 1u);
            master->baud.state = AL_MASTER_BAUD_VERIFY_READY;
            result.kind = AL_MASTER_EVENT_TIMEOUT;
        } else {
            master->baud.recovery_cursor = 0u;
            master->baud.recovery_active = true;
            master->baud.state = AL_MASTER_BAUD_RECOVERY;
            result.kind = AL_MASTER_EVENT_BAUD_RECOVERY;
            result.effect_value =
                al_supported_baud_at(master->baud.recovery_cursor);
        }
    } else {
        result.kind = AL_MASTER_EVENT_TIMEOUT;
    }
    transaction_finish(&master->transaction);
    result.node_index = index;
    return result;
}


al_master_event_t al_master_tick_us(al_master_t *master, al_time_us_t now_us)
{
    return al_master_tick(master, now_us / 1000u);
}

static void request_base(al_master_t *master, gs_frame_t *request,
                         uint8_t addr, uint8_t func)
{
    memset(request, 0, sizeof(*request));
    request->ver = GS_WIRE_VER;
    request->addr = addr;
    request->seq = master->next_seq++;
    request->func = func;
}

static bool normal_address(uint8_t addr)
{
    return addr >= GS_ADDR_MIN && addr <= GS_ADDR_MAX;
}

bool al_master_make_read_sample(al_master_t *master, uint8_t addr,
                                gs_frame_t *request)
{
    if (!normal_address(addr)) return false;
    request_base(master, request, addr, GS_FUNC_READ_SAMPLE);
    return true;
}

bool al_master_make_read_info(al_master_t *master, uint8_t addr, uint8_t sub,
                              gs_frame_t *request)
{
    if (!normal_address(addr) || sub > GS_INFO_SUB_CHDESC) return false;
    request_base(master, request, addr, GS_FUNC_READ_INFO);
    request->len = 1u;
    request->data[0] = sub;
    return true;
}

bool al_master_make_write_config(al_master_t *master, uint8_t addr, uint8_t id,
                                 const uint8_t *value, uint8_t value_len,
                                 gs_frame_t *request)
{
    uint8_t length = al_config_request_length(id);
    if (!normal_address(addr) || value == NULL || length == 0u ||
        length != (uint8_t)(value_len + 1u)) return false;
    request_base(master, request, addr, GS_FUNC_WRITE_CFG);
    request->len = length;
    request->data[0] = id;
    memcpy(request->data + 1u, value, value_len);
    return true;
}

bool al_master_make_diag(al_master_t *master, uint8_t addr,
                         const uint8_t *data, uint8_t len,
                         gs_frame_t *request)
{
    if (!normal_address(addr) || (len != 0u && data == NULL)) return false;
    request_base(master, request, addr, GS_FUNC_DIAG_LOOP);
    request->len = len;
    if (len != 0u) memcpy(request->data, data, len);
    return true;
}

bool al_master_make_sleep(al_master_t *master, uint8_t addr,
                          uint32_t duration_ms, gs_frame_t *request)
{
    if (!normal_address(addr)) return false;
    request_base(master, request, addr, GS_FUNC_SLEEP);
    request->len = 4u;
    gs_put_u32be(request->data, duration_ms);
    return true;
}

bool al_master_make_read_addr(al_master_t *master, uint8_t addr,
                              gs_frame_t *request)
{
    if (!normal_address(addr)) return false;
    request_base(master, request, addr, GS_FUNC_READ_ADDR);
    return true;
}

bool al_master_make_assign(al_master_t *master,
                           const uint8_t uid[GS_UID_LEN], uint8_t new_addr,
                           gs_frame_t *request)
{
    if (!normal_address(new_addr) || master->network_id == 0u || uid == NULL)
        return false;
    request_base(master, request, GS_ADDR_UNASSIGNED, GS_FUNC_ASSIGN_ADDR);
    request->len = 15u;
    memcpy(request->data, uid, GS_UID_LEN);
    request->data[6] = new_addr;
    put_u64be(request->data + 7u, master->network_id);
    return true;
}

bool al_master_make_readdress(al_master_t *master,
                              const uint8_t uid[GS_UID_LEN], uint8_t old_addr,
                              uint8_t new_addr, gs_frame_t *request)
{
    if (!normal_address(old_addr) || !normal_address(new_addr) ||
        master->network_id == 0u || uid == NULL) return false;
    request_base(master, request, GS_ADDR_BROADCAST, GS_FUNC_READDRESS);
    request->len = 15u;
    memcpy(request->data, uid, GS_UID_LEN);
    request->data[6] = new_addr;
    put_u64be(request->data + 7u, master->network_id);
    return true;
}

bool al_master_make_collect(al_master_t *master, uint8_t func,
                            uint32_t seed, uint8_t slots,
                            gs_frame_t *request)
{
    if ((func != GS_FUNC_DISCOVER && func != GS_FUNC_RECONCILE &&
         func != GS_FUNC_RECOVER) ||
        (slots != 8u && slots != 16u && slots != 32u && slots != 64u) ||
        master->network_id == 0u) return false;
    request_base(master, request, GS_ADDR_BROADCAST, func);
    if (func == GS_FUNC_RECOVER) {
        put_u64be(request->data, master->network_id);
        request->data[8] = 1u;
        gs_put_u32be(request->data + 9u, seed);
        request->data[13] = slots;
        request->len = 14u;
    } else {
        gs_put_u32be(request->data, seed);
        request->data[4] = slots;
        put_u64be(request->data + 5u, master->network_id);
        request->len = 13u;
    }
    return true;
}

bool al_master_make_discover(al_master_t *master, uint32_t seed,
                             uint8_t slots, gs_frame_t *request)
{
    return al_master_make_collect(master, GS_FUNC_DISCOVER, seed, slots,
                                  request);
}

bool al_master_make_reconcile(al_master_t *master, uint32_t seed,
                              uint8_t slots, gs_frame_t *request)
{
    return al_master_make_collect(master, GS_FUNC_RECONCILE, seed, slots,
                                  request);
}

bool al_master_make_recover(al_master_t *master, uint32_t seed,
                            uint8_t slots, gs_frame_t *request)
{
    return al_master_make_collect(master, GS_FUNC_RECOVER, seed, slots,
                                  request);
}

bool al_master_make_net_management(al_master_t *master, uint8_t addr,
                                   uint8_t op, uint8_t tid, uint32_t baud,
                                   gs_frame_t *request)
{
    if (op != GS_NET_OP_PREPARE && op != GS_NET_OP_QUERY &&
        op != GS_NET_OP_COMMIT) return false;
    if (op == GS_NET_OP_QUERY && !normal_address(addr)) return false;
    request_base(master, request, op == GS_NET_OP_QUERY ? addr :
                 GS_ADDR_BROADCAST, GS_FUNC_NET_MGMT);
    put_u64be(request->data, master->network_id);
    request->data[8] = op;
    request->data[9] = tid;
    request->len = 10u;
    if (op != GS_NET_OP_QUERY) {
        if (!al_is_supported_baud(baud)) return false;
        gs_put_u32be(request->data + 10u, baud);
        request->len = 14u;
    }
    if (op == GS_NET_OP_COMMIT) {
        gs_put_u16be(request->data + 14u, GS_TIMING_SWITCH_AFTER_MS);
        request->len = 16u;
    }
    return true;
}

static bool baud_node_eligible(const al_master_t *master, uint8_t index,
                               bool recovery)
{
    const al_master_node_t *node = &master->nodes[index];
    return (recovery || node->online) && !node->sleep_lease &&
           node->network_id == master->network_id &&
           normal_address(node->addr);
}

bool al_master_baud_start(al_master_t *master, uint8_t tid,
                          uint32_t target_baud)
{
    if (master == NULL || master->persistence_fault ||
        master->baud.state != AL_MASTER_BAUD_IDLE ||
        master->transaction.state != AL_MASTER_IDLE ||
        master->transaction.awaiting_tx_complete ||
        !al_is_supported_baud(target_baud)) return false;
    memset(&master->baud, 0, sizeof(master->baud));
    master->baud.state = AL_MASTER_BAUD_PREPARE_READY;
    master->baud.target_baud = target_baud;
    master->baud.tid = tid;
    return true;
}

bool al_master_baud_next(al_master_t *master, al_master_time_ms_t now_ms,
                         gs_frame_t *request)
{
    al_master_baud_orchestrator_t *baud;
    if (master == NULL || request == NULL ||
        master->transaction.state != AL_MASTER_IDLE ||
        master->transaction.awaiting_tx_complete) return false;
    baud = &master->baud;
    for (;;) {
        if (baud->state == AL_MASTER_BAUD_PREPARE_READY) {
            if (!al_master_make_net_management(master, GS_ADDR_BROADCAST,
                    GS_NET_OP_PREPARE, baud->tid, baud->target_baud, request) ||
                !al_master_begin(master, request, 0u, 0u)) return false;
            baud->state = AL_MASTER_BAUD_PREPARE_TX;
            return true;
        }
        if (baud->state == AL_MASTER_BAUD_QUERY_READY) {
            while (baud->node_cursor < master->node_count &&
                   !baud_node_eligible(master, baud->node_cursor, false))
                baud->node_cursor++;
            if (baud->node_cursor >= master->node_count) {
                baud->commit_copy = 0u;
                baud->state = AL_MASTER_BAUD_COMMIT_READY;
                continue;
            }
            baud->current_node = baud->node_cursor;
            if (!al_master_make_net_management(master,
                    master->nodes[baud->current_node].addr,
                    GS_NET_OP_QUERY, baud->tid, 0u, request) ||
                !al_master_begin(master, request, request->addr,
                                 GS_TIMING_RESP_TIMEOUT_MS)) return false;
            baud->state = AL_MASTER_BAUD_QUERY_WAIT;
            return true;
        }
        if (baud->state == AL_MASTER_BAUD_COMMIT_READY) {
            if (baud->commit_copy != 0u &&
                !al_master_time_reached(now_ms, baud->deadline_ms)) return false;
            if (!al_master_make_net_management(master, GS_ADDR_BROADCAST,
                    GS_NET_OP_COMMIT, baud->tid, baud->target_baud, request))
                return false;
            if (baud->commit_copy == 0u) baud->commit_seq = request->seq;
            else request->seq = baud->commit_seq;
            if (!al_master_begin(master, request, 0u, 0u)) return false;
            baud->state = AL_MASTER_BAUD_COMMIT_TX;
            return true;
        }
        if (baud->state == AL_MASTER_BAUD_VERIFY_READY) {
            while (baud->node_cursor < master->node_count &&
                   !baud_node_eligible(master, baud->node_cursor,
                                       baud->recovery_active))
                baud->node_cursor++;
            if (baud->node_cursor >= master->node_count) {
                if (baud->recovery_active) {
                    baud->recovery_cursor++;
                    if ((size_t)baud->recovery_cursor >=
                        al_supported_baud_count())
                        baud->state = AL_MASTER_BAUD_FAILED;
                    else
                        baud->state = AL_MASTER_BAUD_RECOVERY;
                } else {
                    baud->state = AL_MASTER_BAUD_COMPLETE;
                }
                return false;
            }
            baud->current_node = baud->node_cursor;
            if (!al_master_make_read_addr(master,
                    master->nodes[baud->current_node].addr, request) ||
                !al_master_begin(master, request, request->addr,
                                 GS_TIMING_RESP_TIMEOUT_MS)) return false;
            baud->state = AL_MASTER_BAUD_VERIFY_WAIT;
            return true;
        }
        return false;
    }
}

void al_master_baud_recovery_applied(al_master_t *master)
{
    if (master == NULL || master->baud.state != AL_MASTER_BAUD_RECOVERY)
        return;
    master->baud.target_baud =
        al_supported_baud_at(master->baud.recovery_cursor);
    master->baud.node_cursor = 0u;
    master->baud.state = AL_MASTER_BAUD_VERIFY_READY;
}

static bool sleep_recovery_probe_due(al_master_node_t *node,
                                     al_master_time_ms_t now_ms)
{
    if (node->sleep_state != AL_MASTER_SLEEP_RECOVERY_VERIFY ||
        !node->sleep_lease || node->sleep_recovery_probe_in_flight)
        return false;
    if (!node->sleep_recovery_schedule_armed) {
        node->sleep_recovery_next_probe_ms = al_master_time_after(
            now_ms, AL_MASTER_SLEEP_VERIFY_BASE_MS);
        node->sleep_recovery_schedule_armed = true;
    }
    return al_master_time_reached(now_ms,
                                  node->sleep_recovery_next_probe_ms);
}

static uint8_t next_available_address(const al_master_t *master)
{
    uint16_t address;
    for (address = GS_ADDR_MIN; address <= GS_ADDR_MAX; ++address)
        if (al_master_find_addr(master, (uint8_t)address) < 0)
            return (uint8_t)address;
    return GS_ADDR_INVALID;
}

static al_master_work_kind_t next_sleep_recovery_work(
    al_master_t *master, al_master_time_ms_t now_ms, gs_frame_t *request)
{
    uint8_t offset;
    for (offset = 0u; offset < master->node_count; ++offset) {
        uint8_t index = (uint8_t)((master->work_sleep_cursor + offset) %
                                  master->node_count);
        al_master_node_t *node = &master->nodes[index];
        if (!sleep_recovery_probe_due(node, now_ms)) continue;
        if (!al_master_make_read_addr(master, node->addr, request) ||
            !al_master_begin(master, request, request->addr,
                             GS_TIMING_RESP_TIMEOUT_MS))
            return AL_MASTER_WORK_NONE;
        master->transaction.sleep_recovery_node_index = (int16_t)index;
        node->sleep_recovery_probe_in_flight = true;
        node->sleep_recovery_last_result = AL_MASTER_SLEEP_VERIFY_IN_FLIGHT;
        master->work_sleep_cursor =
            (uint8_t)((index + 1u) % master->node_count);
        return AL_MASTER_WORK_SLEEP_VERIFY;
    }
    return AL_MASTER_WORK_NONE;
}

static al_master_work_kind_t next_assign_work(al_master_t *master,
                                               gs_frame_t *request)
{
    uint8_t offset;
    if (master->node_count == 0u) return AL_MASTER_WORK_NONE;
    for (offset = 0u; offset < master->node_count; ++offset) {
        uint8_t i = (uint8_t)((master->work_assign_cursor + offset) %
                              master->node_count);
        uint8_t address;
        if (master->nodes[i].addr != GS_ADDR_UNASSIGNED) continue;
        address = next_available_address(master);
        if (address == GS_ADDR_INVALID ||
            !al_master_make_assign(master, master->nodes[i].uid,
                                   address, request) ||
            !al_master_begin(master, request, GS_ADDR_UNASSIGNED,
                              GS_TIMING_RESP_TIMEOUT_MS))
            return AL_MASTER_WORK_NONE;
        master->work_assign_cursor =
            (uint8_t)((i + 1u) % master->node_count);
        return AL_MASTER_WORK_ASSIGN;
    }
    return AL_MASTER_WORK_NONE;
}

static al_master_work_kind_t next_discover_work(al_master_t *master,
                                                 al_master_time_ms_t now_ms,
                                                 uint32_t seed,
                                                 uint8_t slots,
                                                 gs_frame_t *request)
{
    if (!al_master_time_reached(now_ms, master->work_next_discover_ms))
        return AL_MASTER_WORK_NONE;
    master->work_next_discover_ms = al_master_time_after(
        now_ms, GS_TIMING_SCAN_INTERVAL_MS);
    if (!al_master_make_discover(master, seed, slots, request) ||
        !al_master_begin(master, request, 0u,
                         (uint32_t)slots * GS_TIMING_T_SLOT_DISCOVER_MS +
                         GS_TIMING_T_IFG_MS + GS_TIMING_T_JITTER_MS))
        return AL_MASTER_WORK_NONE;
    return AL_MASTER_WORK_DISCOVER;
}

static al_master_work_kind_t next_info_work(al_master_t *master,
                                             al_master_time_ms_t now_ms,
                                             gs_frame_t *request)
{
    uint8_t offset;
    if (!al_master_time_reached(now_ms, master->work_next_info_ms) ||
        master->node_count == 0u) return AL_MASTER_WORK_NONE;
    for (offset = 0u; offset < master->node_count; ++offset) {
        uint8_t index = (uint8_t)((master->work_info_cursor + offset) %
                                  master->node_count);
        al_master_node_t *node = &master->nodes[index];
        uint8_t sub;
        if (!node->online || node->sleep_lease ||
            node->network_id != master->network_id ||
            !normal_address(node->addr)) continue;
        if (node->peer_proto_ver == 0u) {
            sub = GS_INFO_SUB_BASE;
        } else if (node->peer_proto_ver == GS_PROTO_VER &&
                   node->descriptor_count == 0u &&
                   node->descriptor_signature == 0u) {
            sub = GS_INFO_SUB_CHDESC;
        } else {
            continue;
        }
        if (!al_master_make_read_info(master, node->addr, sub, request) ||
            !al_master_begin(master, request, request->addr,
                             GS_TIMING_RESP_TIMEOUT_MS))
            return AL_MASTER_WORK_NONE;
        master->work_info_cursor =
            (uint8_t)((index + 1u) % master->node_count);
        master->work_next_info_ms = al_master_time_after(
            now_ms, GS_TIMING_RESP_TIMEOUT_MS);
        return AL_MASTER_WORK_INFO;
    }
    return AL_MASTER_WORK_NONE;
}

static al_master_work_kind_t next_offline_recovery_work(
    al_master_t *master, al_master_time_ms_t now_ms, gs_frame_t *request)
{
    uint8_t offset;
    if (master->node_count == 0u ||
        master->node_count > AL_MASTER_NODE_CAPACITY ||
        master->offline_probe_node_index >= 0)
        return AL_MASTER_WORK_NONE;
    for (offset = 0u; offset < master->node_count; ++offset) {
        uint8_t index = (uint8_t)((master->work_offline_probe_cursor + offset) %
                                  master->node_count);
        al_master_node_t *node = &master->nodes[index];
        if (node->online || node->sleep_lease ||
            node->network_id != master->network_id ||
            !normal_address(node->addr) ||
            !al_master_time_reached(now_ms,
                                    master->offline_probe_next_ms[index]))
            continue;
        if (!al_master_make_read_addr(master, node->addr, request) ||
            !al_master_begin(master, request, request->addr,
                             GS_TIMING_RESP_TIMEOUT_MS))
            return AL_MASTER_WORK_NONE;
        master->offline_probe_node_index = (int16_t)index;
        master->work_offline_probe_cursor =
            (uint8_t)((index + 1u) % master->node_count);
        return AL_MASTER_WORK_OFFLINE_PROBE;
    }
    return AL_MASTER_WORK_NONE;
}

static al_master_work_kind_t next_sample_work(al_master_t *master,
                                               al_master_time_ms_t now_ms,
                                               gs_frame_t *request)
{
    uint8_t offset;
    if (!al_master_time_reached(now_ms, master->work_next_poll_ms) ||
        master->node_count == 0u) return AL_MASTER_WORK_NONE;
    for (offset = 0u; offset < master->node_count; ++offset) {
        uint8_t index = (uint8_t)((master->work_poll_cursor + offset) %
                                  master->node_count);
        al_master_node_t *node = &master->nodes[index];
        if (!node->online || node->sleep_lease ||
            node->network_id != master->network_id ||
            !normal_address(node->addr) ||
            node->peer_proto_ver != GS_PROTO_VER ||
            node->descriptor_count == 0u) continue;
        if (!al_master_make_read_sample(master, node->addr, request) ||
            !al_master_begin(master, request, request->addr,
                             GS_TIMING_RESP_TIMEOUT_MS))
            return AL_MASTER_WORK_NONE;
        master->work_next_poll_ms = al_master_time_after(
            now_ms, master->work_sample_interval_ms);
        master->work_poll_cursor =
            (uint8_t)((index + 1u) % master->node_count);
        return AL_MASTER_WORK_SAMPLE;
    }
    return AL_MASTER_WORK_NONE;
}

al_master_work_kind_t al_master_next_work(al_master_t *master,
                                          al_master_time_ms_t now_ms,
                                          uint32_t discovery_seed,
                                          uint8_t slots,
                                          gs_frame_t *request)
{
    uint8_t offset;
    if (master == NULL || request == NULL || master->persistence_fault ||
        master->persist_kind != AL_MASTER_PERSIST_NONE ||
        master->transaction.state != AL_MASTER_IDLE ||
        master->transaction.awaiting_tx_complete ||
        master->baud.state != AL_MASTER_BAUD_IDLE)
        return AL_MASTER_WORK_NONE;
    for (offset = 0u; offset < 6u; ++offset) {
        uint8_t work_class = (uint8_t)((master->work_class_cursor + offset) % 6u);
        al_master_work_kind_t kind = AL_MASTER_WORK_NONE;
        if (work_class == 0u)
            kind = next_sleep_recovery_work(master, now_ms, request);
        else if (work_class == 1u)
            kind = next_assign_work(master, request);
        else if (work_class == 2u)
            kind = next_discover_work(master, now_ms, discovery_seed, slots,
                                      request);
        else if (work_class == 3u)
            kind = next_info_work(master, now_ms, request);
        else if (work_class == 4u)
            kind = next_offline_recovery_work(master, now_ms, request);
        else
            kind = next_sample_work(master, now_ms, request);
        if (kind != AL_MASTER_WORK_NONE) {
            master->work_class_cursor = (uint8_t)((work_class + 1u) % 6u);
            return kind;
        }
    }
    return AL_MASTER_WORK_NONE;
}

al_master_next_tx_result_t al_master_next_transmission(
    al_master_t *master, al_master_time_ms_t now_ms,
    uint32_t discovery_seed, uint8_t slots, gs_frame_t *request)
{
    if (master == NULL || request == NULL) return AL_MASTER_NEXT_TX_INVALID;
    if (master->transaction.state != AL_MASTER_IDLE ||
        master->transaction.awaiting_tx_complete)
        return AL_MASTER_NEXT_TX_NONE;
    if (master->baud.state != AL_MASTER_BAUD_IDLE)
        return al_master_baud_next(master, now_ms, request) ?
               AL_MASTER_NEXT_TX_READY : AL_MASTER_NEXT_TX_NONE;
    return al_master_next_work(master, now_ms, discovery_seed, slots,
                               request) != AL_MASTER_WORK_NONE ?
           AL_MASTER_NEXT_TX_READY : AL_MASTER_NEXT_TX_NONE;
}
