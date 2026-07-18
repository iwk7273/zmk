/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>

#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/studio/rpc.h>
#include <zmk/studio/uuid.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#define GATT_INDICATION_CONTEXT_COUNT 2
#define GATT_TRANSIENT_RETRY_DELAY_MS 10

static atomic_t handling_rx;
static atomic_t ccc_enabled;

static K_MUTEX_DEFINE(gatt_session_control_mutex);
static K_MUTEX_DEFINE(gatt_tx_mutex);
static struct k_spinlock gatt_session_lock;
static uint32_t gatt_session_generation;
static bool gatt_session_active;
static int gatt_session_error = -ENOTCONN;
static struct bt_conn *gatt_session_conn;

/* indicate_sem represents the one indication slot owned by the current
 * session. stop/cancel never releases it; accepted indications release it only
 * from their completion callback. */
static K_SEM_DEFINE(indicate_sem, 0, 1);
static K_SEM_DEFINE(tx_data_sem, 0, 1);
static K_SEM_DEFINE(cancel_sem, 0, 1);

static void gatt_start_session(void);
static void gatt_stop_session(int error);

static bool session_is_current(uint32_t generation) {
    k_spinlock_key_t key = k_spin_lock(&gatt_session_lock);
    bool current = gatt_session_active && gatt_session_generation == generation;
    k_spin_unlock(&gatt_session_lock, key);
    return current;
}

static uint32_t session_generation_get(void) {
    k_spinlock_key_t key = k_spin_lock(&gatt_session_lock);
    uint32_t generation = gatt_session_generation;
    k_spin_unlock(&gatt_session_lock, key);
    return generation;
}

static int session_status(uint32_t generation) {
    k_spinlock_key_t key = k_spin_lock(&gatt_session_lock);
    int status;

    if (gatt_session_generation != generation) {
        status = -ECANCELED;
    } else if (!gatt_session_active) {
        status = gatt_session_error;
    } else {
        status = 0;
    }

    k_spin_unlock(&gatt_session_lock, key);
    return status;
}

static struct bt_conn *session_conn_ref(uint32_t generation) {
    struct bt_conn *conn = NULL;
    k_spinlock_key_t key = k_spin_lock(&gatt_session_lock);

    if (gatt_session_active && gatt_session_generation == generation && gatt_session_conn) {
        conn = bt_conn_ref(gatt_session_conn);
    }

    k_spin_unlock(&gatt_session_lock, key);
    return conn;
}

static void fail_session(uint32_t generation, int error) {
    bool failed = false;
    struct bt_conn *session_conn = NULL;
    k_spinlock_key_t key = k_spin_lock(&gatt_session_lock);

    if (gatt_session_generation == generation && gatt_session_active) {
        gatt_session_active = false;
        gatt_session_error = error;
        session_conn = gatt_session_conn;
        gatt_session_conn = NULL;
        failed = true;
    }

    k_spin_unlock(&gatt_session_lock, key);

    if (failed) {
        if (session_conn) {
            bt_conn_unref(session_conn);
        }
        k_sem_give(&cancel_sem);
        k_sem_give(&tx_data_sem);
    }
}

static void release_unsubmitted_slot(uint32_t generation) {
    k_spinlock_key_t key = k_spin_lock(&gatt_session_lock);

    if (gatt_session_generation == generation && gatt_session_active) {
        k_sem_give(&indicate_sem);
    }

    k_spin_unlock(&gatt_session_lock, key);
}

static void rpc_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    ARG_UNUSED(attr);

    bool enabled = (value == BT_GATT_CCC_INDICATE);
    atomic_set(&ccc_enabled, enabled);

    LOG_INF("RPC Notifications %s", enabled ? "enabled" : "disabled");

    if (enabled) {
        zmk_ble_studio_discovery_stop();
        if (atomic_get(&handling_rx)) {
            gatt_start_session();
        }
    } else {
        gatt_stop_session(-EACCES);
    }

#if CONFIG_ZMK_STUDIO_TRANSPORT_BLE_PREF_LATENCY < CONFIG_BT_PERIPHERAL_PREF_LATENCY
    struct bt_conn *conn = zmk_ble_active_profile_conn();
    if (conn) {
        uint8_t latency = enabled ? CONFIG_ZMK_STUDIO_TRANSPORT_BLE_PREF_LATENCY
                                  : CONFIG_BT_PERIPHERAL_PREF_LATENCY;

        int ret = bt_conn_le_param_update(
            conn,
            BT_LE_CONN_PARAM(CONFIG_BT_PERIPHERAL_PREF_MIN_INT, CONFIG_BT_PERIPHERAL_PREF_MAX_INT,
                             latency, CONFIG_BT_PERIPHERAL_PREF_TIMEOUT));
        if (ret < 0) {
            LOG_WRN("Failed to request lower latency while studio is active (%d)", ret);
        }

        bt_conn_unref(conn);
    }
#endif
}

static ssize_t read_rpc_resp(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                             uint16_t len, uint16_t offset) {
    LOG_DBG("Read response for length %d at offset %d", len, offset);
    return 0;
}

static ssize_t write_rpc_req(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                             uint16_t len, uint16_t offset, uint8_t flags) {
    if (!atomic_get(&handling_rx) || session_status(session_generation_get()) < 0) {
        return len;
    }

    uint32_t copied = 0;
    struct ring_buf *rpc_buf = zmk_rpc_get_rx_buf();
    while (copied < len) {
        uint8_t *buffer;
        uint32_t claim_len = ring_buf_put_claim(rpc_buf, &buffer, len - copied);

        if (claim_len > 0) {
            memcpy(buffer, ((const uint8_t *)buf) + copied, claim_len);
            copied += claim_len;
        }

        ring_buf_put_finish(rpc_buf, claim_len);
    }

    zmk_rpc_rx_notify();
    return len;
}

BT_GATT_SERVICE_DEFINE(
    rpc_interface, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(ZMK_STUDIO_BT_SERVICE_UUID)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ZMK_STUDIO_BT_RPC_CHRC_UUID),
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP |
                               BT_GATT_CHRC_READ | BT_GATT_CHRC_INDICATE,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT, read_rpc_resp,
                           write_rpc_req, NULL),
    BT_GATT_CCC(rpc_ccc_cfg_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT));

struct gatt_indication_context {
    struct bt_gatt_indicate_params params;
    uint32_t generation;
    uint8_t data[CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE];
};

K_MEM_SLAB_DEFINE_STATIC(gatt_indication_slab, sizeof(struct gatt_indication_context),
                         GATT_INDICATION_CONTEXT_COUNT, 4);

static uint16_t get_notify_size_for_conn(struct bt_conn *conn) {
    uint16_t payload_size = 20;

    if (!conn) {
        return MIN(payload_size, CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE);
    }

    uint16_t mtu = bt_gatt_get_mtu(conn);
    if (mtu >= 23) {
        payload_size = mtu - 3;
    }

    return MIN(payload_size, CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE);
}

static uint16_t current_notify_size(uint32_t generation) {
    struct bt_conn *conn = session_conn_ref(generation);
    uint16_t ns = get_notify_size_for_conn(conn);

    if (conn) {
        bt_conn_unref(conn);
    }

    return ns;
}

static void gatt_start_session(void) {
    struct bt_conn *conn = zmk_ble_active_profile_conn();
    if (!conn) {
        gatt_stop_session(-ENOTCONN);
        return;
    }

    k_mutex_lock(&gatt_session_control_mutex, K_FOREVER);

    /* The CCC/profile callback may have observed handling_rx just before an
     * endpoint switch stopped this transport. Recheck both gates after taking
     * the session-control mutex so that delayed callbacks cannot restart an
     * orphaned BLE RPC session. */
    if (!atomic_get(&handling_rx) || !atomic_get(&ccc_enabled)) {
        k_mutex_unlock(&gatt_session_control_mutex);
        bt_conn_unref(conn);
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&gatt_session_lock);
    if (gatt_session_active && gatt_session_conn == conn) {
        k_spin_unlock(&gatt_session_lock, key);
        k_mutex_unlock(&gatt_session_control_mutex);
        bt_conn_unref(conn);
        return;
    }

    struct bt_conn *old_conn = gatt_session_conn;
    gatt_session_conn = NULL;
    gatt_session_generation++;
    gatt_session_active = false;
    gatt_session_error = -ECANCELED;
    k_spin_unlock(&gatt_session_lock, key);

    if (old_conn) {
        bt_conn_unref(old_conn);
    }

    k_sem_give(&cancel_sem);
    k_sem_give(&tx_data_sem);

    /* A reconnect/CCC restart can keep BLE selected, so the RPC core does not
     * necessarily run an endpoint switch. Synchronize with any active encoder
     * and discard bytes belonging to the invalidated session here. */
    zmk_rpc_reset_tx_buffer();

    /* Wait only for the synchronous ring/submit critical section. An older
     * accepted indication keeps its own params/data context until destroy. */
    k_mutex_lock(&gatt_tx_mutex, K_FOREVER);
    k_sem_reset(&indicate_sem);
    k_sem_reset(&cancel_sem);
    k_sem_reset(&tx_data_sem);

    key = k_spin_lock(&gatt_session_lock);
    gatt_session_active = true;
    gatt_session_error = 0;
    gatt_session_conn = conn;
    uint32_t generation = gatt_session_generation;
    k_sem_give(&indicate_sem);
    k_spin_unlock(&gatt_session_lock, key);

    LOG_DBG("GATT RPC session %u started", generation);
    k_mutex_unlock(&gatt_tx_mutex);
    k_mutex_unlock(&gatt_session_control_mutex);
}

static void gatt_stop_session(int error) {
    k_mutex_lock(&gatt_session_control_mutex, K_FOREVER);

    k_spinlock_key_t key = k_spin_lock(&gatt_session_lock);
    struct bt_conn *session_conn = gatt_session_conn;
    gatt_session_conn = NULL;
    gatt_session_generation++;
    gatt_session_active = false;
    gatt_session_error = error;
    uint32_t generation = gatt_session_generation;
    k_spin_unlock(&gatt_session_lock, key);

    if (session_conn) {
        bt_conn_unref(session_conn);
    }

    /* Cancellation wakes the TX worker without pretending an indication has
     * completed. A late callback belongs to the old generation and is ignored. */
    k_sem_give(&cancel_sem);
    k_sem_give(&tx_data_sem);

    k_mutex_lock(&gatt_tx_mutex, K_FOREVER);
    k_mutex_unlock(&gatt_tx_mutex);
    LOG_DBG("GATT RPC session %u stopped (%d)", generation, error);
    k_mutex_unlock(&gatt_session_control_mutex);
}

static int gatt_start_rx(void) {
    atomic_set(&handling_rx, 1);

    if (atomic_get(&ccc_enabled)) {
        gatt_start_session();
    } else {
        gatt_stop_session(-EACCES);
    }

    return 0;
}

static int gatt_stop_rx(void) {
    atomic_clear(&handling_rx);
    gatt_stop_session(-ECANCELED);
    return 0;
}

static void indicate_destroy(struct bt_gatt_indicate_params *params) {
    struct gatt_indication_context *context =
        CONTAINER_OF(params, struct gatt_indication_context, params);
    k_mem_slab_free(&gatt_indication_slab, context);
}

static void indicate_cb(struct bt_conn *conn, struct bt_gatt_indicate_params *params, uint8_t err) {
    struct gatt_indication_context *context =
        CONTAINER_OF(params, struct gatt_indication_context, params);
    bool wake_tx = false;
    bool failed = false;
    struct bt_conn *session_conn = NULL;

    k_spinlock_key_t key = k_spin_lock(&gatt_session_lock);
    if (gatt_session_generation == context->generation) {
        k_sem_give(&indicate_sem);
        if (err && gatt_session_active) {
            gatt_session_active = false;
            gatt_session_error = -EIO;
            session_conn = gatt_session_conn;
            gatt_session_conn = NULL;
            failed = true;
        } else if (!err && gatt_session_active) {
            wake_tx = true;
        }
    }
    k_spin_unlock(&gatt_session_lock, key);

    if (err) {
        LOG_WRN("Indication callback error: %d", err);
    }
    if (failed) {
        if (session_conn) {
            bt_conn_unref(session_conn);
        }
        k_sem_give(&cancel_sem);
    }
    if (failed || wake_tx) {
        k_sem_give(&tx_data_sem);
    }
}

static int wait_for_indication_slot(uint32_t generation) {
    for (;;) {
        struct k_poll_event events[] = {
            K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SEM_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY,
                                     &indicate_sem),
            K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SEM_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY,
                                     &cancel_sem),
        };

        int err = k_poll(events, ARRAY_SIZE(events),
                         K_MSEC(CONFIG_ZMK_STUDIO_TRANSPORT_BLE_INDICATE_TIMEOUT_MS));
        if (err < 0) {
            return err == -EAGAIN ? -ETIMEDOUT : err;
        }

        if (events[1].state == K_POLL_STATE_SEM_AVAILABLE) {
            k_sem_take(&cancel_sem, K_NO_WAIT);
            int status = session_status(generation);
            if (status < 0) {
                return status;
            }
            continue;
        }

        if (events[0].state == K_POLL_STATE_SEM_AVAILABLE &&
            k_sem_take(&indicate_sem, K_NO_WAIT) == 0) {
            int status = session_status(generation);
            if (status < 0) {
                return status;
            }
            return 0;
        }
    }
}

/* Peek a chunk and remove it only after bt_gatt_indicate() has accepted the
 * operation. The context remains owned by Zephyr until indicate_destroy(). */
static int gatt_send_one(struct bt_conn *conn, uint32_t generation) {
    struct ring_buf *tx_buf = zmk_rpc_get_tx_buf();
    if (ring_buf_size_get(tx_buf) == 0) {
        return 0;
    }

    int err = wait_for_indication_slot(generation);
    if (err < 0) {
        return err;
    }

    struct gatt_indication_context *context;
    err = k_mem_slab_alloc(&gatt_indication_slab, (void **)&context, K_NO_WAIT);
    if (err < 0) {
        release_unsubmitted_slot(generation);
        return -ENOMEM;
    }

    memset(context, 0, sizeof(*context));
    context->generation = generation;

    k_mutex_lock(&gatt_tx_mutex, K_FOREVER);

    if (!session_is_current(generation)) {
        err = session_status(generation);
        goto unsubmitted;
    }

    uint16_t chunk_size = MIN(get_notify_size_for_conn(conn), sizeof(context->data));
    uint16_t added = ring_buf_peek(tx_buf, context->data, chunk_size);
    if (added == 0) {
        err = 0;
        goto unsubmitted;
    }

    context->params.attr = &rpc_interface.attrs[1];
    context->params.data = context->data;
    context->params.len = added;
    context->params.func = indicate_cb;
    context->params.destroy = indicate_destroy;

    err = bt_gatt_indicate(conn, &context->params);
    if (err < 0) {
        goto unsubmitted;
    }

    uint32_t consumed = ring_buf_get(tx_buf, NULL, added);
    if (consumed != added) {
        LOG_ERR("GATT RPC TX ring consume mismatch (%u/%u)", consumed, added);
    }

    k_mutex_unlock(&gatt_tx_mutex);
    return 0;

unsubmitted:
    k_mutex_unlock(&gatt_tx_mutex);
    k_mem_slab_free(&gatt_indication_slab, context);
    release_unsubmitted_slot(generation);
    return err;
}

static bool is_transient_send_error(int error) { return error == -ENOMEM || error == -EAGAIN; }

static void gatt_tx_main(void *, void *, void *) {
    struct ring_buf *tx_buf = zmk_rpc_get_tx_buf();

    for (;;) {
        k_sem_take(&tx_data_sem, K_FOREVER);

        uint32_t generation = session_generation_get();
        uint8_t retries = 0;

        while (session_is_current(generation) && ring_buf_size_get(tx_buf) > 0) {
            struct bt_conn *conn = session_conn_ref(generation);
            if (!conn) {
                fail_session(generation, -ENOTCONN);
                break;
            }

            int err = gatt_send_one(conn, generation);
            bt_conn_unref(conn);

            if (err == 0) {
                retries = 0;
                continue;
            }

            if (err == -ECANCELED || !session_is_current(generation)) {
                break;
            }

            if (is_transient_send_error(err) &&
                retries < CONFIG_ZMK_STUDIO_TRANSPORT_BLE_TRANSIENT_RETRY_COUNT) {
                retries++;
                LOG_WRN("GATT RPC TX transient retry %u after error %d", retries, err);
                k_sleep(K_MSEC(GATT_TRANSIENT_RETRY_DELAY_MS));
                continue;
            }

            LOG_ERR("GATT RPC TX session failed (%d)", err);
            fail_session(generation, err);
            break;
        }
    }
}

K_THREAD_DEFINE(gatt_rpc_tx_thread, CONFIG_ZMK_STUDIO_TRANSPORT_BLE_TX_STACK_SIZE, gatt_tx_main,
                NULL, NULL, NULL, CONFIG_ZMK_STUDIO_TRANSPORT_BLE_TX_PRIORITY, 0, 0);

static void *gatt_tx_user_data(void) { return (void *)(uintptr_t)session_generation_get(); }

static int gatt_tx_notify(struct ring_buf *tx_buf, size_t added, bool msg_done, void *user_data) {
    ARG_UNUSED(added);

    uint32_t generation = (uint32_t)(uintptr_t)user_data;
    int status = session_status(generation);
    if (status < 0) {
        return status;
    }

    uint16_t ns = current_notify_size(generation);
    if (msg_done || ring_buf_space_get(tx_buf) == 0 || ring_buf_size_get(tx_buf) >= ns) {
        k_sem_give(&tx_data_sem);
    }

    return 0;
}

static void gatt_tx_abort(struct ring_buf *tx_buf) {
    k_mutex_lock(&gatt_tx_mutex, K_FOREVER);
    ring_buf_reset(tx_buf);
    k_mutex_unlock(&gatt_tx_mutex);
}

ZMK_RPC_TRANSPORT(gatt, ZMK_TRANSPORT_BLE, gatt_start_rx, gatt_stop_rx, gatt_tx_user_data,
                  gatt_tx_notify, gatt_tx_abort);

static int gatt_rpc_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);

    struct bt_conn *conn = zmk_ble_active_profile_conn();
    bool connected = conn != NULL;
    if (conn) {
        bt_conn_unref(conn);
    }

    if (atomic_get(&handling_rx)) {
        if (connected && atomic_get(&ccc_enabled)) {
            gatt_start_session();
        } else {
            gatt_stop_session(-ENOTCONN);
        }
    }

#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOCK_ON_DISCONNECT)
    if (!connected) {
        zmk_studio_core_lock();
    }
#endif

    return 0;
}

ZMK_LISTENER(gatt_rpc_listener, gatt_rpc_listener);
ZMK_SUBSCRIPTION(gatt_rpc_listener, zmk_ble_active_profile_changed);
