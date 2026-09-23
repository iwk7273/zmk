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
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <dt-bindings/zmk/modifiers.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/stdlib.h>
#include <zmk/user_macros.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define USER_MACRO_SETTING_SUBTREE "macros"
#define USER_MACRO_SETTING_SLOT_KEY "s/%d"
#define USER_MACRO_SETTING_TAP_MS "tap_ms"
#define USER_MACRO_RECORD_VERSION_V1 1
#define USER_MACRO_RECORD_VERSION 2
#define USER_MACRO_FLAG_ENABLED BIT(0)
#define USER_MACRO_FLAG_TOMBSTONE BIT(1)
#define PACKED_SHIFT 0x80
#define PACKED_USAGE_MASK 0x7f
#define KEY_PRESS_DEVICE_NAME DEVICE_DT_NAME(DT_NODELABEL(kp))

BUILD_ASSERT(CONFIG_ZMK_BEHAVIORS_QUEUE_SIZE >= 4,
             "The behavior queue must hold a short user macro");

enum user_macro_occupancy {
    USER_MACRO_VACANT = 0,
    USER_MACRO_ACTIVE = 1,
    USER_MACRO_TOMBSTONE = 2,
};

struct user_macro_v1_step {
    uint8_t action;
    uint16_t behavior_local_id;
    uint32_t param1;
    uint16_t wait_ms;
} __packed;

struct user_macro_v1_record {
    uint8_t version;
    bool enabled;
    uint8_t step_count;
    char name[CONFIG_ZMK_MACRO_SETTINGS_NAME_MAX_LEN];
    struct user_macro_v1_step steps[CONFIG_ZMK_MACRO_SETTINGS_MAX_STEPS_PER_MACRO];
} __packed;

struct user_macro_slot_state {
    uint8_t occupancy;
    bool dirty;
    char name[CONFIG_ZMK_MACRO_SETTINGS_NAME_MAX_LEN];
    uint16_t offset;
    uint16_t length;
};

struct behavior_user_macro_config {
    uint8_t slot_index;
};

static struct user_macro_slot_state current_slots[CONFIG_ZMK_MACRO_SETTINGS_MAX_MACROS];
static uint8_t current_pool[CONFIG_ZMK_MACRO_SETTINGS_POOL_BYTES];
static uint8_t pool_scratch[CONFIG_ZMK_MACRO_SETTINGS_POOL_BYTES];
static uint16_t current_used;
static uint32_t current_tap_ms;
static bool tap_ms_dirty;
static const char *macro_behavior_names[CONFIG_ZMK_MACRO_SETTINGS_MAX_MACROS];
static K_MUTEX_DEFINE(user_macro_mutex);

static uint32_t packed_to_usage(uint8_t packed) {
    uint32_t usage = ZMK_HID_USAGE(HID_USAGE_KEY, (packed & PACKED_USAGE_MASK));
    if (packed & PACKED_SHIFT) {
        usage = APPLY_MODS(MOD_LSFT, usage);
    }
    return usage;
}

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_user_macro_default)
static int char_to_packed(char ch) {
    unsigned char c = (unsigned char)ch;
    if (c >= 'A' && c <= 'Z') {
        return PACKED_SHIFT | (0x04 + (c - 'A'));
    }
    if (c >= 'a' && c <= 'z') {
        return 0x04 + (c - 'a');
    }
    if (c >= '1' && c <= '9') {
        return 0x1e + (c - '1');
    }
    switch (c) {
    case '0':
        return 0x27;
    case '\n':
        return 0x28;
    case '\t':
        return 0x2b;
    case ' ':
        return 0x2c;
    case '-':
        return 0x2d;
    case '=':
        return 0x2e;
    case '[':
        return 0x2f;
    case ']':
        return 0x30;
    case '\\':
        return 0x31;
    case ';':
        return 0x33;
    case '\'':
        return 0x34;
    case '`':
        return 0x35;
    case ',':
        return 0x36;
    case '.':
        return 0x37;
    case '/':
        return 0x38;
    case '!':
        return PACKED_SHIFT | 0x1e;
    case '@':
        return PACKED_SHIFT | 0x1f;
    case '#':
        return PACKED_SHIFT | 0x20;
    case '$':
        return PACKED_SHIFT | 0x21;
    case '%':
        return PACKED_SHIFT | 0x22;
    case '^':
        return PACKED_SHIFT | 0x23;
    case '&':
        return PACKED_SHIFT | 0x24;
    case '*':
        return PACKED_SHIFT | 0x25;
    case '(':
        return PACKED_SHIFT | 0x26;
    case ')':
        return PACKED_SHIFT | 0x27;
    case '_':
        return PACKED_SHIFT | 0x2d;
    case '+':
        return PACKED_SHIFT | 0x2e;
    case '{':
        return PACKED_SHIFT | 0x2f;
    case '}':
        return PACKED_SHIFT | 0x30;
    case '|':
        return PACKED_SHIFT | 0x31;
    case ':':
        return PACKED_SHIFT | 0x33;
    case '"':
        return PACKED_SHIFT | 0x34;
    case '~':
        return PACKED_SHIFT | 0x35;
    case '<':
        return PACKED_SHIFT | 0x36;
    case '>':
        return PACKED_SHIFT | 0x37;
    case '?':
        return PACKED_SHIFT | 0x38;
    default:
        return -EINVAL;
    }
}
#endif

static bool key_press_binding_is_valid(uint16_t behavior_local_id, uint32_t param1) {
    const char *behavior_name = zmk_behavior_find_behavior_name_from_local_id(behavior_local_id);
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

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_user_macro_default)
static int encode_text_body(const char *text, uint8_t *body, size_t body_max, uint16_t *out_len) {
    uint8_t packed[CONFIG_ZMK_MACRO_SETTINGS_MAX_BYTES];
    size_t packed_len = 0;
    size_t text_len = text ? strlen(text) : 0;

    if (text_len == 0) {
        return -EINVAL;
    }
    for (size_t i = 0; i < text_len; i++) {
        int value = char_to_packed(text[i]);
        if (value < 0 || packed_len >= ARRAY_SIZE(packed)) {
            return -EINVAL;
        }
        packed[packed_len++] = (uint8_t)value;
    }

    size_t need = 1 + 1 + 2 + packed_len;
    if (need > body_max || need > CONFIG_ZMK_MACRO_SETTINGS_MAX_BYTES) {
        return -ENOSPC;
    }

    body[0] = 1;
    body[1] = ZMK_USER_MACRO_STEP_TEXT;
    sys_put_le16((uint16_t)packed_len, &body[2]);
    memcpy(&body[4], packed, packed_len);
    *out_len = (uint16_t)need;
    return 0;
}
#endif

static int decode_body(const uint8_t *body, uint16_t body_len, struct zmk_user_macro_slot *slot) {
    if (!body || !slot || body_len < 1) {
        return -EINVAL;
    }

    memset(slot->steps, 0, sizeof(slot->steps));
    slot->step_count = 0;
    slot->packed_len = 0;
    slot->encoded_size = body_len;

    uint8_t step_count = body[0];
    size_t offset = 1;
    if (step_count > CONFIG_ZMK_MACRO_SETTINGS_MAX_STEPS_PER_MACRO) {
        return -EINVAL;
    }

    for (uint8_t i = 0; i < step_count; i++) {
        if (offset >= body_len) {
            return -EINVAL;
        }
        struct zmk_user_macro_step *step = &slot->steps[i];
        step->action = body[offset++];
        if (step->action == ZMK_USER_MACRO_STEP_WAIT) {
            if (offset + 2 > body_len) {
                return -EINVAL;
            }
            step->wait_ms = sys_get_le16(&body[offset]);
            offset += 2;
        } else if (step->action == ZMK_USER_MACRO_STEP_TEXT) {
            if (offset + 2 > body_len) {
                return -EINVAL;
            }
            uint16_t packed_len = sys_get_le16(&body[offset]);
            offset += 2;
            if (packed_len == 0 || offset + packed_len > body_len ||
                slot->packed_len + packed_len > CONFIG_ZMK_MACRO_SETTINGS_MAX_BYTES) {
                return -EINVAL;
            }
            step->packed_off = slot->packed_len;
            step->packed_len = packed_len;
            memcpy(&slot->packed_keys[slot->packed_len], &body[offset], packed_len);
            slot->packed_len += packed_len;
            offset += packed_len;
        } else if (step->action == ZMK_USER_MACRO_STEP_TAP ||
                   step->action == ZMK_USER_MACRO_STEP_PRESS ||
                   step->action == ZMK_USER_MACRO_STEP_RELEASE) {
            if (offset + 6 > body_len) {
                return -EINVAL;
            }
            step->behavior_local_id = sys_get_le16(&body[offset]);
            offset += 2;
            step->param1 = sys_get_le32(&body[offset]);
            offset += 4;
        } else {
            return -EINVAL;
        }
        slot->step_count++;
    }

    return offset == body_len ? 0 : -EINVAL;
}

static int encode_body(const struct zmk_user_macro_slot *slot, uint8_t *body, size_t body_max,
                       uint16_t *out_len) {
    if (!slot || !body || !out_len) {
        return -EINVAL;
    }
    if (slot->step_count > CONFIG_ZMK_MACRO_SETTINGS_MAX_STEPS_PER_MACRO) {
        return -EINVAL;
    }

    size_t offset = 0;
    if (body_max < 1) {
        return -ENOSPC;
    }
    body[offset++] = slot->step_count;

    for (uint8_t i = 0; i < slot->step_count; i++) {
        const struct zmk_user_macro_step *step = &slot->steps[i];
        if (offset >= body_max) {
            return -ENOSPC;
        }
        body[offset++] = step->action;
        if (step->action == ZMK_USER_MACRO_STEP_WAIT) {
            if (offset + 2 > body_max) {
                return -ENOSPC;
            }
            sys_put_le16(step->wait_ms, &body[offset]);
            offset += 2;
        } else if (step->action == ZMK_USER_MACRO_STEP_TEXT) {
            if (step->packed_len == 0 ||
                (uint32_t)step->packed_off + step->packed_len > slot->packed_len ||
                offset + 2 + step->packed_len > body_max) {
                return -EINVAL;
            }
            sys_put_le16(step->packed_len, &body[offset]);
            offset += 2;
            memcpy(&body[offset], &slot->packed_keys[step->packed_off], step->packed_len);
            offset += step->packed_len;
        } else if (step->action == ZMK_USER_MACRO_STEP_TAP ||
                   step->action == ZMK_USER_MACRO_STEP_PRESS ||
                   step->action == ZMK_USER_MACRO_STEP_RELEASE) {
            if (offset + 6 > body_max) {
                return -ENOSPC;
            }
            sys_put_le16(step->behavior_local_id, &body[offset]);
            offset += 2;
            sys_put_le32(step->param1, &body[offset]);
            offset += 4;
        } else {
            return -EINVAL;
        }
    }

    if (offset > CONFIG_ZMK_MACRO_SETTINGS_MAX_BYTES) {
        return -ENOSPC;
    }
    *out_len = (uint16_t)offset;
    return 0;
}

static uint16_t expanded_event_count(const struct zmk_user_macro_slot *slot) {
    uint16_t count = slot->step_count > 0 && slot->steps[0].action == ZMK_USER_MACRO_STEP_WAIT ? 1 : 0;
    for (uint8_t i = 0; i < slot->step_count; i++) {
        const struct zmk_user_macro_step *step = &slot->steps[i];
        if (step->action == ZMK_USER_MACRO_STEP_TAP) {
            count += 2;
        } else if (step->action == ZMK_USER_MACRO_STEP_PRESS ||
                   step->action == ZMK_USER_MACRO_STEP_RELEASE) {
            count += 1;
        } else if (step->action == ZMK_USER_MACRO_STEP_TEXT) {
            count += (uint16_t)(step->packed_len * 2);
        }
    }
    return count;
}

static int validate_slot(const struct zmk_user_macro_slot *slot) {
    size_t name_len = strnlen(slot->name, sizeof(slot->name));
    if (name_len == 0 || name_len == sizeof(slot->name)) {
        return -ENAMETOOLONG;
    }
    if (!slot->enabled) {
        return slot->step_count == 0 && slot->packed_len == 0 ? 0 : -EINVAL;
    }
    if (slot->step_count == 0 || slot->step_count > CONFIG_ZMK_MACRO_SETTINGS_MAX_STEPS_PER_MACRO) {
        return -EINVAL;
    }
    if (expanded_event_count(slot) > CONFIG_ZMK_BEHAVIORS_QUEUE_SIZE) {
        return -ENOSPC;
    }

    uint32_t pressed[CONFIG_ZMK_MACRO_SETTINGS_MAX_STEPS_PER_MACRO] = {0};
    uint8_t pressed_count = 0;

    for (uint8_t i = 0; i < slot->step_count; i++) {
        const struct zmk_user_macro_step *step = &slot->steps[i];
        if (step->action == ZMK_USER_MACRO_STEP_WAIT) {
            if (step->wait_ms == 0 || step->wait_ms > CONFIG_ZMK_MACRO_SETTINGS_MAX_WAIT_MS ||
                step->packed_len != 0) {
                return -EINVAL;
            }
            continue;
        }
        if (step->action == ZMK_USER_MACRO_STEP_TEXT) {
            if (step->packed_len == 0 ||
                (uint32_t)step->packed_off + step->packed_len > slot->packed_len) {
                return -EINVAL;
            }
            continue;
        }
        if ((step->action != ZMK_USER_MACRO_STEP_TAP &&
             step->action != ZMK_USER_MACRO_STEP_PRESS &&
             step->action != ZMK_USER_MACRO_STEP_RELEASE) ||
            step->packed_len != 0 ||
            !key_press_binding_is_valid(step->behavior_local_id, step->param1)) {
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

static void clear_slot_state(struct user_macro_slot_state *state) {
    memset(state, 0, sizeof(*state));
    state->occupancy = USER_MACRO_VACANT;
}

static void default_name(uint8_t slot_index, char *name, size_t name_size) {
    snprintf(name, name_size, "Macro %u", slot_index + 1);
}

static int pool_store_body(uint8_t slot_index, const uint8_t *body, uint16_t length) {
    uint16_t used = 0;
    uint16_t offsets[CONFIG_ZMK_MACRO_SETTINGS_MAX_MACROS];

    for (uint8_t i = 0; i < ARRAY_SIZE(current_slots); i++) {
        if (i == slot_index || current_slots[i].length == 0) {
            offsets[i] = 0;
            continue;
        }
        if (used + current_slots[i].length > ARRAY_SIZE(pool_scratch)) {
            return -ENOSPC;
        }
        memcpy(&pool_scratch[used], &current_pool[current_slots[i].offset],
               current_slots[i].length);
        offsets[i] = used;
        used += current_slots[i].length;
    }

    if (length > 0) {
        if (used + length > ARRAY_SIZE(current_pool)) {
            return -ENOSPC;
        }
        memcpy(&pool_scratch[used], body, length);
        current_slots[slot_index].offset = used;
        current_slots[slot_index].length = length;
        used += length;
    } else {
        current_slots[slot_index].offset = 0;
        current_slots[slot_index].length = 0;
    }

    memcpy(current_pool, pool_scratch, used);
    current_used = used;
    for (uint8_t i = 0; i < ARRAY_SIZE(current_slots); i++) {
        if (i == slot_index || current_slots[i].length == 0) {
            continue;
        }
        current_slots[i].offset = offsets[i];
    }
    return 0;
}

static zmk_behavior_local_id_t slot_behavior_id(uint8_t slot_index) {
    return macro_behavior_names[slot_index]
               ? zmk_behavior_get_local_id(macro_behavior_names[slot_index])
               : UINT16_MAX;
}

static void fill_summary_locked(uint8_t slot_index, struct zmk_user_macro_slot *slot) {
    const struct user_macro_slot_state *state = &current_slots[slot_index];
    memset(slot, 0, sizeof(*slot));
    slot->slot_index = slot_index;
    slot->behavior_local_id = slot_behavior_id(slot_index);
    if (state->occupancy == USER_MACRO_ACTIVE) {
        strlcpy(slot->name, state->name, sizeof(slot->name));
        slot->enabled = true;
        slot->encoded_size = state->length;
    } else {
        default_name(slot_index, slot->name, sizeof(slot->name));
        slot->enabled = false;
        slot->encoded_size = 0;
    }
    slot->dirty = state->dirty;
}

static int fill_full_locked(uint8_t slot_index, struct zmk_user_macro_slot *slot) {
    fill_summary_locked(slot_index, slot);
    if (current_slots[slot_index].occupancy != USER_MACRO_ACTIVE) {
        return 0;
    }
    return decode_body(&current_pool[current_slots[slot_index].offset],
                       current_slots[slot_index].length, slot);
}

static int apply_body_locked(uint8_t slot_index, const char *name, uint8_t occupancy,
                             const uint8_t *body, uint16_t body_len, bool dirty) {
    int ret = pool_store_body(slot_index, body, occupancy == USER_MACRO_ACTIVE ? body_len : 0);
    if (ret < 0) {
        return ret;
    }
    struct user_macro_slot_state *state = &current_slots[slot_index];
    state->occupancy = occupancy;
    state->dirty = dirty;
    memset(state->name, 0, sizeof(state->name));
    if (occupancy == USER_MACRO_ACTIVE) {
        strlcpy(state->name, name, sizeof(state->name));
    } else {
        default_name(slot_index, state->name, sizeof(state->name));
    }
    return 0;
}

static int encode_settings_blob(uint8_t slot_index, uint8_t *blob, size_t blob_max, size_t *out_len) {
    const struct user_macro_slot_state *state = &current_slots[slot_index];
    uint8_t flags = 0;
    size_t name_len = 0;
    uint16_t body_len = 0;

    if (state->occupancy == USER_MACRO_ACTIVE) {
        flags = USER_MACRO_FLAG_ENABLED;
        name_len = strnlen(state->name, sizeof(state->name));
        body_len = state->length;
    } else if (state->occupancy == USER_MACRO_TOMBSTONE) {
        flags = USER_MACRO_FLAG_TOMBSTONE;
    } else {
        return -ENOENT;
    }

    size_t need = 1 + 1 + 1 + name_len + 2 + body_len;
    if (need > blob_max) {
        return -ENOSPC;
    }

    size_t offset = 0;
    blob[offset++] = USER_MACRO_RECORD_VERSION;
    blob[offset++] = flags;
    blob[offset++] = (uint8_t)name_len;
    if (name_len > 0) {
        memcpy(&blob[offset], state->name, name_len);
        offset += name_len;
    }
    sys_put_le16(body_len, &blob[offset]);
    offset += 2;
    if (body_len > 0) {
        memcpy(&blob[offset], &current_pool[state->offset], body_len);
        offset += body_len;
    }
    *out_len = offset;
    return 0;
}

static int load_v2_blob(uint8_t slot_index, const uint8_t *blob, size_t len) {
    if (len < 5 || blob[0] != USER_MACRO_RECORD_VERSION) {
        return -EINVAL;
    }
    uint8_t flags = blob[1];
    uint8_t name_len = blob[2];
    if (3u + name_len + 2u > len) {
        return -EINVAL;
    }
    char name[CONFIG_ZMK_MACRO_SETTINGS_NAME_MAX_LEN] = {0};
    if (name_len >= sizeof(name)) {
        return -EINVAL;
    }
    memcpy(name, &blob[3], name_len);
    uint16_t body_len = sys_get_le16(&blob[3 + name_len]);
    if (5u + name_len + body_len != len) {
        return -EINVAL;
    }
    const uint8_t *body = &blob[5 + name_len];
    uint8_t occupancy = USER_MACRO_TOMBSTONE;
    if (flags & USER_MACRO_FLAG_ENABLED) {
        occupancy = USER_MACRO_ACTIVE;
        if (name_len == 0 || body_len == 0) {
            return -EINVAL;
        }
    } else if (!(flags & USER_MACRO_FLAG_TOMBSTONE)) {
        occupancy = USER_MACRO_VACANT;
        body_len = 0;
    }
    return apply_body_locked(slot_index, name, occupancy, body, body_len, false);
}

static int load_v1_record(uint8_t slot_index, const struct user_macro_v1_record *record) {
    if (!record->enabled) {
        return apply_body_locked(slot_index, record->name, USER_MACRO_TOMBSTONE, NULL, 0, false);
    }

    struct zmk_user_macro_slot slot = {
        .slot_index = slot_index,
        .enabled = true,
        .step_count = record->step_count,
    };
    /* Preserve the fixed-size field so validation rejects an unterminated name
     * without reading beyond the stored record. */
    memcpy(slot.name, record->name, sizeof(slot.name));
    for (uint8_t i = 0; i < record->step_count && i < ARRAY_SIZE(slot.steps); i++) {
        slot.steps[i] = (struct zmk_user_macro_step){
            .action = record->steps[i].action,
            .behavior_local_id = record->steps[i].behavior_local_id,
            .param1 = record->steps[i].param1,
            .wait_ms = record->steps[i].wait_ms,
        };
    }
    if (validate_slot(&slot) < 0) {
        return -EINVAL;
    }

    uint8_t body[CONFIG_ZMK_MACRO_SETTINGS_MAX_BYTES];
    uint16_t body_len = 0;
    int ret = encode_body(&slot, body, sizeof(body), &body_len);
    if (ret < 0) {
        return ret;
    }
    return apply_body_locked(slot_index, slot.name, USER_MACRO_ACTIVE, body, body_len, false);
}

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_user_macro_default)
static int seed_text_locked(uint8_t slot_index, const char *name, const char *text) {
    if (slot_index >= ARRAY_SIZE(current_slots) ||
        current_slots[slot_index].occupancy != USER_MACRO_VACANT) {
        return 0;
    }

    uint8_t body[CONFIG_ZMK_MACRO_SETTINGS_MAX_BYTES];
    uint16_t body_len = 0;
    int ret = encode_text_body(text, body, sizeof(body), &body_len);
    if (ret < 0) {
        LOG_WRN("Skipping user macro default for slot %u (%d)", slot_index, ret);
        return 0;
    }

    char seeded_name[CONFIG_ZMK_MACRO_SETTINGS_NAME_MAX_LEN];
    if (name && name[0]) {
        strlcpy(seeded_name, name, sizeof(seeded_name));
    } else {
        default_name(slot_index, seeded_name, sizeof(seeded_name));
    }
    return apply_body_locked(slot_index, seeded_name, USER_MACRO_ACTIVE, body, body_len, false);
}

static void seed_defaults_locked(void) {
#define USER_MACRO_SEED_NODE(n)                                                                    \
    seed_text_locked(DT_PROP(n, slot), DT_PROP_OR(n, macro_name, ""), DT_PROP(n, text));
    DT_FOREACH_STATUS_OKAY(zmk_user_macro_default, USER_MACRO_SEED_NODE)
#undef USER_MACRO_SEED_NODE
}
#else
static void seed_defaults_locked(void) {}
#endif

static int reset_slot_to_factory_locked(uint8_t slot_index) {
    char setting_name[24];
    snprintf(setting_name, sizeof(setting_name), USER_MACRO_SETTING_SUBTREE "/" USER_MACRO_SETTING_SLOT_KEY,
             slot_index);
    settings_delete(setting_name);
    clear_slot_state(&current_slots[slot_index]);
    pool_store_body(slot_index, NULL, 0);
    seed_defaults_locked();
    current_slots[slot_index].dirty = false;
    return 0;
}

size_t zmk_user_macro_get_slot_count(void) {
    return CONFIG_ZMK_MACRO_SETTINGS_MAX_MACROS;
}

size_t zmk_user_macro_get_max_steps(void) {
    return CONFIG_ZMK_MACRO_SETTINGS_MAX_STEPS_PER_MACRO;
}

size_t zmk_user_macro_get_max_bytes(void) {
    return CONFIG_ZMK_MACRO_SETTINGS_MAX_BYTES;
}

size_t zmk_user_macro_get_pool_total(void) {
    return CONFIG_ZMK_MACRO_SETTINGS_POOL_BYTES;
}

size_t zmk_user_macro_get_pool_used(void) {
    k_mutex_lock(&user_macro_mutex, K_FOREVER);
    size_t used = current_used;
    k_mutex_unlock(&user_macro_mutex);
    return used;
}

uint32_t zmk_user_macro_get_tap_ms(void) {
    k_mutex_lock(&user_macro_mutex, K_FOREVER);
    uint32_t tap_ms = current_tap_ms;
    k_mutex_unlock(&user_macro_mutex);
    return tap_ms;
}

int zmk_user_macro_set_tap_ms(uint32_t tap_ms) {
    if (tap_ms == 0 || tap_ms > CONFIG_ZMK_MACRO_SETTINGS_MAX_TAP_MS) {
        return -EINVAL;
    }
    k_mutex_lock(&user_macro_mutex, K_FOREVER);
    tap_ms_dirty = tap_ms_dirty || current_tap_ms != tap_ms;
    current_tap_ms = tap_ms;
    k_mutex_unlock(&user_macro_mutex);
    return 0;
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

int zmk_user_macro_get_summary(uint8_t slot_index, struct zmk_user_macro_slot *slot) {
    if (!slot || slot_index >= ARRAY_SIZE(current_slots)) {
        return -EINVAL;
    }
    k_mutex_lock(&user_macro_mutex, K_FOREVER);
    fill_summary_locked(slot_index, slot);
    k_mutex_unlock(&user_macro_mutex);
    return 0;
}

int zmk_user_macro_get_slot(uint8_t slot_index, struct zmk_user_macro_slot *slot) {
    if (!slot || slot_index >= ARRAY_SIZE(current_slots)) {
        return -EINVAL;
    }
    k_mutex_lock(&user_macro_mutex, K_FOREVER);
    int ret = fill_full_locked(slot_index, slot);
    k_mutex_unlock(&user_macro_mutex);
    return ret;
}

int zmk_user_macro_set_slot(const struct zmk_user_macro_slot *slot) {
    if (!slot || slot->slot_index >= ARRAY_SIZE(current_slots)) {
        return -EINVAL;
    }
    int ret = validate_slot(slot);
    if (ret < 0) {
        return ret;
    }

    uint8_t body[CONFIG_ZMK_MACRO_SETTINGS_MAX_BYTES];
    uint16_t body_len = 0;
    uint8_t occupancy = slot->enabled ? USER_MACRO_ACTIVE : USER_MACRO_TOMBSTONE;
    if (slot->enabled) {
        ret = encode_body(slot, body, sizeof(body), &body_len);
        if (ret < 0) {
            return ret;
        }
    }

    k_mutex_lock(&user_macro_mutex, K_FOREVER);
    ret = apply_body_locked(slot->slot_index, slot->name, occupancy, body, body_len, true);
    k_mutex_unlock(&user_macro_mutex);
    return ret;
}

int zmk_user_macro_reset_slot(uint8_t slot_index) {
    if (slot_index >= ARRAY_SIZE(current_slots)) {
        return -EINVAL;
    }
    k_mutex_lock(&user_macro_mutex, K_FOREVER);
    int ret = reset_slot_to_factory_locked(slot_index);
    k_mutex_unlock(&user_macro_mutex);
    return ret;
}

struct user_macro_queue_state {
    const struct zmk_user_macro_slot *slot;
    uint32_t tap_ms;
    uint8_t step_index;
    uint16_t packed_index;
    bool release;
};

static void next_macro_entry(void *context, struct zmk_behavior_queue_entry *entry) {
    struct user_macro_queue_state *state = context;
    const struct zmk_user_macro_slot *slot = state->slot;
    if (state->step_index == 0 && slot->steps[0].action == ZMK_USER_MACRO_STEP_WAIT) {
        // Leading waits have no preceding release to attach to; reserve a delay-only entry.
        while (state->step_index < slot->step_count &&
               slot->steps[state->step_index].action == ZMK_USER_MACRO_STEP_WAIT) {
            entry->wait_ms += slot->steps[state->step_index++].wait_ms;
        }
        return;
    }
    const struct zmk_user_macro_step *step = &slot->steps[state->step_index];
    bool text = step->action == ZMK_USER_MACRO_STEP_TEXT;
    bool tap = text || step->action == ZMK_USER_MACRO_STEP_TAP;
    entry->binding = (struct zmk_behavior_binding){
        .behavior_dev = KEY_PRESS_DEVICE_NAME,
        .param1 = text ? packed_to_usage(slot->packed_keys[step->packed_off + state->packed_index])
                      : step->param1,
    };
    entry->press = tap ? !state->release : step->action == ZMK_USER_MACRO_STEP_PRESS;
    if (tap && !state->release) {
        state->release = true;
        entry->wait_ms = state->tap_ms;
        return;
    }
    state->release = false;
    entry->wait_ms = CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS;
    if (text && ++state->packed_index < step->packed_len) {
        return;
    }
    state->packed_index = 0;
    state->step_index++;
    while (state->step_index < slot->step_count &&
           slot->steps[state->step_index].action == ZMK_USER_MACRO_STEP_WAIT) {
        entry->wait_ms += slot->steps[state->step_index++].wait_ms;
    }
}

int zmk_user_macro_queue(uint8_t slot_index, const struct zmk_behavior_binding_event *event) {
    if (!event || slot_index >= ARRAY_SIZE(current_slots)) {
        return -EINVAL;
    }

    struct zmk_user_macro_slot slot;
    uint32_t tap_ms;
    k_mutex_lock(&user_macro_mutex, K_FOREVER);
    int ret = fill_full_locked(slot_index, &slot);
    tap_ms = current_tap_ms;
    k_mutex_unlock(&user_macro_mutex);
    if (ret < 0) {
        return ret;
    }
    if (!slot.enabled) {
        return 0;
    }

    /* Validate persisted bodies too, before any key can be pressed. */
    ret = validate_slot(&slot);
    if (ret < 0) {
        return ret;
    }
    struct user_macro_queue_state state = {.slot = &slot, .tap_ms = tap_ms};
    return zmk_behavior_queue_add_batch(event, expanded_event_count(&slot), next_macro_entry, &state);
}

int zmk_user_macro_check_unsaved_changes(void) {
    k_mutex_lock(&user_macro_mutex, K_FOREVER);
    int dirty = tap_ms_dirty ? 1 : 0;
    for (uint8_t i = 0; i < ARRAY_SIZE(current_slots) && !dirty; i++) {
        if (current_slots[i].dirty) {
            dirty = 1;
        }
    }
    k_mutex_unlock(&user_macro_mutex);
    return dirty;
}

int zmk_user_macro_save_changes(void) {
    k_mutex_lock(&user_macro_mutex, K_FOREVER);
    for (uint8_t i = 0; i < ARRAY_SIZE(current_slots); i++) {
        char setting_name[24];
        snprintf(setting_name, sizeof(setting_name),
                 USER_MACRO_SETTING_SUBTREE "/" USER_MACRO_SETTING_SLOT_KEY, i);
        if (current_slots[i].occupancy == USER_MACRO_VACANT) {
            settings_delete(setting_name);
            current_slots[i].dirty = false;
            continue;
        }
        uint8_t blob[3 + CONFIG_ZMK_MACRO_SETTINGS_NAME_MAX_LEN + 2 +
                     CONFIG_ZMK_MACRO_SETTINGS_MAX_BYTES];
        size_t blob_len = 0;
        int ret = encode_settings_blob(i, blob, sizeof(blob), &blob_len);
        if (ret < 0) {
            k_mutex_unlock(&user_macro_mutex);
            return ret;
        }
        ret = settings_save_one(setting_name, blob, blob_len);
        if (ret < 0) {
            k_mutex_unlock(&user_macro_mutex);
            return ret;
        }
        current_slots[i].dirty = false;
    }
    if (tap_ms_dirty) {
        int ret = settings_save_one(USER_MACRO_SETTING_SUBTREE "/" USER_MACRO_SETTING_TAP_MS,
                                    &current_tap_ms, sizeof(current_tap_ms));
        if (ret < 0) {
            k_mutex_unlock(&user_macro_mutex);
            return ret;
        }
        tap_ms_dirty = false;
    }
    k_mutex_unlock(&user_macro_mutex);
    return 0;
}

static int user_macro_handle_set(const char *name, size_t len, settings_read_cb read_cb,
                                 void *cb_arg) {
    const char *next;
    if (settings_name_steq(name, USER_MACRO_SETTING_TAP_MS, &next) && (!next || !next[0])) {
        uint32_t tap_ms = 0;
        int ret = read_cb(cb_arg, &tap_ms, MIN(len, sizeof(tap_ms)));
        if (ret != sizeof(tap_ms) || tap_ms == 0 ||
            tap_ms > CONFIG_ZMK_MACRO_SETTINGS_MAX_TAP_MS) {
            LOG_WRN("Ignoring invalid user macro tap_ms");
            return 0;
        }
        current_tap_ms = tap_ms;
        tap_ms_dirty = false;
        return 0;
    }

    if (!settings_name_steq(name, "s", &next) || !next) {
        return 0;
    }

    char *endptr;
    unsigned long slot = strtoul(next, &endptr, 10);
    if (*endptr != '\0' || slot >= ARRAY_SIZE(current_slots)) {
        LOG_WRN("Ignoring unavailable user macro slot %s", next);
        return 0;
    }

    /* Share storage between formats instead of keeping two legacy records on
     * the boot thread's stack. V2 has a five-byte header plus name and body. */
    union {
        struct user_macro_v1_record v1;
        uint8_t bytes[MAX(sizeof(struct user_macro_v1_record),
                          5 + CONFIG_ZMK_MACRO_SETTINGS_NAME_MAX_LEN - 1 +
                              CONFIG_ZMK_MACRO_SETTINGS_MAX_BYTES)];
    } blob;
    if (len == 0 || len > sizeof(blob.bytes)) {
        LOG_WRN("Ignoring invalid user macro setting %lu", slot);
        return 0;
    }
    int ret = read_cb(cb_arg, blob.bytes, len);
    if (ret < 1 || (size_t)ret != len) {
        LOG_WRN("Ignoring invalid user macro setting %lu", slot);
        return 0;
    }
    if (blob.bytes[0] == USER_MACRO_RECORD_VERSION_V1 &&
        (size_t)ret == sizeof(struct user_macro_v1_record) &&
        len == sizeof(struct user_macro_v1_record)) {
        if (load_v1_record((uint8_t)slot, &blob.v1) < 0) {
            LOG_WRN("Ignoring invalid v1 user macro %lu", slot);
        }
        return 0;
    }
    if (load_v2_blob((uint8_t)slot, blob.bytes, (size_t)ret) < 0) {
        LOG_WRN("Ignoring invalid user macro setting %lu", slot);
    }
    return 0;
}

static int user_macro_handle_commit(void) {
    seed_defaults_locked();
    tap_ms_dirty = false;
    for (uint8_t i = 0; i < ARRAY_SIZE(current_slots); i++) {
        current_slots[i].dirty = false;
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(user_macros, USER_MACRO_SETTING_SUBTREE, NULL,
                               user_macro_handle_set, user_macro_handle_commit, NULL);

static void reset_all_locked(bool delete_settings) {
    for (uint8_t i = 0; i < ARRAY_SIZE(current_slots); i++) {
        if (delete_settings) {
            char setting_name[24];
            snprintf(setting_name, sizeof(setting_name),
                     USER_MACRO_SETTING_SUBTREE "/" USER_MACRO_SETTING_SLOT_KEY, i);
            settings_delete(setting_name);
        }
        clear_slot_state(&current_slots[i]);
    }
    current_used = 0;
    current_tap_ms = CONFIG_ZMK_MACRO_DEFAULT_TAP_MS;
    tap_ms_dirty = false;
    if (delete_settings) {
        settings_delete(USER_MACRO_SETTING_SUBTREE "/" USER_MACRO_SETTING_TAP_MS);
        seed_defaults_locked();
    }
}

int zmk_user_macro_discard_changes(void) {
    k_mutex_lock(&user_macro_mutex, K_FOREVER);
    reset_all_locked(false);
    k_mutex_unlock(&user_macro_mutex);
    return settings_load_subtree(USER_MACRO_SETTING_SUBTREE);
}

int zmk_user_macro_reset_settings(void) {
    k_mutex_lock(&user_macro_mutex, K_FOREVER);
    reset_all_locked(true);
    k_mutex_unlock(&user_macro_mutex);
    return 0;
}

static int user_macro_init(void) {
    current_tap_ms = CONFIG_ZMK_MACRO_DEFAULT_TAP_MS;
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
