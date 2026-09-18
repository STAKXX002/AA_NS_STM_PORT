#ifndef COMMANDS_H
#define COMMANDS_H

#include "main.h"
#include <stdint.h>

/* Call once at startup, after MX_USART2_UART_Init(). Stores the UART
 * handle and arms the first receive-interrupt. */
void commands_init(UART_HandleTypeDef *huart);

/* Call once per main-loop iteration. If a full line has been received,
 * looks it up in the command table and dispatches it. */
void commands_update(uint32_t now);

#endif /* COMMANDS_H */