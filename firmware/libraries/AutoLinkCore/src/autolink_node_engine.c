#include "autolink_node_engine.h"
#include "generated/autolink_semantics.h"

#include <string.h>

#define AL_SNAPSHOT_SCHEMA_VERSION 2u

static al_node_snapshot_t snapshot_of(const al_node_engine_t *engine)
{
    al_node_snapshot_t value;
    const al_node_t *node = &engine->node;
    memset(&value, 0, sizeof(value));
    value.schema_version = AL_SNAPSHOT_SCHEMA_VERSION;
    value.addr = node->addr;
    value.commission = (uint8_t)node->commission;
    value.administrative = (uint8_t)node->administrative;
    value.quarantine = (node->service == AL_SERVICE_QUARANTINED_REG ||
                        node->service == AL_SERVICE_QUARANTINED_SERVICE) ?
                       (uint8_t)node->service : (uint8_t)AL_SERVICE_NORMAL;
    value.network_id = node->network_id;
    value.baud = node->baud == AL_BAUD_SWITCH_WAIT ?
                 node->baud_transaction.pending_baud : node->current_baud;
    value.descriptor_schema_rev = node->schema_rev;
    value.descriptor_signature = node->descriptor_signature;
    value.config_present_mask = engine->config_present_mask;
    memcpy(value.config_len, engine->config_len, sizeof(value.config_len));
    memcpy(value.config_value, engine->config_value, sizeof(value.config_value));
    return value;
}

static bool config_snapshot_valid(const al_node_snapshot_t *value)
{
    uint8_t i;
    if ((value->config_present_mask & 0xF0u) != 0u) return false;
    for (i = 0u; i < 4u; ++i) {
        bool present = (value->config_present_mask & (uint8_t)(1u << i)) != 0u;
        if ((present && (value->config_len[i] == 0u ||
                         value->config_len[i] > 4u)) ||
            (!present && value->config_len[i] != 0u)) return false;
    }
    return true;
}

static bool restore_snapshot(al_node_engine_t *engine,
                             const al_node_snapshot_t *value)
{
    al_node_t *node = &engine->node;
    if (value->schema_version != AL_SNAPSHOT_SCHEMA_VERSION ||
        !al_is_supported_baud(value->baud) || !config_snapshot_valid(value) ||
        value->administrative > AL_ADMIN_RETIRED ||
        value->commission > AL_COMMISSION_COMMITTED ||
        (value->quarantine != AL_SERVICE_NORMAL &&
         value->quarantine != AL_SERVICE_QUARANTINED_REG &&
         value->quarantine != AL_SERVICE_QUARANTINED_SERVICE)) return false;

    node->administrative = (al_admin_t)value->administrative;
    node->current_baud = value->baud;
    node->schema_rev = value->descriptor_schema_rev;
    node->descriptor_signature = value->descriptor_signature;
    engine->config_present_mask = value->config_present_mask;
    memcpy(engine->config_len, value->config_len, sizeof(engine->config_len));
    memcpy(engine->config_value, value->config_value,
           sizeof(engine->config_value));

    if (value->commission == AL_COMMISSION_PENDING) {
        node->addr = value->addr;
        node->network_id = value->network_id;
        node->commission = AL_COMMISSION_PENDING;
        node->network = AL_NETWORK_ATTACHED;
        if (!al_node_timer_start(&node->assign_confirm_timer, 0u,
                                 GS_TIMING_ASSIGN_CONFIRM_TIMEOUT_MS))
            return false;
    } else if (value->commission == AL_COMMISSION_COMMITTED) {
        node->addr = value->addr;
        node->network_id = value->network_id;
        node->commission = AL_COMMISSION_COMMITTED;
        node->network = AL_NETWORK_PROBATION;
    } else if (value->commission != AL_COMMISSION_NONE) {
        return false;
    }

    if (value->quarantine != AL_SERVICE_NORMAL) {
        node->service = (al_service_t)value->quarantine;
        if (!al_node_timer_start(&node->quarantine_timer, 0u,
                                 GS_Q_BACKOFF_1_MS)) return false;
    }
    node->power = AL_POWER_AWAKE;
    node->baud = AL_BAUD_IDLE;
    memset(&node->baud_transaction, 0, sizeof(node->baud_transaction));
    memset(&node->effects, 0, sizeof(node->effects));
    return al_node_invariants(node);
}

void al_node_engine_init(al_node_engine_t *engine,
                         const uint8_t uid[GS_UID_LEN],
                         al_storage_port_t storage)
{
    memset(engine, 0, sizeof(*engine));
    al_node_init(&engine->node, uid);
    al_journal_init(&engine->journal, storage);
}

void al_node_engine_set_application_port(al_node_engine_t *engine,
                                         al_node_application_port_t port)
{
    engine->application = port;
}

bool al_node_engine_platform_status(const al_node_engine_t *engine,
                                    al_node_platform_status_t *status)
{
    if (engine == NULL || status == NULL) return false;
    status->active_baud = engine->node.current_baud;
    return true;
}

bool al_node_engine_effect_is_due(const al_node_engine_t *engine,
                                  const al_action_t *action)
{
    if (engine == NULL) return false;
    return al_node_action_effect_is_due(&engine->node, action);
}

static bool device_info_valid(const al_device_info_t *info,
                              uint32_t *signature)
{
    uint8_t encoded[GS_CODEC_CHANNEL_CAPACITY * 8u];
    uint8_t i;
    if (info == NULL || !gs_sensor_type_valid(info->sensor_type) ||
        !gs_capabilities_valid(info->capabilities) ||
        info->descriptor_count > GS_CODEC_CHANNEL_CAPACITY) return false;
    for (i = 0u; i < info->descriptor_count; ++i) {
        uint8_t j;
        if (gs_build_chdesc(encoded + (uint16_t)i * 8u,
                            &info->descriptors[i]) == 0u) return false;
        for (j = 0u; j < i; ++j)
            if (info->descriptors[j].ch_id == info->descriptors[i].ch_id)
                return false;
    }
    if (signature != NULL)
        *signature = gs_murmur3_x86_32(encoded,
            (size_t)info->descriptor_count * 8u, 0x5343484Du);
    return true;
}

static bool stage_schema(al_node_engine_t *engine, bool *changed)
{
    al_device_info_t info;
    uint32_t signature;
    *changed = false;
    if (engine == NULL || engine->application.read_device_info == NULL ||
        !engine->application.read_device_info(engine->application.context,
                                              &info) ||
        !device_info_valid(&info, &signature)) return false;
    if (engine->node.descriptor_signature == signature) {
        engine->schema_info = info;
        engine->schema_ready = true;
        return true;
    }
    engine->node.schema_rev = (uint16_t)(engine->node.schema_rev + 1u);
    if (engine->node.schema_rev == 0u) engine->node.schema_rev = 1u;
    engine->node.descriptor_signature = signature;
    engine->schema_info = info;
    engine->schema_ready = true;
    *changed = true;
    return true;
}

bool al_node_engine_sync_schema(al_node_engine_t *engine)
{
    al_node_t before_node;
    al_device_info_t before_info;
    bool before_ready;
    bool changed;
    al_node_snapshot_t value;
    if (engine == NULL) return false;
    before_node = engine->node;
    before_info = engine->schema_info;
    before_ready = engine->schema_ready;
    if (!stage_schema(engine, &changed)) return false;
    if (!changed) return true;
    value = snapshot_of(engine);
    if (al_journal_commit(&engine->journal, &value, sizeof(value))) return true;
    engine->node = before_node;
    engine->schema_info = before_info;
    engine->schema_ready = before_ready;
    return false;
}

static bool sample_matches_info(const gs_sample_t *sample,
                                const al_device_info_t *info)
{
    uint8_t i;
    if (sample->sensor_type != info->sensor_type ||
        sample->channel_count != info->descriptor_count) return false;
    for (i = 0u; i < sample->channel_count; ++i) {
        uint8_t j;
        bool matched = false;
        for (j = 0u; j < info->descriptor_count; ++j) {
            if (sample->channels[i].id == info->descriptors[j].ch_id) {
                matched = sample->channels[i].value_len ==
                          al_data_type_width(info->descriptors[j].data_type);
                break;
            }
        }
        if (!matched) return false;
    }
    return true;
}

bool al_node_engine_config_get(const al_node_engine_t *engine, uint8_t id,
                               const uint8_t **value, uint8_t *value_len)
{
    uint8_t index;
    if (engine == NULL || value == NULL || value_len == NULL ||
        id < 1u || id > 4u) return false;
    index = (uint8_t)(id - 1u);
    if ((engine->config_present_mask & (uint8_t)(1u << index)) == 0u)
        return false;
    *value = engine->config_value[index];
    *value_len = engine->config_len[index];
    return true;
}

static bool decorate_response(al_node_engine_t *engine,
                              const al_request_t *request,
                              al_action_t *action)
{
    bool schema_changed;
    if (action->kind == AL_ACTION_SCHEDULED_RESPONSE) {
        action->response_data[0] = request->func == GS_FUNC_DISCOVER ?
                                   GS_REPORT_REGISTER :
                                   (request->func == GS_FUNC_RECONCILE ?
                                    GS_REPORT_RECONCILE : GS_REPORT_RECOVER);
        memcpy(action->response_data + 1u, engine->node.uid, GS_UID_LEN);
        action->response_len = 7u;
        if (request->func != GS_FUNC_DISCOVER)
            action->response_data[action->response_len++] = engine->node.addr;
        if (request->func == GS_FUNC_RECOVER) {
            uint64_t net = engine->node.network_id;
            int i;
            for (i = 7; i >= 0; --i) {
                action->response_data[action->response_len + (uint8_t)i] =
                    (uint8_t)net;
                net >>= 8;
            }
            action->response_len = (uint8_t)(action->response_len + 8u);
        }
        return true;
    }
    if (action->kind != AL_ACTION_IMMEDIATE_RESPONSE) return true;
    if (request->func == GS_FUNC_READ_SAMPLE) {
        gs_sample_t sample;
        if (!stage_schema(engine, &schema_changed) ||
            engine->application.read_sample == NULL ||
            !engine->application.read_sample(engine->application.context,
                                              &sample) ||
            !sample_matches_info(&sample, &engine->schema_info)) return false;
        if (schema_changed)
            action->plan.durable_ops |= (uint16_t)AL_DURABLE_SCHEMA;
        sample.schema_rev = engine->node.schema_rev;
        action->response_len = gs_build_sample_v22(action->response_data,
                                                   &sample);
        return action->response_len != 0u;
    }
    if (request->func == GS_FUNC_READ_INFO) {
        const al_device_info_t *info;
        uint8_t i;
        uint8_t pos;
        if (!stage_schema(engine, &schema_changed)) return false;
        if (schema_changed)
            action->plan.durable_ops |= (uint16_t)AL_DURABLE_SCHEMA;
        info = &engine->schema_info;
        if (request->data[0] == GS_INFO_SUB_BASE) {
            memcpy(action->response_data, engine->node.uid, GS_UID_LEN);
            gs_put_u16be(action->response_data + 6u, info->firmware_version);
            action->response_data[8] = info->sensor_type;
            action->response_data[9] = engine->node.addr;
            action->response_data[10] = info->capabilities;
            gs_put_u16be(action->response_data + 11u, GS_PROTO_VER);
            gs_put_u16be(action->response_data + 13u, engine->node.schema_rev);
            action->response_len = 15u;
            return true;
        }
        if (info->descriptor_count > GS_CODEC_CHANNEL_CAPACITY) return false;
        action->response_data[0] = info->sensor_type;
        action->response_data[1] = info->descriptor_count;
        gs_put_u16be(action->response_data + 2u, engine->node.schema_rev);
        pos = 4u;
        for (i = 0u; i < info->descriptor_count; ++i) {
            uint8_t length = gs_build_chdesc(action->response_data + pos,
                                             &info->descriptors[i]);
            if (length == 0u || pos + length > GS_DATA_MAX_LEN) return false;
            pos = (uint8_t)(pos + length);
        }
        action->response_len = pos;
    } else if (request->func == GS_FUNC_DIAG_LOOP) {
        memcpy(action->response_data, request->data, request->len);
        action->response_len = request->len;
    } else if (request->func == GS_FUNC_READ_ADDR) {
        action->response_data[0] = engine->node.addr;
        action->response_len = 1u;
    }
    return true;
}

bool al_node_engine_restore(al_node_engine_t *engine)
{
    al_node_snapshot_t value;
    size_t length = 0u;
    if (!al_journal_load(&engine->journal, &value, sizeof(value), &length) ||
        length != sizeof(value) || !restore_snapshot(engine, &value)) {
        uint8_t uid[GS_UID_LEN];
        al_storage_port_t storage = engine->journal.storage;
        memcpy(uid, engine->node.uid, sizeof(uid));
        al_node_engine_init(engine, uid, storage);
        return false;
    }
    return true;
}

static al_action_t persist_failure(const al_request_t *request,
                                   const al_node_t *node,
                                   const al_action_t *prior)
{
    al_action_t failed;
    memset(&failed, 0, sizeof(failed));
    if (request->func == GS_FUNC_WRITE_CFG &&
        (prior->plan.durable_ops & AL_DURABLE_CONFIG) != 0u) {
        failed.cacheable_completion = prior->cacheable_completion;
        failed.kind = AL_ACTION_IMMEDIATE_RESPONSE;
        failed.response_func = GS_FUNC_WRITE_CFG;
        failed.response_addr = node->addr;
        failed.response_len = 2u;
        failed.response_data[0] = request->data[0];
        failed.response_data[1] = 3u;
        failed.purpose = AL_PURPOSE_NORMAL_REPLY;
    } else if ((request->func == GS_FUNC_ASSIGN_ADDR ||
                request->func == GS_FUNC_READDRESS) &&
               (prior->plan.durable_ops &
                AL_DURABLE_IDENTITY_PENDING) != 0u) {
        failed.cacheable_completion = prior->cacheable_completion;
        failed.kind = AL_ACTION_ERROR;
        failed.wire_error = GS_WIRE_ERR_PERSIST_FAILED;
        failed.response_func = (uint8_t)(GS_FUNC_ERR_BASE | request->func);
        failed.response_addr = request->func == GS_FUNC_ASSIGN_ADDR ?
                               GS_ADDR_UNASSIGNED : node->addr;
        failed.response_len = 1u;
        failed.response_data[0] = GS_WIRE_ERR_PERSIST_FAILED;
        failed.purpose = AL_PURPOSE_NORMAL_REPLY;
    }
    return failed;
}

static void stage_config(al_node_engine_t *engine, const al_request_t *request)
{
    uint8_t index = (uint8_t)(request->data[0] - 1u);
    uint8_t length = (uint8_t)(request->len - 1u);
    engine->config_present_mask |= (uint8_t)(1u << index);
    engine->config_len[index] = length;
    memset(engine->config_value[index], 0, sizeof(engine->config_value[index]));
    memcpy(engine->config_value[index], request->data + 1u, length);
}

al_action_t al_node_engine_execute(al_node_engine_t *engine,
                                   const al_request_t *request)
{
    al_node_t before_node = engine->node;
    uint8_t before_present = engine->config_present_mask;
    uint8_t before_len[4];
    uint8_t before_value[4][4];
    al_device_info_t before_schema_info = engine->schema_info;
    bool before_schema_ready = engine->schema_ready;
    al_action_t action;
    memcpy(before_len, engine->config_len, sizeof(before_len));
    memcpy(before_value, engine->config_value, sizeof(before_value));

    action = al_node_prepare(&engine->node, request);
    if ((action.plan.durable_ops & AL_DURABLE_CONFIG) != 0u)
        stage_config(engine, request);
    if (!decorate_response(engine, request, &action)) {
        memset(&action, 0, sizeof(action));
        engine->node = before_node;
        engine->config_present_mask = before_present;
        memcpy(engine->config_len, before_len, sizeof(before_len));
        memcpy(engine->config_value, before_value, sizeof(before_value));
        engine->schema_info = before_schema_info;
        engine->schema_ready = before_schema_ready;
        action.kind = AL_ACTION_ERROR;
        action.wire_error = GS_WIRE_ERR_SENSOR;
        action.response_func = (uint8_t)(GS_FUNC_ERR_BASE | request->func);
        action.response_addr = engine->node.addr;
        action.response_len = 1u;
        action.response_data[0] = GS_WIRE_ERR_SENSOR;
        return action;
    }
    if (action.plan.durable_ops != 0u) {
        al_node_snapshot_t value = snapshot_of(engine);
        if (!al_journal_commit(&engine->journal, &value, sizeof(value))) {
            al_action_t failed;
            engine->node = before_node;
            engine->config_present_mask = before_present;
            memcpy(engine->config_len, before_len, sizeof(before_len));
            memcpy(engine->config_value, before_value, sizeof(before_value));
            engine->schema_info = before_schema_info;
            engine->schema_ready = before_schema_ready;
            failed = persist_failure(request, &engine->node, &action);
            al_node_complete(&engine->node, request, &failed);
            return failed;
        }
        action.plan.durable_ops = 0u;
    }
    al_node_complete(&engine->node, request, &action);
    return action;
}

bool al_node_engine_advance(al_node_engine_t *engine, uint32_t now_ms,
                            al_action_t *action)
{
    al_node_t before;
    if (engine == NULL || action == NULL) return false;
    before = engine->node;
    *action = al_node_tick(&engine->node, now_ms);
    if (action->plan.durable_ops != 0u) {
        al_node_snapshot_t value = snapshot_of(engine);
        if (!al_journal_commit(&engine->journal, &value, sizeof(value))) {
            engine->node = before;
            memset(action, 0, sizeof(*action));
            return false;
        }
        action->plan.durable_ops = 0u;
    }
    return true;
}

al_action_t al_node_engine_tick(al_node_engine_t *engine, uint32_t now_ms)
{
    al_action_t action;
    memset(&action, 0, sizeof(action));
    (void)al_node_engine_advance(engine, now_ms, &action);
    return action;
}

static bool commit_lifecycle_transition(al_node_engine_t *engine,
                                        const al_node_t *before,
                                        al_action_t *transition)
{
    if (transition->plan.durable_ops != 0u) {
        al_node_snapshot_t value = snapshot_of(engine);
        if (!al_journal_commit(&engine->journal, &value, sizeof(value))) {
            engine->node = *before;
            return false;
        }
        transition->plan.durable_ops = 0u;
    }
    return true;
}

bool al_node_engine_on_delivery(al_node_engine_t *engine,
                                const al_action_t *action,
                                al_delivery_outcome_t outcome,
                                uint32_t delivery_end_ms)
{
    al_node_t before;
    al_action_t transition;
    if (engine == NULL || action == NULL ||
        outcome > AL_DELIVERY_FAILED) return false;
    before = engine->node;
    transition = al_node_on_delivery_at(&engine->node, action, outcome,
                                        delivery_end_ms);
    return transition.kind != AL_ACTION_IGNORE &&
           commit_lifecycle_transition(engine, &before, &transition);
}

bool al_node_engine_enter_quarantine(al_node_engine_t *engine,
                                     al_service_t reason, uint32_t now_ms)
{
    al_node_t before;
    al_action_t transition;
    if (engine == NULL ||
        (reason != AL_SERVICE_QUARANTINED_REG &&
         reason != AL_SERVICE_QUARANTINED_SERVICE)) return false;
    before = engine->node;
    memset(&transition, 0, sizeof(transition));
    transition.kind = AL_ACTION_STATE_ONLY;
    al_enter_quarantine_at(&engine->node, reason, now_ms);
    if (engine->node.service != reason) return false;
    transition.plan.durable_ops |= (uint16_t)AL_DURABLE_QUARANTINE;
    return commit_lifecycle_transition(engine, &before, &transition);
}
