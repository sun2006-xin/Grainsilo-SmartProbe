/* GrainSilo changes: sampling configuration and Master scheduler state. */
#ifndef AUTOLINK_MASTER_H
#define AUTOLINK_MASTER_H

#include <stdbool.h>
#include <stdint.h>
#include <gs_proto.h>
#include "autolink_time.h"
#include "autolink_transport_timing.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AL_MASTER_NODE_CAPACITY
#define AL_MASTER_NODE_CAPACITY 64u
#endif
#if AL_MASTER_NODE_CAPACITY > UINT8_MAX
#error "AL_MASTER_NODE_CAPACITY must fit the uint8_t node_count domain"
#endif

/* GrainSilo local API extension; the frozen generated default remains 1 s. */
#define AL_MASTER_SAMPLE_INTERVAL_MIN_MS UINT32_C(1000)
#define AL_MASTER_SAMPLE_INTERVAL_MAX_MS UINT32_C(86400000)

typedef enum { AL_MASTER_IDLE, AL_MASTER_WAIT_RESPONSE, AL_MASTER_COLLECT }
    al_master_tx_state_t;
typedef enum { AL_MASTER_EVENT_NONE, AL_MASTER_EVENT_ACCEPTED,
               AL_MASTER_EVENT_REJECTED, AL_MASTER_EVENT_TIMEOUT,
               AL_MASTER_EVENT_RETRANSMIT,
               AL_MASTER_EVENT_REGISTRY_CHANGED,
               AL_MASTER_EVENT_SET_BAUD,
               AL_MASTER_EVENT_BAUD_COMPLETE,
               AL_MASTER_EVENT_BAUD_RECOVERY,
               AL_MASTER_EVENT_BAUD_ABORTED,
               AL_MASTER_EVENT_PERSIST_REQUIRED } al_master_event_kind_t;
typedef enum { AL_MASTER_PAYLOAD_NONE, AL_MASTER_PAYLOAD_SAMPLE }
    al_master_payload_kind_t;
typedef enum { AL_MASTER_SLEEP_NONE, AL_MASTER_SLEEP_UNCERTAIN,
               AL_MASTER_SLEEP_CONFIRMED,
               AL_MASTER_SLEEP_RECOVERY_VERIFY } al_master_sleep_state_t;
typedef enum { AL_MASTER_SLEEP_VERIFY_NONE,
               AL_MASTER_SLEEP_VERIFY_IN_FLIGHT,
               AL_MASTER_SLEEP_VERIFY_TIMEOUT,
               AL_MASTER_SLEEP_VERIFY_EARLY_WAKE }
    al_master_sleep_verify_result_t;
typedef enum { AL_MASTER_WORK_NONE, AL_MASTER_WORK_SLEEP_VERIFY,
               AL_MASTER_WORK_ASSIGN, AL_MASTER_WORK_DISCOVER,
               AL_MASTER_WORK_SAMPLE, AL_MASTER_WORK_INFO,
               AL_MASTER_WORK_OFFLINE_PROBE }
    al_master_work_kind_t;
typedef enum { AL_MASTER_NEXT_TX_INVALID, AL_MASTER_NEXT_TX_NONE,
               AL_MASTER_NEXT_TX_READY } al_master_next_tx_result_t;

typedef struct {
    uint8_t uid[GS_UID_LEN];
    uint8_t addr;
    uint64_t network_id;
    bool online;
    uint8_t missed;
    bool sleep_lease;
    bool sleep_lease_confirmed;
    al_master_sleep_state_t sleep_state;
    al_master_time_ms_t sleep_first_tx_ms;
    al_master_time_ms_t expected_wake_ms;
    uint32_t sleep_duration_ms;
    al_master_time_ms_t sleep_recovery_next_probe_ms;
    uint8_t sleep_recovery_probe_attempts;
    bool sleep_recovery_schedule_armed;
    bool sleep_recovery_probe_in_flight;
    al_master_sleep_verify_result_t sleep_recovery_last_result;
    uint16_t schema_rev;
    uint32_t descriptor_signature;
    uint16_t peer_proto_ver;
    uint8_t sensor_type;
    uint8_t capabilities;
    uint8_t descriptor_count;
    gs_chdesc_t descriptors[GS_CODEC_CHANNEL_CAPACITY];
    bool sample_trusted;
} al_master_node_t;

/* Durable registry record: no boot-local monotonic deadline is serialised. */
typedef struct {
    uint8_t uid[GS_UID_LEN];
    uint8_t addr;
    uint64_t network_id;
    uint8_t sleep_state;
    uint32_t sleep_duration_ms;
    uint16_t schema_rev;
    uint32_t descriptor_signature;
    uint16_t peer_proto_ver;
    uint8_t sensor_type;
    uint8_t capabilities;
    uint8_t descriptor_count;
    gs_chdesc_t descriptors[GS_CODEC_CHANNEL_CAPACITY];
} al_master_node_record_t;

/* Stable durable registry facts.  Node records are exported atomically into
   caller storage by al_master_export_registry(). */
typedef struct {
    uint64_t network_id;
    uint32_t current_bus_baud;
    bool bus_baud_known;
    uint8_t node_count;
} al_master_registry_snapshot_t;

typedef enum {
    AL_MASTER_REGISTRY_EXPORT_OK,
    AL_MASTER_REGISTRY_EXPORT_INVALID_ARGUMENT,
    AL_MASTER_REGISTRY_EXPORT_INSUFFICIENT_CAPACITY
} al_master_registry_export_result_t;

/* Stable platform facts projected by Master Core. */
typedef struct {
    uint32_t active_baud;
} al_master_platform_status_t;

typedef struct {
    al_master_tx_state_t state;
    gs_frame_t request;
    uint8_t op;
    uint8_t old_addr;
    al_master_time_ms_t deadline_ms;
    uint32_t timeout_ms;
    uint32_t sleep_duration_ms;
    int16_t lease_node_index;
    int16_t sleep_recovery_node_index;
    uint8_t retry_count;
    al_master_tx_state_t after_tx_state;
    bool awaiting_tx_complete;
    bool sleep_epoch_established;
    bool sleep_epoch_persisted;
} al_master_transaction_t;

typedef struct {
    al_master_event_kind_t kind;
    int16_t node_index;
    uint8_t wire_error;
    bool persist_registry;
    al_master_payload_kind_t payload_kind;
    bool payload_trusted;
    uint32_t effect_value;
    gs_frame_t tx_frame;
} al_master_event_t;

typedef enum {
    AL_MASTER_BAUD_IDLE,
    AL_MASTER_BAUD_PREPARE_READY,
    AL_MASTER_BAUD_PREPARE_TX,
    AL_MASTER_BAUD_QUERY_READY,
    AL_MASTER_BAUD_QUERY_WAIT,
    AL_MASTER_BAUD_COMMIT_READY,
    AL_MASTER_BAUD_COMMIT_TX,
    AL_MASTER_BAUD_SILENCE,
    AL_MASTER_BAUD_VERIFY_READY,
    AL_MASTER_BAUD_VERIFY_WAIT,
    AL_MASTER_BAUD_RECOVERY,
    AL_MASTER_BAUD_RECOVERY_PERSIST,
    AL_MASTER_BAUD_COMPLETE,
    AL_MASTER_BAUD_FAILED
} al_master_baud_state_t;

typedef struct {
    al_master_baud_state_t state;
    uint32_t target_baud;
    al_master_time_ms_t deadline_ms;
    uint8_t tid;
    uint8_t node_cursor;
    uint8_t current_node;
    uint8_t commit_copy;
    uint8_t commit_seq;
    uint8_t recovery_cursor;
    bool recovery_active;
} al_master_baud_orchestrator_t;

typedef enum { AL_MASTER_PERSIST_NONE, AL_MASTER_PERSIST_NODE,
               AL_MASTER_PERSIST_BAUD,
               AL_MASTER_PERSIST_REGISTRY } al_master_persist_kind_t;

typedef struct {
    uint64_t network_id;
    uint8_t next_seq;
    al_master_node_t nodes[AL_MASTER_NODE_CAPACITY];
    uint8_t node_count;
    al_master_transaction_t transaction;
    al_master_baud_orchestrator_t baud;
    bool registry_dirty;
    uint32_t current_bus_baud;
    bool bus_baud_known;
    al_master_persist_kind_t persist_kind;
    int16_t persist_node_index;
    uint8_t persist_node_count;
    al_master_node_t persist_before_node;
    uint32_t persist_before_baud;
    bool baud_effect_ready;
    bool persistence_fault;
    uint8_t work_class_cursor;
    uint8_t work_sleep_cursor;
    uint8_t work_assign_cursor;
    uint8_t work_poll_cursor;
    al_master_time_ms_t work_next_discover_ms;
    al_master_time_ms_t work_next_poll_ms;
    uint32_t work_sample_interval_ms;
    uint8_t work_info_cursor;
    al_master_time_ms_t work_next_info_ms;
    /* Boot-local offline-node recovery; excluded from durable node records. */
    uint8_t work_offline_probe_cursor;
    int16_t offline_probe_node_index;
    uint8_t offline_probe_failures[AL_MASTER_NODE_CAPACITY];
    al_master_time_ms_t offline_probe_next_ms[AL_MASTER_NODE_CAPACITY];
} al_master_t;

void al_master_init(al_master_t *master, uint64_t network_id);
/* Local GrainSilo extension: configure Core-owned READ_SAMPLE scheduling.
 * A changed value makes the next sample eligible immediately; the new
 * interval is applied after that dispatch. False means invalid arguments. */
bool al_master_set_sample_interval_ms(al_master_t *master,
                                      uint32_t interval_ms,
                                      al_master_time_ms_t now_ms);
/* False means invalid arguments.  Call after initialization/restore. */
bool al_master_platform_status(const al_master_t *master,
                               al_master_platform_status_t *status);
bool al_master_begin(al_master_t *master, const gs_frame_t *request,
                      uint8_t old_addr, uint32_t timeout_ms);
/* True only while this exact Core-owned frame awaits physical completion. */
bool al_master_tx_authorized(const al_master_t *master,
                             const gs_frame_t *frame);
bool al_master_on_tx_complete(al_master_t *master,
                              al_master_time_ms_t request_end_ms);
bool al_master_on_tx_complete_us(al_master_t *master,
                                 al_time_us_t request_crc_stop_us);
bool al_master_on_tx_complete_timestamp(
    al_master_t *master, const al_transport_timestamp_t *timestamp);
bool al_master_baud_start(al_master_t *master, uint8_t tid,
                          uint32_t target_baud);
bool al_master_baud_next(al_master_t *master, al_master_time_ms_t now_ms,
                          gs_frame_t *request);
void al_master_baud_recovery_applied(al_master_t *master);
al_master_event_t al_master_receive_at(al_master_t *master,
                                       const gs_frame_t *response,
                                       al_master_time_ms_t response_end_ms);
al_master_event_t al_master_receive_at_us(al_master_t *master,
                                          const gs_frame_t *response,
                                          al_time_us_t response_crc_stop_us);
al_master_event_t al_master_receive_at_timestamp(
    al_master_t *master, const gs_frame_t *response,
    const al_transport_timestamp_t *timestamp);
al_master_event_t al_master_tick(al_master_t *master,
                                 al_master_time_ms_t now_ms);
al_master_event_t al_master_tick_us(al_master_t *master, al_time_us_t now_us);
bool al_master_make_read_sample(al_master_t *master, uint8_t addr,
                                gs_frame_t *request);
bool al_master_make_read_info(al_master_t *master, uint8_t addr, uint8_t sub,
                              gs_frame_t *request);
bool al_master_make_write_config(al_master_t *master, uint8_t addr, uint8_t id,
                                 const uint8_t *value, uint8_t value_len,
                                 gs_frame_t *request);
bool al_master_make_diag(al_master_t *master, uint8_t addr,
                         const uint8_t *data, uint8_t len,
                         gs_frame_t *request);
bool al_master_make_sleep(al_master_t *master, uint8_t addr,
                          uint32_t duration_ms, gs_frame_t *request);
bool al_master_make_read_addr(al_master_t *master, uint8_t addr,
                              gs_frame_t *request);
bool al_master_make_assign(al_master_t *master,
                           const uint8_t uid[GS_UID_LEN], uint8_t new_addr,
                           gs_frame_t *request);
bool al_master_make_readdress(al_master_t *master,
                              const uint8_t uid[GS_UID_LEN], uint8_t old_addr,
                              uint8_t new_addr, gs_frame_t *request);
bool al_master_make_collect(al_master_t *master, uint8_t func,
                            uint32_t seed, uint8_t slots,
                            gs_frame_t *request);
bool al_master_make_discover(al_master_t *master, uint32_t seed,
                             uint8_t slots, gs_frame_t *request);
bool al_master_make_reconcile(al_master_t *master, uint32_t seed,
                              uint8_t slots, gs_frame_t *request);
bool al_master_make_recover(al_master_t *master, uint32_t seed,
                            uint8_t slots, gs_frame_t *request);
bool al_master_make_net_management(al_master_t *master, uint8_t addr,
                                   uint8_t op, uint8_t tid, uint32_t baud,
                                   gs_frame_t *request);
int16_t al_master_find_uid(const al_master_t *master,
                           const uint8_t uid[GS_UID_LEN]);
int16_t al_master_find_addr(const al_master_t *master, uint8_t addr);
uint8_t al_master_export_records(const al_master_t *master,
                                 al_master_node_record_t *records,
                                 uint8_t capacity);
/* On error, snapshot and records are not modified.  A NULL records pointer is
   valid only when the registry contains zero nodes. */
al_master_registry_export_result_t al_master_export_registry(
    const al_master_t *master, al_master_registry_snapshot_t *snapshot,
    al_master_node_record_t *records, uint8_t capacity);
bool al_master_restore_records(al_master_t *master,
                               const al_master_node_record_t *records,
                               uint8_t count);
bool al_master_take_registry_dirty(al_master_t *master);
bool al_master_on_persist_result(al_master_t *master, bool success);
void al_master_mark_bus_baud_unknown(al_master_t *master);
bool al_master_restore_bus_baud(al_master_t *master, uint32_t baud);
bool al_master_stage_registry_persistence(al_master_t *master);
al_master_work_kind_t al_master_next_work(al_master_t *master,
                                          al_master_time_ms_t now_ms,
                                          uint32_t discovery_seed,
                                          uint8_t slots,
                                          gs_frame_t *request);
/* Selects baud-management or normal work through one Core-owned scheduler.
   READY returns an already-started, TX-authorized transaction in request. */
al_master_next_tx_result_t al_master_next_transmission(
    al_master_t *master, al_master_time_ms_t now_ms,
    uint32_t discovery_seed, uint8_t slots, gs_frame_t *request);

#ifdef __cplusplus
}
#endif
#endif
