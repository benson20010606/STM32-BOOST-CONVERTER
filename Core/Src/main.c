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

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */




/*-----------------------------------------------------------
 * Stores ADC and output-voltage measurement data.
 *
 * adc_raw:
 *     Raw 12-bit ADC conversion result.
 *     Valid range: 0 ~ 4095.
 *
 * adc_mv:
 *     Voltage measured directly at the ADC input pin
 *     (PA0 / ADC1_IN0), in millivolts.
 *
 * vout_mv:
 *     Reconstructed Boost converter output voltage
 *     calculated from the feedback divider ratio.
 *
 * sample_count:
 *     Incremented after every ADC conversion.
 *     Since the ADC sampling rate is 5 kHz,
 *     this counter increases approximately 5000 times
 *     per second.
 ------------------------------------------------------------*/


typedef struct
{
    uint32_t adc_raw;
    uint32_t adc_mv;
		uint32_t vout_mv;
    uint32_t sample_count;
} Measurement_t;


/*-------------------------------------------------
 * Stores PI controller parameters, dynamic states,
 * and protection states.
 *
 * vref_mv:
 *     Target output voltage in millivolts.
 *
 * error_mv:
 *     Voltage regulation error:
 *
 *         error = Vref - Vout
 *
 *
 * kp / ki:
 *     PI controller gains.
 *
 * integral:
 *     Integral accumulator state:
 *
 *         integral += error * Ts
 *
 * duty_ff_permille:
 *     Nominal feed-forward duty cycle.
 *
 *     Example:
 *         300 = 30.0%
 *
 * duty_min_permille / duty_max_permille:
 *     Allowed duty-cycle range during normal
 *     closed-loop operation.
 *
 * enabled:
 *     Controller enable flag.
 *     Cleared when a protection fault occurs.
 *
 * fault_ovp:
 *     Latched over-voltage protection flag.
 *
 * fb_low_count:
 *     Number of consecutive low-feedback ADC samples.
 *
 * fault_feedback:
 *     Latched low-feedback fault flag.
 --------------------------------------------------*/


typedef struct
{
    uint32_t vref_mv;
	  int32_t error_mv;
		
		float kp;
		float ki;
	  float integral;

    uint16_t duty_ff_permille;
    uint16_t duty_min_permille;
    uint16_t duty_max_permille;

    uint8_t enabled;
	  uint8_t fault_ovp;
	
		uint16_t fb_low_count;
	  uint8_t fault_feedback;

} Controller_t;

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */


/* ============================================================
 * PWM Configuration
 * ============================================================
 *
 * Duty cycle is represented in permille:
 *
 *     1000 = 100.0%
 *      300 =  30.0%
 *       50 =   5.0%
 *
 * Permille representation provides 0.1% resolution
 * without using floating-point values in the PWM driver.
 */

/*
 * Absolute PWM duty-cycle limit.
 *
 * PWM_SetDutyPermille() will never allow the PWM output
 * to exceed this value, even if the controller generates
 * an invalid command.
 */


#define PWM_DUTY_MAX_PERMILLE           600U
/* ============================================================
 * Controller Configuration
 * ============================================================ */

/* Target Boost converter output voltage: 7.0 V */

#define CONTROL_VREF_MV                7000U

/*
 * Nominal feed-forward duty cycle.
 *
 * For an ideal Boost converter:
 *
 *     Vout = Vin / (1 - D)
 *
 * Therefore:
 *
 *     D = 1 - Vin / Vout
 *
 * For Vin = 5 V and Vout = 7 V:
 *
 *     D = 1 - 5/7
 *       = 0.286
 *
 * A nominal duty cycle of 30% is therefore used.
 */

#define CONTROL_DUTY_FF_PERMILLE        300U

/*
 * Allowed duty-cycle range during normal
 * closed-loop operation:
 *
 *     5% <= duty <= 60%
 *
 * Protection functions may bypass the minimum duty
 * and force the PWM output to 0%.
 */

#define CONTROL_DUTY_MIN_PERMILLE        50U
#define CONTROL_DUTY_MAX_PERMILLE       600U

/*
 * Maximum contribution from the integral term.
 *
 *     +0.05 = +5% duty correction
 *     -0.05 = -5% duty correction
 *
 * This prevents excessive integral accumulation and
 * improves recovery after large operating-point changes.
 */

#define CONTROL_I_TERM_LIMIT    0.1f

/*
 * Over-voltage protection threshold.
 *
 * When reconstructed Vout >= 8.0 V:
 *
 *     fault_ovp = 1
 *     controller is disabled
 *     PWM duty is forced to 0%
 *
 * The fault is latched until MCU reset.
 */

#define CONTROL_OVP_MV    8000U 


/*
 * Low-feedback protection threshold.
 *
 * If Vout remains below 3.0 V for a sufficiently long
 * period, the feedback path or power stage is considered
 * abnormal.
 */

#define CONTROL_FB_LOW_MV            3000U

/*
 * The control loop runs at 5 kHz.
 *
 * 500 samples / 5000 samples/s
 * = 0.1 s
 * = 100 ms
 */


#define CONTROL_FB_LOW_COUNT_LIMIT    500U

/* ============================================================
 * ADC Configuration
 * ============================================================ */

/*
 * STM32F401 ADC:
 *
 *     Resolution: 12 bit
 *     Raw range:  0 ~ 4095
 *
 * VDDA is currently assumed to be 3.3 V.
 *
 * If higher measurement accuracy is required later,
 * the actual VDDA value can be measured and calibrated.
 */

#define ADC_FULL_SCALE              4095U
#define ADC_VREF_MV                 3300U

/* ============================================================
 * PI Controller Parameters
 * ============================================================ */

/*
 * ADC / controller update frequency:
 *
 *     fs = 5 kHz
 *
 * Therefore:
 *
 *     Ts = 1 / 5000
 *        = 0.0002 s
 */

#define CONTROL_TS_SEC      0.0002f
#define CONTROL_KP_TEST    0.02f
#define CONTROL_KI_TEST     0.05f

#define FB_R_TOP_OHM               47000U
#define FB_R_BOTTOM_OHM            10000U
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim3;
TIM_HandleTypeDef htim4;

UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */


static volatile Measurement_t measurement = {0};
static volatile uint16_t pwm_duty_permille = 0;
static volatile uint8_t telemetry_due = 0;
static volatile uint8_t uart_tx_busy = 0;
static char uart_tx_buf[128];



static volatile Controller_t controller = {0};

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_TIM3_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_TIM2_Init(void);
static void MX_TIM4_Init(void);
/* USER CODE BEGIN PFP */

/* Application startup */
static void App_Start(void);


/* Measurement layer */
static uint32_t ADC_RawToMilliVolts(uint32_t raw);
static uint32_t Feedback_ADCToVoutMilliVolts(uint32_t adc_mv);
static void Measurement_Update(uint32_t raw);

/* PWM abstraction layer */
static void PWM_SetDutyPermille(uint16_t duty_permille);

/* PI controller */
static uint16_t Control_ClampDutyPermille(int32_t duty_permille);
static void Control_Init(void);
static void Control_Reset(void);
static void Control_Update(uint32_t vout_mv);

/* Protection */
static void Protection_Check(uint32_t vout_mv);


/* UART telemetry */
static void Telemetry_Request(void);
static void Telemetry_Service(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

static uint32_t ADC_RawToMilliVolts(uint32_t raw)
{
    if (raw > ADC_FULL_SCALE)
    {
        raw = ADC_FULL_SCALE;
    }

    return (raw * ADC_VREF_MV) / ADC_FULL_SCALE;
}

static void Measurement_Update(uint32_t raw)
{
    measurement.adc_raw = raw;
    measurement.adc_mv = ADC_RawToMilliVolts(raw);
	  measurement.vout_mv = Feedback_ADCToVoutMilliVolts( measurement.adc_mv);
    measurement.sample_count++;
}

static uint32_t Feedback_ADCToVoutMilliVolts(uint32_t adc_mv)
{
    return (adc_mv * (FB_R_TOP_OHM + FB_R_BOTTOM_OHM))
           / FB_R_BOTTOM_OHM;
}



static void PWM_SetDutyPermille(uint16_t duty_permille)
{
    if (duty_permille > PWM_DUTY_MAX_PERMILLE)
    {
        duty_permille = PWM_DUTY_MAX_PERMILLE;
    }

    uint32_t arr =
        __HAL_TIM_GET_AUTORELOAD(&htim3);

    uint32_t ccr =
        ((uint32_t)duty_permille * (arr + 1U)) / 1000U;

    __HAL_TIM_SET_COMPARE(
        &htim3,
        TIM_CHANNEL_1,
        ccr
    );

    pwm_duty_permille = duty_permille;
}



void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM4)
    {
       Telemetry_Request();
    }
}


static void Control_Init(void)
{
    controller.vref_mv = CONTROL_VREF_MV;
		controller.kp = CONTROL_KP_TEST;
	  controller.ki = CONTROL_KI_TEST;

    controller.duty_ff_permille =
        CONTROL_DUTY_FF_PERMILLE;

    controller.duty_min_permille =
        CONTROL_DUTY_MIN_PERMILLE;

    controller.duty_max_permille =
        CONTROL_DUTY_MAX_PERMILLE;

    controller.enabled = 1U;
}

static void Control_Reset(void)
{
    /*
     * Controller dynamic states will be reset here.
     * Integral state will be added later.
     */
		controller.error_mv = 0;
	  controller.integral = 0.0f;
	  controller.fault_ovp = 0U;
		controller.fb_low_count = 0U;
		controller.fault_feedback = 0U;
	
    PWM_SetDutyPermille(
        controller.duty_ff_permille
    );
}


static void Control_Update(uint32_t vout_mv)
{
    if (controller.enabled == 0U)
    {
        return;
    }

    controller.error_mv =
        (int32_t)controller.vref_mv -
        (int32_t)vout_mv;

    /* mV -> V */
    float error_v =
        (float)controller.error_mv / 1000.0f;

    /* P term */
    float p_term =
        controller.kp * error_v;

    /*
     * Calculate candidate integral first.
     * Do not immediately store it.
     */
    float integral_candidate =
        controller.integral +
        error_v * CONTROL_TS_SEC;

    float i_term_candidate =
        controller.ki * integral_candidate;
		/*
		 * Limit integral contribution to -+5% duty.
		 */
		if (i_term_candidate > CONTROL_I_TERM_LIMIT)
		{
				i_term_candidate = CONTROL_I_TERM_LIMIT;

				integral_candidate =
						CONTROL_I_TERM_LIMIT / controller.ki;
		}
		else if (i_term_candidate < -CONTROL_I_TERM_LIMIT)
		{
				i_term_candidate = -CONTROL_I_TERM_LIMIT;

				integral_candidate =
						-CONTROL_I_TERM_LIMIT / controller.ki;
		}
		
		
    float duty_ff =
        (float)controller.duty_ff_permille / 1000.0f;

    float duty_min =
        (float)controller.duty_min_permille / 1000.0f;

    float duty_max =
        (float)controller.duty_max_permille / 1000.0f;

    /*
     * Unsaturated duty using candidate integral.
     */
    float duty_unsat =
        duty_ff +
        p_term +
        i_term_candidate;

    /*
     * Conditional integration anti-windup.
     *
     * Stop integrating only when integration would
     * push the controller further into saturation.
     */
    if (!((duty_unsat >= duty_max && error_v > 0.0f) ||
          (duty_unsat <= duty_min && error_v < 0.0f)))
    {
        controller.integral = integral_candidate;
    }

    /*
     * Recalculate output using accepted integral.
     */
    float i_term =
        controller.ki * controller.integral;

    float duty_command =
        duty_ff +
        p_term +
        i_term;

    int32_t duty_permille =
        (int32_t)(duty_command * 1000.0f);

    uint16_t duty =
        Control_ClampDutyPermille(duty_permille);

    PWM_SetDutyPermille(duty);
}

static uint16_t Control_ClampDutyPermille(int32_t duty_permille)
{
    if (duty_permille < (int32_t)controller.duty_min_permille)
    {
        return controller.duty_min_permille;
    }

    if (duty_permille > (int32_t)controller.duty_max_permille)
    {
        return controller.duty_max_permille;
    }

    return (uint16_t)duty_permille;
}

static void Protection_Check(uint32_t vout_mv)
{
    if (vout_mv >= CONTROL_OVP_MV)
    {
        controller.fault_ovp = 1U;
        controller.enabled = 0U;

        /*
         * Emergency shutdown.
         * Bypass controller minimum duty limit.
         */
        PWM_SetDutyPermille(0U);
			  return;
    }
		 /*
     * Abnormally low feedback.
     * Require 100 ms continuously below threshold.
     */
    if (vout_mv < CONTROL_FB_LOW_MV)
    {
        if (controller.fb_low_count < CONTROL_FB_LOW_COUNT_LIMIT)
        {
            controller.fb_low_count++;
        }

        if (controller.fb_low_count >= CONTROL_FB_LOW_COUNT_LIMIT)
        {
            controller.fault_feedback = 1U;
            controller.enabled = 0U;

            PWM_SetDutyPermille(0U);
        }
    }
    else
    {
        controller.fb_low_count = 0U;
    }
}


static void Telemetry_Request(void)
{
    telemetry_due = 1U;
}


static void Telemetry_Service(void)
{
    /*
     * Nothing to send.
     */
    if (telemetry_due == 0U)
    {
        return;
    }

    /*
     * Previous interrupt-driven UART transmission
     * has not completed yet.
     */
    if (uart_tx_busy != 0U)
    {
        return;
    }

    /*
     * Snapshot shared data.
     */
    uint32_t raw = measurement.adc_raw;
    uint32_t adc_mv = measurement.adc_mv;
    uint32_t count = measurement.sample_count;
    uint16_t duty = pwm_duty_permille;
		uint32_t vout_mv = measurement.vout_mv;
		int32_t error_mv = controller.error_mv;
		
		uint8_t fault_ovp = controller.fault_ovp;
		uint8_t fb_fault=controller.fault_feedback;
		int len = snprintf(
				uart_tx_buf,
				sizeof(uart_tx_buf),
				"vout=%lu mV, err=%ld mV, duty=%u.%u%%, ovp=%u,  fb_fault=%u, count=%lu\r\n",
				(unsigned long)vout_mv,
				(long)error_mv,
				duty / 10U,
				duty % 10U,
				fault_ovp,
				fb_fault,
				(unsigned long)count
		);
    if (len <= 0)
    {
        return;
    }

    if ((uint32_t)len >= sizeof(uart_tx_buf))
    {
        len = sizeof(uart_tx_buf) - 1U;
    }

    /*
     * Consume current telemetry request.
     */
    telemetry_due = 0U;

    /*
     * Buffer must not be modified until
     * HAL_UART_TxCpltCallback().
     */
    uart_tx_busy = 1U;

    if (HAL_UART_Transmit_IT(
            &huart2,
            (uint8_t *)uart_tx_buf,
            (uint16_t)len) != HAL_OK)
    {
        /*
         * Transmission failed to start.
         * Release buffer and retry later.
         */
        uart_tx_busy = 0U;
        telemetry_due = 1U;
    }
}

static void App_Start(void)
{
	
	  Control_Init();

    Control_Reset();

    if (HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1) != HAL_OK)
    {
        Error_Handler();
    }

    /*
     * ADC is armed first.
     * Actual conversions are triggered by TIM2 TRGO.
     */
    if (HAL_ADC_Start_IT(&hadc1) != HAL_OK)
    {
        Error_Handler();
    }

    /*
     * TIM2:
     * 5 kHz TRGO for ADC.
     * No TIM2 interrupt.
     */
    if (HAL_TIM_Base_Start(&htim2) != HAL_OK)
    {
        Error_Handler();
    }

    /*
     * TIM4:
     * 50 Hz telemetry scheduler.
     */
    if (HAL_TIM_Base_Start_IT(&htim4) != HAL_OK)
    {
        Error_Handler();
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
  MX_ADC1_Init();
  MX_TIM3_Init();
  MX_USART2_UART_Init();
  MX_TIM2_Init();
  MX_TIM4_Init();
  /* USER CODE BEGIN 2 */




	char msg[] = "\r\nSTM32 BOOST CONTROLLER READY\r\n";

	HAL_UART_Transmit(
			&huart2,
			(uint8_t *)msg,
			sizeof(msg) - 1U,
			HAL_MAX_DELAY
	);
	App_Start();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {  
		
		/* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
		Telemetry_Service();
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
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 16;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
  hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T2_TRGO;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_84CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 83;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 199;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

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
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM3_Init 1 */

  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 0;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 839;
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
  if (HAL_TIM_PWM_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 252;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */

  /* USER CODE END TIM3_Init 2 */
  HAL_TIM_MspPostInit(&htim3);

}

/**
  * @brief TIM4 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM4_Init(void)
{

  /* USER CODE BEGIN TIM4_Init 0 */

  /* USER CODE END TIM4_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM4_Init 1 */

  /* USER CODE END TIM4_Init 1 */
  htim4.Instance = TIM4;
  htim4.Init.Prescaler = 83;
  htim4.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim4.Init.Period = 19999;
  htim4.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim4.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim4) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim4, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim4, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM4_Init 2 */

  /* USER CODE END TIM4_Init 2 */

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
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : B1_Pin */
  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : LD2_Pin */
  GPIO_InitStruct.Pin = LD2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD2_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)
    {
        uint32_t raw = HAL_ADC_GetValue(hadc);

        Measurement_Update(raw);
				Protection_Check(measurement.vout_mv);
        Control_Update(measurement.vout_mv);
    }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        uart_tx_busy = 0U;

    }
}


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
