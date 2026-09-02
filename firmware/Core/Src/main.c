/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Ultra-precision voltage & current meter with Bluetooth
  *                   telemetry.
  *
  *   Target : STM32F401CEU6 ("Black Pill", 48-pin)
  *
  *   ---- Measurement front ends --------------------------------------------
  *   VOLTAGE : on-chip 12-bit ADC1, channel 9 (PB1), fed from a resistive
  *             divider R1 = 9906 ohm (low side), R2 = 39010 ohm (high side).
  *             Full-scale input approximately 16 V.
  *   CURRENT : INA226 precision monitor on I2C2, address 0x44, driving a
  *             0.1 ohm sense resistor. 25 uA per raw bit.
  *
  *   ---- User interface -----------------------------------------------------
  *   PB6 -> VOLTAGE mode      Row0: "   VOLTAGE      "
  *                            Row1: "   X.XXXX V     "
  *   PB7 -> MILLIAMP mode     Row0: "   CURRENT      "
  *                            Row1: "   X.XXX mA     "
  *   PB8 -> MICROAMP mode     Row0: "   CURRENT      "
  *                            Row1: "   XX.XX uA     "
  *                            Row1: "   OVERLOAD!    "  when reading > 99 uA
  *
  *          Mode buttons are active HIGH with internal pull-downs, and are
  *          debounced in firmware on the rising edge (50 ms).
  *
  *   PC13 -> Bluetooth ON/OFF slide switch, wired to GND with the internal
  *           pull-up enabled:
  *             closed (ON)  -> pin reads LOW  -> telemetry enabled
  *             open   (OFF) -> pin reads HIGH -> telemetry stopped
  *
  *   ---- Telemetry ----------------------------------------------------------
  *   HC-05 module on USART1 (PA9 = TX, PA10 = RX), 9600 baud, 8N1.
  *   While the switch is ON the active reading is transmitted once every
  *   5 seconds. Turning the switch OFF halts transmission immediately.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"
#include "liquidcrystal_i2c.h"
#include "INA226_STM32.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* ---- Defines -------------------------------------------------------------- */
#define NUM_CAL_SAMPLES         512u
#define NUM_LIVE_SAMPLES        16u
#define MAX_ALLOWED_OFFSET      100u
#define ADC_DEADBAND_COUNTS     4u
#define MAGIC_INIT              0xA55A5AA5u

#define INA226_LSB_TO_MA        0.025f
#define INA226_LSB_TO_UA        25.0f

#define UA_OVERLOAD_THRESHOLD   99.0f

#define MODE_BTN_PORT           GPIOB
#define MODE_BTN_V_PIN          GPIO_PIN_6
#define MODE_BTN_MA_PIN         GPIO_PIN_7
#define MODE_BTN_UA_PIN         GPIO_PIN_8
#define MODE_BTN_DEBOUNCE_MS    50u

/* PC13 -- ON/OFF switch to GND, internal PULLUP
   LOW  = switch ON  = BT streaming enabled
   HIGH = switch OFF = BT streaming disabled                                 */
#define BT_SW_PORT              GPIOC
#define BT_SW_PIN               GPIO_PIN_13

/* Bluetooth auto-send cadence. HAL_GetTick() counts milliseconds, so
   5000 ms yields one telemetry frame every 5 seconds while enabled.        */
#define BT_AUTO_INTERVAL_MS     5000u
#define BT_TX_TIMEOUT_MS        100u

/* ---- HAL handles ---------------------------------------------------------- */
ADC_HandleTypeDef  hadc1;
I2C_HandleTypeDef  hi2c2;
TIM_HandleTypeDef  htim2;
UART_HandleTypeDef huart1;

/* ---- .noinit -------------------------------------------------------------- */
__attribute__((section(".noinit"))) uint32_t adcCalDone;
__attribute__((section(".noinit"))) uint16_t adcOffset;

/* ---- Globals -------------------------------------------------------------- */
INA226_t ina226;

typedef enum { MODE_VOLTS = 0, MODE_MA, MODE_UA } DisplayMode_t;
static DisplayMode_t currentMode     = MODE_VOLTS;
static DisplayMode_t lastDisplayMode = MODE_VOLTS;

static float Vin, Vo;
static float ImA, IuA;

static char lcdBuf[20];
static char btBuf[64];

/* LCD value cache -- only redraw when value changes */
static uint32_t lastV_whole  = 0xFFFFFFFFu;
static uint32_t lastV_frac   = 0xFFFFFFFFu;
static uint32_t lastMA_whole = 0xFFFFFFFFu;
static uint32_t lastMA_frac  = 0xFFFFFFFFu;
static uint32_t lastUA_whole = 0xFFFFFFFFu;
static uint32_t lastUA_frac  = 0xFFFFFFFFu;
static uint8_t  lastOvl      = 0xFFu;

typedef struct { uint8_t lastState; uint32_t lastTick; } BtnState_t;
static BtnState_t btnV  = {0, 0};
static BtnState_t btnMA = {0, 0};
static BtnState_t btnUA = {0, 0};

static uint32_t lastBtSendTick = 0;

/* ---- Prototypes ----------------------------------------------------------- */
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C2_Init(void);
static void MX_ADC1_Init(void);
static void MX_TIM2_Init(void);
static void MX_USART1_UART_Init(void);

/* ===========================================================================
   LCD CACHE INVALIDATE
   =========================================================================== */
static void LCD_InvalidateCache(void)
{
    lastV_whole  = 0xFFFFFFFFu;
    lastV_frac   = 0xFFFFFFFFu;
    lastMA_whole = 0xFFFFFFFFu;
    lastMA_frac  = 0xFFFFFFFFu;
    lastUA_whole = 0xFFFFFFFFu;
    lastUA_frac  = 0xFFFFFFFFu;
    lastOvl      = 0xFFu;
}

/* ===========================================================================
   ADC
   =========================================================================== */
static uint16_t ADC_Read(void)
{
    HAL_ADC_Start(&hadc1);
    HAL_ADC_PollForConversion(&hadc1, 500);
    uint16_t v = (uint16_t)HAL_ADC_GetValue(&hadc1);
    HAL_ADC_Stop(&hadc1);
    return v;
}

static uint16_t ADC_ReadAveraged(uint8_t n)
{
    uint32_t acc = 0;
    for (uint8_t i = 0; i < n; i++) acc += ADC_Read();
    return (uint16_t)((acc + (n / 2u)) / n);
}

static void ADC_CalibrateOffset(void)
{
    HD44780_Clear();
    HD44780_SetCursor(0, 0); HD44780_PrintStr(" CALIBRATING... ");
    HD44780_SetCursor(0, 1); HD44780_PrintStr(" OPEN PROBES    ");
    for (uint8_t i = 0; i < 32; i++) { ADC_Read(); HAL_Delay(2); }
    uint32_t acc = 0;
    for (uint16_t i = 0; i < NUM_CAL_SAMPLES; i++) { acc += ADC_Read(); HAL_Delay(1); }
    uint16_t mean = (uint16_t)((acc + (NUM_CAL_SAMPLES / 2u)) / NUM_CAL_SAMPLES);
    adcOffset = (mean < MAX_ALLOWED_OFFSET) ? mean : 0u;
    HD44780_Clear();
}

/* ===========================================================================
   BUTTON HELPERS -- rising edge for mode buttons (PB6/7/8)
   =========================================================================== */
static uint8_t Button_RisingEdge(GPIO_TypeDef *port, uint16_t pin, BtnState_t *btn)
{
    uint8_t  state = (HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_SET) ? 1u : 0u;
    uint32_t now   = HAL_GetTick();
    if (state != btn->lastState && (now - btn->lastTick) > MODE_BTN_DEBOUNCE_MS)
    {
        btn->lastState = state;
        btn->lastTick  = now;
        if (state == 1u) return 1;
    }
    return 0;
}

/* ===========================================================================
   BT SWITCH STATE -- reads live pin state, no edge detection needed.
   PC13 connected to GND via switch + internal PULLUP:
     Switch ON  (closed) --> pin LOW  --> returns 1 (BT enabled)
     Switch OFF (open)   --> pin HIGH --> returns 0 (BT disabled)
   =========================================================================== */
static uint8_t BT_Switch_IsOn(void)
{
    return (HAL_GPIO_ReadPin(BT_SW_PORT, BT_SW_PIN) == GPIO_PIN_RESET) ? 1u : 0u;
}

/* ===========================================================================
   LCD DISPLAY -- only writes when value changes
   =========================================================================== */

/* PB6: Voltage -- Row0: "   VOLTAGE      "  Row1: "   X.XXXX V     " */
static void LCD_Display_Voltage(float voltage)
{
    if (voltage < 0.0f)  voltage = 0.0f;
    if (voltage > 16.0f) voltage = 16.0f;

    uint32_t whole = (uint32_t)voltage;
    uint32_t frac  = (uint32_t)((voltage - (float)whole) * 10000.0f + 0.5f);
    if (frac >= 10000) { whole++; frac = 0; }

    if (whole == lastV_whole && frac == lastV_frac) return;
    lastV_whole = whole;
    lastV_frac  = frac;

    HD44780_SetCursor(0, 0);
    HD44780_PrintStr("   VOLTAGE      ");
    snprintf(lcdBuf, sizeof(lcdBuf), "   %u.%04u V     ", (unsigned)whole, (unsigned)frac);
    lcdBuf[16] = '\0';
    HD44780_SetCursor(0, 1);
    HD44780_PrintStr(lcdBuf);
}

/* PB7: mA -- Row0: "   CURRENT      "  Row1: "   X.XXX mA     " */
static void LCD_Display_mA(float mA)
{
    if (mA < 0.0f) mA = 0.0f;

    uint32_t whole = (uint32_t)mA;
    uint32_t frac  = (uint32_t)((mA - (float)whole) * 1000.0f + 0.5f);
    if (frac >= 1000) { whole++; frac = 0; }

    if (whole == lastMA_whole && frac == lastMA_frac) return;
    lastMA_whole = whole;
    lastMA_frac  = frac;

    HD44780_SetCursor(0, 0);
    HD44780_PrintStr("   CURRENT      ");
    snprintf(lcdBuf, sizeof(lcdBuf), "   %u.%03u mA     ", (unsigned)whole, (unsigned)frac);
    lcdBuf[16] = '\0';
    HD44780_SetCursor(0, 1);
    HD44780_PrintStr(lcdBuf);
}

/* PB8: uA -- Row0: "   CURRENT      "  Row1: "   XX.XX uA     "
              or    "   OVERLOAD!    " if > 99uA                             */
static void LCD_Display_uA(float uA)
{
    if (uA < 0.0f) uA = 0.0f;

    uint8_t overload = (uA > UA_OVERLOAD_THRESHOLD) ? 1u : 0u;

    if (overload)
    {
        if (lastOvl == 1u) return;
        lastOvl      = 1u;
        lastUA_whole = 0xFFFFFFFFu;
        HD44780_SetCursor(0, 0);
        HD44780_PrintStr("   CURRENT      ");
        HD44780_SetCursor(0, 1);
        HD44780_PrintStr("   OVERLOAD!    ");
        return;
    }

    uint32_t whole = (uint32_t)uA;
    uint32_t frac  = (uint32_t)((uA - (float)whole) * 100.0f + 0.5f);
    if (frac >= 100) { whole++; frac = 0; }

    if (whole == lastUA_whole && frac == lastUA_frac && lastOvl == 0u) return;
    lastUA_whole = whole;
    lastUA_frac  = frac;
    lastOvl      = 0u;

    HD44780_SetCursor(0, 0);
    HD44780_PrintStr("   CURRENT      ");
    snprintf(lcdBuf, sizeof(lcdBuf), "   %2u.%02u uA     ", (unsigned)whole, (unsigned)frac);
    lcdBuf[16] = '\0';
    HD44780_SetCursor(0, 1);
    HD44780_PrintStr(lcdBuf);
}

/* ===========================================================================
   BLUETOOTH SEND
   =========================================================================== */
static void BT_SendReading(void)
{
    uint32_t v_w = (uint32_t)Vo;
    uint32_t v_f = (uint32_t)((Vo - (float)v_w) * 10000.0f + 0.5f);
    if (v_f >= 10000) { v_w++; v_f = 0; }

    uint32_t ma_w = (uint32_t)ImA;
    uint32_t ma_f = (uint32_t)((ImA - (float)ma_w) * 1000.0f + 0.5f);
    if (ma_f >= 1000) { ma_w++; ma_f = 0; }

    uint32_t ua_w = (uint32_t)IuA;
    uint32_t ua_f = (uint32_t)((IuA - (float)ua_w) * 100.0f + 0.5f);
    if (ua_f >= 100) { ua_w++; ua_f = 0; }

    switch (currentMode)
    {
        default:
        case MODE_VOLTS:
            snprintf(btBuf, sizeof(btBuf),
                     "Vo: %u.%04u V\r\n",
                     (unsigned)v_w, (unsigned)v_f);
            break;
        case MODE_MA:
            snprintf(btBuf, sizeof(btBuf),
                     "I: %u.%03u mA\r\n",
                     (unsigned)ma_w, (unsigned)ma_f);
            break;
        case MODE_UA:
            if (IuA > UA_OVERLOAD_THRESHOLD)
                snprintf(btBuf, sizeof(btBuf), "I: OVERLOAD >99uA\r\n");
            else
                snprintf(btBuf, sizeof(btBuf),
                         "I: %u.%02u uA\r\n",
                         (unsigned)ua_w, (unsigned)ua_f);
            break;
    }

    HAL_UART_Transmit(&huart1, (uint8_t *)btBuf, strlen(btBuf), BT_TX_TIMEOUT_MS);
}

/* ====
   main()
   === */
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_I2C2_Init();
    MX_ADC1_Init();
    MX_TIM2_Init();
    MX_USART1_UART_Init();

    HD44780_Init(2);

    if (adcCalDone != MAGIC_INIT)
    {
        ADC_CalibrateOffset();
        adcCalDone = MAGIC_INIT;
    }

    bool ina_ok = INA226_Init(&ina226, &hi2c2, INA226_ADDRESS);
    if (!ina_ok)
    {
        HD44780_Clear();
        HD44780_SetCursor(0, 0); HD44780_PrintStr("INA226 ERROR!   ");
        HD44780_SetCursor(0, 1); HD44780_PrintStr("Check wiring    ");
        HAL_UART_Transmit(&huart1,
            (uint8_t *)"INA226 INIT FAILED\r\n", 20, BT_TX_TIMEOUT_MS);
        while (1) {}
    }

    /* Splash */
    HD44780_Clear();
    HD44780_SetCursor(0, 0); HD44780_PrintStr("  V & I METER   ");
    HD44780_SetCursor(0, 1); HD44780_PrintStr("  Initializing  ");
    HAL_Delay(1500);
    HD44780_Clear();
    LCD_InvalidateCache();

    HAL_UART_Transmit(&huart1,
        (uint8_t *)"== V&I Meter Ready ==\r\n", 23, BT_TX_TIMEOUT_MS);

    lastBtSendTick = HAL_GetTick();

    /* ---- Main loop ------------------------------------------------------- */
    while (1)
    {
        /* 1. Mode buttons (PB6/PB7/PB8) ------------------------------------ */
        if (Button_RisingEdge(MODE_BTN_PORT, MODE_BTN_V_PIN,  &btnV))
            currentMode = MODE_VOLTS;
        if (Button_RisingEdge(MODE_BTN_PORT, MODE_BTN_MA_PIN, &btnMA))
            currentMode = MODE_MA;
        if (Button_RisingEdge(MODE_BTN_PORT, MODE_BTN_UA_PIN, &btnUA))
            currentMode = MODE_UA;

        /* Clear LCD and redraw if mode changed */
        if (currentMode != lastDisplayMode)
        {
            HD44780_Clear();
            LCD_InvalidateCache();
            lastDisplayMode = currentMode;
        }

        /* 2. ADC -> Voltage ------------------------------------------------ */
        uint16_t adcAvg       = ADC_ReadAveraged(NUM_LIVE_SAMPLES);
        uint16_t adcCorrected = (adcAvg > adcOffset) ? (adcAvg - adcOffset) : 0u;
        if (adcCorrected < ADC_DEADBAND_COUNTS) adcCorrected = 0u;

        Vin = (adcCorrected / 4095.0f) * 3.313f;
        Vo  = Vin * ((9906.0f + 39010.0f) / 9906.0f);

        /* 3. INA226 -> Current --------------------------------------------- */
        int16_t cur_raw = INA226_ReadCurrent_raw(&ina226);
        ImA = (float)cur_raw * INA226_LSB_TO_MA;
        IuA = (float)cur_raw * INA226_LSB_TO_UA;
        if (ImA < 0.0f) ImA = 0.0f;
        if (IuA < 0.0f) IuA = 0.0f;

        /* 4. LCD update ---------------------------------------------------- */
        switch (currentMode)
        {
            default:
            case MODE_VOLTS: LCD_Display_Voltage(Vo);  break;
            case MODE_MA:    LCD_Display_mA(ImA);       break;
            case MODE_UA:    LCD_Display_uA(IuA);       break;
        }

        /* 5. Bluetooth -- transmit only while the PC13 switch is ON (LOW).
         *    The live pin state is sampled every loop. While the switch is
         *    OFF the interval timer is held reset, so switching back ON
         *    does not immediately release a queued frame.                 */
        if (BT_Switch_IsOn())
        {
            if ((HAL_GetTick() - lastBtSendTick) >= BT_AUTO_INTERVAL_MS)
            {
                BT_SendReading();
                lastBtSendTick = HAL_GetTick();
            }
        }
        else
        {
            /* Switch is OFF -- keep resetting the timer so when it turns ON
               it waits a full 5 seconds before the first send              */
            lastBtSendTick = HAL_GetTick();
        }

        /* Loop cadence. 500 ms gives a calm, readable ~2 Hz refresh on the
           character LCD and keeps I2C traffic light. The INA226 continues
           converting independently at its own rate (16-sample average,
           1.1 ms per conversion), so this delay governs only how often a
           result is presented, not how often the sensor measures.        */
        HAL_Delay(500);
    }
}

/* ===========================================================================
   PERIPHERAL INIT
   =========================================================================== */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);
    RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState            = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_NONE;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();
    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK  | RCC_CLOCKTYPE_SYSCLK
                                     | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK) Error_Handler();
}

static void MX_ADC1_Init(void)
{
    ADC_ChannelConfTypeDef sConfig = {0};
    hadc1.Instance                   = ADC1;
    hadc1.Init.ClockPrescaler        = ADC_CLOCK_SYNC_PCLK_DIV2;
    hadc1.Init.Resolution            = ADC_RESOLUTION_12B;
    hadc1.Init.ScanConvMode          = DISABLE;
    hadc1.Init.ContinuousConvMode    = ENABLE;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc1.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
    hadc1.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    hadc1.Init.NbrOfConversion       = 1;
    hadc1.Init.DMAContinuousRequests = DISABLE;
    hadc1.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;
    if (HAL_ADC_Init(&hadc1) != HAL_OK) Error_Handler();
    sConfig.Channel      = ADC_CHANNEL_9;
    sConfig.Rank         = 1;
    sConfig.SamplingTime = ADC_SAMPLETIME_3CYCLES;
    if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) Error_Handler();
}

static void MX_I2C2_Init(void)
{
    hi2c2.Instance             = I2C2;
    hi2c2.Init.ClockSpeed      = 100000;
    hi2c2.Init.DutyCycle       = I2C_DUTYCYCLE_2;
    hi2c2.Init.OwnAddress1     = 0;
    hi2c2.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    hi2c2.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c2.Init.OwnAddress2     = 0;
    hi2c2.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c2.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&hi2c2) != HAL_OK) Error_Handler();
}

static void MX_TIM2_Init(void)
{
    TIM_ClockConfigTypeDef  sClockSourceConfig = {0};
    TIM_MasterConfigTypeDef sMasterConfig      = {0};
    TIM_OC_InitTypeDef      sConfigOC          = {0};
    htim2.Instance               = TIM2;
    htim2.Init.Prescaler         = 0;
    htim2.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim2.Init.Period            = 4294967295;
    htim2.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_Base_Init(&htim2) != HAL_OK) Error_Handler();
    sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
    if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK) Error_Handler();
    if (HAL_TIM_PWM_Init(&htim2) != HAL_OK) Error_Handler();
    sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
    sMasterConfig.MasterSlaveMode     = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK) Error_Handler();
    sConfigOC.OCMode     = TIM_OCMODE_PWM1;
    sConfigOC.Pulse      = 0;
    sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
    sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_1) != HAL_OK) Error_Handler();
    HAL_TIM_MspPostInit(&htim2);
}

static void MX_USART1_UART_Init(void)
{
    huart1.Instance          = USART1;
    huart1.Init.BaudRate     = 9600;
    huart1.Init.WordLength   = UART_WORDLENGTH_8B;
    huart1.Init.StopBits     = UART_STOPBITS_1;
    huart1.Init.Parity       = UART_PARITY_NONE;
    huart1.Init.Mode         = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart1) != HAL_OK) Error_Handler();
}

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* PC13 -- ON/OFF switch to GND, internal PULLUP
       LOW = switch closed = BT ON
       HIGH = switch open  = BT OFF                                          */
    GPIO_InitStruct.Pin  = GPIO_PIN_13;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    /* PB6/PB7/PB8 -- mode buttons, active HIGH pulse, PULLDOWN */
    GPIO_InitStruct.Pin  = MODE_BTN_V_PIN | MODE_BTN_MA_PIN | MODE_BTN_UA_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(MODE_BTN_PORT, &GPIO_InitStruct);
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) {}
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {}
#endif
