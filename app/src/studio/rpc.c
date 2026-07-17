/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "msg_framing.h"

#include <errno.h>
#include <string.h>

#include <pb_encode.h>
#include <pb_decode.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/debug/thread_analyzer.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/studio/core.h>
#include <zmk/studio/rpc.h>

ZMK_EVENT_IMPL(zmk_studio_rpc_notification);

static struct zmk_rpc_subsystem *find_subsystem_for_choice(uint8_t choice) {
    STRUCT_SECTION_FOREACH(zmk_rpc_subsystem, sub) {
        if (sub->subsystem_choice == choice) {
            return sub;
        }
    }

    return NULL;
}

zmk_studio_Response zmk_rpc_subsystem_delegate_to_subs(const struct zmk_rpc_subsystem *subsys,
                                                       const zmk_studio_Request *req,
                                                       uint8_t which_req) {
    LOG_DBG("Got subsystem func for %d", subsys->subsystem_choice);

    for (int i = subsys->handlers_start_index; i <= subsys->handlers_end_index; i++) {
        struct zmk_rpc_subsystem_handler *sub_handler;
        STRUCT_SECTION_GET(zmk_rpc_subsystem_handler, i, &sub_handler);
        if (sub_handler->request_choice == which_req) {
            if (sub_handler->security == ZMK_STUDIO_RPC_HANDLER_SECURED &&
                zmk_studio_core_get_lock_state() != ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED) {
                return ZMK_RPC_RESPONSE(meta, simple_error,
                                        zmk_meta_ErrorConditions_UNLOCK_REQUIRED);
            }
            return sub_handler->func(req);
        }
    }
    LOG_ERR("No handler func found for %d", which_req);
    return ZMK_RPC_RESPONSE(meta, simple_error, zmk_meta_ErrorConditions_RPC_NOT_FOUND);
}

static zmk_studio_Response handle_request(const zmk_studio_Request *req) {
    zmk_studio_core_reschedule_lock_timeout();
    struct zmk_rpc_subsystem *sub = find_subsystem_for_choice(req->which_subsystem);
    if (!sub) {
        LOG_WRN("No subsystem found for choice %d", req->which_subsystem);
        return ZMK_RPC_RESPONSE(meta, simple_error, zmk_meta_ErrorConditions_RPC_NOT_FOUND);
    }

    zmk_studio_Response resp = sub->func(sub, req);
    resp.type.request_response.request_id = req->request_id;

    return resp;
}

RING_BUF_DECLARE(rpc_rx_buf, CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE);

static K_SEM_DEFINE(rpc_rx_sem, 0, 1);
static K_SEM_DEFINE(rpc_work_sem, 0, 1);

static enum studio_framing_state rpc_framing_state;

static K_MUTEX_DEFINE(rpc_transport_mutex);
static K_MUTEX_DEFINE(rpc_transport_switch_mutex);
static K_MUTEX_DEFINE(rpc_tx_mutex);
static struct zmk_rpc_transport *selected_transport;
static uint32_t selected_transport_generation;

K_MSGQ_DEFINE(rpc_notification_msgq, sizeof(zmk_studio_Notification),
              CONFIG_ZMK_STUDIO_RPC_NOTIFICATION_QUEUE_SIZE, 4);

struct ring_buf *zmk_rpc_get_rx_buf(void) { return &rpc_rx_buf; }

void zmk_rpc_rx_notify(void) {
    k_sem_give(&rpc_rx_sem);
    k_sem_give(&rpc_work_sem);
}

static bool rpc_read_cb(pb_istream_t *stream, uint8_t *buf, size_t count) {
    uint32_t write_offset = 0;

    do {
        uint8_t *buffer;
        uint32_t len = ring_buf_get_claim(&rpc_rx_buf, &buffer, count - write_offset);

        if (len > 0) {
            for (int i = 0; i < len; i++) {
                if (studio_framing_process_byte(&rpc_framing_state, buffer[i])) {
                    buf[write_offset++] = buffer[i];
                }
            }
        } else {
            k_sem_take(&rpc_rx_sem, K_FOREVER);
        }

        ring_buf_get_finish(&rpc_rx_buf, len);
    } while (write_offset < count && rpc_framing_state != FRAMING_STATE_EOF);

    if (rpc_framing_state == FRAMING_STATE_EOF) {
        stream->bytes_left = 0;
        return false;
    } else {
        return true;
    }
}

static pb_istream_t pb_istream_for_rx_ring_buf() {
    pb_istream_t stream = {&rpc_read_cb, NULL, SIZE_MAX};
    return stream;
}

RING_BUF_DECLARE(rpc_tx_buf, CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE);

struct ring_buf *zmk_rpc_get_tx_buf(void) { return &rpc_tx_buf; }

void zmk_rpc_reset_tx_buffer(void) {
    k_mutex_lock(&rpc_tx_mutex, K_FOREVER);

    k_mutex_lock(&rpc_transport_mutex, K_FOREVER);
    struct zmk_rpc_transport *transport = selected_transport;
    k_mutex_unlock(&rpc_transport_mutex);

    if (transport && transport->tx_abort) {
        transport->tx_abort(&rpc_tx_buf);
    } else {
        ring_buf_reset(&rpc_tx_buf);
    }

    k_mutex_unlock(&rpc_tx_mutex);
}

struct rpc_tx_stream_state {
    struct zmk_rpc_transport *transport;
    uint32_t transport_generation;
    void *user_data;
    int error;
};

static bool rpc_tx_notify(struct rpc_tx_stream_state *tx_state, size_t added, bool message_done) {
    k_mutex_lock(&rpc_transport_mutex, K_FOREVER);

    if (selected_transport != tx_state->transport ||
        selected_transport_generation != tx_state->transport_generation) {
        k_mutex_unlock(&rpc_transport_mutex);
        tx_state->error = -ECANCELED;
        return false;
    }

    int err = tx_state->transport->tx_notify(&rpc_tx_buf, added, message_done, tx_state->user_data);
    k_mutex_unlock(&rpc_transport_mutex);

    if (err < 0) {
        tx_state->error = err;
        return false;
    }

    return true;
}

static bool rpc_tx_wait_for_space(struct rpc_tx_stream_state *tx_state) {
    int64_t timeout_at = k_uptime_get() + CONFIG_ZMK_STUDIO_RPC_TX_TIMEOUT_MS;

    while (ring_buf_space_get(&rpc_tx_buf) == 0) {
        if (!rpc_tx_notify(tx_state, 0, false)) {
            return false;
        }

        if (ring_buf_space_get(&rpc_tx_buf) > 0) {
            return true;
        }

        if (k_uptime_get() >= timeout_at) {
            tx_state->error = -ETIMEDOUT;
            return false;
        }

        k_sleep(K_MSEC(1));
    }

    return true;
}

static bool rpc_tx_write_framing_byte(struct rpc_tx_stream_state *tx_state, uint8_t byte,
                                      bool message_done) {
    while (ring_buf_put(&rpc_tx_buf, &byte, 1) != 1) {
        if (!rpc_tx_wait_for_space(tx_state)) {
            return false;
        }
    }

    return rpc_tx_notify(tx_state, 1, message_done);
}

static bool rpc_tx_buffer_write(pb_ostream_t *stream, const uint8_t *buf, size_t count) {
    struct rpc_tx_stream_state *tx_state = stream->state;
    size_t written = 0;

    bool escape_byte_already_written = false;
    do {
        uint32_t write_idx = 0;

        uint8_t *write_buf;
        uint32_t claim_len = ring_buf_put_claim(&rpc_tx_buf, &write_buf, count - written);

        if (claim_len == 0) {
            if (!rpc_tx_wait_for_space(tx_state)) {
                return false;
            }
            continue;
        }

        int escapes_written = 0;
        for (int i = 0; i < claim_len && write_idx < claim_len; i++) {
            uint8_t b = buf[written + i];
            switch (b) {
            case FRAMING_EOF:
            case FRAMING_ESC:
            case FRAMING_SOF:
                // Care to be taken. We may need to write the escape byte,
                // but that's the last available spot for this claim, so we track
                // if the escape has already been written in the previous iteration
                // of our loop.
                if (!escape_byte_already_written) {
                    escapes_written++;
                    write_buf[write_idx++] = FRAMING_ESC;
                    escape_byte_already_written = true;
                    if (write_idx >= claim_len) {
                        LOG_WRN("Skipping on, no room to write escape and real byte");
                        continue;
                    }
                }
            default:
                write_buf[write_idx++] = b;
                escape_byte_already_written = false;
                break;
            }
        }

        ring_buf_put_finish(&rpc_tx_buf, write_idx);

        written += (write_idx - escapes_written);

        if (!rpc_tx_notify(tx_state, write_idx, false)) {
            return false;
        }
    } while (written < count);

    return true;
}

static pb_ostream_t pb_ostream_for_tx_buf(struct rpc_tx_stream_state *tx_state) {
    pb_ostream_t stream = {&rpc_tx_buffer_write, (void *)tx_state, SIZE_MAX, 0};
    return stream;
}

static zmk_studio_Response notification_response;

static int send_response(const zmk_studio_Response *resp) {
    int ret = 0;

    k_mutex_lock(&rpc_tx_mutex, K_FOREVER);

    struct rpc_tx_stream_state tx_state = {0};

    k_mutex_lock(&rpc_transport_mutex, K_FOREVER);

    if (!selected_transport) {
        k_mutex_unlock(&rpc_transport_mutex);
        goto exit;
    }

    tx_state.transport = selected_transport;
    tx_state.transport_generation = selected_transport_generation;
    tx_state.user_data =
        selected_transport->tx_user_data ? selected_transport->tx_user_data() : NULL;
    k_mutex_unlock(&rpc_transport_mutex);

    pb_ostream_t stream = pb_ostream_for_tx_buf(&tx_state);

    if (!rpc_tx_write_framing_byte(&tx_state, FRAMING_SOF, false)) {
        ret = tx_state.error ? tx_state.error : -ENOSPC;
        goto abort_exit;
    }

    /* Now we are ready to encode the message! */
    bool status = pb_encode(&stream, &zmk_studio_Response_msg, resp);

    if (!status) {
        if (tx_state.error) {
            ret = tx_state.error;
            goto abort_exit;
        }
#if !IS_ENABLED(CONFIG_NANOPB_NO_ERRMSG)
        LOG_ERR("Failed to encode the message %s", stream.errmsg);
#endif // !IS_ENABLED(CONFIG_NANOPB_NO_ERRMSG)
        ret = -EINVAL;
        goto abort_exit;
    }

    if (!rpc_tx_write_framing_byte(&tx_state, FRAMING_EOF, true)) {
        ret = tx_state.error ? tx_state.error : -ENOSPC;
        goto abort_exit;
    }
    goto exit;

abort_exit:
    /* Discard only the unsent remainder. A transport error must not tear down
     * the shared BLE connection (which also carries HID reports). Receivers
     * resynchronize on the next unescaped SOF if part of this frame was already
     * transmitted. */
    if (tx_state.transport && tx_state.transport->tx_abort) {
        tx_state.transport->tx_abort(&rpc_tx_buf);
    } else {
        ring_buf_reset(&rpc_tx_buf);
    }

exit:
    k_mutex_unlock(&rpc_tx_mutex);
    return ret;
}

int zmk_rpc_send_notification(const zmk_studio_Notification *notification) {
    int ret = k_msgq_put(&rpc_notification_msgq, notification, K_NO_WAIT);
    if (ret < 0) {
        LOG_WRN("Dropping RPC notification: outbound queue is full");
        return ret;
    }

    k_sem_give(&rpc_work_sem);
    return 0;
}

static void rpc_main(void) {
    bool notification_pending = false;

    for (;;) {
        /* Requests always win over queued notifications. In particular, any
         * notification raised synchronously by a request handler is not sent
         * until that request's response has been encoded. */
        if (ring_buf_size_get(&rpc_rx_buf) == 0) {
            if (!notification_pending) {
                memset(&notification_response, 0, sizeof(notification_response));
                if (k_msgq_get(&rpc_notification_msgq, &notification_response.type.notification,
                               K_NO_WAIT) == 0) {
                    notification_response.which_type = zmk_studio_Response_notification_tag;
                    notification_pending = true;
                }
            }

            if (ring_buf_size_get(&rpc_rx_buf) == 0 && notification_pending) {
                int err = send_response(&notification_response);
                notification_pending = false;
                if (err < 0) {
                    LOG_ERR("Failed to send the RPC notification %d", err);
                }
                continue;
            }

            if (ring_buf_size_get(&rpc_rx_buf) == 0) {
                k_sem_take(&rpc_work_sem, K_FOREVER);
                continue;
            }
        }

        pb_istream_t stream = pb_istream_for_rx_ring_buf();
        zmk_studio_Request req = zmk_studio_Request_init_zero;
#if IS_ENABLED(CONFIG_THREAD_ANALYZER)
        thread_analyzer_print(0);
#endif // IS_ENABLED(CONFIG_THREAD_ANALYZER)
        bool status = pb_decode(&stream, &zmk_studio_Request_msg, &req);

        rpc_framing_state = FRAMING_STATE_IDLE;

        if (status) {
            zmk_studio_Response resp = handle_request(&req);

            int err = send_response(&resp);
#if IS_ENABLED(CONFIG_THREAD_ANALYZER)
            thread_analyzer_print(0);
#endif // IS_ENABLED(CONFIG_THREAD_ANALYZER)
            if (err < 0) {
                LOG_ERR("Failed to send the RPC response %d", err);
            }
        } else {
            LOG_DBG("Decode failed");
        }
    }
}

K_THREAD_DEFINE(studio_rpc_thread, CONFIG_ZMK_STUDIO_RPC_THREAD_STACK_SIZE, rpc_main, NULL, NULL,
                NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

static void refresh_selected_transport(void) {
    enum zmk_transport transport = zmk_endpoint_get_selected().transport;
    struct zmk_rpc_transport *old_transport;
    struct zmk_rpc_transport *new_transport = NULL;

    k_mutex_lock(&rpc_transport_switch_mutex, K_FOREVER);

    k_mutex_lock(&rpc_transport_mutex, K_FOREVER);

    if (selected_transport && selected_transport->transport == transport) {
        k_mutex_unlock(&rpc_transport_mutex);
        goto exit_refresh;
    }

    old_transport = selected_transport;
    selected_transport = NULL;
    selected_transport_generation++;
    k_mutex_unlock(&rpc_transport_mutex);

    /* Invalidate the selected transport before waiting for an encoder. Its
     * next non-blocking notify observes the generation change and exits, so a
     * transport switch cannot wait behind transport backpressure. */
    if (old_transport && old_transport->rx_stop) {
        old_transport->rx_stop();
    }

    k_mutex_lock(&rpc_tx_mutex, K_FOREVER);

    if (old_transport && old_transport->tx_abort) {
        old_transport->tx_abort(&rpc_tx_buf);
    } else {
        ring_buf_reset(&rpc_tx_buf);
    }

    if (old_transport) {
#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOCK_ON_DISCONNECT)
        zmk_studio_core_lock();
#endif
    }

    STRUCT_SECTION_FOREACH(zmk_rpc_transport, t) {
        if (t->transport == transport) {
            new_transport = t;
            break;
        }
    }

    int err = 0;
    if (new_transport && new_transport->rx_start) {
        err = new_transport->rx_start();
    }

    if (err >= 0) {
        k_mutex_lock(&rpc_transport_mutex, K_FOREVER);
        selected_transport = new_transport;
        selected_transport_generation++;
        k_mutex_unlock(&rpc_transport_mutex);
    } else {
        LOG_ERR("Failed to start RPC transport (%d)", err);
    }

    if (!new_transport) {
        LOG_WRN("Failed to select a transport!");
    }

    k_mutex_unlock(&rpc_tx_mutex);

exit_refresh:
    k_mutex_unlock(&rpc_transport_switch_mutex);
}

static int zmk_rpc_init(void) {
    int prev_choice = -1;
    struct zmk_rpc_subsystem *prev_sub = NULL;
    int i = 0;

    STRUCT_SECTION_FOREACH(zmk_rpc_subsystem_handler, handler) {
        struct zmk_rpc_subsystem *sub = find_subsystem_for_choice(handler->subsystem_choice);

        __ASSERT(sub != NULL, "RPC Handler for unknown subsystem choice %d",
                 handler->subsystem_choice);

        if (prev_choice < 0) {
            sub->handlers_start_index = i;
        } else if ((prev_choice != handler->subsystem_choice) && prev_sub) {
            prev_sub->handlers_end_index = i - 1;
            sub->handlers_start_index = i;
        }

        prev_choice = handler->subsystem_choice;
        prev_sub = sub;
        i++;
    }

    if (prev_sub) {
        prev_sub->handlers_end_index = i - 1;
    }

    refresh_selected_transport();

    return 0;
}

SYS_INIT(zmk_rpc_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static int studio_rpc_listener_cb(const zmk_event_t *eh) {
    struct zmk_endpoint_changed *ep_changed = as_zmk_endpoint_changed(eh);
    if (ep_changed) {
        refresh_selected_transport();
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct zmk_studio_rpc_notification *rpc_notify = as_zmk_studio_rpc_notification(eh);
    if (rpc_notify) {
        zmk_rpc_send_notification(&rpc_notify->notification);
        return ZMK_EV_EVENT_BUBBLE;
    }

    zmk_studio_Notification n = zmk_studio_Notification_init_zero;
    STRUCT_SECTION_FOREACH(zmk_rpc_event_mapper, mapper) {
        int ret = mapper->func(eh, &n);
        if (ret >= 0) {
            zmk_rpc_send_notification(&n);
            break;
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(studio_rpc, studio_rpc_listener_cb);
ZMK_SUBSCRIPTION(studio_rpc, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(studio_rpc, zmk_studio_rpc_notification);
