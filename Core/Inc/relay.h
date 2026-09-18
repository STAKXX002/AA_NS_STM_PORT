#ifndef RELAY_H
#define RELAY_H

#include <stdint.h>

/* Immediate actions */
void light_on(void);
void light_off(void);
void fan_on(void);
void fan_off(void);

/* Call once per OFF command: light_off() already happened, this arms the
 * delayed fan shutdown. relay_update() is what actually turns the fan off
 * once FAN_DELAY_MS has elapsed. */
void relay_schedule_fan_off(uint32_t now);

/* Call once per main-loop iteration. Handles the delayed fan-off timer. */
void relay_update(uint32_t now);

#endif /* RELAY_H */