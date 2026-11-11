#include "autolink_core.h"
#include "generated/autolink_semantics.h"

#include <assert.h>
#include <string.h>

#ifndef NDEBUG
#define AL_ASSERT_NODE(node_) assert(al_node_invariants(node_))
#else
#define AL_ASSERT_NODE(node_) ((void)0)
#endif

enum { CFG_SUCCESS = 0, CFG_UNSUPPORTED_ITEM = 2 };

_Static_assert(GS_TIMING_REG_TIMEOUT_MS <= AL_NODE_TIMER_MAX_INTERVAL_MS,
               "REG_TIMEOUT exceeds Node timer domain");
_Static_assert(GS_TIMING_ASSIGN_CONFIRM_TIMEOUT_MS <=
                   AL_NODE_TIMER_MAX_INTERVAL_MS,
               "ASSIGN_CONFIRM_TIMEOUT exceeds Node timer domain");
_Static_assert(GS_TIMING_PREPARE_TIMEOUT_MS <= AL_NODE_TIMER_MAX_INTERVAL_MS,
               "PREPARE_TIMEOUT exceeds Node timer domain");
_Static_assert(GS_TIMING_SLEEP_ARMED_WINDOW_MS <=
                   AL_NODE_TIMER_MAX_INTERVAL_MS,
               "SLEEP_ARMED_WINDOW exceeds Node timer domain");
_Static_assert(GS_Q_BACKOFF_MAX_MS <= AL_NODE_TIMER_MAX_INTERVAL_MS,
               "quarantine backoff exceeds Node timer domain");
_Static_assert(GS_TIMING_SWITCH_AFTER_MS <= AL_NODE_TIMER_MAX_INTERVAL_MS,
               "baud switch interval exceeds Node timer domain");
_Static_assert(GS_TIMING_DUP_CACHE_TTL_MS <= AL_NODE_TIMER_MAX_INTERVAL_MS,
               "duplicate cache TTL exceeds Node timer domain");
_Static_assert(GS_TIMING_COMMIT_REPLAY_WINDOW_MS <=
                   AL_NODE_TIMER_MAX_INTERVAL_MS,
               "completed COMMIT retention exceeds Node timer domain");
_Static_assert(GS_TIMING_GRACE_PERIOD_MS <= AL_NODE_TIMER_MAX_INTERVAL_MS,
               "migration grace exceeds Node timer domain");

static void timer_start(al_node_timer_t *timer, uint32_t now_ms,
                        uint32_t interval_ms)
{
    bool started = al_node_timer_start(timer, now_ms, interval_ms);
    assert(started);
    (void)started;
}

static uint64_t read_u64be(const uint8_t *p)
{
    uint64_t value = 0;
    uint8_t i;
    for (i = 0; i < GS_NET_ID_LEN; ++i) value = (value << 8) | p[i];
    return value;
}

static void write_u64be(uint8_t *p, uint64_t value)
{
    int i;
    for (i = 7; i >= 0; --i) { p[i] = (uint8_t)value; value >>= 8; }
}

static al_action_t action_base(void)
{
    al_action_t action;
    memset(&action, 0, sizeof(action));
    action.kind = AL_ACTION_IGNORE;
    return action;
}

static void plan_add(al_action_t *action, al_durable_operation_t operation)
{
    action->plan.durable_ops |= (uint16_t)operation;
}

static uint32_t quarantine_backoff(uint8_t stage)
{
    if (stage == 0u) return GS_Q_BACKOFF_1_MS;
    if (stage == 1u) return GS_Q_BACKOFF_2_MS;
    if (stage == 2u) return GS_Q_BACKOFF_3_MS;
    return GS_Q_BACKOFF_MAX_MS;
}

static al_action_t error_action(const al_node_t *node,
                                const al_request_t *request, uint8_t code,
                                uint8_t address)
{
    al_action_t action = action_base();
    (void)node;
    action.kind = AL_ACTION_ERROR;
    action.wire_error = code;
    action.response_func = (uint8_t)(GS_FUNC_ERR_BASE | request->func);
    action.response_addr = address;
    action.response_len = 1u;
    action.response_data[0] = code;
    action.purpose = AL_PURPOSE_NORMAL_REPLY;
    return action;
}

static al_action_t cache_replay(const al_effect_cache_t *cache)
{
    al_action_t action = action_base();
    action.kind = cache->response_kind;
    action.wire_error = cache->wire_error;
    action.response_func = cache->response_func;
    action.response_addr = cache->response_addr;
    action.response_len = cache->response_len;
    memcpy(action.response_data, cache->response_data, cache->response_len);
    action.purpose = AL_PURPOSE_NORMAL_REPLY;
    action.replayed = true;
    return action;
}

static bool cache_identity_equal(const al_effect_cache_t *cache,
                                 const al_request_t *request)
{
    return cache->func == request->func &&
           cache->request_len == request->len &&
           (request->len == 0u ||
            memcmp(cache->request_data, request->data, request->len) == 0);
}

static void cache_store(al_node_t *node, const al_request_t *request,
                        const al_action_t *action)
{
    al_effect_cache_t *cache = &node->effects;
    cache->valid = true;
    cache->seq = request->seq;
    cache->func = request->func;
    cache->request_len = request->len;
    if (request->len != 0u)
        memcpy(cache->request_data, request->data, request->len);
    cache->response_kind = action->kind;
    cache->wire_error = action->wire_error;
    cache->response_func = action->response_func;
    cache->response_addr = action->response_addr;
    cache->response_len = action->response_len > sizeof(cache->response_data) ?
                          (uint8_t)sizeof(cache->response_data) : action->response_len;
    memcpy(cache->response_data, action->response_data, cache->response_len);
    timer_start(&cache->retention_timer, request->now_ms,
                GS_TIMING_DUP_CACHE_TTL_MS);
}

static void clear_completed_commit(al_baud_transaction_t *transaction)
{
    transaction->completed = false;
    transaction->completed_seq = 0u;
    transaction->completed_tid = 0u;
    transaction->completed_baud = 0u;
    transaction->completed_switch_after_ms = 0u;
    al_node_timer_cancel(&transaction->completed_timer);
}

static void clear_prepared_baud(al_baud_transaction_t *transaction)
{
    transaction->prepared = false;
    transaction->tid = 0u;
    transaction->prepared_baud = 0u;
    al_node_timer_cancel(&transaction->prepare_timer);
}

static void expire_completed_commit(al_node_t *node, uint32_t now_ms)
{
    if (node->baud_transaction.completed &&
        al_node_timer_expired(&node->baud_transaction.completed_timer,
                              now_ms))
        clear_completed_commit(&node->baud_transaction);
}

static uint8_t simple_slot(const al_node_t *node, const al_request_t *request)
{
    const uint8_t *seed = request->func == GS_FUNC_RECOVER ?
                          request->data + 9u : request->data;
    uint8_t slots = request->func == GS_FUNC_RECOVER ?
                    request->data[13] : request->data[4];
    return (uint8_t)(gs_murmur3_x86_32(node->uid, GS_UID_LEN,
                                      gs_get_u32be(seed)) % slots);
}

void al_node_init(al_node_t *node, const uint8_t uid[GS_UID_LEN])
{
    memset(node, 0, sizeof(*node));
    memcpy(node->uid, uid, GS_UID_LEN);
    node->addr = GS_ADDR_UNASSIGNED;
    node->power = AL_POWER_AWAKE;
    node->administrative = AL_ADMIN_ACTIVE;
    node->current_baud = GS_BAUD_DEFAULT;
    AL_ASSERT_NODE(node);
}

bool al_node_invariants(const al_node_t *node)
{
    if (node == NULL) return false;
    if (!al_node_timer_valid(&node->effects.retention_timer) ||
        !al_node_timer_valid(&node->baud_transaction.prepare_timer) ||
        !al_node_timer_valid(&node->baud_transaction.completed_timer) ||
        !al_node_timer_valid(&node->baud_transaction.switch_timer) ||
        !al_node_timer_valid(&node->sleep_timer) ||
        !al_node_timer_valid(&node->registration_timer) ||
        !al_node_timer_valid(&node->assign_confirm_timer) ||
        !al_node_timer_valid(&node->quarantine_timer)) return false;
    if (node->commission > AL_COMMISSION_COMMITTED ||
        node->network > AL_NETWORK_MIGRATION_CANDIDATE ||
        node->service > AL_SERVICE_QUARANTINED_SERVICE ||
        node->power > AL_POWER_SLEEPING || node->baud > AL_BAUD_SWITCH_WAIT ||
        node->administrative > AL_ADMIN_RETIRED) return false;
    if (node->commission == AL_COMMISSION_NONE &&
        (node->addr != GS_ADDR_UNASSIGNED || node->network_id != 0u ||
         node->network != AL_NETWORK_NO_NET)) return false;
    if ((node->commission == AL_COMMISSION_PENDING ||
         node->commission == AL_COMMISSION_COMMITTED) &&
        (node->addr < GS_ADDR_MIN || node->addr > GS_ADDR_MAX ||
         node->network_id == 0u || node->network == AL_NETWORK_NO_NET)) return false;
    if (node->commission == AL_COMMISSION_REGISTERING &&
        (node->addr != GS_ADDR_UNASSIGNED || node->network_id != 0u ||
         node->network != AL_NETWORK_NO_NET ||
         !node->registration_timer.active)) return false;
    if (node->commission != AL_COMMISSION_REGISTERING &&
        node->registration_timer.active) return false;
    if (node->commission == AL_COMMISSION_PENDING &&
        !node->assign_confirm_timer.active) return false;
    if (node->commission != AL_COMMISSION_PENDING &&
        node->assign_confirm_timer.active) return false;
    if (node->network == AL_NETWORK_PROBATION &&
        node->commission != AL_COMMISSION_COMMITTED) return false;
    if (node->network == AL_NETWORK_MIGRATION_CANDIDATE &&
        (node->commission != AL_COMMISSION_COMMITTED ||
         !node->migration_seen || node->migration_network_id == 0u ||
         node->migration_network_id == node->network_id)) return false;
    if (node->service == AL_SERVICE_QUARANTINED_REG &&
        (node->commission != AL_COMMISSION_NONE ||
         !node->quarantine_timer.active)) return false;
    if (node->service == AL_SERVICE_QUARANTINED_SERVICE &&
        (node->commission != AL_COMMISSION_COMMITTED ||
         (!node->service_retry_due && !node->quarantine_timer.active) ||
         (node->service_retry_due && node->quarantine_timer.active)))
        return false;
    if (node->quarantine_timer.active &&
        node->service != AL_SERVICE_QUARANTINED_REG &&
        node->service != AL_SERVICE_QUARANTINED_SERVICE) return false;
    if (node->service_retry_due &&
        node->service != AL_SERVICE_QUARANTINED_SERVICE) return false;
    if (node->power != AL_POWER_AWAKE &&
        node->commission != AL_COMMISSION_COMMITTED) return false;
    if (node->power == AL_POWER_SLEEP_ARMED &&
        (!node->sleep_timer.active || node->sleep_duration_ms == 0u))
        return false;
    if (node->power != AL_POWER_SLEEP_ARMED && node->sleep_timer.active)
        return false;
    if (node->power == AL_POWER_SLEEPING && node->sleep_duration_ms == 0u)
        return false;
    if (node->baud == AL_BAUD_PREPARED &&
        (!node->baud_transaction.prepared ||
         node->baud_transaction.prepared_baud == 0u ||
         !node->baud_transaction.prepare_timer.active)) return false;
    if (node->baud != AL_BAUD_PREPARED &&
        node->baud_transaction.prepared) return false;
    if (node->baud != AL_BAUD_PREPARED &&
        node->baud_transaction.prepare_timer.active) return false;
    if (node->baud == AL_BAUD_SWITCH_WAIT &&
        (node->baud_transaction.pending_baud == 0u ||
         !node->baud_transaction.switch_timer.active)) return false;
    if (node->baud != AL_BAUD_SWITCH_WAIT &&
        node->baud_transaction.switch_timer.active) return false;
    if (node->baud_transaction.completed !=
        node->baud_transaction.completed_timer.active) return false;
    if (node->effects.valid != node->effects.retention_timer.active)
        return false;
    if (node->effects.valid && node->effects.response_len >
        sizeof(node->effects.response_data)) return false;
    return true;
}

bool al_node_action_effect_is_due(const al_node_t *node,
                                  const al_action_t *action)
{
    if (node == NULL || action == NULL ||
        action->kind != AL_ACTION_STATE_ONLY ||
        action->plan.durable_ops != 0u) return false;
    if (action->effect == AL_EFFECT_SET_BAUD)
        return node->baud == AL_BAUD_IDLE &&
               !node->baud_transaction.switch_timer.active &&
               node->current_baud == action->effect_value;
    if (action->effect == AL_EFFECT_ENTER_SLEEP)
        return node->power == AL_POWER_SLEEPING &&
               !node->sleep_timer.active;
    return false;
}

void al_assign_pending(al_node_t *node, uint8_t addr, uint64_t network_id,
                       uint32_t now_ms, al_action_t *action)
{
    AL_ASSERT_NODE(node);
    assert(addr >= GS_ADDR_MIN && addr <= GS_ADDR_MAX && network_id != 0u);
    node->addr = addr;
    node->network_id = network_id;
    node->commission = AL_COMMISSION_PENDING;
    node->network = AL_NETWORK_ATTACHED;
    node->service = AL_SERVICE_NORMAL;
    node->service_retry_due = false;
    al_node_timer_cancel(&node->registration_timer);
    al_node_timer_cancel(&node->quarantine_timer);
    node->quarantine_attempt = 0u;
    timer_start(&node->assign_confirm_timer, now_ms,
                GS_TIMING_ASSIGN_CONFIRM_TIMEOUT_MS);
    plan_add(action, AL_DURABLE_IDENTITY_PENDING);
    AL_ASSERT_NODE(node);
}

void al_commit_commission(al_node_t *node, al_action_t *action)
{
    AL_ASSERT_NODE(node);
    if (node->commission == AL_COMMISSION_PENDING) {
        node->commission = AL_COMMISSION_COMMITTED;
        al_node_timer_cancel(&node->assign_confirm_timer);
        plan_add(action, AL_DURABLE_COMMISSION_COMMITTED);
    }
    AL_ASSERT_NODE(node);
}

void al_clear_commission_atomic(al_node_t *node, al_action_t *action)
{
    AL_ASSERT_NODE(node);
    node->addr = GS_ADDR_UNASSIGNED;
    node->network_id = 0u;
    node->commission = AL_COMMISSION_NONE;
    node->network = AL_NETWORK_NO_NET;
    node->migration_network_id = 0u;
    node->migration_seq = 0u;
    node->migration_seen = false;
    node->service = AL_SERVICE_NORMAL;
    node->service_retry_due = false;
    al_node_timer_cancel(&node->registration_timer);
    al_node_timer_cancel(&node->assign_confirm_timer);
    al_node_timer_cancel(&node->quarantine_timer);
    al_node_timer_cancel(&node->sleep_timer);
    node->sleep_duration_ms = 0u;
    plan_add(action, AL_DURABLE_IDENTITY_CLEAR);
    AL_ASSERT_NODE(node);
}

void al_on_own_network_activity(al_node_t *node, uint32_t now_ms)
{
    AL_ASSERT_NODE(node);
    node->last_own_network_ms = now_ms;
    node->migration_network_id = 0u;
    node->migration_seq = 0u;
    node->migration_seen = false;
    if (node->network == AL_NETWORK_PROBATION ||
        node->network == AL_NETWORK_MIGRATION_CANDIDATE)
        node->network = AL_NETWORK_ATTACHED;
    AL_ASSERT_NODE(node);
}

void al_enter_quarantine(al_node_t *node, al_service_t reason)
{
    al_enter_quarantine_at(node, reason, 0u);
}

void al_enter_quarantine_at(al_node_t *node, al_service_t reason,
                            uint32_t now_ms)
{
    AL_ASSERT_NODE(node);
    if ((reason == AL_SERVICE_QUARANTINED_REG &&
         node->commission != AL_COMMISSION_NONE) ||
        (reason == AL_SERVICE_QUARANTINED_SERVICE &&
         node->commission != AL_COMMISSION_COMMITTED)) return;
    if (reason == AL_SERVICE_QUARANTINED_REG ||
        reason == AL_SERVICE_QUARANTINED_SERVICE) {
        node->service = reason;
        node->service_retry_due = false;
        node->quarantine_attempt = 0u;
        timer_start(&node->quarantine_timer, now_ms,
                    quarantine_backoff(node->quarantine_attempt));
    }
    AL_ASSERT_NODE(node);
}

void al_node_wake(al_node_t *node, uint32_t now_ms)
{
    if (node == NULL || node->power != AL_POWER_SLEEPING) return;
    AL_ASSERT_NODE(node);
    node->power = AL_POWER_AWAKE;
    al_node_timer_cancel(&node->sleep_timer);
    node->sleep_duration_ms = 0u;
    node->last_own_network_ms = now_ms;
    if (node->commission == AL_COMMISSION_COMMITTED)
        node->network = AL_NETWORK_ATTACHED;
    AL_ASSERT_NODE(node);
}

void al_quarantine_expire(al_node_t *node)
{
    AL_ASSERT_NODE(node);
    if (node->service == AL_SERVICE_QUARANTINED_REG) {
        node->service = AL_SERVICE_NORMAL;
        al_node_timer_cancel(&node->quarantine_timer);
    } else if (node->service == AL_SERVICE_QUARANTINED_SERVICE) {
        node->service_retry_due = true;
        al_node_timer_cancel(&node->quarantine_timer);
    }
    AL_ASSERT_NODE(node);
}

al_action_t al_node_on_delivery_at(al_node_t *node, const al_action_t *action,
                                   al_delivery_outcome_t outcome,
                                   uint32_t delivery_end_ms)
{
    al_action_t transition = action_base();
    bool delivered = outcome == AL_DELIVERY_DELIVERED;
    AL_ASSERT_NODE(node);
    if (action != NULL && action->purpose == AL_PURPOSE_SERVICE_RECOVERY_REPORT &&
        node->service == AL_SERVICE_QUARANTINED_SERVICE) {
        transition.kind = AL_ACTION_STATE_ONLY;
        node->service_retry_due = false;
        if (delivered) {
            node->service = AL_SERVICE_NORMAL;
            node->quarantine_attempt = 0u;
            al_node_timer_cancel(&node->quarantine_timer);
            plan_add(&transition, AL_DURABLE_QUARANTINE);
        } else {
            if (node->quarantine_attempt < 3u) node->quarantine_attempt++;
            timer_start(&node->quarantine_timer, delivery_end_ms,
                        quarantine_backoff(node->quarantine_attempt));
        }
    }
    if (action != NULL && action->purpose == AL_PURPOSE_REGISTRATION_REPORT &&
        node->commission == AL_COMMISSION_NONE) {
        transition.kind = AL_ACTION_STATE_ONLY;
        if (delivered) {
            if (node->service == AL_SERVICE_QUARANTINED_REG)
                plan_add(&transition, AL_DURABLE_QUARANTINE);
            node->service = AL_SERVICE_NORMAL;
            node->service_retry_due = false;
            al_node_timer_cancel(&node->quarantine_timer);
            node->commission = AL_COMMISSION_REGISTERING;
            timer_start(&node->registration_timer, delivery_end_ms,
                        GS_TIMING_REG_TIMEOUT_MS);
        } else {
            if (node->registration_failures < 255u)
                node->registration_failures++;
            al_enter_quarantine_at(node, AL_SERVICE_QUARANTINED_REG,
                                   delivery_end_ms);
            node->quarantine_attempt = node->registration_failures > 3u ? 3u :
                (uint8_t)(node->registration_failures - 1u);
            timer_start(&node->quarantine_timer, delivery_end_ms,
                        quarantine_backoff(node->quarantine_attempt));
            plan_add(&transition, AL_DURABLE_QUARANTINE);
        }
    }
    AL_ASSERT_NODE(node);
    return transition;
}

static al_action_t execute_collect(al_node_t *node, const al_request_t *request)
{
    al_action_t action = action_base();
    uint64_t net = request->func == GS_FUNC_RECOVER ?
                   read_u64be(request->data) : read_u64be(request->data + 5u);
    uint8_t slots = request->func == GS_FUNC_RECOVER ?
                    request->data[13] : request->data[4];
    if (request->addr != GS_ADDR_BROADCAST || net == 0u ||
        (slots != 8u && slots != 16u && slots != 32u && slots != 64u))
        return action;
    if (request->func == GS_FUNC_RECOVER && request->data[8] != 1u)
        return action;

    if (net == node->network_id && node->network_id != 0u)
        al_on_own_network_activity(node, request->now_ms);
    if (request->func == GS_FUNC_DISCOVER &&
        node->commission == AL_COMMISSION_COMMITTED && net != node->network_id &&
        al_node_time_elapsed_ms(node->last_own_network_ms, request->now_ms) >=
            GS_TIMING_GRACE_PERIOD_MS) {
        if (node->migration_seen && node->migration_network_id == net &&
            node->migration_seq != request->seq) {
            al_clear_commission_atomic(node, &action);
            action.kind = AL_ACTION_STATE_ONLY;
            return action;
        }
        node->migration_seen = true;
        node->migration_network_id = net;
        node->migration_seq = request->seq;
        node->network = AL_NETWORK_MIGRATION_CANDIDATE;
        action.kind = AL_ACTION_STATE_ONLY;
        return action;
    }

    if (request->func == GS_FUNC_DISCOVER &&
        node->commission == AL_COMMISSION_NONE &&
        (node->service == AL_SERVICE_NORMAL ||
         node->service == AL_SERVICE_QUARANTINED_REG)) {
        action.kind = AL_ACTION_SCHEDULED_RESPONSE;
        action.purpose = AL_PURPOSE_REGISTRATION_REPORT;
    } else if (request->func == GS_FUNC_RECONCILE &&
               net == node->network_id &&
               (node->service == AL_SERVICE_NORMAL ||
                node->service == AL_SERVICE_DEGRADED ||
                (node->service == AL_SERVICE_QUARANTINED_SERVICE &&
                 node->service_retry_due))) {
        action.kind = AL_ACTION_SCHEDULED_RESPONSE;
        action.purpose = node->service == AL_SERVICE_QUARANTINED_SERVICE ?
                         AL_PURPOSE_SERVICE_RECOVERY_REPORT :
                         AL_PURPOSE_REGISTRATION_REPORT;
    } else if (request->func == GS_FUNC_RECOVER) {
        /* RECOVER is a bus participant scan, deliberately independent of net. */
        action.kind = AL_ACTION_SCHEDULED_RESPONSE;
        action.purpose = AL_PURPOSE_REGISTRATION_REPORT;
    }
    if (action.kind == AL_ACTION_SCHEDULED_RESPONSE) {
        uint32_t slot_ms;
        if (request->request_crc_stop.event != AL_BUS_EVENT_RX_CRC_STOP)
            return action_base();
        action.response_func = GS_FUNC_REG_REQ;
        action.response_addr = node->commission == AL_COMMISSION_NONE ?
                               GS_ADDR_UNASSIGNED : node->addr;
        action.slot = simple_slot(node, request);
        action.effect = AL_EFFECT_SEND_SCHEDULED;
        slot_ms = request->func == GS_FUNC_DISCOVER ?
                  GS_TIMING_T_SLOT_DISCOVER_MS :
                  (request->func == GS_FUNC_RECONCILE ?
                   GS_TIMING_T_SLOT_RECONCILE_MS :
                   GS_TIMING_T_SLOT_RECOVER_MS);
        action.effect_due_ms = al_node_time_projection_after(
            request->now_ms, GS_TIMING_T_IFG_MS +
            (uint32_t)action.slot * slot_ms);
        action.scheduled_start_bit_us = request->request_crc_stop.at_us +
            (al_time_us_t)GS_TIMING_T_IFG_MS * UINT64_C(1000) +
            (al_time_us_t)action.slot * slot_ms * UINT64_C(1000);
        action.scheduled_start_accuracy_us = AL_SLOT_ACCURACY_US;
    }
    return action;
}

static al_action_t execute_net(al_node_t *node, const al_request_t *request)
{
    al_action_t action = action_base();
    uint64_t net = read_u64be(request->data);
    uint8_t op = request->data[8];
    uint8_t tid = request->data[9];
    uint32_t baud;
    bool completed_identity;
    if (op != GS_NET_OP_PREPARE && op != GS_NET_OP_QUERY &&
        op != GS_NET_OP_COMMIT) {
        if (request->addr == node->addr)
            return error_action(node, request, GS_WIRE_ERR_PROTO_FEATURE,
                                node->addr);
        return action;
    }
    if ((op == GS_NET_OP_PREPARE && request->len != 14u) ||
        (op == GS_NET_OP_QUERY && request->len != 10u) ||
        (op == GS_NET_OP_COMMIT && request->len != 16u)) {
        if (request->addr == node->addr)
            return error_action(node, request, GS_WIRE_ERR_BAD_PAYLOAD_LEN,
                                node->addr);
        return action;
    }
    if (net == 0u) {
        if (op == GS_NET_OP_QUERY && request->addr == node->addr)
            return error_action(node, request, GS_WIRE_ERR_INVALID_VALUE,
                                node->addr);
        return action;
    }
    if (net != node->network_id) {
        if (op == GS_NET_OP_QUERY && request->addr == node->addr)
            return error_action(node, request, GS_WIRE_ERR_NET_ID, node->addr);
        return action;
    }
    if (op == GS_NET_OP_QUERY && request->addr != node->addr) return action;
    if (op != GS_NET_OP_QUERY && request->addr != GS_ADDR_BROADCAST) return action;
    al_on_own_network_activity(node, request->now_ms);

    if (op == GS_NET_OP_PREPARE) {
        baud = gs_get_u32be(request->data + 10u);
        if (!al_is_supported_baud(baud)) return action;
        if (node->baud_transaction.prepared &&
            node->baud_transaction.tid == tid &&
            node->baud_transaction.prepared_baud != baud) return action;
        node->baud_transaction.prepared = true;
        node->baud_transaction.tid = tid;
        node->baud_transaction.prepared_baud = baud;
        timer_start(&node->baud_transaction.prepare_timer, request->now_ms,
                    GS_TIMING_PREPARE_TIMEOUT_MS);
        node->baud = AL_BAUD_PREPARED;
        action.kind = AL_ACTION_STATE_ONLY;
        return action;
    }
    if (op == GS_NET_OP_QUERY) {
        if (!node->baud_transaction.prepared) {
            action.kind = AL_ACTION_IMMEDIATE_RESPONSE;
            action.response_func = GS_FUNC_NET_MGMT;
            action.response_addr = node->addr;
            action.response_len = 2u;
            action.response_data[0] = 1u;
            action.response_data[1] = 2u;
        } else if (node->baud_transaction.tid != tid) {
            action = error_action(node, request, GS_WIRE_ERR_TXN_CONFLICT,
                                  node->addr);
        } else {
            action.kind = AL_ACTION_IMMEDIATE_RESPONSE;
            action.response_func = GS_FUNC_NET_MGMT;
            action.response_addr = node->addr;
            action.response_len = 2u;
            action.response_data[0] = 0u;
            action.response_data[1] = 0u;
        }
        action.purpose = AL_PURPOSE_NORMAL_REPLY;
        return action;
    }

    baud = gs_get_u32be(request->data + 10u);
    if (node->baud_transaction.completed &&
        node->baud_transaction.completed_seq == request->seq) {
        completed_identity = node->baud_transaction.completed_tid == tid &&
            node->baud_transaction.completed_baud == baud &&
            node->baud_transaction.completed_switch_after_ms ==
                gs_get_u16be(request->data + 14u);
        if (!completed_identity && node->protocol_error_count < UINT32_MAX)
            node->protocol_error_count++;
        return action;
    }
    if (node->baud_transaction.prepared &&
        node->baud_transaction.tid == tid &&
        node->baud_transaction.prepared_baud == baud &&
        gs_get_u16be(request->data + 14u) == GS_TIMING_SWITCH_AFTER_MS) {
        clear_prepared_baud(&node->baud_transaction);
        node->baud_transaction.completed = true;
        node->baud_transaction.completed_seq = request->seq;
        node->baud_transaction.completed_tid = tid;
        node->baud_transaction.completed_baud = baud;
        node->baud_transaction.completed_switch_after_ms =
            gs_get_u16be(request->data + 14u);
        timer_start(&node->baud_transaction.completed_timer, request->now_ms,
                    GS_TIMING_COMMIT_REPLAY_WINDOW_MS);
        node->baud_transaction.pending_baud = baud;
        timer_start(&node->baud_transaction.switch_timer, request->now_ms,
                    GS_TIMING_SWITCH_AFTER_MS);
        node->baud = AL_BAUD_SWITCH_WAIT;
        action.kind = AL_ACTION_STATE_ONLY;
        plan_add(&action, AL_DURABLE_BAUD);
        action.effect = AL_EFFECT_SET_BAUD;
        action.effect_due_ms =
            al_node_timer_due_tick(&node->baud_transaction.switch_timer);
        action.effect_value = baud;
        action.purpose = AL_PURPOSE_BAUD_SWITCH;
    }
    return action;
}

static al_action_t execute_assign(al_node_t *node, const al_request_t *request)
{
    al_action_t action = action_base();
    uint64_t net;
    uint8_t old_addr = node->addr;
    if (memcmp(request->data, node->uid, GS_UID_LEN) != 0) return action;
    if (request->func == GS_FUNC_ASSIGN_ADDR && request->addr != GS_ADDR_UNASSIGNED)
        return action;
    if (request->func == GS_FUNC_READDRESS && request->addr != GS_ADDR_BROADCAST)
        return action;
    net = read_u64be(request->data + 7u);
    if (request->data[6] < GS_ADDR_MIN || request->data[6] > GS_ADDR_MAX ||
        net == 0u)
        return error_action(node, request, GS_WIRE_ERR_INVALID_VALUE,
                            request->func == GS_FUNC_ASSIGN_ADDR ?
                            GS_ADDR_UNASSIGNED : old_addr);
    al_assign_pending(node, request->data[6], net, request->now_ms, &action);
    action.kind = AL_ACTION_IMMEDIATE_RESPONSE;
    action.response_func = GS_FUNC_REG_CONFIRM;
    action.response_addr = request->func == GS_FUNC_ASSIGN_ADDR ?
                           GS_ADDR_UNASSIGNED : node->addr;
    action.response_len = 15u;
    memcpy(action.response_data, node->uid, GS_UID_LEN);
    action.response_data[6] = node->addr;
    write_u64be(action.response_data + 7u, node->network_id);
    action.purpose = AL_PURPOSE_NORMAL_REPLY;
    return action;
}

static al_action_t execute_ordinary(al_node_t *node, const al_request_t *request)
{
    al_action_t action = action_base();
    if (request->func == GS_FUNC_EVENT_PULL)
        return error_action(node, request, GS_WIRE_ERR_PROTO_FEATURE, node->addr);
    if (request->func == GS_FUNC_READ_INFO && request->data[0] > GS_INFO_SUB_CHDESC)
        return error_action(node, request, GS_WIRE_ERR_PROTO_FEATURE, node->addr);
    action.kind = AL_ACTION_IMMEDIATE_RESPONSE;
    action.response_func = request->func;
    action.response_addr = node->addr;
    action.purpose = AL_PURPOSE_NORMAL_REPLY;
    if (request->func == GS_FUNC_WRITE_CFG) {
        uint8_t id = request->data[0];
        uint8_t expected = al_config_request_length(id);
        action.response_len = 2u;
        action.response_data[0] = id;
        if (id < 1u || id > 4u) {
            action.response_data[1] = CFG_UNSUPPORTED_ITEM;
            return action;
        }
        if (request->len != expected)
            return error_action(node, request, GS_WIRE_ERR_BAD_PAYLOAD_LEN,
                                node->addr);
        action.response_data[1] = CFG_SUCCESS;
        plan_add(&action, AL_DURABLE_CONFIG);
    } else if (request->func == GS_FUNC_SLEEP) {
        action.response_len = 1u;
        action.response_data[0] = 0u;
        if (gs_get_u32be(request->data) != 0u) {
            node->power = AL_POWER_SLEEP_ARMED;
            timer_start(&node->sleep_timer, request->now_ms,
                        GS_TIMING_SLEEP_ARMED_WINDOW_MS);
            node->sleep_duration_ms = gs_get_u32be(request->data);
            action.effect = AL_EFFECT_ENTER_SLEEP;
            action.effect_due_ms = al_node_timer_due_tick(&node->sleep_timer);
            action.effect_value = node->sleep_duration_ms;
        }
    }
    return action;
}

static al_action_t prepare_current(al_node_t *node,
                                   const al_request_t *request)
{
    al_action_t action = action_base();
    al_action_t confirmation = action_base();
    al_node_t planned;
    uint8_t op = 0u;
    bool confirms_candidate;
    if (node == NULL || request == NULL ||
        (request->len != 0u && request->data == NULL)) return action;
    AL_ASSERT_NODE(node);

    if (!al_is_known_request(request->func)) {
        if (request->addr == node->addr && node->commission != AL_COMMISSION_NONE)
            return error_action(node, request, GS_WIRE_ERR_ILLEGAL_FUNC,
                                node->addr);
        return action;
    }
    if (!al_request_length_allowed(request->func, request->data,
                                   request->len)) {
        if (request->addr == node->addr && node->commission != AL_COMMISSION_NONE)
            return error_action(node, request, GS_WIRE_ERR_BAD_PAYLOAD_LEN,
                                node->addr);
        return action;
    }
    if (request->func == GS_FUNC_NET_MGMT && request->len >= 9u)
        op = request->data[8];
    confirms_candidate = node->commission == AL_COMMISSION_PENDING &&
        request->addr == node->addr &&
        al_candidate_confirmation_eligible(request->func, op);

    /* Generic transaction replay precedes the power gate so a SLEEP retry can
       recover an ACK while the original request is still armed.  Timer expiry
       is owned by the time-advance stage that precedes this classifier. */
    if (al_is_generic_idempotent(request->func)) {
        if (node->effects.valid && node->effects.seq == request->seq) {
            if (cache_identity_equal(&node->effects, request))
                return cache_replay(&node->effects);
            return error_action(node, request, GS_WIRE_ERR_TXN_CONFLICT,
                                request->func == GS_FUNC_ASSIGN_ADDR ?
                                GS_ADDR_UNASSIGNED : node->addr);
        }
    }

    if (node->administrative == AL_ADMIN_RETIRED ||
        node->power != AL_POWER_AWAKE) return action;
    if (!al_node_state_allowed(request->func, (uint8_t)node->commission,
                               (uint8_t)node->service)) return action;

    /* Execute the semantic transition against a transaction-local node.  A
       candidate confirmation is part of that plan, so operation state never
       exposes an otherwise-illegal PENDING intermediate (for example,
       SLEEP_ARMED).  Only the combined, invariant-valid result is published. */
    planned = *node;
    if (confirms_candidate)
        al_commit_commission(&planned, &confirmation);

    if (request->func == GS_FUNC_DISCOVER ||
        request->func == GS_FUNC_RECONCILE ||
        request->func == GS_FUNC_RECOVER)
        action = execute_collect(&planned, request);
    else if (request->func == GS_FUNC_NET_MGMT)
        action = execute_net(&planned, request);
    else {
        if (request->func != GS_FUNC_ASSIGN_ADDR &&
            request->func != GS_FUNC_READDRESS && request->addr != planned.addr)
            return action;
        if (request->func == GS_FUNC_ASSIGN_ADDR ||
            request->func == GS_FUNC_READDRESS)
            action = execute_assign(&planned, request);
        else
            action = execute_ordinary(&planned, request);
    }

    if (confirms_candidate) {
        if (action.kind != AL_ACTION_ERROR && action.kind != AL_ACTION_IGNORE) {
            action.plan.durable_ops |= confirmation.plan.durable_ops;
        } else {
            planned.commission = node->commission;
            planned.assign_confirm_timer = node->assign_confirm_timer;
        }
    }
    if (planned.commission == AL_COMMISSION_COMMITTED)
        al_node_timer_cancel(&planned.assign_confirm_timer);
    if (al_is_generic_idempotent(request->func) &&
        action.kind != AL_ACTION_IGNORE)
        action.cacheable_completion = true;
    AL_ASSERT_NODE(&planned);
    if (!al_node_invariants(&planned)) return action_base();
    *node = planned;
    AL_ASSERT_NODE(node);
    return action;
}

al_action_t al_node_prepare(al_node_t *node, const al_request_t *request)
{
    al_action_t elapsed;
    al_action_t action;
    if (node == NULL || request == NULL ||
        (request->len != 0u && request->data == NULL)) return action_base();

    /* A request is also a clock observation.  Expiries at the observed
       timestamp precede classification, so an elapsed identity cannot be
       revived by a request at the same timestamp.  External effects are
       delivered one at a time; the caller resumes the same request timestamp
       after applying a due effect. */
    elapsed = al_node_tick(node, request->now_ms);
    if (elapsed.effect != AL_EFFECT_NONE) return elapsed;

    action = prepare_current(node, request);
    action.plan.durable_ops |= elapsed.plan.durable_ops;
    if (action.kind == AL_ACTION_IGNORE &&
        elapsed.kind == AL_ACTION_STATE_ONLY)
        action.kind = AL_ACTION_STATE_ONLY;
    return action;
}

void al_node_complete(al_node_t *node, const al_request_t *request,
                      al_action_t *action)
{
    if (node == NULL || request == NULL || action == NULL) return;
    AL_ASSERT_NODE(node);
    if (action->replayed || !action->cacheable_completion) return;
    cache_store(node, request, action);
    action->cacheable_completion = false;
    AL_ASSERT_NODE(node);
}

al_action_t al_node_execute(al_node_t *node, const al_request_t *request)
{
    al_action_t action = al_node_prepare(node, request);
    al_node_complete(node, request, &action);
    return action;
}

al_action_t al_node_tick(al_node_t *node, uint32_t now_ms)
{
    al_action_t action = action_base();
    if (node == NULL) return action;
    AL_ASSERT_NODE(node);
    if (node->effects.valid &&
        al_node_timer_expired(&node->effects.retention_timer, now_ms))
        memset(&node->effects, 0, sizeof(node->effects));
    if (node->commission == AL_COMMISSION_REGISTERING &&
        al_node_timer_expired(&node->registration_timer, now_ms)) {
        node->commission = AL_COMMISSION_NONE;
        al_node_timer_cancel(&node->registration_timer);
        if (node->registration_failures < 255u) node->registration_failures++;
        al_enter_quarantine_at(node, AL_SERVICE_QUARANTINED_REG, now_ms);
        node->quarantine_attempt = node->registration_failures > 3u ? 3u :
                                   (uint8_t)(node->registration_failures - 1u);
        timer_start(&node->quarantine_timer, now_ms,
                    quarantine_backoff(node->quarantine_attempt));
        action.kind = AL_ACTION_STATE_ONLY;
        plan_add(&action, AL_DURABLE_QUARANTINE);
    }
    if (node->commission == AL_COMMISSION_PENDING &&
        al_node_timer_expired(&node->assign_confirm_timer, now_ms)) {
        al_clear_commission_atomic(node, &action);
        action.kind = AL_ACTION_STATE_ONLY;
    }
    if ((node->service == AL_SERVICE_QUARANTINED_REG ||
         node->service == AL_SERVICE_QUARANTINED_SERVICE) &&
        al_node_timer_expired(&node->quarantine_timer, now_ms)) {
        al_quarantine_expire(node);
        action.kind = AL_ACTION_STATE_ONLY;
        if (node->service == AL_SERVICE_NORMAL)
            plan_add(&action, AL_DURABLE_QUARANTINE);
    }
    if (node->baud == AL_BAUD_PREPARED &&
        al_node_timer_expired(&node->baud_transaction.prepare_timer, now_ms)) {
        clear_prepared_baud(&node->baud_transaction);
        node->baud = AL_BAUD_IDLE;
        action.kind = AL_ACTION_STATE_ONLY;
    }
    expire_completed_commit(node, now_ms);
    if (node->baud == AL_BAUD_SWITCH_WAIT &&
        al_node_timer_expired(&node->baud_transaction.switch_timer, now_ms)) {
        uint32_t due_ms =
            al_node_timer_due_tick(&node->baud_transaction.switch_timer);
        node->current_baud = node->baud_transaction.pending_baud;
        node->baud_transaction.pending_baud = 0u;
        al_node_timer_cancel(&node->baud_transaction.switch_timer);
        node->baud = AL_BAUD_IDLE;
        action.kind = AL_ACTION_STATE_ONLY;
        action.effect = AL_EFFECT_SET_BAUD;
        action.effect_due_ms = due_ms;
        action.effect_value = node->current_baud;
        action.purpose = AL_PURPOSE_BAUD_SWITCH;
        AL_ASSERT_NODE(node);
        return action;
    }
    if (node->power == AL_POWER_SLEEP_ARMED &&
        al_node_timer_expired(&node->sleep_timer, now_ms)) {
        uint32_t due_ms = al_node_timer_due_tick(&node->sleep_timer);
        node->power = AL_POWER_SLEEPING;
        al_node_timer_cancel(&node->sleep_timer);
        action.kind = AL_ACTION_STATE_ONLY;
        action.effect = AL_EFFECT_ENTER_SLEEP;
        action.effect_due_ms = due_ms;
        action.effect_value = node->sleep_duration_ms;
    }
    AL_ASSERT_NODE(node);
    return action;
}
