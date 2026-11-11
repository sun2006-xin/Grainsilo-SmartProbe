#ifndef GRAIN_SILO_RS485_SERVICE_POLICY_H
#define GRAIN_SILO_RS485_SERVICE_POLICY_H

#include <stdbool.h>

/* Adapter-owned state; does not inspect or mutate AutoLink Core internals. */
typedef struct {
    bool response_pending;
} gs_rs485_service_gate_t;

static inline bool gs_rs485_http_service_allowed(
    const gs_rs485_service_gate_t *gate)
{
    return gate != NULL && !gate->response_pending;
}

static inline void gs_rs485_response_started(gs_rs485_service_gate_t *gate)
{
    if (gate != NULL) gate->response_pending = true;
}

static inline void gs_rs485_response_completed(gs_rs485_service_gate_t *gate)
{
    if (gate != NULL) gate->response_pending = false;
}

#endif
