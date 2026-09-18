#ifndef ALIGNMENT_H
#define ALIGNMENT_H

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

/* Call once at startup, after MX_TIM3_Init(). Stores the timer handle and
 * wires up the two axes' GPIO pins internally. */
void alignment_init(TIM_HandleTypeDef *htim);

/* Call once per main-loop iteration. Runs the CAL/GO/RET/RECOVERY state
 * machine. */
void alignment_update(uint32_t now);

/* Commands - caller (commands.c) is responsible for the "is this allowed
 * right now" checks using the query functions below. */
void alignment_start_cal(void);
void alignment_go(void);
void alignment_return(void);
void alignment_reset(void);  /* RST: stop, zero, de-calibrate */

/* Query functions - this is the ONLY window other modules get into
 * alignment's state. Nobody outside this file touches the state enum. */
bool alignment_is_idle(void);
bool alignment_is_returned(void);
bool alignment_is_idle_or_returned(void); /* gate used by hatch OPEN/CLOSE */
bool alignment_is_hold(void);
bool alignment_is_calibrated(void);

/* Called by system_fault(): stop both axes and disable the drivers,
 * without printing anything (system.c owns the FAULT message). */
void alignment_emergency_stop(void);

/* Note: HAL_TIM_PeriodElapsedCallback() for TIM3 is defined inside
 * alignment.c itself (this module owns the timer and both axes), so
 * there is nothing to wire up here or in main.c beyond alignment_init(). */

#endif /* ALIGNMENT_H */
