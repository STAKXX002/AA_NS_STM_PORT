#ifndef RELAY_H
#define RELAY_H

#include <stdint.h>

/* Call once at startup, after MX_GPIO_Init(). Forces light + fan off
 * regardless of whatever level CubeMX's GPIO init left the pins at -
 * don't rely on the .ioc's "GPIO output level" setting alone. */
void relay_init(void);

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