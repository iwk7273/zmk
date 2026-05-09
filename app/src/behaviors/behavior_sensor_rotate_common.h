/*
 * Copyright (c) 2023 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/sensors.h>

#define BEHAVIOR_SENSOR_ROTATE_CONFIG_MAGIC 0x5a524f54

struct behavior_sensor_rotate_config {
    uint32_t magic;
    struct zmk_behavior_binding cw_binding;
    struct zmk_behavior_binding ccw_binding;
    int tap_ms;
    bool override_params;
};

struct behavior_sensor_rotate_data {
    struct sensor_value remainder[ZMK_KEYMAP_SENSORS_LEN][ZMK_KEYMAP_LAYERS_LEN];
    int triggers[ZMK_KEYMAP_SENSORS_LEN][ZMK_KEYMAP_LAYERS_LEN];
};

int zmk_behavior_sensor_rotate_common_accept_data(
    struct zmk_behavior_binding *binding, struct zmk_behavior_binding_event event,
    const struct zmk_sensor_config *sensor_config, size_t channel_data_size,
    const struct zmk_sensor_channel_data *channel_data);
int zmk_behavior_sensor_rotate_common_process(struct zmk_behavior_binding *binding,
                                              struct zmk_behavior_binding_event event,
                                              enum behavior_sensor_binding_process_mode mode);
int zmk_behavior_sensor_rotate_get_binding_param(
    const struct zmk_behavior_binding *sensor_binding, enum zmk_keymap_sensor_binding_param param,
    struct zmk_behavior_binding *binding);
