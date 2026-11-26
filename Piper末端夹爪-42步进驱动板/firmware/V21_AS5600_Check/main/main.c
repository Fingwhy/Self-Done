#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum {
    AS5600_ADDRESS = 0x36,
    STATUS_REG = 0x0b,
    AGC_REG = 0x1a,
    MAGNET_DETECTED = 1u << 5,
    MAGNET_TOO_WEAK = 1u << 4,
    MAGNET_TOO_STRONG = 1u << 3,
    I2C_TIMEOUT_MS = 50,
};

#define SDA_PIN GPIO_NUM_19
#define SCL_PIN GPIO_NUM_23
#define MOTOR_ENN_PIN GPIO_NUM_27

static const char *TAG = "as5600_check";

static esp_err_t read_registers(i2c_master_dev_handle_t sensor, uint8_t address,
                                uint8_t *data, size_t length)
{
    return i2c_master_transmit_receive(sensor, &address, 1, data, length, I2C_TIMEOUT_MS);
}

static unsigned read_u12(const uint8_t data[2])
{
    return ((unsigned)(data[0] & 0x0f) << 8) | data[1];
}

void app_main(void)
{
    // Release the open drain so that the V21 pull-up keeps ENN high.
    ESP_ERROR_CHECK(gpio_set_level(MOTOR_ENN_PIN, 1));
    ESP_ERROR_CHECK(gpio_set_direction(MOTOR_ENN_PIN, GPIO_MODE_OUTPUT_OD));

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = SDA_PIN,
        .scl_io_num = SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };
    const i2c_device_config_t sensor_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AS5600_ADDRESS,
        .scl_speed_hz = 100000,
    };
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t sensor;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &sensor_config, &sensor));

    ESP_LOGI(TAG, "I2C 100 kHz, SDA=D19 SCL=D23, address=0x36, motor disabled");
    for (;;) {
        // STATUS followed by RAW ANGLE high and low.
        uint8_t status_angle[3];
        esp_err_t err = read_registers(sensor, STATUS_REG, status_angle, sizeof(status_angle));
        if (err != ESP_OK) {
            esp_err_t probe = i2c_master_probe(bus, AS5600_ADDRESS, I2C_TIMEOUT_MS);
            ESP_LOGW(TAG, "0x36 probe=%s, STATUS/RAW ANGLE read=%s",
                     esp_err_to_name(probe), esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        const unsigned status = status_angle[0];
        const unsigned raw = read_u12(&status_angle[1]);
        const bool detected = (status & MAGNET_DETECTED) != 0;
        const bool weak = (status & MAGNET_TOO_WEAK) != 0;
        const bool strong = (status & MAGNET_TOO_STRONG) != 0;
        const char *quality = detected && !weak && !strong ? "VALID" : "INVALID";
        ESP_LOGI(TAG, "STATUS=0x%02X MD=%d ML=%d MH=%d raw=%u angle=%.2f deg %s",
                 status, detected, weak, strong, raw, raw * (360.0 / 4096.0), quality);

        // AGC followed by MAGNITUDE high and low.
        uint8_t field[3];
        err = read_registers(sensor, AGC_REG, field, sizeof(field));
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "AGC=%u MAG=%u", (unsigned)field[0], read_u12(&field[1]));
        } else {
            ESP_LOGW(TAG, "AGC/MAGNITUDE read=%s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
