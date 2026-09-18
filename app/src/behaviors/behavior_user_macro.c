/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_user_macro

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/stdlib.h>
#include <zmk/user_macros.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define USER_MACRO_SETTING_SUBTREE "macros"
#define USER_MACRO_SETTING_SLOT_KEY "s/%d"
#define USER_MACRO_RECORD_VERSION 1
#define KEY_PRESS_DEVICE_NAME DEVICE_DT_NAME(DT_NODELABEL(kp))

BUILD_ASSERT(CONFIG_ZMK_BEHAVIORS_QUEUE_SIZE >=
                 (CONFIG_ZMK_MACRO_SETTINGS_MAX_STEPS_PER_MACRO * 2),
             "The behavior queue must hold the largest all-tap user macro");

struct user_macro_record_step {
    uint8_t action;
    uint16_t behavior_local_id;
    uint32_t param1;
    uint16_t wait_ms;
} __packed;

struct user_macro_record {
    uint8_t version;
    bool enabled;
    uint8_t step_count;
    char name[CONFIG_ZMK_MACRO_SETTINGS_NAME_MAX_LEN];
    struct user_macro_record_step steps[CONFIG_ZMK_MACRO_SETTINGS_MAX_STEPS_PER_MACRO];
} __packed;

struct behavior_user_macro_config {
    uint8_t slot_index;
};

static struct user_macro_record current_macros[CONFIG_ZMK_MACRO_SETTINGS_MAX_MACROS];
static struct user_macro_record saved_macros[CONFIG_ZMK_MACRO_SETTINGS_MAX_MACROS];
static const char *macro_behavior_names[CONFIG_ZMK_MACRO_SETTINGS_MAX_MACROS];
static K_MUTEX_DEFINE(user_macro_mutex);

static void default_macro(uint8_t slot_index, struct user_macro_record *record) {
    memset(record, 0, sizeof(*record));
    record->version = USER_MACRO_RECORD_VERSION;
    snprintf(record->name, sizeof(record->name), "Macro %u", slot_index + 1);
}

static void reset_current_to_defaults(void) {
    for (uint8_t i = 0; i < ARRAY_SIZE(current_macros); i++) {
        default_macro(i, &current_macros[i]);
    }
}

static bool macro_record_equal(const struct user_macro_record *left,
                               const struct user_macro_record *right) {
    return memcmp(left, right, sizeof(*left)) == 0;
}

static bool key_press_binding_is_valid(uint16_t behavior_local_id, uint32_t param1,
                                       uint32_t param2) {
    if (param2 != 0) {
        return false;
    }

    const char *behavior_name =
        zmk_behavior_find_behavior_name_from_local_id(behavior_local_id);
    if (!behavior_name || strcmp(behavior_name, KEY_PRESS_DEVICE_NAME) != 0) {
        return false;
    }

    struct zmk_behavior_binding binding = {
        .behavior_dev = behavior_name,
        .param1 = param1,
        .param2 = 0,
    };
    return zmk_behavior_validate_binding(&binding) >= 0;
}

static int validate_record(const struct user_macro_record *record) {
    size_t name_len = strnlen(record->name, sizeof(record->name));
    if (record->version != USER_MACRO_RECORD_VERSION || name_len == 0 ||
        name_len == sizeof(record->name) ||
        record->step_count > CONFIG_ZMK_MACRO_SETTINGS_MAX_STEPS_PER_MACRO) {
        return -EINVAL;
    }

    if (!record->enabled) {
        return record->step_count == 0 ? 0 : -EINVAL;
    }
    if (record->step_count == 0) {
        return -EINVAL;
    }

    uint32_t pressed[CONFIG_ZMK_MACRO_SETTINGS_MAX_STEPS_PER_MACRO] = {0};
    uint8_t pressed_count = 0;
    bool previous_was_wait = false;

    for (uint8_t i = 0; i < record->step_count; i++) {
        const struct user_macro_record_step *step = &record->steps[i];
        if (step->action == ZMK_USER_MACRO_STEP_WAIT) {
            if (i == 0 || i + 1 == record->step_count || previous_was_wait ||
                step->wait_ms == 0 || step->wait_ms > CONFIG_ZMK_MACRO_SETTINGS_MAX_WAIT_MS) {
                return -EINVAL;
            }
            previous_was_wait = true;
            continue;
        }
        previous_was_wait = false;

        if (step->wait_ms != 0 ||
            (step->action != ZMK_USER_MACRO_STEP_TAP &&
             step->action != ZMK_USER_MACRO_STEP_PRESS &&
             step->action != ZMK_USER_MACRO_STEP_RELEASE) ||
            !key_press_binding_is_valid(step->behavior_local_id, step->param1, 0)) {
            return -EINVAL;
        }

        int pressed_index = -1;
        for (uint8_t p = 0; p < pressed_count; p++) {
            if (pressed[p] == step->param1) {
                pressed_index = p;
                break;
            }
        }

        if (step->action == ZMK_USER_MACRO_STEP_PRESS) {
            if (pressed_index >= 0) {
                return -EINVAL;
            }
            pressed[pressed_count++] = step->param1;
        } else if (step->action == ZMK_USER_MACRO_STEP_RELEASE) {
            if (pressed_index < 0) {
                return -EINVAL;
            }
            pressed[pressed_index] = pressed[--pressed_count];
        } else if (pressed_index >= 0) {
            return -EINVAL;
        }
    }

    return pressed_count == 0 ? 0 : -EINVAL;
}

size_t zmk_user_macro_get_slot_count(void) {
    return CONFIG_ZMK_MACRO_SETTINGS_MAX_MACROS;
}

size_t zmk_user_macro_get_max_steps(void) {
    return CONFIG_ZMK_MACRO_SETTINGS_MAX_STEPS_PER_MACRO;
}

int zmk_user_macro_register_behavior(uint8_t slot_index, const char *behavior_name) {
    if (slot_index >= ARRAY_SIZE(macro_behavior_names) || !behavior_name) {
        return -EINVAL;
    }

    k_mutex_lock(&user_macro_mutex, K_FOREVER);
    if (macro_behavior_names[slot_index] &&
        strcmp(macro_behavior_names[slot_index], behavior_name) != 0) {
        k_mutex_unlock(&user_macro_mutex);
        return -EALREADY;
    }
    macro_behavior_names[slot_index] = behavior_name;
    k_mutex_unlock(&user_macro_mutex);
    return 0;
}

int zmk_user_macro_get_slot(uint8_t slot_index, struct zmk_user_macro_slot *slot) {
    if (!slot || slot_index >= ARRAY_SIZE(current_macros)) {
        return -EINVAL;
    }

    k_mutex_lock(&user_macro_mutex, K_FOREVER);
    const struct user_macro_record *record = &current_macros[slot_index];
    memset(slot, 0, sizeof(*slot));
    slot->slot_index = slot_index;
    slot->behavior_local_id = macro_behavior_names[slot_index]
                                  ? zmk_behavior_get_local_id(macro_behavior_names[slot_index])
                                  : UINT16_MAX;
    strlcpy(slot->name, record->name, sizeof(slot->name));
    slot->enabled = record->enabled;
    slot->step_count = record->step_count;
    for (uint8_t i = 0; i < record->step_count; i++) {
        slot->steps[i] = (struct zmk_user_macro_step){
            .action = record->steps[i].action,
            .behavior_local_id = record->steps[i].behavior_local_id,
            .param1 = record->steps[i].param1,
            .param2 = 0,
            .wait_ms = record->steps[i].wait_ms,
        };
    }
    slot->dirty = !macro_record_equal(record, &saved_macros[slot_index]);
    k_mutex_unlock(&user_macro_mutex);
    return 0;
}

int zmk_user_macro_set_slot(const struct zmk_user_macro_slot *slot) {
    if (!slot || slot->slot_index >= ARRAY_SIZE(current_macros)) {
        return -EINVAL;
    }

    struct user_macro_record next;
    default_macro(slot->slot_index, &next);
    if (slot->enabled) {
        next.enabled = true;
        next.step_count = slot->step_count;
        strlcpy(next.name, slot->name, sizeof(next.name));
        for (uint8_t i = 0; i < slot->step_count &&
                            i < CONFIG_ZMK_MACRO_SETTINGS_MAX_STEPS_PER_MACRO;
             i++) {
            next.steps[i] = (struct user_macro_record_step){
                .action = slot->steps[i].action,
                .behavior_local_id = slot->steps[i].behavior_local_id,
                .param1 = slot->steps[i].param1,
                .wait_ms = slot->steps[i].wait_ms,
            };
            if (slot->steps[i].param2 != 0) {
                return -EINVAL;
            }
        }
    }

    int ret = validate_record(&next);
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&user_macro_mutex, K_FOREVER);
    current_macros[slot->slot_index] = next;
    k_mutex_unlock(&user_macro_mutex);
    return 0;
}

int zmk_user_macro_queue(uint8_t slot_index, const struct zmk_behavior_binding_event *event) {
    if (!event || slot_index >= ARRAY_SIZE(current_macros)) {
        return -EINVAL;
    }

    struct user_macro_record macro;
    k_mutex_lock(&user_macro_mutex, K_FOREVER);
    macro = current_macros[slot_index];
    k_mutex_unlock(&user_macro_mutex);

    if (!macro.enabled) {
        return 0;
    }
    if (validate_record(&macro) < 0) {
        return -EINVAL;
    }

    for (uint8_t i = 0; i < macro.step_count; i++) {
        const struct user_macro_record_step *step = &macro.steps[i];
        if (step->action == ZMK_USER_MACRO_STEP_WAIT) {
            continue;
        }

        uint32_t wait_after = CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS;
        if (i + 1 < macro.step_count &&
            macro.steps[i + 1].action == ZMK_USER_MACRO_STEP_WAIT) {
            wait_after += macro.steps[i + 1].wait_ms;
        }

        const char *behavior_name =
            zmk_behavior_find_behavior_name_from_local_id(step->behavior_local_id);
        struct zmk_behavior_binding binding = {
            .behavior_dev = behavior_name,
            .param1 = step->param1,
            .param2 = 0,
        };
        int ret;
        if (step->action == ZMK_USER_MACRO_STEP_TAP) {
            ret = zmk_behavior_queue_add(event, binding, true, CONFIG_ZMK_MACRO_DEFAULT_TAP_MS);
            if (ret >= 0) {
                ret = zmk_behavior_queue_add(event, binding, false, wait_after);
            }
        } else {
            ret = zmk_behavior_queue_add(event, binding,
                                         step->action == ZMK_USER_MACRO_STEP_PRESS, wait_after);
        }
        if (ret < 0) {
            return ret;
        }
    }
    return 0;
}

int zmk_user_macro_check_unsaved_changes(void) {
    k_mutex_lock(&user_macro_mutex, K_FOREVER);
    for (uint8_t i = 0; i < ARRAY_SIZE(current_macros); i++) {
        if (!macro_record_equal(&current_macros[i], &saved_macros[i])) {
            k_mutex_unlock(&user_macro_mutex);
            return 1;
        }
    }
    k_mutex_unlock(&user_macro_mutex);
    return 0;
}

int zmk_user_macro_save_changes(void) {
    k_mutex_lock(&user_macro_mutex, K_FOREVER);
    for (uint8_t i = 0; i < ARRAY_SIZE(current_macros); i++) {
        char setting_name[24];
        snprintf(setting_name, sizeof(setting_name),
                 USER_MACRO_SETTING_SUBTREE "/" USER_MACRO_SETTING_SLOT_KEY, i);
        int ret;
        if (!current_macros[i].enabled) {
            ret = settings_delete(setting_name);
            ARG_UNUSED(ret);
            continue;
        }
        ret = settings_save_one(setting_name, &current_macros[i], sizeof(current_macros[i]));
        if (ret < 0) {
            k_mutex_unlock(&user_macro_mutex);
            return ret;
        }
    }
    memcpy(saved_macros, current_macros, sizeof(saved_macros));
    k_mutex_unlock(&user_macro_mutex);
    return 0;
}

static int user_macro_handle_set(const char *name, size_t len, settings_read_cb read_cb,
                                 void *cb_arg) {
    const char *next;
    if (!settings_name_steq(name, "s", &next) || !next) {
        return 0;
    }

    char *endptr;
    unsigned long slot = strtoul(next, &endptr, 10);
    if (*endptr != '\0' || slot >= ARRAY_SIZE(current_macros)) {
        LOG_WRN("Ignoring unavailable user macro slot %s", next);
        return 0;
    }

    struct user_macro_record record = {0};
    int ret = read_cb(cb_arg, &record, MIN(len, sizeof(record)));
    if (ret != sizeof(record) || len != sizeof(record) || validate_record(&record) < 0) {
        LOG_WRN("Ignoring invalid user macro setting %lu", slot);
        return 0;
    }
    current_macros[slot] = record;
    return 0;
}

static int user_macro_handle_commit(void) {
    memcpy(saved_macros, current_macros, sizeof(saved_macros));
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(user_macros, USER_MACRO_SETTING_SUBTREE, NULL,
                               user_macro_handle_set, user_macro_handle_commit, NULL);

int zmk_user_macro_discard_changes(void) {
    k_mutex_lock(&user_macro_mutex, K_FOREVER);
    reset_current_to_defaults();
    k_mutex_unlock(&user_macro_mutex);
    return settings_load_subtree(USER_MACRO_SETTING_SUBTREE);
}

int zmk_user_macro_reset_settings(void) {
    k_mutex_lock(&user_macro_mutex, K_FOREVER);
    for (uint8_t i = 0; i < ARRAY_SIZE(current_macros); i++) {
        char setting_name[24];
        snprintf(setting_name, sizeof(setting_name),
                 USER_MACRO_SETTING_SUBTREE "/" USER_MACRO_SETTING_SLOT_KEY, i);
        settings_delete(setting_name);
    }
    reset_current_to_defaults();
    memcpy(saved_macros, current_macros, sizeof(saved_macros));
    k_mutex_unlock(&user_macro_mutex);
    return 0;
}

static int user_macro_init(void) {
    reset_current_to_defaults();
    memcpy(saved_macros, current_macros, sizeof(saved_macros));
    return 0;
}

SYS_INIT(user_macro_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static int on_user_macro_pressed(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_user_macro_config *config = dev->config;
    int ret = zmk_user_macro_queue(config->slot_index, &event);
    return ret < 0 ? ret : ZMK_BEHAVIOR_OPAQUE;
}

static int on_user_macro_released(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_user_macro_driver_api = {
    .binding_pressed = on_user_macro_pressed,
    .binding_released = on_user_macro_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define USER_MACRO_INST(n)                                                                         \
    BUILD_ASSERT(DT_INST_PROP(n, slot) < CONFIG_ZMK_MACRO_SETTINGS_MAX_MACROS,                     \
                 "User macro slot exceeds CONFIG_ZMK_MACRO_SETTINGS_MAX_MACROS");                  \
    static const struct behavior_user_macro_config behavior_user_macro_config_##n = {              \
        .slot_index = DT_INST_PROP(n, slot),                                                        \
    };                                                                                             \
    static int behavior_user_macro_init_##n(const struct device *dev) {                            \
        return zmk_user_macro_register_behavior(DT_INST_PROP(n, slot), dev->name);                 \
    }                                                                                              \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_user_macro_init_##n, NULL, NULL,                           \
                            &behavior_user_macro_config_##n, POST_KERNEL,                           \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_user_macro_driver_api);

DT_INST_FOREACH_STATUS_OKAY(USER_MACRO_INST)
