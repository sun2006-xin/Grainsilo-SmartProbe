#ifndef AUTOLINK_TIME_H
#define AUTOLINK_TIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Node adapters expose a wrapping 32-bit millisecond tick.  Runtime intervals
 * are deliberately bounded to the unambiguous half-epoch domain; every
 * generated AutoLink Node interval is checked against this bound in Core. */
#define AL_NODE_TIMER_MAX_INTERVAL_MS UINT32_C(2147483647)

typedef struct {
    bool active;
    uint32_t start_ms;
    uint32_t interval_ms;
} al_node_timer_t;

static inline bool al_node_timer_valid(const al_node_timer_t *timer)
{
    return timer != NULL &&
           ((!timer->active && timer->start_ms == 0u &&
             timer->interval_ms == 0u) ||
            (timer->active && timer->interval_ms != 0u &&
             timer->interval_ms <= AL_NODE_TIMER_MAX_INTERVAL_MS));
}

static inline bool al_node_timer_start(al_node_timer_t *timer,
                                       uint32_t start_ms,
                                       uint32_t interval_ms)
{
    if (timer == NULL || interval_ms == 0u ||
        interval_ms > AL_NODE_TIMER_MAX_INTERVAL_MS) return false;
    timer->active = true;
    timer->start_ms = start_ms;
    timer->interval_ms = interval_ms;
    return true;
}

static inline void al_node_timer_cancel(al_node_timer_t *timer)
{
    if (timer == NULL) return;
    timer->active = false;
    timer->start_ms = 0u;
    timer->interval_ms = 0u;
}

static inline uint32_t al_node_time_elapsed_ms(uint32_t start_ms,
                                               uint32_t now_ms)
{
    return (uint32_t)(now_ms - start_ms);
}

static inline bool al_node_timer_expired(const al_node_timer_t *timer,
                                         uint32_t now_ms)
{
    return timer != NULL && timer->active &&
           al_node_time_elapsed_ms(timer->start_ms, now_ms) >=
               timer->interval_ms;
}

/* A wrapped numeric deadline is an observable projection only.  It never
 * carries activity state and is never used to order Node time. */
static inline uint32_t al_node_timer_due_tick(const al_node_timer_t *timer)
{
    return timer == NULL ? 0u :
           (uint32_t)(timer->start_ms + timer->interval_ms);
}

static inline uint32_t al_node_time_projection_after(uint32_t start_ms,
                                                     uint32_t interval_ms)
{
    return (uint32_t)(start_ms + interval_ms);
}

/* The production Master adapter supplies esp_timer_get_time(), so Master Core
 * consumes a 64-bit monotonic millisecond epoch directly.  Absolute runtime
 * epochs are transient and must never be serialized. */
typedef uint64_t al_master_time_ms_t;

static inline al_master_time_ms_t al_master_time_after(
    al_master_time_ms_t start_ms, uint64_t interval_ms)
{
    return start_ms + interval_ms;
}

static inline bool al_master_time_reached(al_master_time_ms_t now_ms,
                                          al_master_time_ms_t deadline_ms)
{
    return now_ms >= deadline_ms;
}

#ifdef __cplusplus
}
#endif
#endif
