#include "commands.h"
#include "alignment.h"
#include "hatch.h"
#include "relay.h"
#include "system.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

static UART_HandleTypeDef *uart = NULL;

static volatile char rx_buffer[32];
static uint8_t        rx_char;
static uint8_t        rx_idx = 0;
static volatile bool  cmd_ready = false;

/* ---- individual command handlers ---- */

static void cmd_cal(uint32_t now) {
    (void)now;
    if (alignment_is_idle()) alignment_start_cal();
    else printf("BUSY\r\n");
}

static void cmd_go(uint32_t now) {
    (void)now;
    if (!alignment_is_calibrated()) printf("NO CAL\r\n");
    else if (alignment_is_idle() || alignment_is_returned()) alignment_go();
    else printf("BUSY\r\n");
}

static void cmd_ret(uint32_t now) {
    (void)now;
    if (!alignment_is_calibrated()) printf("NO CAL\r\n");
    else if (alignment_is_hold()) alignment_return();
    else printf("INVALID\r\n");
}

static void cmd_rst(uint32_t now) {
    (void)now;
    alignment_reset();
    system_clear_fault();
    printf("RST\r\nNO CAL\r\n");
}

static void cmd_open(uint32_t now) {
    if (hatch_is_busy()) printf("BUSY\r\n");
    else if (alignment_is_idle() || alignment_is_returned()) hatch_open(now);
    else printf("BUSY\r\n");
}

static void cmd_close(uint32_t now) {
    if (hatch_is_busy()) printf("BUSY\r\n");
    else if (alignment_is_idle() || alignment_is_returned()) hatch_close(now);
    else printf("BUSY\r\n");
}

static void cmd_stop(uint32_t now) {
    (void)now;
    hatch_stop_cmd(); /* only affects the hatch's own state - see note below */
}

static void cmd_on(uint32_t now) {
    (void)now;
    light_on();
    fan_on();
    printf("LIGHT & FAN ON\r\n");
}

static void cmd_off(uint32_t now) {
    light_off();
    relay_schedule_fan_off(now);
    printf("LIGHT OFF, FAN TIMER STARTED\r\n");
}

typedef void (*CommandFn)(uint32_t now);
typedef struct { const char *name; CommandFn fn; } Command;

static const Command commandTable[] = {
    { "CAL",   cmd_cal   },
    { "GO",    cmd_go    },
    { "RET",   cmd_ret   },
    { "RST",   cmd_rst   },
    { "OPEN",  cmd_open  },
    { "CLOSE", cmd_close },
    { "STOP",  cmd_stop  },
    { "ON",    cmd_on    },
    { "OFF",   cmd_off   },
};
#define NUM_COMMANDS (sizeof(commandTable) / sizeof(commandTable[0]))

/* ---- public API ---- */

void commands_init(UART_HandleTypeDef *huart) {
    uart = huart;
    HAL_UART_Receive_IT(uart, &rx_char, 1);
}

void commands_update(uint32_t now) {
    if (!cmd_ready) return;

    /* Snapshot under a critical section before clearing cmd_ready - once
     * cmd_ready goes false, the RX ISR is free to start overwriting
     * rx_buffer for the next line, and without this copy that could
     * happen mid-strcmp() against the buffer we're still reading. */
    char local_buf[sizeof(rx_buffer)];
    __disable_irq();
    memcpy(local_buf, (const void*)rx_buffer, sizeof(local_buf));
    cmd_ready = false;
    __enable_irq();

    for (unsigned i = 0; i < NUM_COMMANDS; i++) {
        if (strcmp(local_buf, commandTable[i].name) == 0) {
            commandTable[i].fn(now);
            return;
        }
    }
    printf("UNKNOWN\r\n");
}

/* HAL weak-callback override - without this, a real-world framing/noise/
 * overrun error on the wire aborts HAL's UART receive state and the port
 * goes permanently silent, since nothing re-arms HAL_UART_Receive_IT().
 * Renode's virtual UART never raises these, so this only shows up on
 * real wiring - easy to miss without deliberately looking for it. */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
    if (uart != NULL && huart->Instance == uart->Instance) {
        __HAL_UART_CLEAR_OREFLAG(huart);
        __HAL_UART_CLEAR_NEFLAG(huart);
        __HAL_UART_CLEAR_FEFLAG(huart);
        __HAL_UART_CLEAR_PEFLAG(huart);
        rx_idx = 0; /* discard whatever partial line was in progress */
        HAL_UART_Receive_IT(uart, &rx_char, 1);
    }
}

/* HAL weak-callback override - this is the only file that touches the
 * UART RX line-buffer, so rx_buffer/rx_idx/cmd_ready above can stay
 * file-scope statics instead of project-wide globals. */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
    if (huart->Instance == uart->Instance) {
        if (rx_char == '\r') {
            /* ignore CR so \r\n acts as a single line terminator */
        } else if (rx_char == '\n') {
            if (rx_idx > 0 && !cmd_ready) {
                rx_buffer[rx_idx] = '\0';
                cmd_ready = true;
                rx_idx = 0;
            }
        } else {
            if (rx_idx < sizeof(rx_buffer) - 1 && !cmd_ready) {
                rx_buffer[rx_idx++] = rx_char;
            }
        }
        HAL_UART_Receive_IT(uart, &rx_char, 1);
    }
}
