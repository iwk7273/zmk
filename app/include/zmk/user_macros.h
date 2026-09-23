/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zmk/behavior.h>

enum zmk_user_macro_step_action {
    ZMK_USER_MACRO_STEP_TAP = 1,
    ZMK_USER_MACRO_STEP_PRESS = 2,
    ZMK_USER_MACRO_STEP_RELEASE = 3,
    ZMK_USER_MACRO_STEP_WAIT = 4,
    ZMK_USER_MACRO_STEP_TEXT = 5,
};

struct zmk_user_macro_step {
    uint8_t action;
    zmk_behavior_local_id_t behavior_local_id;
    uint32_t param1;
    uint16_t wait_ms;
    uint16_t packed_off;
    uint16_t packed_len;
};

struct zmk_user_macro_slot {
    uint8_t slot_index;
    zmk_behavior_local_id_t behavior_local_id;
    char name[CONFIG_ZMK_MACRO_SETTINGS_NAME_MAX_LEN];
    bool enabled;
    bool dirty;
    uint8_t step_count;
    uint16_t encoded_size;
    uint16_t packed_len;
    struct zmk_user_macro_step steps[CONFIG_ZMK_MACRO_SETTINGS_MAX_STEPS_PER_MACRO];
    uint8_t packed_keys[CONFIG_ZMK_MACRO_SETTINGS_MAX_BYTES];
};

size_t zmk_user_macro_get_slot_count(void);
size_t zmk_user_macro_get_max_steps(void);
size_t zmk_user_macro_get_max_bytes(void);
size_t zmk_user_macro_get_pool_total(void);
size_t zmk_user_macro_get_pool_used(void);
uint32_t zmk_user_macro_get_tap_ms(void);
int zmk_user_macro_set_tap_ms(uint32_t tap_ms);
int zmk_user_macro_register_behavior(uint8_t slot_index, const char *behavior_name);
int zmk_user_macro_get_summary(uint8_t slot_index, struct zmk_user_macro_slot *slot);
int zmk_user_macro_get_slot(uint8_t slot_index, struct zmk_user_macro_slot *slot);
int zmk_user_macro_set_slot(const struct zmk_user_macro_slot *slot);
int zmk_user_macro_reset_slot(uint8_t slot_index);
int zmk_user_macro_queue(uint8_t slot_index, const struct zmk_behavior_binding_event *event);
int zmk_user_macro_check_unsaved_changes(void);
int zmk_user_macro_save_changes(void);
int zmk_user_macro_discard_changes(void);
int zmk_user_macro_reset_settings(void);
