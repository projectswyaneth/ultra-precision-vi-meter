# Firmware Technical Overview

A file-by-file reference for the firmware in `firmware/Core/`. The application is
bare-metal on the STM32Cube HAL — no RTOS, no dynamic allocation, and no
interrupt-driven measurement path. Everything happens in one cooperative loop in
`main.c`.

---

## Application layer

### `Src/main.c`

The whole instrument. Roughly 600 lines covering peripheral setup, the
measurement pipeline, display formatting and telemetry.

**Startup sequence**

1. `HAL_Init()`, then `SystemClock_Config()` selects the 16 MHz HSI directly as
   the system clock with the PLL disabled, voltage scale 2 and flash latency 0.
   The instrument has no need for speed, and running off the HSI without the PLL
   keeps the clock tree — and its noise contribution to the ADC — as simple as
   possible.
2. `MX_GPIO_Init`, `MX_I2C2_Init`, `MX_ADC1_Init`, `MX_TIM2_Init` and
   `MX_USART1_UART_Init` bring up the peripherals.
3. `HD44780_Init(2)` initialises the LCD in two-line mode.
4. **First-boot calibration.** If the `.noinit` variable `adcCalDone` does not
   hold the magic value `0xA55A5AA5`, `ADC_CalibrateOffset()` runs and the magic
   is written. Because `.noinit` is excluded from startup zeroing, the stored
   offset survives a reset and calibration only repeats on a cold start.
5. `INA226_Init()` probes the sensor. Failure is terminal — the LCD shows
   `INA226 ERROR! / Check wiring`, the same message goes out over Bluetooth, and
   the firmware halts rather than displaying invented numbers.
6. A splash screen, a `== V&I Meter Ready ==` banner over UART, and the loop
   begins.

**The main loop**

Each pass, in order: scan the three mode buttons; if the mode changed, clear the
LCD and invalidate the redraw cache; take an averaged ADC reading and convert it
to volts; read the INA226 current register and scale it to mA and µA; update the
LCD for the active mode; service the Bluetooth interval. Then `HAL_Delay(500)`.

**Key routines**

| Function | Role |
|---|---|
| `ADC_Read()` | One polled conversion, start to stop |
| `ADC_ReadAveraged(n)` | Mean of *n* conversions with round-half-up |
| `ADC_CalibrateOffset()` | 32 discarded warm-up reads, then a 512-sample mean stored as the zero offset |
| `Button_RisingEdge()` | Debounced rising-edge detection, 50 ms guard |
| `BT_Switch_IsOn()` | Live level read of `PC13` (LOW = enabled) |
| `LCD_Display_Voltage/_mA/_uA()` | Format and conditionally redraw one field |
| `LCD_InvalidateCache()` | Force a full redraw after a mode change |
| `BT_SendReading()` | Compose and transmit one ASCII telemetry frame |

**Numeric formatting.** The firmware avoids `%f` entirely. Each value is split
into whole and fractional integer parts with explicit rounding
(`+ 0.5f` before truncation) and a carry check, so `4.99997` prints as `5.0000`
rather than `4.9999`. This keeps `snprintf` free of floating-point formatting
support, which on a Cortex-M is both large and slow.

**Display caching.** Every field keeps a copy of the digits last written. A
redraw is skipped unless those digits change, so a stable reading produces no
I²C traffic. Since the LCD shares its bus with the INA226, this directly protects
sensor read latency.

**Constants of interest**

| Symbol | Value | Meaning |
|---|---|---|
| `NUM_CAL_SAMPLES` | 512 | Samples averaged during zero calibration |
| `NUM_LIVE_SAMPLES` | 16 | Samples averaged per displayed reading |
| `MAX_ALLOWED_OFFSET` | 100 | Calibration sanity ceiling, in ADC counts |
| `ADC_DEADBAND_COUNTS` | 4 | Counts below which a reading clamps to zero |
| `INA226_LSB_TO_MA` | 0.025 | Raw current register bit → milliamps |
| `INA226_LSB_TO_UA` | 25.0 | Raw current register bit → microamps |
| `UA_OVERLOAD_THRESHOLD` | 99.0 | µA above which `OVERLOAD!` is shown |
| `MODE_BTN_DEBOUNCE_MS` | 50 | Button debounce guard window |
| `BT_AUTO_INTERVAL_MS` | 5000 | Telemetry period while enabled |

---

## Drivers

### `Src/INA226_STM32.c` and `Inc/INA226_STM32.h`

A small purpose-built driver for the TI INA226, written against the HAL I²C API
rather than pulled from a library. It carries the full register map, the
calibration arithmetic in comments, and a compact public interface.

Initialisation issues a soft reset (`CONFIG = 0x8000`), waits 2 ms, writes the
working configuration `0x4527` (16-sample average, 1.1 ms bus and shunt
conversions, continuous mode) and then the calibration value `2048`, which fixes
the current LSB at 25 µA.

| Function | Returns |
|---|---|
| `INA226_Init()` | `false` if the device does not acknowledge |
| `INA226_IsConnected()` | Bus-level presence check |
| `INA226_ReadCurrent_raw()` | Signed 16-bit current register |
| `INA226_ReadCurrent_uA/_mA()` | Scaled float current |
| `INA226_ReadBusVoltage()` | Bus voltage, 1.25 mV LSB |
| `INA226_ReadShuntVoltage_mV()` | Shunt voltage, 2.5 µV LSB |
| `INA226_IsConversionReady()` | Mask/enable conversion-ready flag |

Register access goes through two static helpers that handle the big-endian
16-bit word order the device uses. Read failures return `0` rather than
propagating an error code — acceptable here because the device's presence is
validated once at startup and a hard failure halts the instrument.

The application reads only the raw current register in its loop, then scales it
twice locally. The bus-voltage and shunt-voltage accessors are available for
extension but unused by the current display modes.

### `Src/liquidcrystal_i2c.c` and `Inc/liquidcrystal_i2c.h`

HD44780 character LCD driven through a PCF8574 I²C expander at address `0x27`,
in 4-bit mode. A port of the widely used Arduino `LiquidCrystal_I2C` interface
onto the STM32 HAL, exposing the familiar `HD44780_*` calls: `Init`, `Clear`,
`SetCursor`, `PrintStr`, backlight control, cursor and blink control, scrolling,
and custom character loading.

The application uses only `Init`, `Clear`, `SetCursor` and `PrintStr`; the rest
of the surface is left intact for reuse.

---

## Generated support files

These come from STM32CubeMX and are included so the project builds as committed.

| File | Purpose |
|---|---|
| `Src/stm32f4xx_hal_msp.c` | Per-peripheral pin, clock and interrupt setup: `PB1` analog for ADC1; `PB10`/`PB3` for I2C2 in open-drain alternate function; `PA9`/`PA10` for USART1; TIM2 clock enable |
| `Src/stm32f4xx_it.c` and `Inc/stm32f4xx_it.h` | Cortex-M fault and system handlers plus the SysTick and USART1 vectors |
| `Src/system_stm32f4xx.c` | CMSIS system initialisation and `SystemCoreClock` bookkeeping |
| `Src/syscalls.c` | Minimal newlib syscall stubs (`_write`, `_read`, `_sbrk` support and friends) |
| `Src/sysmem.c` | `_sbrk` heap implementation for newlib |
| `Inc/stm32f4xx_hal_conf.h` | HAL module selection — ADC, I2C, TIM, UART, GPIO, EXTI, DMA, RCC, FLASH, PWR and CORTEX are enabled; everything else is compiled out |
| `Inc/main.h` | Application-wide declarations and `Error_Handler()` |
| `Startup/startup_stm32f401ceux.s` | Vector table and reset handler, including the `.noinit` section placement the calibration store depends on |
| `Display.ioc` | The CubeMX project definition; reopening it regenerates the HAL and CMSIS trees |
| `STM32F401CEUX_FLASH.ld` / `STM32F401CEUX_RAM.ld` | Linker scripts for flash and RAM targets |

`TIM2` is initialised for PWM output on channel 1 as part of the CubeMX
configuration but is not driven by the application loop; it remains available for
future use such as LCD backlight dimming.

---

## Design notes

**Why the ADC is oversampled and the INA226 is not.** The INA226 already averages
16 samples in hardware before presenting a result, so the firmware reads it once.
The STM32's ADC has no such facility here, so the equivalent averaging is done in
software — 16 conversions per displayed reading, which improves signal-to-noise
by roughly √16 = 4.

**Why the zero offset lives in `.noinit`.** Calibration takes several hundred
milliseconds and requires open probes. Persisting the result across resets means
the instrument is usable immediately after a reset mid-measurement, while still
re-characterising itself on a genuine power cycle.

**Why the loop rests 500 ms.** A character LCD updating faster than a few hertz
is harder to read, not easier, and the extra I²C traffic competes with sensor
reads. The sensor continues converting independently throughout, so the delay
governs presentation rate rather than measurement rate. The trade-off is button
latency: a very brief press can fall between samples, so the buttons expect a
deliberate press rather than a tap.
