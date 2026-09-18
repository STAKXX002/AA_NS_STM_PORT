#ifndef HATCH_H
#define HATCH_H

#include <stdint.h>
#include <stdbool.h>

/* Commands - caller (commands.c) is responsible for checking that
 * alignment is idle/returned before calling hatch_open/hatch_close. */
void hatch_open(uint32_t now);
void hatch_close(uint32_t now);
void hatch_stop_cmd(void);   /* only affects the hatch's own state */

bool hatch_is_busy(void);    /* true while OPENING or CLOSING */

/* Call once per main-loop iteration. */
void hatch_update(uint32_t now);

/* Called by system_fault(): force the actuator off without printing
 * a hatch-specific status message (system.c prints the FAULT line). */
void hatch_emergency_stop(void);

#endif /* HATCH_H */
