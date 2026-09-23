/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/behavior_queue.h>
#include <zmk/behavior.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <errno.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct q_item {
    uint32_t position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t source;
#endif
    struct zmk_behavior_binding binding;
    bool press : 1;
    uint32_t wait : 31;
};

K_MSGQ_DEFINE(zmk_behavior_queue_msgq, sizeof(struct q_item), CONFIG_ZMK_BEHAVIORS_QUEUE_SIZE, 4);
static struct k_spinlock queue_lock;
/* Includes the time spent invoking a behavior and waiting for its delay. */
static bool queue_processing;

static void behavior_queue_process_next(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(queue_work, behavior_queue_process_next);

static void behavior_queue_process_next(struct k_work *work) {
    struct q_item item = {.wait = 0};

    while (true) {
        k_spinlock_key_t key = k_spin_lock(&queue_lock);
        if (k_msgq_get(&zmk_behavior_queue_msgq, &item, K_NO_WAIT) != 0) {
            queue_processing = false;
            k_spin_unlock(&queue_lock, key);
            return;
        }
        k_spin_unlock(&queue_lock, key);
        if (item.binding.behavior_dev) {
            LOG_DBG("Invoking %s: 0x%02x 0x%02x", item.binding.behavior_dev, item.binding.param1,
                    item.binding.param2);
            struct zmk_behavior_binding_event event = {.position = item.position,
                                                       .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
                                                       .source = item.source
#endif
            };

            if (item.press) {
                zmk_behavior_invoke_binding(&item.binding, event, true);
            } else {
                zmk_behavior_invoke_binding(&item.binding, event, false);
            }
        }

        LOG_DBG("Processing next queued behavior in %dms", item.wait);

        if (item.wait > 0) {
            k_work_schedule(&queue_work, K_MSEC(item.wait));
            break;
        }
    }
}

int zmk_behavior_queue_add(const struct zmk_behavior_binding_event *event,
                           const struct zmk_behavior_binding binding, bool press, uint32_t wait) {
    struct q_item item = {
        .press = press,
        .binding = binding,
        .wait = wait,
        .position = event->position,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = event->source,
#endif
    };

    k_spinlock_key_t key = k_spin_lock(&queue_lock);
    const int ret = k_msgq_put(&zmk_behavior_queue_msgq, &item, K_NO_WAIT);
    bool start = ret == 0 && !queue_processing;
    if (start) {
        queue_processing = true;
    }
    k_spin_unlock(&queue_lock, key);
    if (start) {
        behavior_queue_process_next(&queue_work.work);
    }
    return ret;
}

int zmk_behavior_queue_add_batch(const struct zmk_behavior_binding_event *event, size_t count,
                                 zmk_behavior_queue_next_cb next, void *context) {
    if (!event || !next) {
        return -EINVAL;
    }
    if (count == 0) {
        return 0;
    }

    k_spinlock_key_t key = k_spin_lock(&queue_lock);
    if (count > k_msgq_num_free_get(&zmk_behavior_queue_msgq)) {
        k_spin_unlock(&queue_lock, key);
        return -ENOSPC;
    }
    for (size_t i = 0; i < count; i++) {
        struct zmk_behavior_queue_entry entry = {0};
        next(context, &entry);
        struct q_item item = {
            .position = event->position,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
            .source = event->source,
#endif
            .binding = entry.binding,
            .press = entry.press,
            .wait = entry.wait_ms,
        };
        /* All producers hold queue_lock, so the capacity check covers the entire batch. */
        int ret = k_msgq_put(&zmk_behavior_queue_msgq, &item, K_NO_WAIT);
        __ASSERT_NO_MSG(ret == 0);
        (void)ret;
    }
    bool start = !queue_processing;
    queue_processing = true;
    k_spin_unlock(&queue_lock, key);
    if (start) {
        behavior_queue_process_next(&queue_work.work);
    }
    return 0;
}
