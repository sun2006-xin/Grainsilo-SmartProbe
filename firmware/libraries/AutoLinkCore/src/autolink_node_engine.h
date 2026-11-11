#ifndef AUTOLINK_NODE_ENGINE_H
#define AUTOLINK_NODE_ENGINE_H

#include "autolink_core.h"
#include "autolink_persistence.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t schema_version;
    uint8_t addr;
    uint8_t commission;
    uint8_t administrative;
    uint8_t quarantine;
    uint64_t network_id;
    uint32_t baud;
    uint16_t descriptor_schema_rev;
    uint32_t descriptor_signature;
    uint8_t config_present_mask;
    uint8_t config_len[4];
    uint8_t config_value[4][4];
} al_node_snapshot_t;

typedef struct {
    uint16_t firmware_version;
    uint8_t sensor_type;
    uint8_t capabilities;
    uint8_t descriptor_count;
    gs_chdesc_t descriptors[GS_CODEC_CHANNEL_CAPACITY];
} al_device_info_t;

typedef struct {
    void *context;
    bool (*read_sample)(void *context, gs_sample_t *sample);
    bool (*read_device_info)(void *context, al_device_info_t *info);
} al_node_application_port_t;

typedef struct {
    al_node_t node;
    al_journal_t journal;
    uint8_t config_present_mask;
    uint8_t config_len[4];
    uint8_t config_value[4][4];
    al_node_application_port_t application;
    al_device_info_t schema_info;
    bool schema_ready;
} al_node_engine_t;

/* Stable platform facts projected by NodeEngine.  The engine remains the
   semantic owner; applications must not inspect al_node_engine_t members. */
typedef struct {
    uint32_t active_baud;
} al_node_platform_status_t;

void al_node_engine_init(al_node_engine_t *engine,
                         const uint8_t uid[GS_UID_LEN],
                         al_storage_port_t storage);
bool al_node_engine_restore(al_node_engine_t *engine);
void al_node_engine_set_application_port(al_node_engine_t *engine,
                                         al_node_application_port_t port);
/* False means invalid arguments.  Call after engine initialization/restore. */
bool al_node_engine_platform_status(const al_node_engine_t *engine,
                                    al_node_platform_status_t *status);
/* Authoritative publication gate for an Engine-produced platform effect.
   False means the effect must not be published, including invalid input. */
bool al_node_engine_effect_is_due(const al_node_engine_t *engine,
                                  const al_action_t *action);
bool al_node_engine_sync_schema(al_node_engine_t *engine);
bool al_node_engine_config_get(const al_node_engine_t *engine, uint8_t id,
                               const uint8_t **value, uint8_t *value_len);
al_action_t al_node_engine_execute(al_node_engine_t *engine,
                                    const al_request_t *request);
/* Advance no-request time.  False means a durable transition was rolled back;
   action is cleared and no platform effect may be published. */
bool al_node_engine_advance(al_node_engine_t *engine, uint32_t now_ms,
                            al_action_t *action);
al_action_t al_node_engine_tick(al_node_engine_t *engine, uint32_t now_ms);
bool al_node_engine_on_delivery(al_node_engine_t *engine,
                                const al_action_t *action,
                                al_delivery_outcome_t outcome,
                                uint32_t delivery_end_ms);
bool al_node_engine_enter_quarantine(al_node_engine_t *engine,
                                     al_service_t reason, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
