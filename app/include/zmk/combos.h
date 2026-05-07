/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <zmk/behavior.h>

#define ZMK_COMBOS_UTIL_ONE(n) +1

#define ZMK_COMBOS_LEN                                                                             \
    COND_CODE_1(DT_HAS_COMPAT_STATUS_OKAY(zmk_combos),                                             \
                (0 DT_FOREACH_CHILD_STATUS_OKAY(DT_INST(0, zmk_combos), ZMK_COMBOS_UTIL_ONE)),     \
                (0))

#if IS_ENABLED(CONFIG_ZMK_COMBO_SETTINGS)

enum zmk_combo_source {
    ZMK_COMBO_SOURCE_STOCK = 0,
    ZMK_COMBO_SOURCE_USER = 1,
};

struct zmk_combo_slot {
    char combo_id[CONFIG_ZMK_COMBO_SETTINGS_ID_MAX_LEN];
    uint8_t slot_index;
    enum zmk_combo_source source;
    bool enabled;
    struct zmk_behavior_binding binding;
    uint8_t key_position_count;
    uint8_t key_positions[CONFIG_ZMK_COMBO_SETTINGS_MAX_KEYS_PER_COMBO];
    uint8_t layer_count;
    uint8_t layers[CONFIG_ZMK_COMBO_SETTINGS_MAX_LAYERS];
    uint16_t timeout_ms;
    int16_t require_prior_idle_ms;
    bool slow_release;
    bool dirty;
};

size_t zmk_combo_get_slot_count(void);
size_t zmk_combo_get_max_keys_per_combo(void);
int zmk_combo_get_slot(uint8_t slot_index, struct zmk_combo_slot *slot);
int zmk_combo_set_slot(const struct zmk_combo_slot *slot);
int zmk_combo_check_unsaved_changes(void);
int zmk_combo_save_changes(void);
int zmk_combo_discard_changes(void);
int zmk_combo_reset_settings(void);

#endif
