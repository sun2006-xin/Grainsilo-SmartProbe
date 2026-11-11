#ifndef AUTOLINK_CORE_H
#define AUTOLINK_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include <gs_proto.h>
#include "autolink_time.h"
#include "autolink_transport_timing.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { AL_COMMISSION_NONE, AL_COMMISSION_REGISTERING,
               AL_COMMISSION_PENDING, AL_COMMISSION_COMMITTED } al_commission_t;
typedef enum { AL_NETWORK_NO_NET, AL_NETWORK_ATTACHED, AL_NETWORK_PROBATION,
               AL_NETWORK_MIGRATION_CANDIDATE } al_network_t;
typedef enum { AL_SERVICE_NORMAL, AL_SERVICE_DEGRADED,
               AL_SERVICE_QUARANTINED_REG,
               AL_SERVICE_QUARANTINED_SERVICE } al_service_t;
typedef enum { AL_POWER_AWAKE, AL_POWER_SLEEP_ARMED, AL_POWER_SLEEPING } al_power_t;
typedef enum { AL_BAUD_IDLE, AL_BAUD_PREPARED, AL_BAUD_SWITCH_WAIT } al_baud_t;
typedef enum { AL_ADMIN_ACTIVE, AL_ADMIN_RETIRED } al_admin_t;

typedef enum {
    AL_ACTION_IGNORE,
    AL_ACTION_STATE_ONLY,
    AL_ACTION_ERROR,
    AL_ACTION_IMMEDIATE_RESPONSE,
    AL_ACTION_SCHEDULED_RESPONSE
} al_action_kind_t;

typedef enum {
    AL_PURPOSE_NONE,
    AL_PURPOSE_NORMAL_REPLY,
    AL_PURPOSE_REGISTRATION_REPORT,
    AL_PURPOSE_SERVICE_RECOVERY_REPORT,
    AL_PURPOSE_BAUD_SWITCH
} al_action_purpose_t;

typedef enum { AL_EFFECT_NONE, AL_EFFECT_SET_BAUD, AL_EFFECT_ENTER_SLEEP,
               AL_EFFECT_SEND_SCHEDULED }
    al_effect_kind_t;

typedef enum {
    AL_DELIVERY_DELIVERED,
    AL_DELIVERY_COLLISION,
    AL_DELIVERY_FAILED
} al_delivery_outcome_t;

typedef enum {
    AL_DURABLE_IDENTITY_PENDING = 1u << 0,
    AL_DURABLE_COMMISSION_COMMITTED = 1u << 1,
    AL_DURABLE_IDENTITY_CLEAR = 1u << 2,
    AL_DURABLE_BAUD = 1u << 3,
    AL_DURABLE_CONFIG = 1u << 4,
    AL_DURABLE_QUARANTINE = 1u << 5,
    AL_DURABLE_SCHEMA = 1u << 6
} al_durable_operation_t;

typedef struct {
    uint16_t durable_ops;
} al_transaction_plan_t;

typedef struct {
    uint8_t addr;
    uint8_t seq;
    uint8_t func;
    uint8_t len;
    const uint8_t *data;
    uint32_t now_ms;
    al_transport_timestamp_t request_crc_stop;
} al_request_t;

typedef struct {
    al_action_kind_t kind;
    al_transaction_plan_t plan;
    uint8_t wire_error;
    uint8_t response_func;
    uint8_t response_addr;
    uint8_t slot;
    uint8_t response_len;
    uint8_t response_data[GS_DATA_MAX_LEN];
    al_action_purpose_t purpose;
    al_effect_kind_t effect;
    uint32_t effect_due_ms;
    /* Physical first VER start-bit target for scheduled transmissions. */
    al_time_us_t scheduled_start_bit_us;
    uint32_t scheduled_start_accuracy_us;
    uint32_t effect_value;
    bool cacheable_completion;
    bool replayed;
} al_action_t;

/* Deliberately separate stores: 0x1C TID is not generic SEQ idempotency. */
typedef struct {
    bool valid;
    uint8_t seq;
    uint8_t func;
    uint8_t request_len;
    uint8_t request_data[GS_DATA_MAX_LEN];
    al_action_kind_t response_kind;
    uint8_t wire_error;
    uint8_t response_func;
    uint8_t response_addr;
    uint8_t response_len;
    uint8_t response_data[16];
    al_node_timer_t retention_timer;
} al_effect_cache_t;

typedef struct {
    bool prepared;
    uint8_t tid;
    uint32_t prepared_baud;
    al_node_timer_t prepare_timer;
    bool completed;
    uint8_t completed_seq;
    uint8_t completed_tid;
    uint32_t completed_baud;
    uint16_t completed_switch_after_ms;
    al_node_timer_t completed_timer;
    al_node_timer_t switch_timer;
    uint32_t pending_baud;
} al_baud_transaction_t;

typedef struct {
    uint8_t uid[GS_UID_LEN];
    uint8_t addr;
    uint64_t network_id;
    al_commission_t commission;
    al_network_t network;
    al_service_t service;
    al_power_t power;
    al_baud_t baud;
    al_admin_t administrative;
    uint64_t migration_network_id;
    uint8_t migration_seq;
    bool migration_seen;
    bool service_retry_due;
    uint32_t last_own_network_ms;
    uint32_t current_baud;
    al_node_timer_t sleep_timer;
    uint32_t sleep_duration_ms;
    al_node_timer_t registration_timer;
    al_node_timer_t assign_confirm_timer;
    al_node_timer_t quarantine_timer;
    uint8_t quarantine_attempt;
    uint8_t registration_failures;
    uint16_t schema_rev;
    uint32_t descriptor_signature;
    uint32_t protocol_error_count;
    al_effect_cache_t effects;
    al_baud_transaction_t baud_transaction;
} al_node_t;

void al_node_init(al_node_t *node, const uint8_t uid[GS_UID_LEN]);
bool al_node_invariants(const al_node_t *node);
/* Authoritative publication gate for timer-owned platform effects.  Request
   actions may describe a future effect; adapters execute only a due effect. */
bool al_node_action_effect_is_due(const al_node_t *node,
                                  const al_action_t *action);
void al_assign_pending(al_node_t *node, uint8_t addr, uint64_t network_id,
                       uint32_t now_ms, al_action_t *action);
void al_commit_commission(al_node_t *node, al_action_t *action);
void al_clear_commission_atomic(al_node_t *node, al_action_t *action);
void al_on_own_network_activity(al_node_t *node, uint32_t now_ms);
void al_enter_quarantine(al_node_t *node, al_service_t reason);
void al_enter_quarantine_at(al_node_t *node, al_service_t reason,
                            uint32_t now_ms);
void al_quarantine_expire(al_node_t *node);
void al_node_wake(al_node_t *node, uint32_t now_ms);
al_action_t al_node_on_delivery_at(al_node_t *node, const al_action_t *action,
                                   al_delivery_outcome_t outcome,
                                   uint32_t delivery_end_ms);
al_action_t al_node_prepare(al_node_t *node, const al_request_t *request);
void al_node_complete(al_node_t *node, const al_request_t *request,
                      al_action_t *action);
al_action_t al_node_execute(al_node_t *node, const al_request_t *request);
al_action_t al_node_tick(al_node_t *node, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
