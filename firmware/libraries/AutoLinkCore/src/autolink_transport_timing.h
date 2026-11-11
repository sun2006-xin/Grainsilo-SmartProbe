#ifndef AUTOLINK_TRANSPORT_TIMING_H
#define AUTOLINK_TRANSPORT_TIMING_H

#include <stdbool.h>
#include <stdint.h>

typedef uint64_t al_time_us_t;

typedef enum {
    AL_BUS_EVENT_RX_CRC_STOP,
    AL_BUS_EVENT_TX_VER_START,
    AL_BUS_EVENT_TX_CRC_STOP
} al_bus_event_t;

/* Platform evidence, not an arbitrary software timestamp.  A port sets
 * verified=false when it cannot bound the observation to physical A/B. */
typedef struct {
    al_bus_event_t event;
    al_time_us_t at_us;
    uint32_t uncertainty_us;
    bool verified;
} al_transport_timestamp_t;

static inline al_transport_timestamp_t al_transport_timestamp(
    al_bus_event_t event, al_time_us_t at_us, uint32_t uncertainty_us,
    bool verified)
{
    al_transport_timestamp_t value;
    value.event = event;
    value.at_us = at_us;
    value.uncertainty_us = uncertainty_us;
    value.verified = verified;
    return value;
}

static inline bool al_transport_meets_bound(
    const al_transport_timestamp_t *timestamp, uint32_t bound_us)
{
    return timestamp != NULL && timestamp->verified &&
           timestamp->uncertainty_us <= bound_us;
}

static inline bool al_transport_target_due(al_time_us_t now_us,
                                           al_time_us_t target_us)
{
    return now_us >= target_us;
}

static inline bool al_transport_pair_meets_relative_bound(
    const al_transport_timestamp_t *first,
    const al_transport_timestamp_t *second, uint32_t bound_us)
{
    return first != NULL && second != NULL && first->verified &&
           second->verified && first->uncertainty_us <= bound_us &&
           second->uncertainty_us <= bound_us - first->uncertainty_us;
}

#endif
