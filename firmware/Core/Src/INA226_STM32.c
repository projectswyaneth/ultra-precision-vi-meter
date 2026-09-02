/**
 * INA226_STM32.c — Bare-metal STM32 HAL driver for INA226
 * See INA226_STM32.h for full documentation.
 */

#include "INA226_STM32.h"

/* ── Private helpers ─────────────────────────────────────────────────── */

static HAL_StatusTypeDef _writeReg(INA226_t *dev, uint8_t reg, uint16_t value)
{
    uint8_t buf[3];
    buf[0] = reg;
    buf[1] = (uint8_t)(value >> 8);
    buf[2] = (uint8_t)(value & 0xFF);
    return HAL_I2C_Master_Transmit(dev->hi2c,
                                   (uint16_t)(dev->address << 1),
                                   buf, 3, 10);
}

static uint16_t _readReg(INA226_t *dev, uint8_t reg)
{
    uint8_t cmd  = reg;
    uint8_t data[2] = {0, 0};

    if (HAL_I2C_Master_Transmit(dev->hi2c,
                                (uint16_t)(dev->address << 1),
                                &cmd, 1, 10) != HAL_OK)
        return 0;

    if (HAL_I2C_Master_Receive(dev->hi2c,
                               (uint16_t)(dev->address << 1) | 1u,
                               data, 2, 10) != HAL_OK)
        return 0;

    return (uint16_t)((data[0] << 8) | data[1]);
}

/* ── Public API ──────────────────────────────────────────────────────── */

bool INA226_Init(INA226_t *dev, I2C_HandleTypeDef *hi2c, uint8_t addr)
{
    dev->hi2c        = hi2c;
    dev->address     = addr;
    dev->initialized = false;

    if (!INA226_IsConnected(dev)) return false;

    /* Reset first */
    _writeReg(dev, INA226_REG_CONFIG, 0x8000u);
    HAL_Delay(2);

    /* Set configuration: 16-sample average for better noise rejection */
    _writeReg(dev, INA226_REG_CONFIG, INA226_CONFIG_DEFAULT);

    /*
     * Calibration:
     *   Cal = 0.00512 / (CurrentLSB × Rshunt)
     *       = 0.00512 / (25×10⁻⁶ × 0.1)
     *       = 2048
     *
     * This sets Current_LSB = 25µA per bit.
     * Raw current register × 25µA = actual current.
     */
    _writeReg(dev, INA226_REG_CALIBRATION, INA226_CAL_VALUE);

    dev->initialized = true;
    return true;
}

bool INA226_IsConnected(INA226_t *dev)
{
    return (HAL_I2C_IsDeviceReady(dev->hi2c,
                                  (uint16_t)(dev->address << 1),
                                  2, 10) == HAL_OK);
}

int16_t INA226_ReadCurrent_raw(INA226_t *dev)
{
    return (int16_t)_readReg(dev, INA226_REG_CURRENT);
}

float INA226_ReadCurrent_uA(INA226_t *dev)
{
    int16_t raw = INA226_ReadCurrent_raw(dev);
    if (raw < 0) return 0.0f;
    return (float)raw * INA226_CURRENT_LSB_UA;   /* raw × 25µA */
}

float INA226_ReadCurrent_mA(INA226_t *dev)
{
    return INA226_ReadCurrent_uA(dev) * 0.001f;
}

float INA226_ReadBusVoltage(INA226_t *dev)
{
    uint16_t raw = _readReg(dev, INA226_REG_BUS_V);
    return (float)raw * 1.25e-3f;   /* 1.25 mV per bit, fixed */
}

float INA226_ReadShuntVoltage_mV(INA226_t *dev)
{
    int16_t raw = (int16_t)_readReg(dev, INA226_REG_SHUNT_V);
    return (float)raw * 2.5e-3f;    /* 2.5 µV per bit → result in mV */
}

bool INA226_IsConversionReady(INA226_t *dev)
{
    return (_readReg(dev, INA226_REG_MASK_ENABLE) & INA226_CONV_READY_FLAG) != 0;
}
