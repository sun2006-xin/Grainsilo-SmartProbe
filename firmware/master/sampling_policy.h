#ifndef GRAIN_SILO_SAMPLING_POLICY_H
#define GRAIN_SILO_SAMPLING_POLICY_H

#include <stdint.h>

/* AutoLink v2.2.0 Core default: one addressed work poll per second. */
#define GS_AUTOLINK_DEFAULT_WORK_POLL_INTERVAL_MS UINT32_C(1000)

/* Nominal round-robin cycle only; retries and other Core work can extend it. */
#define GS_AUTOLINK_NOMINAL_NODE_CYCLE_MS(node_count) \
    ((uint32_t)(node_count) * GS_AUTOLINK_DEFAULT_WORK_POLL_INTERVAL_MS)

#endif
