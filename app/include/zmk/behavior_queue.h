/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <stdint.h>
#include <zmk/behavior.h>

int zmk_behavior_queue_add(const struct zmk_behavior_binding_event *event,
                           const struct zmk_behavior_binding behavior, bool press, uint32_t wait);

struct zmk_behavior_queue_entry {
    /* A NULL behavior_dev is a delay-only entry. */
    struct zmk_behavior_binding binding;
    bool press;
    uint32_t wait_ms;
};

/** Fill the next entry synchronously. Called under a spinlock; must not block or enqueue. */
typedef void (*zmk_behavior_queue_next_cb)(void *context, struct zmk_behavior_queue_entry *entry);

/** Enqueue all count entries, or return -ENOSPC without invoking next or changing the queue. */
int zmk_behavior_queue_add_batch(const struct zmk_behavior_binding_event *event, size_t count,
                                 zmk_behavior_queue_next_cb next, void *context);
