/*
 * Copyright (c) 2025 ZitaoTech / ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_mouse_scroll_tick

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <dt-bindings/zmk/pointing.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    if (!dev) {
        LOG_WRN("msc_tick dev not found");
        return -ENODEV;
    }

    int16_t y = MOVE_Y_DECODE(binding->param1);
    int16_t x = MOVE_X_DECODE(binding->param1);

    if (y != 0) {
        int16_t val_y = (y > 0) ? (y >= 10 ? y / 10 : 1) : (y <= -10 ? y / 10 : -1);
        input_report_rel(dev, INPUT_REL_WHEEL, val_y, x == 0, K_NO_WAIT);
    }
    if (x != 0) {
        int16_t val_x = (x > 0) ? (x >= 10 ? x / 10 : 1) : (x <= -10 ? x / 10 : -1);
        input_report_rel(dev, INPUT_REL_HWHEEL, val_x, true, K_NO_WAIT);
    }

    return 0;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return 0;
}

static const struct behavior_driver_api behavior_mouse_scroll_tick_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

static int behavior_mouse_scroll_tick_init(const struct device *dev) {
    return 0;
}

#define MSC_TICK_INST(n)                                                                           \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_mouse_scroll_tick_init, NULL, NULL, NULL, POST_KERNEL,      \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_mouse_scroll_tick_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MSC_TICK_INST)
