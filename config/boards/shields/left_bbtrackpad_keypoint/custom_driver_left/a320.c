/*
 * A320 trackpad HID over I2C Driver (Zephyr Input Subsystem)
 * Robust version with dual-address probing (0x3B & 0x37), polling fallback,
 * and configurable default scroll mode.
 * Copyright (c) 2025 ZitaoTech
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT avago_a320

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <stdlib.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <math.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zmk/hid.h>

#include "trackpad_led.h"
#include "a320.h"

LOG_MODULE_REGISTER(a320, CONFIG_A320_LOG_LEVEL);

#define SCROLL_X_DIR (-CONFIG_A320_SCROLL_X_DIR)
#define SCROLL_Y_DIR CONFIG_A320_SCROLL_Y_DIR

#ifndef CONFIG_A320_SCROLL_SCALE_PERCENT
#define CONFIG_A320_SCROLL_SCALE_PERCENT 300
#endif
#define SCROLL_SCALE_MULT (CONFIG_A320_SCROLL_SCALE_PERCENT / 100.0f)

#ifndef CONFIG_A320_POLL_INTERVAL_MS
#define CONFIG_A320_POLL_INTERVAL_MS 10
#endif

#define MOTION_GPIO_NODE DT_NODELABEL(gpio0)
#define MOTION_GPIO_PIN 8

static const struct device *motion_gpio_dev;
static const struct device *a320_dev;

static bool scroll_key_pressed = false;
static bool arrow_key_pressed = false;
static bool slow_key_pressed = false;
static bool touched = false;

static float scroll_residual_x = 0;
static float scroll_residual_y = 0;
static uint32_t last_read_time = 0;

static zmk_hid_indicators_t current_indicators;
#define HID_INDICATORS_CAPS_LOCK (1 << 1)

static int hid_indicators_listener(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (ev) {
        current_indicators = ev->indicators;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(a320_hid_listener, hid_indicators_listener);
ZMK_SUBSCRIPTION(a320_hid_listener, zmk_hid_indicators_changed);

static int special_key_listener_cb(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (!ev)
        return 0;
    if (ev->position == 20) {
        arrow_key_pressed = ev->state;
    }

    if (ev->position == 48 || ev->position == 49) {
        scroll_key_pressed = ev->state;
    }

    if (ev->position == 22) {
        slow_key_pressed = ev->state;
    }

    return 0;
}
ZMK_LISTENER(a320_special_key_listener, special_key_listener_cb);
ZMK_SUBSCRIPTION(a320_special_key_listener, zmk_position_state_changed);

struct a320_dev_config {
    struct i2c_dt_spec i2c;
};

typedef int (*a320_read_fn_t)(const struct device *dev, int16_t *dx, int16_t *dy);

struct a320_data {
    const struct device *dev;
    struct k_work_delayable poll_work;
    a320_read_fn_t read_motion;
};

/* I2C Read for Address 0x3B */
static int a320_read_motion_3b(const struct device *dev, int16_t *dx, int16_t *dy) {
    const struct a320_dev_config *cfg = dev->config;
    uint8_t buf[3];
    uint8_t reg = 0x82;

    if (i2c_write_dt(&cfg->i2c, &reg, 1) < 0)
        return -EIO;

    if (i2c_burst_read_dt(&cfg->i2c, 0x82, buf, sizeof(buf)) < 0)
        return -EIO;

    *dx = (int8_t)buf[1];
    *dy = -(int8_t)buf[2];
    return 0;
}

/* I2C Read for Address 0x37 */
static int a320_read_motion_37(const struct device *dev, int16_t *dx, int16_t *dy) {
    const struct a320_dev_config *cfg = dev->config;
    uint8_t buf[7];
    uint8_t reg = 0x0A;

    if (i2c_write_dt(&cfg->i2c, &reg, 1) < 0)
        return -EIO;

    if (i2c_burst_read_dt(&cfg->i2c, 0x0A, buf, sizeof(buf)) < 0)
        return -EIO;

    *dx = -(int8_t)buf[1];
    *dy = (int8_t)buf[3];
    return 0;
}

static void a320_poll_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);
    struct a320_data *data = CONTAINER_OF(dwork, struct a320_data, poll_work);
    const struct device *dev = data->dev;

    int pin_state = gpio_pin_get(motion_gpio_dev, MOTION_GPIO_PIN);
    bool capslock = current_indicators & HID_INDICATORS_CAPS_LOCK;

    if (pin_state == 0) {
        int16_t dx = 0, dy = 0;

        if (data->read_motion && data->read_motion(dev, &dx, &dy) == 0 && (dx || dy)) {
            touched = true;

#if CONFIG_A320_START_IN_SCROLL_MODE
            bool is_scroll_mode = (!scroll_key_pressed && !capslock);
#else
            bool is_scroll_mode = (scroll_key_pressed || capslock);
#endif

            if (is_scroll_mode) {
                uint32_t now = k_uptime_get_32();
                if (now - last_read_time > 100) {
                    scroll_residual_x = 0;
                    scroll_residual_y = 0;
                }
                last_read_time = now;

                float speed = sqrtf((float)(dx * dx + dy * dy));
                float scale = ((speed > 80)   ? 0.05f
                              : (speed > 40) ? 0.04f
                              : (speed > 20) ? 0.03f
                              : (speed > 5)  ? 0.02f
                                             : 0.015f) * SCROLL_SCALE_MULT;

                scroll_residual_x += dx * scale * SCROLL_X_DIR;
                scroll_residual_y += dy * scale * SCROLL_Y_DIR;

                int16_t out_x = (int16_t)scroll_residual_x;
                int16_t out_y = (int16_t)scroll_residual_y;

                scroll_residual_x -= out_x;
                scroll_residual_y -= out_y;

                if (out_x || out_y) {
                    input_report_rel(dev, INPUT_REL_HWHEEL, out_x, false, K_FOREVER);
                    input_report_rel(dev, INPUT_REL_WHEEL, -out_y, true, K_FOREVER);
                }
            } else {
                scroll_residual_x = 0;
                scroll_residual_y = 0;

                uint8_t tp_led_brt = indicator_tp_get_last_valid_brightness();
                float tp_factor = 0.4f + 0.01f * tp_led_brt;
                float slow_mult = slow_key_pressed ? 0.5f : 1.0f;

                int fx = (int)(dx * tp_factor * slow_mult);
                int fy = (int)(dy * tp_factor * slow_mult);

                input_report_rel(dev, INPUT_REL_X, fx, false, K_FOREVER);
                input_report_rel(dev, INPUT_REL_Y, fy, true, K_FOREVER);
            }
        }
    } else {
        touched = false;
    }

    k_work_reschedule(&data->poll_work, K_MSEC(CONFIG_A320_POLL_INTERVAL_MS));
}

bool tp_is_touched(void) { return touched; }

static int a320_init(const struct device *dev) {
    const struct a320_dev_config *cfg = dev->config;
    struct a320_data *data = dev->data;

    a320_dev = dev;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("A320 I2C bus not ready");
        return -ENODEV;
    }

    motion_gpio_dev = DEVICE_DT_GET(MOTION_GPIO_NODE);
    if (!device_is_ready(motion_gpio_dev)) {
        LOG_ERR("A320 Motion GPIO device not ready");
        return -ENODEV;
    }

    gpio_pin_configure(motion_gpio_dev, MOTION_GPIO_PIN, GPIO_INPUT | GPIO_PULL_UP);

    uint8_t test_reg;
    struct i2c_dt_spec scan_spec = cfg->i2c;

    int found_addr = 0;
    const uint8_t candidates[] = {0x3B, 0x37};

    for (int i = 0; i < ARRAY_SIZE(candidates); i++) {
        scan_spec.addr = candidates[i];
        if (i2c_read_dt(&scan_spec, &test_reg, 1) == 0) {
            found_addr = candidates[i];
            break;
        }
    }

    if (!found_addr) {
        found_addr = 0x3B;
        LOG_WRN("A320 I2C auto-detect failed, defaulting to 0x3B");
    } else {
        LOG_INF("A320 detected at I2C address 0x%02X", found_addr);
    }

    ((struct a320_dev_config *)cfg)->i2c.addr = found_addr;

    if (found_addr == 0x3B)
        data->read_motion = a320_read_motion_3b;
    else
        data->read_motion = a320_read_motion_37;

    data->dev = dev;

    k_work_init_delayable(&data->poll_work, a320_poll_work_handler);
    k_work_schedule(&data->poll_work, K_MSEC(CONFIG_A320_POLL_INTERVAL_MS));

    LOG_INF("A320 trackpad driver initialized successfully");
    return 0;
}

#define A320_DEFINE(inst)                                                                          \
    static struct a320_data a320_data_##inst;                                                      \
    static struct a320_dev_config a320_cfg_##inst = {                                              \
        .i2c = I2C_DT_SPEC_INST_GET(inst),                                                         \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, a320_init, NULL, &a320_data_##inst, &a320_cfg_##inst, POST_KERNEL, \
                          70, NULL);

DT_INST_FOREACH_STATUS_OKAY(A320_DEFINE);