/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/

/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

TIM_HandleTypeDef htim3;

UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */
#define STEPS_PER_MM    200L
#define GO_STEPS        (-20L * STEPS_PER_MM)
#define RECOVERY_MM     5.0f
#define RECOVERY_STEPS  ((long)(RECOVERY_MM * STEPS_PER_MM))
#define MAX_SKEW_STEPS  (5L * STEPS_PER_MM) // 1000L steps = 5.0 mm
#define CAL_TRAVEL      1000000L

#define CAL_DIR         1
#define BACKOFF_DIR    -1

#define CAL_TIMEOUT       30000UL
#define RECOVERY_TIMEOUT  15000UL
#define MOVE_TIMEOUT      60000UL // CHANGE FROM 20000UL TO 60000UL

#define STEP_INTERVAL_START   60L    // slow start: 10kHz/60 ≈ 167 Hz
#define RAMP_TICKS             5000L // ramp duration: 5000 * 100us = 500ms

#define OPEN_DURATION_MS   5000UL
#define CLOSE_DURATION_MS  5000UL

typedef enum {
    IDLE, CALIBRATING, CAL_STOPPING, CAL_BACKOFF,
    GOING, HOLD, RETURNING, RETURNED,
    RECOVERY, REC_STOPPING, REC_BACKOFF, FAULT,
    OPENING, CLOSING
} SystemState;

typedef struct {
    long current_pos;
    long target_pos;
    long step_accumulator;
    long step_interval;         // keep this — now used as the CRUISE (minimum) interval
    long step_interval_current; // current ramp position
    uint32_t move_start_tick;   // isrTicks value when this move began
    GPIO_TypeDef* step_port; uint16_t step_pin;
    GPIO_TypeDef* dir_port;  uint16_t dir_pin;
} StepperAxis;

volatile StepperAxis z1, z2;
volatile SystemState state = IDLE;

bool calibrated = false;
bool z1Hit = false, z2Hit = false;
long z1HitPos = 0, z2HitPos = 0;
uint32_t stateStart = 0;

volatile char rx_buffer[32];
uint8_t rx_char;
uint8_t rx_idx = 0;
volatile bool cmd_ready = false;
volatile uint32_t isrTicks = 0;   // free-running, incremented every ISR call (100us per tick)
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_TIM3_Init(void);
static void MX_USART2_UART_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
void reset_axis_zero(void) {
    __HAL_TIM_DISABLE_IT(&htim3, TIM_IT_UPDATE);
    z1.current_pos = 0; 
    z1.target_pos = 0;
    z2.current_pos = 0; 
    z2.target_pos = 0;
    __HAL_TIM_ENABLE_IT(&htim3, TIM_IT_UPDATE);
}

int _write(int file, char *ptr, int len) {
    HAL_UART_Transmit(&huart2, (uint8_t*)ptr, len, 100);
    return len;
}

bool limit_pressed(GPIO_TypeDef* port, uint16_t pin) {
    return HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_RESET;
}

void enable_motors(void) {
    HAL_GPIO_WritePin(Z1_EN_GPIO_Port, Z1_EN_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(Z2_EN_GPIO_Port, Z2_EN_Pin, GPIO_PIN_RESET);
}

void disable_motors(void) {
    HAL_GPIO_WritePin(Z1_EN_GPIO_Port, Z1_EN_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(Z2_EN_GPIO_Port, Z2_EN_Pin, GPIO_PIN_SET);
}

void gripper_forward(void) {
    HAL_GPIO_WritePin(GRIP_IN1_GPIO_Port, GRIP_IN1_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GRIP_IN2_GPIO_Port, GRIP_IN2_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GRIP_IN3_GPIO_Port, GRIP_IN3_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GRIP_IN4_GPIO_Port, GRIP_IN4_Pin, GPIO_PIN_RESET);
}

void gripper_reverse(void) {
    HAL_GPIO_WritePin(GRIP_IN1_GPIO_Port, GRIP_IN1_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GRIP_IN2_GPIO_Port, GRIP_IN2_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GRIP_IN3_GPIO_Port, GRIP_IN3_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GRIP_IN4_GPIO_Port, GRIP_IN4_Pin, GPIO_PIN_SET);
}

void gripper_stop(void) {
    HAL_GPIO_WritePin(GRIP_IN1_GPIO_Port, GRIP_IN1_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GRIP_IN2_GPIO_Port, GRIP_IN2_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GRIP_IN3_GPIO_Port, GRIP_IN3_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GRIP_IN4_GPIO_Port, GRIP_IN4_Pin, GPIO_PIN_RESET);
}

void axis_move_to(volatile StepperAxis* axis, long target) {
    axis->target_pos = target;
    axis->move_start_tick = isrTicks;
    axis->step_interval_current = STEP_INTERVAL_START;
    if (target > axis->current_pos) {
        HAL_GPIO_WritePin(axis->dir_port, axis->dir_pin, GPIO_PIN_SET);
    } else if (target < axis->current_pos) {
        HAL_GPIO_WritePin(axis->dir_port, axis->dir_pin, GPIO_PIN_RESET);
    }
}

void axis_stop(volatile StepperAxis* axis) {
    __HAL_TIM_DISABLE_IT(&htim3, TIM_IT_UPDATE);
    axis->target_pos = axis->current_pos;
    __HAL_TIM_ENABLE_IT(&htim3, TIM_IT_UPDATE);
}

bool axes_done(void) {
    return (z1.current_pos == z1.target_pos) && (z2.current_pos == z2.target_pos);
}

void clearHits(void) {
    z1Hit = false; z2Hit = false;
    z1HitPos = 0; z2HitPos = 0;
}

bool skewOK(void) {
    long diff = labs(z1HitPos - z2HitPos);
    
    int32_t whole = (int32_t)(diff / STEPS_PER_MM);
    int32_t frac  = (int32_t)(((float)diff / STEPS_PER_MM - (float)whole) * 1000.0f);
    if (frac < 0) frac = -frac;

    printf("SKEW: %ld.%03ld mm\r\n", whole, frac);
    return diff <= MAX_SKEW_STEPS;
}

void fault(const char *msg) {
    axis_stop(&z1);
    axis_stop(&z2);
    disable_motors();
    calibrated = false;
    state = FAULT;
    printf("FAULT: %s\r\n", msg);
}

void startCal(void) {
    if (limit_pressed(Z1_LIMIT_GPIO_Port, Z1_LIMIT_Pin) || 
        limit_pressed(Z2_LIMIT_GPIO_Port, Z2_LIMIT_Pin)) {
        fault("LIMIT ACTIVE");
        return;
    }
    enable_motors();
    clearHits();
    calibrated = false;

    reset_axis_zero();
    axis_move_to(&z1, CAL_DIR * CAL_TRAVEL);
    axis_move_to(&z2, CAL_DIR * CAL_TRAVEL);

    stateStart = HAL_GetTick();
    state = CALIBRATING;
    printf("CAL\r\n");
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
    if (htim->Instance == TIM3) {
        isrTicks++;

        // Update ramp for both axes every tick, independent of stepping
        uint32_t elapsed1 = isrTicks - z1.move_start_tick;
        if (elapsed1 >= (uint32_t)RAMP_TICKS) {
            z1.step_interval_current = z1.step_interval;
        } else {
            long delta = STEP_INTERVAL_START - z1.step_interval;
            z1.step_interval_current = STEP_INTERVAL_START - (delta * (long)elapsed1) / RAMP_TICKS;
        }

        uint32_t elapsed2 = isrTicks - z2.move_start_tick;
        if (elapsed2 >= (uint32_t)RAMP_TICKS) {
            z2.step_interval_current = z2.step_interval;
        } else {
            long delta = STEP_INTERVAL_START - z2.step_interval;
            z2.step_interval_current = STEP_INTERVAL_START - (delta * (long)elapsed2) / RAMP_TICKS;
        }

        if (z1.current_pos != z1.target_pos) {
            z1.step_accumulator++;
            if (z1.step_accumulator >= z1.step_interval_current) {
                z1.step_accumulator = 0;

                // Z1 Pulse Generation
                HAL_GPIO_WritePin(z1.step_port, z1.step_pin, GPIO_PIN_SET);
                for (volatile int i = 0; i < 120; i++) __NOP(); // ~2.5 µs delay at 180 MHz
                z1.current_pos += (z1.target_pos > z1.current_pos) ? 1 : -1;
                HAL_GPIO_WritePin(z1.step_port, z1.step_pin, GPIO_PIN_RESET);
            }
        }
        if (z2.current_pos != z2.target_pos) {
            z2.step_accumulator++;
            if (z2.step_accumulator >= z2.step_interval_current) {
                z2.step_accumulator = 0;

                // Z2 Pulse Generation
                HAL_GPIO_WritePin(z2.step_port, z2.step_pin, GPIO_PIN_SET);
                for (volatile int i = 0; i < 120; i++) __NOP(); // ~2.5 µs delay at 180 MHz
                z2.current_pos += (z2.target_pos > z2.current_pos) ? 1 : -1;
                HAL_GPIO_WritePin(z2.step_port, z2.step_pin, GPIO_PIN_RESET);
            }
        }
    }
}
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
    if (huart->Instance == USART2) {
        if (rx_char == '\r') {
            // Ignore carriage return so \r\n acts as a single line terminator
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
        HAL_UART_Receive_IT(&huart2, &rx_char, 1);
    }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_TIM3_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
  z1.step_port = Z1_STEP_GPIO_Port; z1.step_pin = Z1_STEP_Pin;
  z1.dir_port  = Z1_DIR_GPIO_Port;  z1.dir_pin  = Z1_DIR_Pin;
  z1.step_interval = 10; // CHANGE FROM 25 TO 10

  z2.step_port = Z2_STEP_GPIO_Port; z2.step_pin = Z2_STEP_Pin;
  z2.dir_port  = Z2_DIR_GPIO_Port;  z2.dir_pin  = Z2_DIR_Pin;
  z2.step_interval = 10; // CHANGE FROM 25 TO 10

  enable_motors();
  HAL_TIM_Base_Start_IT(&htim3);
  HAL_UART_Receive_IT(&huart2, &rx_char, 1);

  printf("READY\r\nCAL REQUIRED\r\n");
  /* USER CODE END 2 */

  /* Initialize leds */
  BSP_LED_Init(LED2);

    /* Initialize USER push-button in pure GPIO mode (no EXTI) */
    BSP_PB_Init(BUTTON_USER, BUTTON_MODE_GPIO);

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    uint32_t now = HAL_GetTick();

    if (state == CALIBRATING) {
        if (now - stateStart > CAL_TIMEOUT) {
            fault("CAL TIMEOUT");
        } else {
            if (limit_pressed(Z1_LIMIT_GPIO_Port, Z1_LIMIT_Pin) && !z1Hit) {
                z1Hit = true; z1HitPos = z1.current_pos;
                axis_stop(&z1);
                printf("Z1 HIT\r\n");
            }
            if (limit_pressed(Z2_LIMIT_GPIO_Port, Z2_LIMIT_Pin) && !z2Hit) {
                z2Hit = true; z2HitPos = z2.current_pos;
                axis_stop(&z2);
                printf("Z2 HIT\r\n");
            }
            if (z1Hit && z2Hit) {
                state = CAL_STOPPING; stateStart = now;
            } else if (z1Hit && !z2Hit && (z2.current_pos == z2.target_pos)) {
                fault("Z2 LIMIT NOT FOUND");
            } else if (z2Hit && !z1Hit && (z1.current_pos == z1.target_pos)) {
                fault("Z1 LIMIT NOT FOUND");
            }
        }
    }
    else if (state == CAL_STOPPING) {
        if (axes_done()) {
            if (!skewOK()) {
                fault("CAL SKEW");
            } else {
                axis_move_to(&z1, z1.current_pos + (BACKOFF_DIR * RECOVERY_STEPS));
                axis_move_to(&z2, z2.current_pos + (BACKOFF_DIR * RECOVERY_STEPS));
                state = CAL_BACKOFF; stateStart = now;
            }
        }
    }
    else if (state == CAL_BACKOFF) {
        if (now - stateStart > CAL_TIMEOUT) {
            fault("CAL BACKOFF TIMEOUT");
        } else if (axes_done()) {
            reset_axis_zero();
            clearHits();
            calibrated = true; state = IDLE;
            printf("CAL OK\r\nZERO\r\n");
        }
    }
    else if (state == GOING) {
        if (now - stateStart > MOVE_TIMEOUT) {
            fault("GO TIMEOUT");
        } else if (axes_done()) {
            state = HOLD;
            printf("HOLD\r\n");
        }
    }
    else if (state == RETURNING) {
        bool enteringRecovery = false;
        if (!z1Hit && limit_pressed(Z1_LIMIT_GPIO_Port, Z1_LIMIT_Pin)) {
            z1Hit = true; z1HitPos = z1.current_pos;
            axis_stop(&z1);
            printf("Z1 HIT\r\n");
            enteringRecovery = true;
        }
        if (!z2Hit && limit_pressed(Z2_LIMIT_GPIO_Port, Z2_LIMIT_Pin)) {
            z2Hit = true; z2HitPos = z2.current_pos;
            axis_stop(&z2);
            printf("Z2 HIT\r\n");
            enteringRecovery = true;
        }
        if (enteringRecovery) {
            if (!z1Hit) axis_move_to(&z1, CAL_DIR * CAL_TRAVEL);
            if (!z2Hit) axis_move_to(&z2, CAL_DIR * CAL_TRAVEL);
            state = RECOVERY; stateStart = now;
        } else if (axes_done()) {
            state = RETURNED;
            printf("RETURNED\r\n");
        } else if (now - stateStart > MOVE_TIMEOUT) {
            fault("RETURN TIMEOUT");
        }
    }
    else if (state == RECOVERY) {
        if (now - stateStart > RECOVERY_TIMEOUT) {
            fault("REC TIMEOUT");
        } else {
            if (!z1Hit && limit_pressed(Z1_LIMIT_GPIO_Port, Z1_LIMIT_Pin)) {
                z1Hit = true; z1HitPos = z1.current_pos;
                axis_stop(&z1);
                printf("Z1 HIT\r\n");
            }
            if (!z2Hit && limit_pressed(Z2_LIMIT_GPIO_Port, Z2_LIMIT_Pin)) {
                z2Hit = true; z2HitPos = z2.current_pos;
                axis_stop(&z2);
                printf("Z2 HIT\r\n");
            }
            if (z1Hit && z2Hit) {
                state = REC_STOPPING; stateStart = now;
            } else if (z1Hit && !z2Hit && (z2.current_pos == z2.target_pos)) {
                fault("Z2 LIMIT NOT FOUND");
            } else if (z2Hit && !z1Hit && (z1.current_pos == z1.target_pos)) {
                fault("Z1 LIMIT NOT FOUND");
            }
        }
    }
    else if (state == REC_STOPPING) {
        if (axes_done()) {
            if (!skewOK()) {
                fault("REC SKEW");
            } else {
                axis_move_to(&z1, z1.current_pos + (BACKOFF_DIR * RECOVERY_STEPS));
                axis_move_to(&z2, z2.current_pos + (BACKOFF_DIR * RECOVERY_STEPS));
                state = REC_BACKOFF; stateStart = now;
            }
        }
    }
    else if (state == REC_BACKOFF) {
        if (now - stateStart > RECOVERY_TIMEOUT) {
            fault("REC BACKOFF TIMEOUT");
        } else if (axes_done()) {
            reset_axis_zero();
            clearHits();
            calibrated = true; state = RETURNED;
            printf("REC OK\r\nZERO\r\nRETURNED\r\n");
        }
    }else if (state == OPENING) {
        if (now - stateStart > OPEN_DURATION_MS) {
            gripper_stop();
            state = IDLE;
            printf("OPENED\r\n");
        }
    }
    else if (state == CLOSING) {
        if (now - stateStart > CLOSE_DURATION_MS) {
            gripper_stop();
            state = IDLE;
            printf("CLOSED\r\n");
        }
    }

    if (cmd_ready) {
        cmd_ready = false;
        if (strcmp((const char*)rx_buffer, "CAL") == 0) {
            if (state == IDLE) startCal();
            else printf("BUSY\r\n");
        } else if (strcmp((const char*)rx_buffer, "GO") == 0) {
            if (!calibrated) printf("NO CAL\r\n");
            else if (state == IDLE || state == RETURNED) {
                clearHits(); enable_motors();
                axis_move_to(&z1, GO_STEPS); axis_move_to(&z2, GO_STEPS);
                state = GOING; stateStart = now;
                printf("GO\r\n");
            } else printf("BUSY\r\n");
        } else if (strcmp((const char*)rx_buffer, "RET") == 0) {
            if (!calibrated) printf("NO CAL\r\n");
            else if (state == HOLD) {
                clearHits();
                axis_move_to(&z1, 0); axis_move_to(&z2, 0);
                state = RETURNING; stateStart = now;
                printf("RET\r\n");
            } else printf("INVALID\r\n");
        } else if (strcmp((const char*)rx_buffer, "RST") == 0) {
            axis_stop(&z1); axis_stop(&z2); enable_motors();
            reset_axis_zero();
            clearHits(); calibrated = false; state = IDLE;
            printf("RST\r\nNO CAL\r\n");
        } else if (strcmp((const char*)rx_buffer, "OPEN") == 0) {
            if (state == IDLE) {
                gripper_forward();
                state = OPENING; stateStart = now;
                printf("OPENING\r\n");
            } else printf("BUSY\r\n");
        } else if (strcmp((const char*)rx_buffer, "CLOSE") == 0) {
            if (state == IDLE) {
                gripper_reverse();
                state = CLOSING; stateStart = now;
                printf("CLOSING\r\n");
            } else printf("BUSY\r\n");
        }
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 360;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Activate the Over-Drive mode
  */
  if (HAL_PWREx_EnableOverDrive() != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief TIM3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM3_Init(void)
{

  /* USER CODE BEGIN TIM3_Init 0 */

  /* USER CODE END TIM3_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM3_Init 1 */

  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 89;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 99;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */

  /* USER CODE END TIM3_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, Z1_STEP_Pin|Z1_DIR_Pin|Z1_EN_Pin|GRIP_IN1_Pin
                          |GRIP_IN2_Pin|GRIP_IN3_Pin|GRIP_IN4_Pin|Z2_STEP_Pin
                          |Z2_DIR_Pin|Z2_EN_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : Z1_STEP_Pin Z1_DIR_Pin Z1_EN_Pin GRIP_IN1_Pin
                           GRIP_IN2_Pin GRIP_IN3_Pin GRIP_IN4_Pin Z2_STEP_Pin
                           Z2_DIR_Pin Z2_EN_Pin */
  GPIO_InitStruct.Pin = Z1_STEP_Pin|Z1_DIR_Pin|Z1_EN_Pin|GRIP_IN1_Pin
                          |GRIP_IN2_Pin|GRIP_IN3_Pin|GRIP_IN4_Pin|Z2_STEP_Pin
                          |Z2_DIR_Pin|Z2_EN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : Z1_LIMIT_Pin Z2_LIMIT_Pin */
  GPIO_InitStruct.Pin = Z1_LIMIT_Pin|Z2_LIMIT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
