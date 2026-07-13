/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_combos

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/dlist.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/stdlib.h>
#include <zmk/virtual_key_position.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO > 0

#warning                                                                                           \
    "CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO is deprecated, and is auto-calculated from the devicetree now."

#endif

#if CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY > 0

#warning "CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY is deprecated, and is auto-calculated."

#endif

#define COMBOS_KEYS_BYTE_ARRAY(node_id)                                                            \
    uint8_t _CONCAT(combo_prop_, node_id)[DT_PROP_LEN(node_id, key_positions)];

#define STOCK_MAX_COMBO_KEYS sizeof(union {DT_INST_FOREACH_CHILD(0, COMBOS_KEYS_BYTE_ARRAY)})

#if IS_ENABLED(CONFIG_ZMK_COMBO_SETTINGS)
#define MAX_COMBO_KEYS MAX(STOCK_MAX_COMBO_KEYS, CONFIG_ZMK_COMBO_SETTINGS_MAX_KEYS_PER_COMBO)
#define COMBO_STORAGE_COUNT CONFIG_ZMK_COMBO_SETTINGS_MAX_COMBOS
#else
#define MAX_COMBO_KEYS STOCK_MAX_COMBO_KEYS
#endif

struct combo_cfg {
#if IS_ENABLED(CONFIG_ZMK_COMBO_SETTINGS)
    char combo_id[CONFIG_ZMK_COMBO_SETTINGS_ID_MAX_LEN];
    enum zmk_combo_source source;
    bool enabled;
#endif
    int32_t key_positions[MAX_COMBO_KEYS];
    int16_t key_position_len;
    int16_t require_prior_idle_ms;
    int32_t timeout_ms;
    uint32_t layer_mask;
    struct zmk_behavior_binding behavior;
    // if slow release is set, the combo releases when the last key is released.
    // otherwise, the combo releases when the first key is released.
    bool slow_release;
};

struct active_combo {
    uint16_t combo_idx;
    // key_positions_pressed is filled with key_positions when the combo is pressed.
    // The keys are removed from this array when they are released.
    // Once this array is empty, the behavior is released.
    uint16_t key_positions_pressed_count;
    struct zmk_position_state_changed_event key_positions_pressed[MAX_COMBO_KEYS];
};

#define PROP_BIT_AT_IDX(n, prop, idx) BIT(DT_PROP_BY_IDX(n, prop, idx))

#define NODE_PROP_BITMASK(n, prop)                                                                 \
    COND_CODE_1(DT_NODE_HAS_PROP(n, prop),                                                         \
                (DT_FOREACH_PROP_ELEM_SEP(n, prop, PROP_BIT_AT_IDX, (|))), (0))

#define GET_KEY_POSITION_MASK_PORTION(idx, n) ((NODE_PROP_BITMASK(n, key_positions) >> idx) & 0xFF)

#if IS_ENABLED(CONFIG_ZMK_COMBO_SETTINGS)
#define COMBO_ID(n) DT_NODE_FULL_NAME(n)
#define COMBO_ENABLED true
#define COMBO_SOURCE ZMK_COMBO_SOURCE_STOCK
#else
#define COMBO_ENABLED
#define COMBO_SOURCE
#endif

#if IS_ENABLED(CONFIG_ZMK_COMBO_SETTINGS)
#define COMBO_SETTINGS_FIELDS(n)                                                                   \
    .combo_id = COMBO_ID(n), .source = COMBO_SOURCE, .enabled = COMBO_ENABLED,
#else
#define COMBO_SETTINGS_FIELDS(n)
#endif

#define COMBO_INST(n, positions)                                                                   \
    COND_CODE_1(IS_EQ(DT_PROP_LEN(n, key_positions), positions),                                   \
                (                                                                                  \
                    {                                                                              \
                        COMBO_SETTINGS_FIELDS(n)                                                    \
                        .timeout_ms = DT_PROP(n, timeout_ms),                                      \
                        .require_prior_idle_ms = DT_PROP(n, require_prior_idle_ms),                \
                        .key_positions = DT_PROP(n, key_positions),                                \
                        .key_position_len = DT_PROP_LEN(n, key_positions),                         \
                        .behavior = ZMK_KEYMAP_EXTRACT_BINDING(0, n),                              \
                        .slow_release = DT_PROP(n, slow_release),                                  \
                        .layer_mask = NODE_PROP_BITMASK(n, layers),                                \
                    }, ),                                                                          \
                ())

#define COMBO_CONFIGS_WITH_MATCHING_POSITIONS_LEN(positions, _ignore)                              \
    DT_INST_FOREACH_CHILD_VARGS(0, COMBO_INST, positions)

// We do some magic here to generate the stock combo array by "key position length", looping
// by key position length and on each iteration, only include entries where the `key-positions`
// length matches. Doing so allows our bitmasks to be "shorted key positions list first".
// `20` is chosen as a reasonable limit, since the theoretical maximum number of keys you might
// reasonably press simultaneously with 10 fingers is 20 keys, two keys per finger.
static const struct combo_cfg stock_combos[] = {
    LISTIFY(20, COMBO_CONFIGS_WITH_MATCHING_POSITIONS_LEN, (), 0)};

#define COMBO_ONE(n) +1
#define COMBO_CHILDREN_COUNT (0 DT_INST_FOREACH_CHILD(0, COMBO_ONE))

#if IS_ENABLED(CONFIG_ZMK_COMBO_SETTINGS)
BUILD_ASSERT(CONFIG_ZMK_COMBO_SETTINGS_MAX_COMBOS >= COMBO_CHILDREN_COUNT,
             "CONFIG_ZMK_COMBO_SETTINGS_MAX_COMBOS must fit all stock combos");
BUILD_ASSERT(CONFIG_ZMK_COMBO_SETTINGS_MAX_KEYS_PER_COMBO >= STOCK_MAX_COMBO_KEYS,
             "CONFIG_ZMK_COMBO_SETTINGS_MAX_KEYS_PER_COMBO must fit stock combo key positions");

static struct combo_cfg combos[COMBO_STORAGE_COUNT];
static struct combo_cfg stock_default_combos[COMBO_STORAGE_COUNT];
static struct combo_cfg saved_combos[COMBO_STORAGE_COUNT];
static bool combos_ready;
static K_MUTEX_DEFINE(combo_mutex);
#else
static const struct combo_cfg *combos = stock_combos;
#define COMBO_STORAGE_COUNT COMBO_CHILDREN_COUNT
#endif

// We need at least 4 bytes to avoid alignment issues.
#define BYTES_FOR_COMBOS_MASK DIV_ROUND_UP(COMBO_STORAGE_COUNT, 32)

static uint8_t pressed_keys_count = 0;
// set of keys pressed
static struct zmk_position_state_changed_event pressed_keys[MAX_COMBO_KEYS] = {};
// the set of candidate combos based on the currently pressed_keys
static uint32_t candidates[BYTES_FOR_COMBOS_MASK];
// the last candidate that was completely pressed
static int16_t fully_pressed_combo = INT16_MAX;
// a lookup dict that maps a key position to all combos on that position
static uint32_t combo_lookup[ZMK_KEYMAP_LEN][BYTES_FOR_COMBOS_MASK] = {};
// combos that have been activated and still have (some) keys pressed
// this array is always contiguous from 0.
static struct active_combo active_combos[CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS] = {};
static uint8_t active_combo_count = 0;

static struct k_work_delayable timeout_task;
static int64_t timeout_task_timeout_at;

// this keeps track of the last non-combo, non-mod key tap
static int64_t last_tapped_timestamp = INT32_MIN;
// this keeps track of the last time a combo was pressed
static int64_t last_combo_timestamp = INT32_MIN;

static void store_last_tapped(int64_t timestamp) {
    if (timestamp > last_combo_timestamp) {
        last_tapped_timestamp = timestamp;
    }
}

static bool combo_is_enabled(uint16_t index) {
#if IS_ENABLED(CONFIG_ZMK_COMBO_SETTINGS)
    return combos[index].enabled;
#else
    ARG_UNUSED(index);
    return true;
#endif
}

static void clear_combo_lookup(void) {
    memset(combo_lookup, 0, sizeof(combo_lookup));
}

// Store the combo key pointer in the lookup array, one pointer for each key position.
static int initialize_combo(size_t index) {
    const struct combo_cfg *new_combo = &combos[index];

    if (!combo_is_enabled(index)) {
        return 0;
    }

    for (size_t kp = 0; kp < new_combo->key_position_len; kp++) {
        sys_bitfield_set_bit((mem_addr_t)&combo_lookup[new_combo->key_positions[kp]], index);
    }

    return 0;
}

static void rebuild_combo_lookup(void) {
    clear_combo_lookup();
    for (int i = 0; i < COMBO_STORAGE_COUNT; i++) {
        initialize_combo(i);
    }
}

static bool combo_active_on_layer(const struct combo_cfg *combo, uint8_t layer) {
    if (!combo->layer_mask) {
        return true;
    }

    return combo->layer_mask & BIT(layer);
}

static bool is_quick_tap(const struct combo_cfg *combo, int64_t timestamp) {
    return (last_tapped_timestamp + combo->require_prior_idle_ms) > timestamp;
}

static int setup_candidates_for_first_keypress(int32_t position, int64_t timestamp) {
    int number_of_combo_candidates = 0;
    uint8_t highest_active_layer = zmk_keymap_highest_layer_active();

    for (size_t i = 0; i < COMBO_STORAGE_COUNT; i++) {
        if (!combo_is_enabled(i)) {
            continue;
        }
        if (sys_bitfield_test_bit((mem_addr_t)&combo_lookup[position], i)) {
            const struct combo_cfg *combo = &combos[i];
            if (combo_active_on_layer(combo, highest_active_layer) &&
                !is_quick_tap(combo, timestamp)) {
                sys_bitfield_set_bit((mem_addr_t)&candidates, i);
                number_of_combo_candidates++;
            }
        }
    }

    return number_of_combo_candidates;
}

static inline uint8_t zero_one_or_more_bits(uint32_t field) {
    if (field == 0) {
        return 0;
    }
    if ((field & (field - 1)) == 0) {
        return 1;
    }
    return 2;
}

static int filter_candidates(int32_t position) {
    int matches = 0;
    for (int i = 0; i < BYTES_FOR_COMBOS_MASK; i++) {
        candidates[i] &= combo_lookup[position][i];
        if (matches < 2) {
            matches += zero_one_or_more_bits(candidates[i]);
        }
    }

    LOG_DBG("combo matches after filter %d", matches);
    return matches;
}

static int64_t first_candidate_timeout(void) {
    if (pressed_keys_count == 0) {
        return INT64_MAX;
    }

    int64_t first_timeout = INT64_MAX;
    for (int i = 0; i < COMBO_STORAGE_COUNT; i++) {
        if (combo_is_enabled(i) && sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {
            first_timeout = MIN(first_timeout, combos[i].timeout_ms);
        }
    }

    return pressed_keys[0].data.timestamp + first_timeout;
}

static inline bool candidate_is_completely_pressed(const struct combo_cfg *candidate) {
    // this code assumes set(pressed_keys) <= set(candidate->key_positions)
    // this invariant is enforced by filter_candidates
    // since events may have been reraised after clearing one or more slots at
    // the start of pressed_keys (see: release_pressed_keys), we have to check
    // that each key needed to trigger the combo was pressed, not just the last.
    return candidate->key_position_len == pressed_keys_count;
}

static int cleanup(void);

static int filter_timed_out_candidates(int64_t timestamp) {
    __ASSERT(pressed_keys_count > 0, "Searching for a candidate timeout with no keys pressed");

    int remaining_candidates = 0;
    for (int i = 0; i < COMBO_STORAGE_COUNT; i++) {
        if (combo_is_enabled(i) && sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {

            if (pressed_keys[0].data.timestamp + combos[i].timeout_ms > timestamp) {
                remaining_candidates++;
            } else {
                sys_bitfield_clear_bit((mem_addr_t)&candidates, i);
            }
        }
    }

    LOG_DBG(
        "after filtering out timed out combo candidates: remaining_candidates=%d timestamp=%lld",
        remaining_candidates, timestamp);

    return remaining_candidates;
}

static int capture_pressed_key(const struct zmk_position_state_changed *ev) {
    if (pressed_keys_count == MAX_COMBO_KEYS) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    pressed_keys[pressed_keys_count++] = copy_raised_zmk_position_state_changed(ev);
    return ZMK_EV_EVENT_CAPTURED;
}

const struct zmk_listener zmk_listener_combo;

static int release_pressed_keys(void) {
    uint8_t count = pressed_keys_count;
    pressed_keys_count = 0;
    for (int i = 0; i < count; i++) {
        struct zmk_position_state_changed_event *ev = &pressed_keys[i];
        if (i == 0) {
            LOG_DBG("combo: releasing position event %d", ev->data.position);
            ZMK_EVENT_RELEASE(*ev);
        } else {
            // reprocess events (see tests/combo/fully-overlapping-combos-3 for why this is needed)
            LOG_DBG("combo: reraising position event %d", ev->data.position);
            ZMK_EVENT_RAISE(*ev);
        }
    }

    return count;
}

static inline int press_combo_behavior(int combo_idx, const struct combo_cfg *combo,
                                       int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_COMBO(combo_idx),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    last_combo_timestamp = timestamp;

    return zmk_behavior_invoke_binding(&combo->behavior, event, true);
}

static inline int release_combo_behavior(int combo_idx, const struct combo_cfg *combo,
                                         int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_COMBO(combo_idx),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    return zmk_behavior_invoke_binding(&combo->behavior, event, false);
}

static void move_pressed_keys_to_active_combo(struct active_combo *active_combo) {

    int combo_length = MIN(pressed_keys_count, combos[active_combo->combo_idx].key_position_len);
    for (int i = 0; i < combo_length; i++) {
        active_combo->key_positions_pressed[i] = pressed_keys[i];
    }
    active_combo->key_positions_pressed_count = combo_length;

    // move any other pressed keys up
    for (int i = 0; i + combo_length < pressed_keys_count; i++) {
        pressed_keys[i] = pressed_keys[i + combo_length];
    }

    pressed_keys_count -= combo_length;
}

static struct active_combo *store_active_combo(int32_t combo_idx) {
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        if (active_combos[i].combo_idx == UINT16_MAX) {
            active_combos[i].combo_idx = combo_idx;
            active_combo_count++;
            return &active_combos[i];
        }
    }
    LOG_ERR("Unable to store combo; already %d active. Increase "
            "CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS",
            CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS);
    return NULL;
}

static void activate_combo(int combo_idx) {
    struct active_combo *active_combo = store_active_combo(combo_idx);
    if (active_combo == NULL) {
        // unable to store combo
        release_pressed_keys();
        return;
    }
    move_pressed_keys_to_active_combo(active_combo);
    press_combo_behavior(combo_idx, &combos[combo_idx],
                         active_combo->key_positions_pressed[0].data.timestamp);
}

static void deactivate_combo(int active_combo_index) {
    active_combo_count--;
    if (active_combo_index != active_combo_count) {
        memcpy(&active_combos[active_combo_index], &active_combos[active_combo_count],
               sizeof(struct active_combo));
    }
    active_combos[active_combo_count] = (struct active_combo){0};
    active_combos[active_combo_count].combo_idx = UINT16_MAX;
}

/* returns true if a key was released. */
static bool release_combo_key(int32_t position, int64_t timestamp) {
    for (int combo_idx = 0; combo_idx < active_combo_count; combo_idx++) {
        struct active_combo *active_combo = &active_combos[combo_idx];

        bool key_released = false;
        bool all_keys_pressed = active_combo->key_positions_pressed_count ==
                                combos[active_combo->combo_idx].key_position_len;
        bool all_keys_released = true;
        for (int i = 0; i < active_combo->key_positions_pressed_count; i++) {
            if (key_released) {
                active_combo->key_positions_pressed[i - 1] = active_combo->key_positions_pressed[i];
                all_keys_released = false;
            } else if (active_combo->key_positions_pressed[i].data.position != position) {
                all_keys_released = false;
            } else { // position matches
                key_released = true;
            }
        }

        if (key_released) {
            active_combo->key_positions_pressed_count--;
            const struct combo_cfg *c = &combos[active_combo->combo_idx];
            if ((c->slow_release && all_keys_released) || (!c->slow_release && all_keys_pressed)) {
                release_combo_behavior(active_combo->combo_idx, c, timestamp);
            }
            if (all_keys_released) {
                deactivate_combo(combo_idx);
            }
            return true;
        }
    }
    return false;
}

static int cleanup(void) {
    k_work_cancel_delayable(&timeout_task);
    memset(candidates, 0, BYTES_FOR_COMBOS_MASK * sizeof(uint32_t));
    if (fully_pressed_combo != INT16_MAX) {
        activate_combo(fully_pressed_combo);
        fully_pressed_combo = INT16_MAX;
    }
    return release_pressed_keys();
}

static void update_timeout_task(void) {
    int64_t first_timeout = first_candidate_timeout();
    if (timeout_task_timeout_at == first_timeout) {
        return;
    }
    if (first_timeout == INT64_MAX) {
        timeout_task_timeout_at = 0;
        k_work_cancel_delayable(&timeout_task);
        return;
    }
    if (k_work_schedule(&timeout_task, K_MSEC(first_timeout - k_uptime_get())) >= 0) {
        timeout_task_timeout_at = first_timeout;
    }
}

static int position_state_down(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    int num_candidates;
    if (!pressed_keys_count) {
        num_candidates = setup_candidates_for_first_keypress(data->position, data->timestamp);
        if (num_candidates == 0) {
            return ZMK_EV_EVENT_BUBBLE;
        }
    } else {
        filter_timed_out_candidates(data->timestamp);
        num_candidates = filter_candidates(data->position);
    }

    LOG_DBG("combo: capturing position event %d", data->position);
    int ret = capture_pressed_key(data);
    update_timeout_task();

    if (num_candidates) {
        for (int i = 0; i < COMBO_STORAGE_COUNT; i++) {
            if (combo_is_enabled(i) && sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {
                const struct combo_cfg *candidate_combo = &combos[i];
                if (candidate_is_completely_pressed(candidate_combo)) {
                    fully_pressed_combo = i;
                    if (num_candidates == 1) {
                        cleanup();
                    }
                }

                return ret;
            }
        }
    } else {
        cleanup();
        return ret;
    }

    return -EINVAL;
}

static int position_state_up(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    int released_keys = cleanup();
    if (release_combo_key(data->position, data->timestamp)) {
        return ZMK_EV_EVENT_HANDLED;
    }
    if (released_keys > 1) {
        // The second and further key down events are re-raised. To preserve
        // correct order for e.g. hold-taps, reraise the key up event too.
        struct zmk_position_state_changed_event dupe_ev =
            copy_raised_zmk_position_state_changed(data);
        ZMK_EVENT_RAISE(dupe_ev);
        return ZMK_EV_EVENT_CAPTURED;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static void combo_timeout_handler(struct k_work *item) {
#if IS_ENABLED(CONFIG_ZMK_COMBO_SETTINGS)
    k_mutex_lock(&combo_mutex, K_FOREVER);
    if (!combos_ready) {
        k_mutex_unlock(&combo_mutex);
        return;
    }
#endif
    if (timeout_task_timeout_at == 0 || k_uptime_get() < timeout_task_timeout_at) {
#if IS_ENABLED(CONFIG_ZMK_COMBO_SETTINGS)
        k_mutex_unlock(&combo_mutex);
#endif
        return;
    }
    if (filter_timed_out_candidates(timeout_task_timeout_at) == 0) {
        LOG_DBG("CLEANUP!");
        cleanup();
    }

    LOG_DBG("ABOUT TO UPDATE IN TIMEOUT");
    update_timeout_task();
#if IS_ENABLED(CONFIG_ZMK_COMBO_SETTINGS)
    k_mutex_unlock(&combo_mutex);
#endif
}

static int position_state_changed_listener(const zmk_event_t *ev) {
    struct zmk_position_state_changed *data = as_zmk_position_state_changed(ev);
    if (data == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_COMBO_SETTINGS)
    k_mutex_lock(&combo_mutex, K_FOREVER);
    if (!combos_ready) {
        k_mutex_unlock(&combo_mutex);
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

    int ret;
    if (data->state) { // keydown
        ret = position_state_down(ev, data);
    } else { // keyup
        ret = position_state_up(ev, data);
    }

#if IS_ENABLED(CONFIG_ZMK_COMBO_SETTINGS)
    k_mutex_unlock(&combo_mutex);
#endif
    return ret;
}

static int keycode_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev->state && !is_mod(ev->usage_page, ev->keycode)) {
        store_last_tapped(ev->timestamp);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

int behavior_combo_listener(const zmk_event_t *eh) {
    if (as_zmk_position_state_changed(eh) != NULL) {
        return position_state_changed_listener(eh);
    } else if (as_zmk_keycode_state_changed(eh) != NULL) {
        return keycode_state_changed_listener(eh);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(combo, behavior_combo_listener);
ZMK_SUBSCRIPTION(combo, zmk_position_state_changed);
ZMK_SUBSCRIPTION(combo, zmk_keycode_state_changed);

#if IS_ENABLED(CONFIG_ZMK_COMBO_SETTINGS)

#define COMBO_SETTING_SUBTREE "combos"
#define COMBO_SETTING_SLOT_KEY "s/%d"

struct combo_setting_record {
    char combo_id[CONFIG_ZMK_COMBO_SETTINGS_ID_MAX_LEN];
    uint8_t source;
    bool enabled;
    uint8_t key_position_count;
    uint8_t layer_count;
    uint16_t behavior_local_id;
    uint32_t param1;
    uint32_t param2;
    uint16_t timeout_ms;
    int16_t require_prior_idle_ms;
    bool slow_release;
    uint8_t key_positions[CONFIG_ZMK_COMBO_SETTINGS_MAX_KEYS_PER_COMBO];
    uint8_t layers[CONFIG_ZMK_COMBO_SETTINGS_MAX_LAYERS];
} __packed;

static struct combo_setting_record loaded_settings[CONFIG_ZMK_COMBO_SETTINGS_MAX_COMBOS];
static bool loaded_setting_present[CONFIG_ZMK_COMBO_SETTINGS_MAX_COMBOS];

static void clear_loaded_settings(void) {
    memset(loaded_settings, 0, sizeof(loaded_settings));
    memset(loaded_setting_present, 0, sizeof(loaded_setting_present));
}

static bool combo_slot_equal(const struct combo_cfg *left, const struct combo_cfg *right) {
    if (left->source != right->source || left->enabled != right->enabled ||
        strcmp(left->combo_id, right->combo_id) != 0) {
        return false;
    }

    if (!left->enabled && !right->enabled) {
        return true;
    }

    if (left->key_position_len != right->key_position_len ||
        left->require_prior_idle_ms != right->require_prior_idle_ms ||
        left->timeout_ms != right->timeout_ms || left->layer_mask != right->layer_mask ||
        left->slow_release != right->slow_release ||
        left->behavior.param1 != right->behavior.param1 ||
        left->behavior.param2 != right->behavior.param2) {
        return false;
    }

    const char *left_behavior = left->behavior.behavior_dev ? left->behavior.behavior_dev : "";
    const char *right_behavior = right->behavior.behavior_dev ? right->behavior.behavior_dev : "";
    if (strcmp(left_behavior, right_behavior) != 0) {
        return false;
    }

    return memcmp(left->key_positions, right->key_positions,
                  sizeof(left->key_positions)) == 0;
}

static bool combo_slot_matches_stock_default(uint8_t slot_index) {
    return combo_slot_equal(&combos[slot_index], &stock_default_combos[slot_index]);
}

static void clear_runtime_state(void) {
    k_work_cancel_delayable(&timeout_task);
    pressed_keys_count = 0;
    memset(pressed_keys, 0, sizeof(pressed_keys));
    memset(candidates, 0, sizeof(candidates));
    fully_pressed_combo = INT16_MAX;
    memset(active_combos, 0, sizeof(active_combos));
    active_combo_count = 0;
    for (size_t i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        active_combos[i].combo_idx = UINT16_MAX;
    }
    timeout_task_timeout_at = 0;
}

static void user_combo_id(uint8_t slot_index, char *buffer, size_t size) {
    snprintf(buffer, size, "user:%u", slot_index);
}

static void disabled_user_combo(uint8_t slot_index, struct combo_cfg *combo) {
    memset(combo, 0, sizeof(*combo));
    combo->source = ZMK_COMBO_SOURCE_USER;
    combo->enabled = false;
    user_combo_id(slot_index, combo->combo_id, sizeof(combo->combo_id));
    combo->require_prior_idle_ms = -1;
    combo->timeout_ms = 50;
}

/*
 * Runtime combo settings are keyed by combo_id, not by devicetree child order.
 * Saving enabled=false for a stock combo is the persistent suppression
 * mechanism; reset_settings deletes that record and restores stock behavior.
 */
static void load_stock_defaults(void) {
    memset(stock_default_combos, 0, sizeof(stock_default_combos));
    for (uint8_t i = 0; i < COMBO_STORAGE_COUNT; i++) {
        if (i < ARRAY_SIZE(stock_combos)) {
            stock_default_combos[i] = stock_combos[i];
            stock_default_combos[i].source = ZMK_COMBO_SOURCE_STOCK;
            stock_default_combos[i].enabled = true;
        } else {
            disabled_user_combo(i, &stock_default_combos[i]);
        }
    }
}

static void reset_to_stock_defaults(void) {
    memcpy(combos, stock_default_combos, sizeof(combos));
}

static int stock_slot_for_id(const char *combo_id) {
    for (uint8_t i = 0; i < ARRAY_SIZE(stock_default_combos); i++) {
        if (stock_default_combos[i].source == ZMK_COMBO_SOURCE_STOCK &&
            strcmp(stock_default_combos[i].combo_id, combo_id) == 0) {
            return i;
        }
    }
    return -ENOENT;
}

static int user_slot_for_id(const char *combo_id) {
    if (strncmp(combo_id, "user:", 5) != 0) {
        return -EINVAL;
    }
    char *endptr;
    unsigned long slot = strtoul(combo_id + 5, &endptr, 10);
    if (*endptr != '\0' || slot >= COMBO_STORAGE_COUNT ||
        slot < ARRAY_SIZE(stock_combos)) {
        return -EINVAL;
    }
    return (int)slot;
}

static int slot_for_record(const struct combo_setting_record *record) {
    if (record->source == ZMK_COMBO_SOURCE_STOCK) {
        return stock_slot_for_id(record->combo_id);
    }
    return user_slot_for_id(record->combo_id);
}

static bool behavior_binding_is_valid(const struct zmk_behavior_binding *binding) {
    if (!binding->behavior_dev) {
        return false;
    }
    return zmk_behavior_validate_binding((struct zmk_behavior_binding *)binding) >= 0;
}

static bool values_are_unique_u8(const uint8_t *values, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        for (uint8_t j = i + 1; j < count; j++) {
            if (values[i] == values[j]) {
                return false;
            }
        }
    }
    return true;
}

static void sort_i32(int32_t *values, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        for (uint8_t j = i + 1; j < count; j++) {
            if (values[j] < values[i]) {
                int32_t tmp = values[i];
                values[i] = values[j];
                values[j] = tmp;
            }
        }
    }
}

static int record_to_combo(const struct combo_setting_record *record, struct combo_cfg *combo) {
    int slot = slot_for_record(record);
    if (slot < 0) {
        return slot;
    }

    struct combo_cfg next = stock_default_combos[slot];
    next.source = record->source;
    next.enabled = record->enabled;
    strlcpy(next.combo_id, record->combo_id, sizeof(next.combo_id));

    if (!next.enabled) {
        *combo = next;
        return 0;
    }

    if (record->key_position_count < 2 ||
        record->key_position_count > CONFIG_ZMK_COMBO_SETTINGS_MAX_KEYS_PER_COMBO ||
        record->layer_count > CONFIG_ZMK_COMBO_SETTINGS_MAX_LAYERS ||
        record->require_prior_idle_ms < -1 || !values_are_unique_u8(record->key_positions, record->key_position_count) ||
        !values_are_unique_u8(record->layers, record->layer_count)) {
        return -EINVAL;
    }

    for (uint8_t i = 0; i < record->key_position_count; i++) {
        if (record->key_positions[i] >= ZMK_KEYMAP_LEN) {
            return -EINVAL;
        }
    }

    for (uint8_t i = 0; i < record->layer_count; i++) {
        if (record->layers[i] >= ZMK_KEYMAP_LAYERS_LEN) {
            return -EINVAL;
        }
    }

    const char *behavior_name = zmk_behavior_find_behavior_name_from_local_id(record->behavior_local_id);
    if (!behavior_name) {
        return -ENODEV;
    }

    next.behavior = (struct zmk_behavior_binding){
        .behavior_dev = behavior_name,
        .param1 = record->param1,
        .param2 = record->param2,
    };
    if (!behavior_binding_is_valid(&next.behavior)) {
        return -EINVAL;
    }

    next.key_position_len = record->key_position_count;
    memset(next.key_positions, 0, sizeof(next.key_positions));
    for (uint8_t i = 0; i < record->key_position_count; i++) {
        next.key_positions[i] = record->key_positions[i];
    }
    sort_i32(next.key_positions, next.key_position_len);

    next.layer_mask = 0;
    for (uint8_t i = 0; i < record->layer_count; i++) {
        next.layer_mask |= BIT(record->layers[i]);
    }
    next.timeout_ms = record->timeout_ms;
    next.require_prior_idle_ms = record->require_prior_idle_ms;
    next.slow_release = record->slow_release;
    *combo = next;
    return 0;
}

static int combo_to_record(const struct combo_cfg *combo, struct combo_setting_record *record) {
    memset(record, 0, sizeof(*record));
    strlcpy(record->combo_id, combo->combo_id, sizeof(record->combo_id));
    record->source = combo->source;
    record->enabled = combo->enabled;
    record->timeout_ms = combo->timeout_ms;
    record->require_prior_idle_ms = combo->require_prior_idle_ms;
    record->slow_release = combo->slow_release;

    if (!combo->enabled) {
        return 0;
    }

    zmk_behavior_local_id_t behavior_id = zmk_behavior_get_local_id(combo->behavior.behavior_dev);
    if (behavior_id == UINT16_MAX) {
        return -ENODEV;
    }
    record->behavior_local_id = behavior_id;
    record->param1 = combo->behavior.param1;
    record->param2 = combo->behavior.param2;

    record->key_position_count = combo->key_position_len;
    for (uint8_t i = 0; i < combo->key_position_len; i++) {
        record->key_positions[i] = (uint8_t)combo->key_positions[i];
    }

    for (uint8_t layer = 0; layer < MIN(ZMK_KEYMAP_LAYERS_LEN, 32); layer++) {
        if ((combo->layer_mask & BIT(layer)) != 0) {
            if (record->layer_count >= CONFIG_ZMK_COMBO_SETTINGS_MAX_LAYERS) {
                return -ENOSPC;
            }
            record->layers[record->layer_count++] = layer;
        }
    }

    return 0;
}

static int combo_handle_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    if (!settings_name_steq(name, "s", &next) || !next) {
        return 0;
    }

    char *endptr;
    unsigned long slot = strtoul(next, &endptr, 10);
    if (*endptr != '\0') {
        LOG_WRN("Invalid combo slot setting: %s", next);
        return -EINVAL;
    }

    struct combo_setting_record record = {0};
    int err = read_cb(cb_arg, &record, MIN(len, sizeof(record)));
    if (err <= 0) {
        LOG_ERR("Failed to read combo setting %s (%d)", name, err);
        return err;
    }

    if (slot >= CONFIG_ZMK_COMBO_SETTINGS_MAX_COMBOS) {
        LOG_WRN("Ignoring combo setting for unavailable slot %lu", slot);
        return 0;
    }

    loaded_settings[slot] = record;
    loaded_setting_present[slot] = true;
    return 0;
}

static int combo_handle_commit(void) {
    k_mutex_lock(&combo_mutex, K_FOREVER);
    combos_ready = false;
    reset_to_stock_defaults();
    clear_runtime_state();

    for (uint8_t i = 0; i < CONFIG_ZMK_COMBO_SETTINGS_MAX_COMBOS; i++) {
        if (!loaded_setting_present[i]) {
            continue;
        }
        struct combo_cfg combo;
        int ret = record_to_combo(&loaded_settings[i], &combo);
        if (ret < 0) {
            LOG_WRN("Ignoring stale/invalid combo setting %d (%d)", i, ret);
            continue;
        }
        int slot = slot_for_record(&loaded_settings[i]);
        if (slot >= 0 && slot < COMBO_STORAGE_COUNT) {
            combos[slot] = combo;
        }
    }

    memcpy(saved_combos, combos, sizeof(saved_combos));
    rebuild_combo_lookup();
    combos_ready = true;
    k_mutex_unlock(&combo_mutex);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(combos, COMBO_SETTING_SUBTREE, NULL, combo_handle_set,
                               combo_handle_commit, NULL);

size_t zmk_combo_get_slot_count(void) { return COMBO_STORAGE_COUNT; }

size_t zmk_combo_get_max_keys_per_combo(void) {
    return CONFIG_ZMK_COMBO_SETTINGS_MAX_KEYS_PER_COMBO;
}

int zmk_combo_get_slot(uint8_t slot_index, struct zmk_combo_slot *slot) {
    if (!slot || slot_index >= COMBO_STORAGE_COUNT) {
        return -EINVAL;
    }

    k_mutex_lock(&combo_mutex, K_FOREVER);
    const struct combo_cfg *combo = &combos[slot_index];
    memset(slot, 0, sizeof(*slot));
    strlcpy(slot->combo_id, combo->combo_id, sizeof(slot->combo_id));
    slot->slot_index = slot_index;
    slot->source = combo->source;
    slot->enabled = combo->enabled;
    slot->binding = combo->behavior;
    slot->key_position_count = combo->key_position_len > 0 ? (uint8_t)combo->key_position_len : 0;
    for (uint8_t i = 0; i < slot->key_position_count; i++) {
        slot->key_positions[i] = combo->key_positions[i];
    }
    for (uint8_t layer = 0; layer < MIN(ZMK_KEYMAP_LAYERS_LEN, 32); layer++) {
        if ((combo->layer_mask & BIT(layer)) != 0 &&
            slot->layer_count < CONFIG_ZMK_COMBO_SETTINGS_MAX_LAYERS) {
            slot->layers[slot->layer_count++] = layer;
        }
    }
    slot->timeout_ms = combo->timeout_ms;
    slot->require_prior_idle_ms = combo->require_prior_idle_ms;
    slot->slow_release = combo->slow_release;
    slot->dirty = !combo_slot_equal(combo, &saved_combos[slot_index]);
    k_mutex_unlock(&combo_mutex);
    return 0;
}

static bool combo_is_busy(void) {
    return pressed_keys_count > 0 || active_combo_count > 0 || fully_pressed_combo != INT16_MAX;
}

int zmk_combo_set_slot(const struct zmk_combo_slot *slot) {
    if (!slot || slot->slot_index >= COMBO_STORAGE_COUNT) {
        return -EINVAL;
    }

    struct combo_setting_record record = {
        .source = slot->source,
        .enabled = slot->enabled,
        .key_position_count = slot->key_position_count,
        .layer_count = slot->layer_count,
        .param1 = slot->binding.param1,
        .param2 = slot->binding.param2,
        .timeout_ms = slot->timeout_ms,
        .require_prior_idle_ms = slot->require_prior_idle_ms,
        .slow_release = slot->slow_release,
    };
    strlcpy(record.combo_id, slot->combo_id, sizeof(record.combo_id));
    record.behavior_local_id = zmk_behavior_get_local_id(slot->binding.behavior_dev);
    for (uint8_t i = 0; i < slot->key_position_count; i++) {
        record.key_positions[i] = slot->key_positions[i];
    }
    for (uint8_t i = 0; i < slot->layer_count; i++) {
        record.layers[i] = slot->layers[i];
    }

    struct combo_cfg combo;
    int ret = record_to_combo(&record, &combo);
    if (ret < 0) {
        return ret;
    }

    int target_slot = slot_for_record(&record);
    if (target_slot != slot->slot_index) {
        return -EINVAL;
    }

    k_mutex_lock(&combo_mutex, K_FOREVER);
    if (combo_is_busy()) {
        k_mutex_unlock(&combo_mutex);
        return -EBUSY;
    }

    combos[slot->slot_index] = combo;
    rebuild_combo_lookup();
    k_mutex_unlock(&combo_mutex);
    return 0;
}

int zmk_combo_check_unsaved_changes(void) {
    k_mutex_lock(&combo_mutex, K_FOREVER);
    for (uint8_t i = 0; i < COMBO_STORAGE_COUNT; i++) {
        if (!combo_slot_equal(&combos[i], &saved_combos[i])) {
            k_mutex_unlock(&combo_mutex);
            return 1;
        }
    }
    k_mutex_unlock(&combo_mutex);
    return 0;
}

int zmk_combo_save_changes(void) {
    k_mutex_lock(&combo_mutex, K_FOREVER);
    for (uint8_t i = 0; i < COMBO_STORAGE_COUNT; i++) {
        int ret;
        if (combo_slot_matches_stock_default(i)) {
            char full_setting_name[24];
            snprintf(full_setting_name, sizeof(full_setting_name),
                     COMBO_SETTING_SUBTREE "/" COMBO_SETTING_SLOT_KEY, i);
            ret = settings_delete(full_setting_name);
            ARG_UNUSED(ret);
            continue;
        }

        struct combo_setting_record record;
        ret = combo_to_record(&combos[i], &record);
        if (ret < 0) {
            k_mutex_unlock(&combo_mutex);
            return ret;
        }

        char full_setting_name[24];
        snprintf(full_setting_name, sizeof(full_setting_name), COMBO_SETTING_SUBTREE "/" COMBO_SETTING_SLOT_KEY, i);
        ret = settings_save_one(full_setting_name, &record, sizeof(record));
        if (ret < 0) {
            k_mutex_unlock(&combo_mutex);
            return ret;
        }
    }

    memcpy(saved_combos, combos, sizeof(saved_combos));
    k_mutex_unlock(&combo_mutex);
    return 0;
}

int zmk_combo_discard_changes(void) {
    clear_loaded_settings();
    int ret = settings_load_subtree(COMBO_SETTING_SUBTREE);
    return ret;
}

int zmk_combo_reset_settings(void) {
    k_mutex_lock(&combo_mutex, K_FOREVER);
    for (uint8_t i = 0; i < CONFIG_ZMK_COMBO_SETTINGS_MAX_COMBOS; i++) {
        char full_setting_name[24];
        snprintf(full_setting_name, sizeof(full_setting_name), COMBO_SETTING_SUBTREE "/" COMBO_SETTING_SLOT_KEY, i);
        settings_delete(full_setting_name);
    }

    clear_loaded_settings();
    reset_to_stock_defaults();
    memcpy(saved_combos, combos, sizeof(saved_combos));
    clear_runtime_state();
    rebuild_combo_lookup();
    combos_ready = true;
    k_mutex_unlock(&combo_mutex);
    return 0;
}

#endif // IS_ENABLED(CONFIG_ZMK_COMBO_SETTINGS)

static int combo_init(void) {
    for (size_t i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        active_combos[i].combo_idx = UINT16_MAX;
    }

    k_work_init_delayable(&timeout_task, combo_timeout_handler);
#if IS_ENABLED(CONFIG_ZMK_COMBO_SETTINGS)
    load_stock_defaults();
    reset_to_stock_defaults();
    memcpy(saved_combos, combos, sizeof(saved_combos));
    combos_ready = false;
#else
    rebuild_combo_lookup();
#endif
    LOG_WRN("Have %d combos!", COMBO_STORAGE_COUNT);
    return 0;
}

SYS_INIT(combo_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

#endif
