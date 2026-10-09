#include "system.h"
#include "alignment.h"
#include "hatch.h"
#include "relay.h"
#include "main.h"
#include <stdio.h>
#include <stdbool.h>

static bool faulted = false;

void system_fault(const char *msg) {
    alignment_emergency_stop();
    hatch_emergency_stop();
    light_off();
    fan_off(); /* total fault means everything off, not just the light -
                * a fault is not a state where anything should keep running
                * unsupervised, including the fan */
    faulted = true;
    printf("FAULT: %s\r\n", msg);
}

bool system_in_fault(void) {
    return faulted;
}

void system_clear_fault(void) {
    faulted = false;
}

void system_force_safe_outputs(void) {
    /* Motor drivers: EN is active-LOW, so SET = disabled */
    HAL_GPIO_WritePin(Z1_EN_GPIO_Port, Z1_EN_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(Z2_EN_GPIO_Port, Z2_EN_Pin, GPIO_PIN_SET);
    /* Hatch H-bridge: all inputs low = outputs off */
    HAL_GPIO_WritePin(GRIP_IN1_GPIO_Port, GRIP_IN1_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GRIP_IN2_GPIO_Port, GRIP_IN2_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GRIP_IN3_GPIO_Port, GRIP_IN3_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GRIP_IN4_GPIO_Port, GRIP_IN4_Pin, GPIO_PIN_RESET);
    /* Relays: active-LOW, so SET = off */
    HAL_GPIO_WritePin(RELAY_LIGHT_GPIO_Port, RELAY_LIGHT_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(RELAY_FAN_GPIO_Port, RELAY_FAN_Pin, GPIO_PIN_SET);
}
