/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/hid.h>
#include <zmk/endpoints.h>

struct zmk_pointing_resolution_multipliers {
    uint8_t wheel;
    uint8_t hor_wheel;
};

#define ZMK_POINTING_RESOLUTION_MULTIPLIER_MAX 16

struct zmk_pointing_resolution_multipliers
zmk_pointing_resolution_multipliers_get_current_profile(void);
struct zmk_pointing_resolution_multipliers
zmk_pointing_resolution_multipliers_get_profile(struct zmk_endpoint_instance endpoint);
uint32_t zmk_pointing_resolution_multipliers_get_profile_generation(
    struct zmk_endpoint_instance endpoint);
void zmk_pointing_resolution_multipliers_set_profile(
    struct zmk_pointing_resolution_multipliers multipliers, struct zmk_endpoint_instance endpoint);
void zmk_pointing_resolution_multipliers_reset_profile(struct zmk_endpoint_instance endpoint);

void zmk_pointing_resolution_multipliers_process_report(
    struct zmk_hid_mouse_resolution_feature_report_body *report,
    struct zmk_endpoint_instance endpoint);
