#ifndef SYSTEM_H
#define SYSTEM_H

#include <stdbool.h>

/* The ONLY function allowed to reach into alignment, hatch, AND relay at
 * once. Any module can call this when it detects an unrecoverable
 * condition (timeout, limit-switch anomaly, skew out of range, ...). */
void system_fault(const char *msg);

bool system_in_fault(void);
void system_clear_fault(void); /* called by the RST command */

/* Drive every output (motor-driver enables, hatch H-bridge, relays) to its
 * safe level using raw GPIO writes only: no module state, no printf, no
 * interrupts needed. Safe to call from Error_Handler()/fault paths. */
void system_force_safe_outputs(void);

#endif /* SYSTEM_H */
