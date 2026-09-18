#include "relay.h"
#include "main.h"   /* RELAY_LIGHT_*, RELAY_FAN_* pin macros from CubeMX */
#include <stdio.h>
#include <stdbool.h>

#define FAN_DELAY_MS 60000UL /* 1 minute fan run-time post light-off */

static bool     fanPendingOff   = false;
static uint32_t fanOffStartTime = 0;

void light_on(void) {
    HAL_GPIO_WritePin(RELAY_LIGHT_GPIO_Port, RELAY_LIGHT_Pin, GPIO_PIN_RESET); /* Active-LOW */
}

void light_off(void) {
    HAL_GPIO_WritePin(RELAY_LIGHT_GPIO_Port, RELAY_LIGHT_Pin, GPIO_PIN_SET);   /* Active-LOW */
}

void fan_on(void) {
    HAL_GPIO_WritePin(RELAY_FAN_GPIO_Port, RELAY_FAN_Pin, GPIO_PIN_RESET);    /* Active-LOW */
    fanPendingOff = false;
}

void fan_off(void) {
    HAL_GPIO_WritePin(RELAY_FAN_GPIO_Port, RELAY_FAN_Pin, GPIO_PIN_SET);      /* Active-LOW */
    fanPendingOff = false;
}

void relay_schedule_fan_off(uint32_t now) {
    fanPendingOff   = true;
    fanOffStartTime = now;
}

void relay_update(uint32_t now) {
    if (fanPendingOff && (now - fanOffStartTime >= FAN_DELAY_MS)) {
        fan_off();
        printf("FAN OFF\r\n");
    }
}