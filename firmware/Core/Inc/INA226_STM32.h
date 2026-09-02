#pragma once
/**
 * INA226_STM32.h -- Minimal STM32 HAL driver for the TI INA226
 *                   precision current / power monitor.
 *
 * Sense resistor : 0.1 ohm
 * Shunt LSB      : 2.5 uV per bit (fixed by the device)
 * Current LSB    : 2.5 uV / 0.1 ohm = 25 uA per raw bit
 *
 * Resolution note:
 *   With a 0.1 ohm sense resistor the smallest resolvable step is 25 uA.
 *   Reaching 1 uA per bit would require a sense resistor of 2.5 ohm or
 *   greater, trading resolution against burden voltage and dissipation.
 *
 * Calibration register:
 *   Cal = 0.00512 / (CurrentLSB * Rshunt)
 *       = 0.00512 / (25e-6 * 0.1)
 *       = 2048
 *
 * Full scale before the current register saturates:
 *   32767 bits * 25 uA = 819.175 mA, comfortably inside the 81.92 mV
 *   differential limit of the 0.1 ohm shunt.
 *
 * I2C address: 0x44 (A1 tied to VS+, A0 tied to GND).
 */

#include "stm32f4xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

/* ── INA226 Register Map ─────────────────────────────────────────────── */
#define INA226_REG_CONFIG       0x00
#define INA226_REG_SHUNT_V      0x01
#define INA226_REG_BUS_V        0x02
#define INA226_REG_POWER        0x03
#define INA226_REG_CURRENT      0x04
#define INA226_REG_CALIBRATION  0x05
#define INA226_REG_MASK_ENABLE  0x06
#define INA226_REG_ALERT_LIMIT  0x07
#define INA226_REG_MFR_ID       0xFE
#define INA226_REG_DIE_ID       0xFF

/* ── Default config: 16 samples avg, 1.1ms BVCT, 1.1ms SVCT, continuous ─ */
/*   Bits[11:9]=100 (16 avg), Bits[8:6]=100 (1.1ms BV), Bits[5:3]=100 (1.1ms SV), Bits[2:0]=111 (cont) */
#define INA226_CONFIG_DEFAULT   0x4527u

#define INA226_ADDRESS          0x44u   /* A1 = VS+, A0 = GND */

/* ── Calibration for 0.1Ω shunt, CurrentLSB=25µA ───────────────────── */
#define INA226_SHUNT_OHM        0.1f
#define INA226_CURRENT_LSB_UA   25.0f   /* µA per raw bit */
#define INA226_CURRENT_LSB_A    25e-6f  /* A   per raw bit */
#define INA226_CAL_VALUE        2048u   /* 0.00512 / (25e-6 × 0.1) */

/* ── Conversion ready flag ───────────────────────────────────────────── */
#define INA226_CONV_READY_FLAG  0x0008u

typedef struct {
    I2C_HandleTypeDef *hi2c;
    uint8_t            address;
    bool               initialized;
} INA226_t;

/* ── Public API ──────────────────────────────────────────────────────── */
bool     INA226_Init(INA226_t *dev, I2C_HandleTypeDef *hi2c, uint8_t addr);
bool     INA226_IsConnected(INA226_t *dev);

/**
 * Read raw current register (signed 16-bit).
 * Multiply by INA226_CURRENT_LSB_UA to get µA.
 * e.g. raw=40 → 40 × 25µA = 1000µA = 1.000mA
 */
int16_t  INA226_ReadCurrent_raw(INA226_t *dev);

/**
 * Read current in µA (float).
 * Resolution: 25µA steps with 0.1Ω shunt.
 */
float    INA226_ReadCurrent_uA(INA226_t *dev);

/**
 * Read current in mA (float).
 */
float    INA226_ReadCurrent_mA(INA226_t *dev);

/**
 * Read bus voltage in Volts. LSB = 1.25mV fixed.
 */
float    INA226_ReadBusVoltage(INA226_t *dev);

/**
 * Read shunt voltage in mV. LSB = 2.5µV fixed.
 */
float    INA226_ReadShuntVoltage_mV(INA226_t *dev);

bool     INA226_IsConversionReady(INA226_t *dev);
