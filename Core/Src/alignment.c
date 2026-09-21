#include "alignment.h"
#include "system.h"   /* system_fault() coordinator */
#include <stdio.h>
#include <stdlib.h>

#define STEPS_PER_MM    200L
#define GO_STEPS        (-40L * STEPS_PER_MM)
#define RECOVERY_MM     5.0f
#define RECOVERY_STEPS  ((long)(RECOVERY_MM * STEPS_PER_MM))
#define MAX_SKEW_STEPS  (5L * STEPS_PER_MM) /* 1000L steps = 5.0 mm */
#define CAL_TRAVEL      1000000L

#define CAL_DIR         1
#define BACKOFF_DIR    -1

#define CAL_TIMEOUT       30000UL
#define RECOVERY_TIMEOUT  15000UL
#define MOVE_TIMEOUT      60000UL

#define STEP_INTERVAL_START   60L    /* slow start: 10kHz/60 ~= 167 Hz */
#define RAMP_TICKS            5000L  /* ramp duration: 5000 * 100us = 500ms */

typedef struct {
    long current_pos;
    long target_pos;
    long step_accumulator;
    long step_interval;         /* CRUISE (minimum) interval */
    long step_interval_current; /* current ramp position */
    uint32_t move_start_tick;   /* isrTicks value when this move began */
    GPIO_TypeDef* step_port; uint16_t step_pin;
    GPIO_TypeDef* dir_port;  uint16_t dir_pin;
} StepperAxis;

typedef enum {
    ALIGN_IDLE, CALIBRATING, CAL_STOPPING, CAL_BACKOFF,
    GOING, HOLD, RETURNING, RETURNED,
    RECOVERY, REC_STOPPING, REC_BACKOFF
} AlignState;

#define AXIS_Z1 0
#define AXIS_Z2 1

static volatile StepperAxis axes[2];
static volatile AlignState  state = ALIGN_IDLE;
static volatile uint32_t    isrTicks = 0; /* free-running, 100us per tick */

/* set inside the ISR when a step pulse needs to be cleared on the *next*
 * tick, so every step gets a full ~100us HIGH pulse instead of a
 * few-nanosecond one - this is what was causing intermittent missed
 * steps that only showed up after unrelated code changed the timing. */
static volatile bool stepPending[2] = { false, false };

static TIM_HandleTypeDef *tim = NULL;

static bool     calibrated = false;
static bool     hit[2]     = { false, false };
static long     hitPos[2]  = { 0, 0 };
static uint32_t stateStart = 0;

static void enable_motors(void) {
    HAL_GPIO_WritePin(Z1_EN_GPIO_Port, Z1_EN_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(Z2_EN_GPIO_Port, Z2_EN_Pin, GPIO_PIN_RESET);
}

static void disable_motors(void) {
    HAL_GPIO_WritePin(Z1_EN_GPIO_Port, Z1_EN_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(Z2_EN_GPIO_Port, Z2_EN_Pin, GPIO_PIN_SET);
}

static bool limit_pressed(GPIO_TypeDef* port, uint16_t pin) {
    return HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_RESET;
}

#define LIMIT_DEBOUNCE_MS 5UL /* mechanical microswitches bounce for ~1-10ms;
                                * Renode's clean level toggles never exercise
                                * this, so it only matters on real hardware */
static bool     debouncing[2]      = { false, false };
static uint32_t debounceStart[2]   = { 0, 0 };

/* Returns true only once the given limit switch has read continuously
 * pressed for LIMIT_DEBOUNCE_MS - use this (not limit_pressed() directly)
 * anywhere a momentary bounce could cause a false hit latch. */
static bool limit_hit_confirmed(int i, GPIO_TypeDef* port, uint16_t pin, uint32_t now) {
    if (!limit_pressed(port, pin)) {
        debouncing[i] = false;
        return false;
    }
    if (!debouncing[i]) {
        debouncing[i] = true;
        debounceStart[i] = now;
    }
    return (now - debounceStart[i]) >= LIMIT_DEBOUNCE_MS;
}

/* Snapshot current_pos with interrupts disabled. A single aligned 32-bit
 * read can't literally tear on Cortex-M, but this removes any doubt and
 * guarantees the value used is from one exact instant rather than
 * possibly a step boundary. */
static long axis_position_snapshot(int i) {
    __disable_irq();
    long pos = axes[i].current_pos;
    __enable_irq();
    return pos;
}

static void reset_axis_zero(void) {
    __disable_irq();
    for (int i = 0; i < 2; i++) {
        axes[i].current_pos = 0;
        axes[i].target_pos = 0;
        axes[i].step_accumulator = 0;
    }
    __enable_irq();
}

static void axis_move_to(int i, long target) {
    __disable_irq();
    axes[i].target_pos = target;
    axes[i].move_start_tick = isrTicks;
    axes[i].step_interval_current = STEP_INTERVAL_START;
    axes[i].step_accumulator = 0;
    if (target > axes[i].current_pos) {
        HAL_GPIO_WritePin(axes[i].dir_port, axes[i].dir_pin, GPIO_PIN_SET);
    } else if (target < axes[i].current_pos) {
        HAL_GPIO_WritePin(axes[i].dir_port, axes[i].dir_pin, GPIO_PIN_RESET);
    }
    __enable_irq();
}

static void axis_stop(int i) {
    __HAL_TIM_DISABLE_IT(tim, TIM_IT_UPDATE);
    axes[i].target_pos = axes[i].current_pos;
    __HAL_TIM_ENABLE_IT(tim, TIM_IT_UPDATE);
}

static bool axes_done(void) {
    __disable_irq();
    bool done = (axes[0].current_pos == axes[0].target_pos) &&
                (axes[1].current_pos == axes[1].target_pos);
    __enable_irq();
    return done;
}

static void clearHits(void) {
    hit[0] = hit[1] = false;
    hitPos[0] = hitPos[1] = 0;
    debouncing[0] = debouncing[1] = false; /* keep all per-axis hit-latch
        * state (hit, hitPos, and the debounce timers) resetting together,
        * so nothing here can carry a stale in-progress debounce into the
        * next calibration/return/recovery run */
}

static bool skewOK(void) {
    long diff = labs(hitPos[AXIS_Z1] - hitPos[AXIS_Z2]);

    int32_t whole = (int32_t)(diff / STEPS_PER_MM);
    int32_t frac  = (int32_t)(((float)diff / STEPS_PER_MM - (float)whole) * 1000.0f);
    if (frac < 0) frac = -frac;

    printf("SKEW: %ld.%03ld mm\r\n", (long)whole, (long)frac);
    return diff <= MAX_SKEW_STEPS;
}

/* ---- public API ---- */

void alignment_init(TIM_HandleTypeDef *htim) {
    tim = htim;

    axes[AXIS_Z1].step_port = Z1_STEP_GPIO_Port; axes[AXIS_Z1].step_pin = Z1_STEP_Pin;
    axes[AXIS_Z1].dir_port  = Z1_DIR_GPIO_Port;  axes[AXIS_Z1].dir_pin  = Z1_DIR_Pin;
    axes[AXIS_Z1].step_interval = 10;

    axes[AXIS_Z2].step_port = Z2_STEP_GPIO_Port; axes[AXIS_Z2].step_pin = Z2_STEP_Pin;
    axes[AXIS_Z2].dir_port  = Z2_DIR_GPIO_Port;  axes[AXIS_Z2].dir_pin  = Z2_DIR_Pin;
    axes[AXIS_Z2].step_interval = 10;

    enable_motors();
}

/* HAL weak-callback override - lives here (not main.c) because this
 * module owns TIM3 and both axes. */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
    if (htim->Instance != tim->Instance) return;

    isrTicks++;

    /* Finish any pulse that was started last tick before starting new
     * ones - guarantees a full ~100us HIGH time on STEP. */
    for (int i = 0; i < 2; i++) {
        if (stepPending[i]) {
            HAL_GPIO_WritePin(axes[i].step_port, axes[i].step_pin, GPIO_PIN_RESET);
            stepPending[i] = false;
        }
    }

    for (int i = 0; i < 2; i++) {
        uint32_t elapsed = isrTicks - axes[i].move_start_tick;
        if (elapsed >= (uint32_t)RAMP_TICKS) {
            axes[i].step_interval_current = axes[i].step_interval;
        } else {
            long delta = STEP_INTERVAL_START - axes[i].step_interval;
            axes[i].step_interval_current = STEP_INTERVAL_START - ((delta * (long)elapsed) / RAMP_TICKS);
        }

        if (axes[i].current_pos != axes[i].target_pos) {
            axes[i].step_accumulator++;
            if (axes[i].step_accumulator >= axes[i].step_interval_current) {
                axes[i].step_accumulator = 0;
                HAL_GPIO_WritePin(axes[i].step_port, axes[i].step_pin, GPIO_PIN_SET);
                axes[i].current_pos += (axes[i].target_pos > axes[i].current_pos) ? 1 : -1;
                stepPending[i] = true; /* cleared at the top of the next tick */
            }
        }
    }
}

void alignment_start_cal(void) {
    if (limit_pressed(Z1_LIMIT_GPIO_Port, Z1_LIMIT_Pin) ||
        limit_pressed(Z2_LIMIT_GPIO_Port, Z2_LIMIT_Pin)) {
        system_fault("LIMIT ACTIVE");
        return;
    }
    enable_motors();
    clearHits();
    calibrated = false;

    reset_axis_zero();
    axis_move_to(AXIS_Z1, CAL_DIR * CAL_TRAVEL);
    axis_move_to(AXIS_Z2, CAL_DIR * CAL_TRAVEL);

    stateStart = HAL_GetTick();
    state = CALIBRATING;
    printf("CAL\r\n");
}

void alignment_go(void) {
    clearHits();
    enable_motors();
    axis_move_to(AXIS_Z1, GO_STEPS);
    axis_move_to(AXIS_Z2, GO_STEPS);
    state = GOING;
    stateStart = HAL_GetTick();
    printf("GO\r\n");
}

void alignment_return(void) {
    clearHits();
    axis_move_to(AXIS_Z1, 0);
    axis_move_to(AXIS_Z2, 0);
    state = RETURNING;
    stateStart = HAL_GetTick();
    printf("RET\r\n");
}

void alignment_reset(void) {
    axis_stop(AXIS_Z1);
    axis_stop(AXIS_Z2);
    enable_motors();
    reset_axis_zero();
    clearHits();
    calibrated = false;
    state = ALIGN_IDLE;
}

void alignment_emergency_stop(void) {
    axis_stop(AXIS_Z1);
    axis_stop(AXIS_Z2);
    disable_motors();
    calibrated = false;
    state = ALIGN_IDLE;
}

/* system_in_fault() is checked here (not just in commands.c) so that a
 * fault continues to block CAL/GO/RET/OPEN/CLOSE even though
 * alignment_emergency_stop() has to leave `state` at ALIGN_IDLE - a
 * dedicated FAULT value doesn't belong in this module's own enum since
 * fault is a cross-cutting, system-wide concept. */
bool alignment_is_idle(void)             { return !system_in_fault() && state == ALIGN_IDLE; }
bool alignment_is_returned(void)         { return !system_in_fault() && state == RETURNED; }
bool alignment_is_idle_or_returned(void) { return !system_in_fault() && (state == ALIGN_IDLE || state == RETURNED); }
bool alignment_is_hold(void)             { return !system_in_fault() && state == HOLD; }
bool alignment_is_calibrated(void)       { return calibrated; }

void alignment_update(uint32_t now) {
    switch (state) {

    case CALIBRATING:
        if (now - stateStart > CAL_TIMEOUT) {
            system_fault("CAL TIMEOUT");
            break;
        }
        if (limit_hit_confirmed(AXIS_Z1, Z1_LIMIT_GPIO_Port, Z1_LIMIT_Pin, now) && !hit[AXIS_Z1]) {
            hit[AXIS_Z1] = true; hitPos[AXIS_Z1] = axis_position_snapshot(AXIS_Z1);
            axis_stop(AXIS_Z1);
            printf("Z1 HIT\r\n");
        }
        if (limit_hit_confirmed(AXIS_Z2, Z2_LIMIT_GPIO_Port, Z2_LIMIT_Pin, now) && !hit[AXIS_Z2]) {
            hit[AXIS_Z2] = true; hitPos[AXIS_Z2] = axis_position_snapshot(AXIS_Z2);
            axis_stop(AXIS_Z2);
            printf("Z2 HIT\r\n");
        }
        if (hit[AXIS_Z1] && hit[AXIS_Z2]) {
            state = CAL_STOPPING; stateStart = now;
        } else if (hit[AXIS_Z1] && !hit[AXIS_Z2] && (axes[AXIS_Z2].current_pos == axes[AXIS_Z2].target_pos)) {
            system_fault("Z2 LIMIT NOT FOUND");
        } else if (hit[AXIS_Z2] && !hit[AXIS_Z1] && (axes[AXIS_Z1].current_pos == axes[AXIS_Z1].target_pos)) {
            system_fault("Z1 LIMIT NOT FOUND");
        }
        break;

    case CAL_STOPPING:
        if (now - stateStart > CAL_TIMEOUT) {
            system_fault("CAL STOP TIMEOUT");
        } else if (axes_done()) {
            if (!skewOK()) {
                system_fault("CAL SKEW");
            } else {
                printf("ZERO\r\n");
                axis_move_to(AXIS_Z1, axis_position_snapshot(AXIS_Z1) + (BACKOFF_DIR * RECOVERY_STEPS));
                axis_move_to(AXIS_Z2, axis_position_snapshot(AXIS_Z2) + (BACKOFF_DIR * RECOVERY_STEPS));
                state = CAL_BACKOFF;
                stateStart = now;
            }
        }
        break;

    case CAL_BACKOFF:
        if (now - stateStart > CAL_TIMEOUT) {
            system_fault("CAL BACKOFF TIMEOUT");
        } else if (axes_done()) {
            reset_axis_zero();
            clearHits();
            calibrated = true;
            state = ALIGN_IDLE;
            printf("CAL OK\r\n");
        }
        break;

    case GOING:
        if (now - stateStart > MOVE_TIMEOUT) {
            system_fault("GO TIMEOUT");
        } else if (axes_done()) {
            state = HOLD;
            printf("HOLD\r\n");
        }
        break;

    case RETURNING: {
        bool enteringRecovery = false;
        if (!hit[AXIS_Z1] && limit_hit_confirmed(AXIS_Z1, Z1_LIMIT_GPIO_Port, Z1_LIMIT_Pin, now)) {
            hit[AXIS_Z1] = true; hitPos[AXIS_Z1] = axis_position_snapshot(AXIS_Z1);
            axis_stop(AXIS_Z1);
            printf("Z1 HIT\r\n");
            enteringRecovery = true;
        }
        if (!hit[AXIS_Z2] && limit_hit_confirmed(AXIS_Z2, Z2_LIMIT_GPIO_Port, Z2_LIMIT_Pin, now)) {
            hit[AXIS_Z2] = true; hitPos[AXIS_Z2] = axis_position_snapshot(AXIS_Z2);
            axis_stop(AXIS_Z2);
            printf("Z2 HIT\r\n");
            enteringRecovery = true;
        }
        if (enteringRecovery) {
            if (!hit[AXIS_Z1]) axis_move_to(AXIS_Z1, CAL_DIR * CAL_TRAVEL);
            if (!hit[AXIS_Z2]) axis_move_to(AXIS_Z2, CAL_DIR * CAL_TRAVEL);
            state = RECOVERY; stateStart = now;
        } else if (axes_done()) {
            state = RETURNED;
            printf("RETURNED\r\n");
        } else if (now - stateStart > MOVE_TIMEOUT) {
            system_fault("RETURN TIMEOUT");
        }
        break;
    }

    case RECOVERY:
        if (now - stateStart > RECOVERY_TIMEOUT) {
            system_fault("REC TIMEOUT");
            break;
        }
        if (!hit[AXIS_Z1] && limit_hit_confirmed(AXIS_Z1, Z1_LIMIT_GPIO_Port, Z1_LIMIT_Pin, now)) {
            hit[AXIS_Z1] = true; hitPos[AXIS_Z1] = axis_position_snapshot(AXIS_Z1);
            axis_stop(AXIS_Z1);
            printf("Z1 HIT\r\n");
        }
        if (!hit[AXIS_Z2] && limit_hit_confirmed(AXIS_Z2, Z2_LIMIT_GPIO_Port, Z2_LIMIT_Pin, now)) {
            hit[AXIS_Z2] = true; hitPos[AXIS_Z2] = axis_position_snapshot(AXIS_Z2);
            axis_stop(AXIS_Z2);
            printf("Z2 HIT\r\n");
        }
        if (hit[AXIS_Z1] && hit[AXIS_Z2]) {
            state = REC_STOPPING; stateStart = now;
        } else if (hit[AXIS_Z1] && !hit[AXIS_Z2] && (axes[AXIS_Z2].current_pos == axes[AXIS_Z2].target_pos)) {
            system_fault("Z2 LIMIT NOT FOUND");
        } else if (hit[AXIS_Z2] && !hit[AXIS_Z1] && (axes[AXIS_Z1].current_pos == axes[AXIS_Z1].target_pos)) {
            system_fault("Z1 LIMIT NOT FOUND");
        }
        break;

    case REC_STOPPING:
        if (now - stateStart > RECOVERY_TIMEOUT) {
            system_fault("REC STOP TIMEOUT");
        } else if (axes_done()) {
            if (!skewOK()) {
                system_fault("REC SKEW");
            } else {
                axis_move_to(AXIS_Z1, axis_position_snapshot(AXIS_Z1) + (BACKOFF_DIR * RECOVERY_STEPS));
                axis_move_to(AXIS_Z2, axis_position_snapshot(AXIS_Z2) + (BACKOFF_DIR * RECOVERY_STEPS));
                state = REC_BACKOFF; stateStart = now;
            }
        }
        break;

    case REC_BACKOFF:
        if (now - stateStart > RECOVERY_TIMEOUT) {
            system_fault("REC BACKOFF TIMEOUT");
        } else if (axes_done()) {
            reset_axis_zero();
            clearHits();
            calibrated = true;
            state = RETURNED;
            printf("REC OK\r\nZERO\r\nRETURNED\r\n");
        }
        break;

    case ALIGN_IDLE:
    case HOLD:
    case RETURNED:
    default:
        break; /* nothing to poll */
    }
}
