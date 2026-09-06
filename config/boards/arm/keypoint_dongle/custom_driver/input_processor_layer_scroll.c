/*
 * Copyright (c) 2025 ZitaoTech / ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_layer_scroll

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <drivers/input_processor.h>
#include <zephyr/logging/log.h>
#include <zmk/keymap.h>
#include <stdlib.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int32_t residual_x = 0;
static int32_t residual_y = 0;
static uint32_t last_event_time = 0;

static int layer_scroll_handle_event(const struct device *dev, struct input_event *event,
                                     uint32_t param1, uint32_t param2,
                                     struct zmk_input_processor_state *state) {
    if (event->type != INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    // If param1 specifies a layer (> 0), verify that layer is active
    if (param1 > 0 && !zmk_keymap_layer_active((zmk_keymap_layer_id_t)param1)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    // If it's already a wheel event, pass it through unchanged
    if (event->code == INPUT_REL_WHEEL || event->code == INPUT_REL_HWHEEL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    uint32_t now = k_uptime_get_32();
    if (now - last_event_time > 150) {
        residual_x = 0;
        residual_y = 0;
    }
    last_event_time = now;

    int32_t divisor = (param2 > 0) ? (int32_t)param2 : 12;

    if (event->code == INPUT_REL_X) {
        residual_x += event->value;
        int16_t out_x = (int16_t)(residual_x / divisor);
        residual_x -= (out_x * divisor);

        event->code = INPUT_REL_HWHEEL;
        event->value = out_x;
        if (event->value == 0) {
            return ZMK_INPUT_PROC_STOP;
        }
        return ZMK_INPUT_PROC_CONTINUE;
    } else if (event->code == INPUT_REL_Y) {
        // Invert Y coordinate so pushing up scrolls up
        residual_y += -event->value;
        int16_t out_y = (int16_t)(residual_y / divisor);
        residual_y -= (out_y * divisor);

        event->code = INPUT_REL_WHEEL;
        event->value = out_y;
        if (event->value == 0) {
            return ZMK_INPUT_PROC_STOP;
        }
        return ZMK_INPUT_PROC_CONTINUE;
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static struct zmk_input_processor_driver_api layer_scroll_driver_api = {
    .handle_event = layer_scroll_handle_event,
};

#define LAYER_SCROLL_INST(n)                                                                       \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                  \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &layer_scroll_driver_api);

DT_INST_FOREACH_STATUS_OKAY(LAYER_SCROLL_INST)
