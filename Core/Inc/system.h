#ifndef SYSTEM_H
#define SYSTEM_H

#include <stdbool.h>

/* The ONLY function allowed to reach into alignment, hatch, AND relay at
 * once. Any module can call this when it detects an unrecoverable
 * condition (timeout, limit-switch anomaly, skew out of range, ...). */
void system_fault(const char *msg);

bool system_in_fault(void);
void system_clear_fault(void); /* called by the RST command */

#endif /* SYSTEM_H */
