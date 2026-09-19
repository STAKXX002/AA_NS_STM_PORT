#include "hatch.h"
#include "main.h"   /* GRIP_IN1..4 pin macros from CubeMX */
#include <stdio.h>

#define OPEN_DURATION_MS   20000UL /* full stroke */
#define CLOSE_DURATION_MS  20000UL /* full stroke */
#define HATCH_REFRESH_MS     300UL /* re-assert drive signal this often during a stroke */
#define DEADTIME_MS           100UL /* break-before-make: H-bridge fully off this long
                                      * before driving the opposite direction, to avoid
                                      * shoot-through/back-EMF on direction reversal */

typedef enum { HATCH_IDLE, HATCH_DEADTIME, HATCH_OPENING, HATCH_CLOSING } HatchState;

static HatchState hatchState      = HATCH_IDLE;
static HatchState pendingState    = HATCH_IDLE; /* only meaningful during HATCH_DEADTIME */
static uint32_t   stateStart      = 0;
static uint32_t   lastRefresh     = 0;

static void hatch_forward(void) {
    /* Enable H-bridge to push actuator out to full extension */
    HAL_GPIO_WritePin(GRIP_IN1_GPIO_Port, GRIP_IN1_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GRIP_IN2_GPIO_Port, GRIP_IN2_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GRIP_IN3_GPIO_Port, GRIP_IN3_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GRIP_IN4_GPIO_Port, GRIP_IN4_Pin, GPIO_PIN_RESET);
}

static void hatch_reverse(void) {
    /* Enable H-bridge in reverse polarity to fully retract actuator */
    HAL_GPIO_WritePin(GRIP_IN1_GPIO_Port, GRIP_IN1_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GRIP_IN2_GPIO_Port, GRIP_IN2_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GRIP_IN3_GPIO_Port, GRIP_IN3_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GRIP_IN4_GPIO_Port, GRIP_IN4_Pin, GPIO_PIN_SET);
}

static void hatch_stop_gpio(void) {
    /* Cut H-bridge output completely */
    HAL_GPIO_WritePin(GRIP_IN1_GPIO_Port, GRIP_IN1_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GRIP_IN2_GPIO_Port, GRIP_IN2_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GRIP_IN3_GPIO_Port, GRIP_IN3_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GRIP_IN4_GPIO_Port, GRIP_IN4_Pin, GPIO_PIN_RESET);
}

/* Both hatch_open() and hatch_close() always route through a stop +
 * DEADTIME_MS window before actually driving the new direction, whether
 * the hatch was previously idle, mid-stroke, or just stopped. This is
 * what makes reversal safe without a blocking delay: hatch_update()
 * does the waiting, not the caller. */
static void hatch_start(uint32_t now, HatchState direction) {
    hatch_stop_gpio();
    pendingState = direction;
    stateStart   = now;
    hatchState   = HATCH_DEADTIME;
}

void hatch_open(uint32_t now) {
    hatch_start(now, HATCH_OPENING);
    printf("OPENING\r\n");
}

void hatch_close(uint32_t now) {
    hatch_start(now, HATCH_CLOSING);
    printf("CLOSING\r\n");
}

void hatch_stop_cmd(void) {
    hatch_stop_gpio();
    hatchState = HATCH_IDLE;
    printf("HATCH STOPPED\r\n");
}

void hatch_emergency_stop(void) {
    hatch_stop_gpio();
    hatchState = HATCH_IDLE;
}

bool hatch_is_busy(void) {
    return hatchState != HATCH_IDLE;
}

void hatch_update(uint32_t now) {
    switch (hatchState) {
    case HATCH_DEADTIME:
        if (now - stateStart >= DEADTIME_MS) {
            if (pendingState == HATCH_OPENING) hatch_forward();
            else                               hatch_reverse();
            stateStart  = now; /* stroke timeout counts from actual engagement */
            lastRefresh = now;
            hatchState  = pendingState;
        }
        break;

    case HATCH_OPENING:
        if (now - stateStart > OPEN_DURATION_MS) {
            hatch_stop_gpio();
            hatchState = HATCH_IDLE;
            printf("OPENED\r\n");
        } else if (now - lastRefresh > HATCH_REFRESH_MS) {
            hatch_forward(); /* re-assert in case the driver dropped it */
            lastRefresh = now;
        }
        break;

    case HATCH_CLOSING:
        if (now - stateStart > CLOSE_DURATION_MS) {
            hatch_stop_gpio();
            hatchState = HATCH_IDLE;
            printf("CLOSED\r\n");
        } else if (now - lastRefresh > HATCH_REFRESH_MS) {
            hatch_reverse();
            lastRefresh = now;
        }
        break;

    case HATCH_IDLE:
    default:
        break;
    }
}
