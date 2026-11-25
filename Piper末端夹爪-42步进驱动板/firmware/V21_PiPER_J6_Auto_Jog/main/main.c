#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define MOTOR_UART UART_NUM_2
#define STEP_PIN GPIO_NUM_25
#define DIR_PIN GPIO_NUM_26
#define EN_PIN GPIO_NUM_27
#define TX_PIN GPIO_NUM_32
#define RX_PIN GPIO_NUM_33

enum {
    TMC_SYNC = 0x05, TMC_ADDRESS = 0, TMC_REPLY_ADDRESS = 0xff,
    GCONF = 0x00, IFCNT = 0x02, IOIN = 0x06, IHOLD_IRUN = 0x10,
    VACTUAL = 0x22, CHOPCONF = 0x6c,
};

static uint8_t tmc_crc(const uint8_t *data, size_t length)
{
    uint8_t crc = 0;
    for (size_t index = 0; index < length; ++index) {
        uint8_t byte = data[index];
        for (int bit = 0; bit < 8; ++bit) {
            bool feedback = ((crc >> 7) ^ byte) & 1u;
            crc <<= 1;
            if (feedback) {
                crc ^= 0x07;
            }
            byte >>= 1;
        }
    }
    return crc;
}

static bool send_frame(const uint8_t *frame, size_t length)
{
    return uart_write_bytes(MOTOR_UART, frame, length) == (int)length &&
           uart_wait_tx_done(MOTOR_UART, pdMS_TO_TICKS(40)) == ESP_OK;
}

static bool read_register(uint8_t reg, uint32_t *value)
{
    uint8_t request[] = {TMC_SYNC, TMC_ADDRESS, reg, 0};
    request[3] = tmc_crc(request, 3);
    uart_flush_input(MOTOR_UART);
    if (!send_frame(request, sizeof(request))) {
        return false;
    }

    // J4.6 receives both the board's single-wire echo and the TMC2209 reply.
    uint8_t response[24];
    int count = uart_read_bytes(MOTOR_UART, response, sizeof(response), pdMS_TO_TICKS(40));
    for (int offset = 0; offset + 8 <= count; ++offset) {
        const uint8_t *frame = &response[offset];
        if (frame[0] == TMC_SYNC && frame[1] == TMC_REPLY_ADDRESS &&
            frame[2] == reg && frame[7] == tmc_crc(frame, 7)) {
            *value = ((uint32_t)frame[3] << 24) | ((uint32_t)frame[4] << 16) |
                     ((uint32_t)frame[5] << 8) | frame[6];
            return true;
        }
    }
    return false;
}

static bool write_register(uint8_t reg, uint32_t value)
{
    uint8_t frame[] = {
        TMC_SYNC, TMC_ADDRESS, reg | 0x80, value >> 24, value >> 16, value >> 8, value, 0,
    };
    frame[7] = tmc_crc(frame, 7);
    return send_frame(frame, sizeof(frame));
}

static bool configure_driver(void)
{
    uint32_t ioin, before, after, gconf, chopconf;
    if (!read_register(IOIN, &ioin) || (ioin >> 24) != 0x21) {
        puts("TMC2209 IOIN read failed. Motor disabled.");
        return false;
    }
    printf("IOIN = 0x%08lX\n", (unsigned long)ioin);
    if (!read_register(IFCNT, &before) || !read_register(GCONF, &gconf) ||
        !read_register(CHOPCONF, &chopconf)) {
        puts("TMC2209 configuration read failed. Motor disabled.");
        return false;
    }

    // External sense resistors, SpreadCycle, UART and register-controlled microsteps.
    gconf = (gconf & ~0x03u) | (1u << 2) | (1u << 6) | (1u << 7);
    // 1/8 microsteps, low sense voltage (0.180 V), TOFF = 4.
    chopconf = (chopconf & ~((0x0fu << 24) | 0x0fu)) | (5u << 24) | (1u << 17) | 4u;
    // R1/R2 = 0.110 ohm. IRUN = 9 gives approximately 0.30 A RMS.
    const uint32_t hold_run = (4u << 16) | (9u << 8) | 9u;
    if (!write_register(GCONF, gconf) || !write_register(CHOPCONF, chopconf) ||
        !write_register(IHOLD_IRUN, hold_run) || !write_register(VACTUAL, 0) ||
        !read_register(IFCNT, &after) || (uint8_t)(after - before) != 4) {
        puts("TMC2209 did not acknowledge all four writes. Motor disabled.");
        return false;
    }
    printf("IFCNT: %u -> %u. UART OK.\n", (unsigned)(uint8_t)before, (unsigned)(uint8_t)after);
    return true;
}

enum {
    TARGET_ID = 0x417,
    CONTROL_ID = 0x427,
    BASELINE_SETTLE_US = 250000,
    COMMAND_PAIR_US = 1000000,
    TARGET_TIMEOUT_US = 150000,
    MIN_TARGET_CHANGE = 64,
    STABLE_TARGET_FRAMES = 3,
    MIN_TARGET_RAW = 0x7FFF,
    MAX_TARGET_RAW = 0x91DF,
    MAX_PULSES = 32,
    STEP_PERIOD_US = 10000,
    ENABLE_SETTLE_US = 20000,
};

typedef struct {
    uint16_t id;
    uint16_t position;
    bool permit_motion;
    int64_t timestamp;
} j6_frame_t;

typedef enum { TAKE_BASELINE, WAIT_FOR_CHANGE, JOGGING } jog_phase_t;

typedef struct {
    jog_phase_t phase;
    uint16_t baseline;
    uint16_t candidate;
    unsigned stable_frames;
    unsigned pulses;
    int64_t enable_at;
    int64_t candidate_since;
    int64_t last_target;
    int64_t next_step;
} jog_state_t;

static bool receive_j6(twai_node_handle_t node,
                       const twai_rx_done_event_data_t *event, void *context)
{
    (void)event;
    uint8_t bytes[8];
    twai_frame_t frame = {.buffer = bytes, .buffer_len = sizeof(bytes)};
    if (twai_node_receive_from_isr(node, &frame) != ESP_OK ||
        frame.header.ide || frame.header.rtr || frame.header.fdf) {
        return false;
    }

    j6_frame_t received = {.id = frame.header.id};
    if (frame.header.id == TARGET_ID && frame.header.dlc == 8) {
        received.position = ((uint16_t)bytes[0] << 8) | bytes[1];
    } else if (frame.header.id == CONTROL_ID && frame.header.dlc == 2) {
        received.permit_motion = bytes[0] == 1 && bytes[1] == 1;
    } else {
        return false;
    }
    received.timestamp = esp_timer_get_time();
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR((QueueHandle_t)context, &received, &woken);
    return woken == pdTRUE;
}

static void stop_jog(jog_state_t *jog, const char *reason)
{
    gpio_set_level(STEP_PIN, 0);
    gpio_set_level(EN_PIN, 1);
    printf("%s. %u STEP pulses. Motor disabled.\n", reason, jog->pulses);
    *jog = (jog_state_t){0};
}

static void handle_j6(jog_state_t *jog, const j6_frame_t *frame, int64_t now)
{
    if (now - frame->timestamp >= TARGET_TIMEOUT_US) {
        return;
    }
    if (frame->id == CONTROL_ID) {
        if (!frame->permit_motion) {
            if (jog->phase != TAKE_BASELINE || jog->candidate_since != 0) {
                stop_jog(jog, "PiPER withdrew enable or sent another control state");
            }
        } else if (jog->phase == WAIT_FOR_CHANGE) {
            jog->enable_at = frame->timestamp;
        }
        return;
    }
    if (frame->position < MIN_TARGET_RAW || frame->position > MAX_TARGET_RAW) {
        if (jog->phase != TAKE_BASELINE || jog->candidate_since != 0) {
            stop_jog(jog, "Target outside the bench test range");
        }
        return;
    }

    jog->last_target = frame->timestamp;
    if (jog->phase == TAKE_BASELINE) {
        if (jog->candidate_since == 0 || jog->baseline != frame->position) {
            jog->baseline = frame->position;
            jog->candidate_since = frame->timestamp;
        } else if (frame->timestamp - jog->candidate_since >= BASELINE_SETTLE_US) {
            jog->phase = WAIT_FOR_CHANGE;
            jog->candidate_since = 0;
            printf("BASELINE raw=%04X. Waiting for a new target and PiPER enable.\n",
                   (unsigned)jog->baseline);
        }
        return;
    }
    if (jog->phase == JOGGING) {
        if (frame->position != jog->candidate) {
            stop_jog(jog, "Target changed during jog");
        }
        return;
    }

    int delta = (int)frame->position - (int)jog->baseline;
    if (delta > -MIN_TARGET_CHANGE && delta < MIN_TARGET_CHANGE) {
        jog->stable_frames = 0;
        jog->candidate_since = 0;
        return;
    }
    if (frame->position != jog->candidate || jog->stable_frames == 0) {
        jog->candidate = frame->position;
        jog->stable_frames = 1;
        jog->candidate_since = frame->timestamp;
        return;
    }
    if (jog->stable_frames < STABLE_TARGET_FRAMES) {
        ++jog->stable_frames;
    }
    if (jog->enable_at == 0 || jog->stable_frames < STABLE_TARGET_FRAMES ||
        now - jog->enable_at > COMMAND_PAIR_US ||
        now - jog->candidate_since > COMMAND_PAIR_US) {
        return;
    }

    printf("JOG raw=%04X -> %04X, DIR=%u, limit=%u pulses\n",
           (unsigned)jog->baseline, (unsigned)jog->candidate, delta > 0 ? 1u : 0u,
           (unsigned)MAX_PULSES);
    gpio_set_level(DIR_PIN, delta > 0);
    gpio_set_level(EN_PIN, 0);
    jog->enable_at = 0;
    jog->phase = JOGGING;
    jog->next_step = esp_timer_get_time() + ENABLE_SETTLE_US;
}

void app_main(void)
{
    ESP_ERROR_CHECK(gpio_set_level(EN_PIN, 1));
    ESP_ERROR_CHECK(gpio_set_direction(EN_PIN, GPIO_MODE_OUTPUT_OD));
    ESP_ERROR_CHECK(gpio_set_level(STEP_PIN, 0));
    ESP_ERROR_CHECK(gpio_set_direction(STEP_PIN, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(DIR_PIN, 0));
    ESP_ERROR_CHECK(gpio_set_direction(DIR_PIN, GPIO_MODE_OUTPUT));

    const uart_config_t uart_config = {
        .baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(MOTOR_UART, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(MOTOR_UART, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(MOTOR_UART, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    uint32_t ioin;
    while (!configure_driver() || !read_register(IOIN, &ioin) || (ioin & 1u) == 0) {
        puts("Waiting for TMC2209 with ENN high. Motor disabled.");
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    QueueHandle_t queue = xQueueCreate(32, sizeof(j6_frame_t));
    ESP_ERROR_CHECK(queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    const twai_onchip_node_config_t can_config = {
        .io_cfg = {
            .tx = GPIO_NUM_NC, .rx = GPIO_NUM_22,
            .quanta_clk_out = GPIO_NUM_NC, .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing = {.bitrate = 1000000},
        .flags = {.enable_listen_only = true},
    };
    twai_node_handle_t node = NULL;
    ESP_ERROR_CHECK(twai_new_node_onchip(&can_config, &node));
    const twai_event_callbacks_t callbacks = {.on_rx_done = receive_j6};
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node, &callbacks, queue));
    ESP_ERROR_CHECK(twai_node_enable(node));

    puts("J6 AUTO JOG READY: node 7, CAN listen-only");
    jog_state_t jog = {0};
    for (;;) {
        j6_frame_t frame;
        bool received = xQueueReceive(queue, &frame, 1) == pdTRUE;
        int64_t now = esp_timer_get_time();
        if (jog.last_target != 0 && now - jog.last_target >= TARGET_TIMEOUT_US) {
            stop_jog(&jog, "CAN target stream timed out");
        }
        if (received) {
            handle_j6(&jog, &frame, now);
        }
        if (jog.phase == JOGGING && now >= jog.next_step) {
            gpio_set_level(STEP_PIN, 1);
            esp_rom_delay_us(10);
            gpio_set_level(STEP_PIN, 0);
            if (++jog.pulses >= MAX_PULSES) {
                uint16_t target = jog.candidate;
                int64_t last_target = jog.last_target;
                stop_jog(&jog, "Jog done");
                jog.phase = WAIT_FOR_CHANGE;
                jog.baseline = target;
                jog.last_target = last_target;
            } else {
                jog.next_step = now + STEP_PERIOD_US;
            }
        }
    }
}
