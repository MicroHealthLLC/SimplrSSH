/*
 * Battery Measurement: Tab5
 * The INA226 power monitor (I2C 0x41 on the system bus) measures the battery bus voltage
 * of the 2S NP-F550 pack: 8.4 V full, about 6.0 V at shutdown. Only the bus voltage
 * register is read, so no shunt calibration is needed.
 */

#include "battery_measurement.hpp"
#include "bsp/esp-bsp.h"

static const uint8_t INA226_ADDR = 0x41;
static const uint8_t REG_CONFIG = 0x00;
static const uint8_t REG_BUS_VOLTAGE = 0x02;       // 1.25 mV per bit
static const uint16_t CONFIG_AVG16_CONTINUOUS = 0x4527;  // 16 samples, 1.1 ms, shunt + bus continuous

// 2S Li-ion under light load
const std::vector<BatteryMeasurement::BatteryLevel> BatteryMeasurement::batteryCurve = {
    {8.30f, 100},
    {8.00f, 90},
    {7.70f, 75},
    {7.40f, 50},
    {7.20f, 25},
    {7.00f, 10},
    {6.40f, 0},
};

BatteryMeasurement::BatteryMeasurement() {}

BatteryMeasurement::~BatteryMeasurement()
{
    deinit();
}

esp_err_t BatteryMeasurement::init()
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_device_config_t dev_config = {};
    dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_config.device_address = INA226_ADDR;
    dev_config.scl_speed_hz = 400000;
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_config, &ina226);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "INA226 not reachable: %s", esp_err_to_name(ret));
        return ret;
    }
    uint8_t config[3] = {REG_CONFIG, CONFIG_AVG16_CONTINUOUS >> 8, CONFIG_AVG16_CONTINUOUS & 0xFF};
    ret = i2c_master_transmit(ina226, config, sizeof(config), 100);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "INA226 setup failed: %s", esp_err_to_name(ret));
        deinit();
    }
    return ret;
}

float BatteryMeasurement::readBatteryVoltage()
{
    if (!ina226) {
        return 0.0f;
    }
    uint8_t reg = REG_BUS_VOLTAGE;
    uint8_t value[2] = {};
    if (i2c_master_transmit_receive(ina226, &reg, 1, value, sizeof(value), 100) != ESP_OK) {
        ESP_LOGW(TAG, "INA226 read failed");
        return 0.0f;
    }
    return ((value[0] << 8) | value[1]) * 0.00125f;
}

int BatteryMeasurement::voltageToPercentage(float voltage)
{
    if (voltage >= batteryCurve.front().voltage) {
        return 100;
    }
    if (voltage <= batteryCurve.back().voltage) {
        return 0;
    }
    return interpolateVoltage(voltage);
}

int BatteryMeasurement::interpolateVoltage(float voltage)
{
    for (size_t i = 0; i < batteryCurve.size() - 1; ++i) {
        const auto &p1 = batteryCurve[i];
        const auto &p2 = batteryCurve[i + 1];
        if (voltage <= p1.voltage && voltage > p2.voltage) {
            return p1.percentage + static_cast<int>((voltage - p1.voltage) * (p2.percentage - p1.percentage) / (p2.voltage - p1.voltage));
        }
    }
    return 0;
}

void BatteryMeasurement::deinit()
{
    if (ina226) {
        i2c_master_bus_rm_device(ina226);
        ina226 = nullptr;
    }
}
