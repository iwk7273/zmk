/* SPDX-License-Identifier: MIT */
/* Deterministic boundaries for the actual production functions included below. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#define CONFIG_ZMK_SPLIT 0
#define IS_ENABLED(x) (x)
#define CONFIG_ZMK_BEHAVIORS_QUEUE_SIZE 128
#define CONFIG_ZMK_MACRO_SETTINGS_MAX_MACROS 16
#define CONFIG_ZMK_MACRO_SETTINGS_MAX_STEPS_PER_MACRO 16
#define CONFIG_ZMK_MACRO_SETTINGS_NAME_MAX_LEN 24
#define CONFIG_ZMK_MACRO_SETTINGS_MAX_BYTES 64
#define CONFIG_ZMK_MACRO_SETTINGS_MAX_WAIT_MS 60000
#define CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS 15
#define KEY_PRESS_DEVICE_NAME "kp"
#define K_FOREVER 0
#define K_NO_WAIT 0
#define K_MSEC(ms) (ms)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define LOG_MODULE_DECLARE(...)
#define LOG_DBG(...)
#define __ASSERT_NO_MSG(x) assert(x)

typedef uint16_t zmk_behavior_local_id_t;
struct zmk_behavior_binding {
    const char *behavior_dev;
    uint32_t param1, param2;
};
struct zmk_behavior_binding_event {
    uint32_t position;
    int64_t timestamp;
};
struct k_spinlock {
    int locked;
};
typedef int k_spinlock_key_t;
static int lock_depth;
static int k_spin_lock(struct k_spinlock *lock) {
    assert(!lock->locked);
    lock->locked = 1;
    lock_depth++;
    return 0;
}
static void k_spin_unlock(struct k_spinlock *lock, int key) {
    assert(lock->locked);
    lock->locked = 0;
    lock_depth--;
}
struct k_work {
    int unused;
};
struct k_work_delayable {
    struct k_work work;
    void (*fn)(struct k_work *);
    bool pending;
    uint32_t wait;
};
#define K_WORK_DELAYABLE_DEFINE(name, callback) struct k_work_delayable name = {.fn = callback}
static int k_work_schedule(struct k_work_delayable *work, uint32_t ms) {
    work->pending = true;
    work->wait = ms;
    return 1;
}
struct k_msgq {
    unsigned char data[8192];
    size_t size, count, read, write;
};
#define K_MSGQ_DEFINE(name, bytes, capacity, alignment) struct k_msgq name = {.size = bytes}
static int k_msgq_put(struct k_msgq *q, const void *item, int timeout) {
    if (q->count == CONFIG_ZMK_BEHAVIORS_QUEUE_SIZE)
        return -ENOMSG;
    memcpy(q->data + q->write * q->size, item, q->size);
    q->write = (q->write + 1) % CONFIG_ZMK_BEHAVIORS_QUEUE_SIZE;
    q->count++;
    return 0;
}
static int k_msgq_get(struct k_msgq *q, void *item, int timeout) {
    if (!q->count)
        return -ENOMSG;
    memcpy(item, q->data + q->read * q->size, q->size);
    q->read = (q->read + 1) % CONFIG_ZMK_BEHAVIORS_QUEUE_SIZE;
    q->count--;
    return 0;
}
static size_t k_msgq_num_free_get(struct k_msgq *q) {
    return CONFIG_ZMK_BEHAVIORS_QUEUE_SIZE - q->count;
}
static int64_t now;
static int64_t k_uptime_get(void) { return now; }
static struct {
    uint32_t key;
    bool press;
    int64_t time;
} events[512];
static size_t event_count;
static bool pressed[256];
static int zmk_behavior_invoke_binding(struct zmk_behavior_binding *binding,
                                       struct zmk_behavior_binding_event event, bool press) {
    assert(lock_depth == 0); /* Reentrant behavior dispatch must never run under queue_lock. */
    assert(event_count < ARRAY_SIZE(events));
    events[event_count].key = binding->param1;
    events[event_count].press = press;
    events[event_count++].time = event.timestamp;
    pressed[binding->param1 & 0xff] = press;
    return 0;
}
#include "queue.inc"
#include <zmk/user_macros.h>
static struct zmk_user_macro_slot test_slot;
static int current_slots[16], user_macro_mutex;
static uint32_t current_tap_ms = 30;
static void k_mutex_lock(int *mutex, int timeout) {
    assert(!*mutex);
    *mutex = 1;
}
static void k_mutex_unlock(int *mutex) {
    assert(*mutex);
    *mutex = 0;
}
static int fill_full_locked(uint8_t slot, struct zmk_user_macro_slot *result) {
    current_slots[slot]++;
    *result = test_slot;
    return 0;
}
static uint32_t packed_to_usage(uint8_t packed) {
    return 0x70000 | (packed & 0x7f) | ((packed & 0x80) ? 0x02000000 : 0);
}
static bool key_press_binding_is_valid(uint16_t behavior, uint32_t usage) { return behavior == 1; }
#include "macro.inc"

/* The settings reader/migration uses production code; only flash and pool writes are doubled. */
#define __packed __attribute__((packed))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define LOG_WRN(...)
#define CONFIG_ZMK_MACRO_SETTINGS_MAX_TAP_MS 1000
#define USER_MACRO_SETTING_TAP_MS "tap_ms"
#define USER_MACRO_RECORD_VERSION_V1 1
#define USER_MACRO_RECORD_VERSION 2
#define USER_MACRO_FLAG_ENABLED 1
#define USER_MACRO_FLAG_TOMBSTONE 2
#define USER_MACRO_VACANT 0
#define USER_MACRO_ACTIVE 1
#define USER_MACRO_TOMBSTONE 2
static bool tap_ms_dirty;
typedef int (*settings_read_cb)(void *, void *, size_t);
static bool settings_name_steq(const char *name, const char *key, const char **next) {
    size_t len = strlen(key);
    if (strncmp(name, key, len) || (name[len] && name[len] != '/'))
        return false;
    *next = name[len] ? name + len + 1 : NULL;
    return true;
}
static uint16_t sys_get_le16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t sys_get_le32(const uint8_t *p) {
    return sys_get_le16(p) | ((uint32_t)sys_get_le16(p + 2) << 16);
}
static void sys_put_le16(uint16_t n, uint8_t *p) {
    p[0] = n;
    p[1] = n >> 8;
}
static void sys_put_le32(uint32_t n, uint8_t *p) {
    sys_put_le16(n, p);
    sys_put_le16(n >> 16, p + 2);
}
static struct {
    unsigned writes;
    uint8_t slot, occupancy, body[CONFIG_ZMK_MACRO_SETTINGS_MAX_BYTES];
    char name[CONFIG_ZMK_MACRO_SETTINGS_NAME_MAX_LEN];
    uint16_t length;
} loaded;
static int apply_body_locked(uint8_t slot, const char *name, uint8_t occupancy, const uint8_t *body,
                             uint16_t length, bool dirty) {
    assert(!dirty && length <= sizeof(loaded.body));
    loaded.writes++;
    loaded.slot = slot;
    loaded.occupancy = occupancy;
    loaded.length = length;
    if (occupancy == USER_MACRO_ACTIVE) {
        assert(strlen(name) < sizeof(loaded.name));
        strcpy(loaded.name, name);
        memcpy(loaded.body, body, length);
    }
    return 0;
}
#include "macro-settings.inc"
struct stored_value {
    const void *data;
    size_t readable;
    unsigned reads;
};
static int read_setting(void *arg, void *buffer, size_t length) {
    struct stored_value *value = arg;
    value->reads++;
    length = MIN(length, value->readable);
    memcpy(buffer, value->data, length);
    return (int)length;
}
static void test_macro_settings(void) {
    struct user_macro_v1_record legacy = {
        .version = 1, .enabled = true, .step_count = 2, .name = "Saved"};
    legacy.steps[0].action = ZMK_USER_MACRO_STEP_WAIT;
    legacy.steps[0].wait_ms = 200;
    legacy.steps[1].action = ZMK_USER_MACRO_STEP_TAP;
    legacy.steps[1].behavior_local_id = 1;
    legacy.steps[1].param1 = 4;
    struct stored_value value = {&legacy, sizeof(legacy), 0};
    memset(&loaded, 0, sizeof(loaded));
    assert(user_macro_handle_set("s/3", sizeof(legacy), read_setting, &value) == 0);
    assert(loaded.writes == 1 && loaded.slot == 3 && !strcmp(loaded.name, "Saved"));
    struct zmk_user_macro_slot decoded = {0};
    assert(decode_body(loaded.body, loaded.length, &decoded) == 0);
    assert(decoded.step_count == 2 && decoded.steps[0].wait_ms == 200 &&
           decoded.steps[1].param1 == 4);
    memset(legacy.name, 'x', sizeof(legacy.name));
    user_macro_handle_set("s/3", sizeof(legacy), read_setting, &value);
    assert(loaded.writes == 1); /* Unterminated legacy names are rejected. */
    legacy.enabled = false;
    user_macro_handle_set("s/3", sizeof(legacy), read_setting, &value);
    assert(loaded.writes == 2 && loaded.occupancy == USER_MACRO_TOMBSTONE);

    /* V2 header, name, body length, one Text step containing two packed keys. */
    uint8_t modern[] = {2, 1, 1, 'M', 6, 0, 1, 5, 2, 0, 4, 5};
    value = (struct stored_value){modern, sizeof(modern), 0};
    user_macro_handle_set("s/4", sizeof(modern), read_setting, &value);
    assert(loaded.writes == 3 && loaded.slot == 4 && !strcmp(loaded.name, "M"));
    memset(&decoded, 0, sizeof(decoded));
    assert(decode_body(loaded.body, loaded.length, &decoded) == 0 && decoded.packed_len == 2);
    value.readable--;
    user_macro_handle_set("s/4", sizeof(modern), read_setting, &value);
    assert(loaded.writes == 3); /* A truncated read must not replace saved state. */
    value.reads = 0;
    user_macro_handle_set("s/4", 65535, read_setting, &value);
    assert(loaded.writes == 3 && value.reads == 0);

    uint32_t tap_ms = 45;
    value = (struct stored_value){&tap_ms, sizeof(tap_ms), 0};
    tap_ms_dirty = true;
    user_macro_handle_set("tap_ms", sizeof(tap_ms), read_setting, &value);
    assert(current_tap_ms == 45 && !tap_ms_dirty);
    puts("PASS: boot settings restore v1/v2/tap timing and reject malformed or truncated records");
}

/* Combo tests supply events and candidate masks; candidate selection and lifecycle are production
 * code. */
#define CONFIG_ZMK_COMBO_SETTINGS_MAX_COMBOS 4
#define COMBO_STORAGE_COUNT 4
#define COMBO_SETTING_SUBTREE "combos"
#define COMBO_SETTING_SLOT_KEY "s/%d"
#define ZMK_EV_EVENT_BUBBLE 0
typedef int zmk_event_t;
typedef uintptr_t mem_addr_t;
struct zmk_position_state_changed {
    int32_t position;
    int64_t timestamp;
};
struct combo_cfg {
    int key_position_len;
    int binding;
};
static struct combo_cfg combos[4], saved_combos[4];
static int combo_mutex, pressed_keys_count, active_combo_count, triggered_combo;
static int16_t fully_pressed_combo = INT16_MAX;
static uint32_t candidates[1];
static bool combos_ready;
static int deletes, rebuilds;
static int settings_delete(const char *name) {
    deletes++;
    return 0;
}
struct combo_setting_record {
    int unused;
};
static bool combo_slot_matches_stock_default(uint8_t slot_index) { return false; }
static int combo_to_record(const struct combo_cfg *combo, struct combo_setting_record *record) {
    return 0;
}
static int settings_save_one(const char *name, const void *value, size_t len) { return 0; }
#define ARG_UNUSED(x) (void)(x)
static void clear_loaded_settings(void) {}
static void clear_runtime_state(void) {
    pressed_keys_count = active_combo_count = 0;
    fully_pressed_combo = INT16_MAX;
}
static void reset_to_stock_defaults(void) { memset(combos, 0, sizeof(combos)); }
static void rebuild_combo_lookup(void) { rebuilds++; }
static bool combo_is_enabled(int index) { return combos[index].key_position_len > 0; }
static bool sys_bitfield_test_bit(mem_addr_t addr, int index) {
    return (*(uint32_t *)addr & (1u << index)) != 0;
}
static int filter_candidates(int position) {
    int n = 0;
    for (int i = 0; i < 4; i++)
        n += (candidates[0] >> i) & 1u;
    return n;
}
static int setup_candidates_for_first_keypress(int position, int64_t timestamp) {
    return filter_candidates(position);
}
static void filter_timed_out_candidates(int64_t timestamp) {}
static int capture_pressed_key(struct zmk_position_state_changed *data) {
    pressed_keys_count++;
    return 1;
}
static void update_timeout_task(void) {}
static void cleanup(void) {
    triggered_combo = fully_pressed_combo;
    fully_pressed_combo = INT16_MAX;
}
#include "combo.inc"

static void reset_queue(void) {
    zmk_behavior_queue_msgq.count = zmk_behavior_queue_msgq.read = zmk_behavior_queue_msgq.write =
        0;
    queue_work.pending = queue_processing = false;
    now = 0;
    event_count = 0;
    memset(pressed, 0, sizeof(pressed));
    memset(&test_slot, 0, sizeof(test_slot));
    test_slot.enabled = true;
    strcpy(test_slot.name, "Test");
}
static void drain(void) {
    int limit = 1000;
    while (queue_work.pending) {
        assert(--limit > 0);
        now += queue_work.wait;
        queue_work.pending = false;
        queue_work.fn(&queue_work.work);
    }
    assert(!queue_processing);
}
static struct zmk_user_macro_step tap(uint32_t usage) {
    return (struct zmk_user_macro_step){
        .action = ZMK_USER_MACRO_STEP_TAP, .behavior_local_id = 1, .param1 = usage};
}
static struct zmk_user_macro_step wait_step(uint16_t ms) {
    return (struct zmk_user_macro_step){.action = ZMK_USER_MACRO_STEP_WAIT, .wait_ms = ms};
}
static void test_queue_capacity(void) {
    reset_queue();
    struct zmk_behavior_binding_event event = {0};
    test_slot.step_count = 1;
    test_slot.packed_len = 60;
    test_slot.steps[0] =
        (struct zmk_user_macro_step){.action = ZMK_USER_MACRO_STEP_TEXT, .packed_len = 60};
    memset(test_slot.packed_keys, 4, 60);
    assert(zmk_user_macro_queue(0, &event) == 0);
    assert(event_count == 1 && zmk_behavior_queue_msgq.count == 119);
    memset(test_slot.packed_keys, 5, 60);
    assert(zmk_user_macro_queue(0, &event) == -ENOSPC);
    assert(event_count == 1 && zmk_behavior_queue_msgq.count == 119);
    drain();
    assert(event_count == 120 && !pressed[4] && !pressed[5]);
    assert(zmk_user_macro_queue(0, &event) == 0);
    drain();
    assert(event_count == 240 && !pressed[5]);
    puts("PASS: full queue rejects an entire second macro and preserves releases");
}
static void test_waits(void) {
    reset_queue();
    struct zmk_behavior_binding_event event = {0};
    test_slot.step_count = 5;
    test_slot.steps[0] = wait_step(400);
    test_slot.steps[1] = wait_step(600);
    test_slot.steps[2] = tap(4);
    test_slot.steps[3] = wait_step(100);
    test_slot.steps[4] = tap(5);
    assert(expanded_event_count(&test_slot) == 5);
    assert(zmk_user_macro_queue(0, &event) == 0);
    assert(event_count == 0);
    /* An ordinary producer must not bypass the leading delay. */
    assert(zmk_behavior_queue_add(&event,
                                  (struct zmk_behavior_binding){.behavior_dev = "kp", .param1 = 6},
                                  true, 0) == 0);
    assert(zmk_behavior_queue_add(&event,
                                  (struct zmk_behavior_binding){.behavior_dev = "kp", .param1 = 6},
                                  false, 0) == 0);
    drain();
    assert(event_count == 6 && events[0].time == 1000 && events[1].time == 1030);
    assert(events[2].time == 1145 && events[3].time == 1175);
    assert(events[4].key == 6 && events[4].time == 1190);
    puts("PASS: leading/consecutive waits and mixed producers preserve timing and order");
}
static void test_balanced_press(void) {
    reset_queue();
    struct zmk_behavior_binding_event event = {0};
    test_slot.step_count = 4;
    test_slot.steps[0] = tap(0xe0);
    test_slot.steps[0].action = ZMK_USER_MACRO_STEP_PRESS;
    test_slot.steps[1] = wait_step(100);
    test_slot.steps[2] = tap(6);
    test_slot.steps[3] = tap(0xe0);
    test_slot.steps[3].action = ZMK_USER_MACRO_STEP_RELEASE;
    assert(zmk_user_macro_queue(0, &event) == 0);
    drain();
    assert(event_count == 4 && events[1].time == 115 && !pressed[0xe0] && !pressed[6]);
    test_slot.step_count = 1;
    assert(zmk_user_macro_queue(0, &event) == -EINVAL && event_count == 4);
    puts("PASS: explicit Press/Release stays balanced and invalid bodies do not start");
}
static void test_combo_order(void) {
    struct zmk_position_state_changed data = {.position = 1};
    memset(combos, 0, sizeof(combos));
    combos[0].key_position_len = 3;
    combos[1].key_position_len = 2;
    candidates[0] = 3;
    pressed_keys_count = 1;
    fully_pressed_combo = INT16_MAX;
    position_state_down(NULL, &data);
    assert(fully_pressed_combo == 1);
    cleanup();
    assert(triggered_combo == 1);
    pressed_keys_count = 2;
    candidates[0] = 1;
    position_state_down(NULL, &data);
    assert(triggered_combo == 0);
    puts("PASS: shorter and longer overlapping combos work independently of slot order");
}
static void test_combo_discard(void) {
    combos[0].binding = 42;
    saved_combos[0].binding = 7;
    pressed_keys_count = 0;
    active_combo_count = 1;
    fully_pressed_combo = INT16_MAX;
    assert(zmk_combo_save_changes() == -EBUSY && saved_combos[0].binding == 7 &&
           active_combo_count == 1);
    assert(zmk_combo_discard_changes() == -EBUSY && combos[0].binding == 42 &&
           active_combo_count == 1);
    assert(zmk_combo_reset_settings() == -EBUSY && deletes == 0 && active_combo_count == 1);
    active_combo_count = 0;
    pressed_keys_count = 1;
    assert(zmk_combo_discard_changes() == -EBUSY && combos[0].binding == 42);
    pressed_keys_count = 0;
    assert(zmk_combo_discard_changes() == 0 && combos[0].binding == 7 && rebuilds == 1);
    assert(zmk_combo_reset_settings() == 0 && deletes == 4 && combos_ready);
    puts("PASS: held/pending combos block Save, Discard, and Reset; idle operations restore "
         "settings");
}
int main(void) {
    test_queue_capacity();
    test_waits();
    test_balanced_press();
    test_combo_order();
    test_combo_discard();
    test_macro_settings();
    return 0;
}
