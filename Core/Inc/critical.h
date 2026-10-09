#ifndef CRITICAL_H
#define CRITICAL_H

#include "main.h"
#include <stdint.h>

/* Nestable critical section: saves PRIMASK on entry and restores it on
 * exit, so a helper never re-enables interrupts that its caller (or an
 * ISR) had already disabled. Use ONE pair per scope:
 *
 *     CRITICAL_ENTER();
 *     ... touch state shared with the ISR ...
 *     CRITICAL_EXIT();
 */
#define CRITICAL_ENTER()  uint32_t _primask_saved = __get_PRIMASK(); __disable_irq()
#define CRITICAL_EXIT()   __set_PRIMASK(_primask_saved)

#endif /* CRITICAL_H */
