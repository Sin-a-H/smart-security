/*
 * sensor_thread.h
 * -----------------------------------------------------------------------
 * Background thread that samples CPU temp / memory / CPU load every
 * 2 seconds and writes results into g_state. Also implements adaptive thermal
 * management: if temperature crosses a threshold it lowers
 * g_state.target_fps / target_width / target_height and fires an
 * email notification (via email_notify_thermal_event()).
 */

#ifndef SENSOR_THREAD_H
#define SENSOR_THREAD_H

#include "board_state.h"

/* Thermal throttle threshold in Celsius. */
#define THERMAL_THROTTLE_ON_C   70.0
#define THERMAL_THROTTLE_OFF_C  60.0   /* hysteresis: must cool below this to un-throttle */

/* Starts the sampling thread (detached). Returns 0 on success. */
int sensor_thread_start(board_state_t *st);

#endif /* SENSOR_THREAD_H */
