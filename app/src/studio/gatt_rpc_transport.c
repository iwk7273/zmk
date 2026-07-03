/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <sys/types.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/ring_buffer.h>

#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/studio/rpc.h>

#include <zmk/studio/uuid.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

static bool handling_rx = false;

static K_SEM_DEFINE(indicate_sem, 1, 1);
static atomic_t notify_size;

static void rpc_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    ARG_UNUSED(attr);

    bool notif_enabled = (value == BT_GATT_CCC_INDICATE);

    LOG_INF("RPC Notifications %s", notif_enabled ? "enabled" : "disabled");

    if (notif_enabled) {
        zmk_ble_studio_discovery_stop();
    }

#if CONFIG_ZMK_STUDIO_TRANSPORT_BLE_PREF_LATENCY < CONFIG_BT_PERIPHERAL_PREF_LATENCY
    struct bt_conn *conn = zmk_ble_active_profile_conn();
    if (conn) {
        uint8_t latency = notif_enabled ? CONFIG_ZMK_STUDIO_TRANSPORT_BLE_PREF_LATENCY
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
    if (!handling_rx) {
        return len;
    }

    uint32_t copied = 0;
    struct ring_buf *rpc_buf = zmk_rpc_get_rx_buf();
    while (copied < len) {
        uint8_t *buffer;
        uint32_t claim_len = ring_buf_put_claim(rpc_buf, &buffer, len - copied);

        if (claim_len > 0) {
            memcpy(buffer, ((uint8_t *)buf) + copied, claim_len);
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
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_READ | BT_GATT_CHRC_INDICATE,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT, read_rpc_resp,
                           write_rpc_req, NULL),
    BT_GATT_CCC(rpc_ccc_cfg_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT));

static uint16_t get_notify_size_for_conn(struct bt_conn *conn) {
    uint16_t payload_size = 20;

    if (!conn) {
        return MIN(payload_size, CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE);
    }
    uint16_t mtu = bt_gatt_get_mtu(conn);
    if (mtu >= 23) {
        payload_size = mtu - 3;
    } else {
        /* bt_gatt_get_mtu returns 0 for a connection that isn't (yet) in the
         * connected state; fall back to the minimum ATT payload instead of
         * letting `mtu - 3` underflow. */
    }
    return MIN(payload_size, CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE);
}

static uint16_t current_notify_size(void) {
    struct bt_conn *conn = zmk_ble_active_profile_conn();
    uint16_t ns = get_notify_size_for_conn(conn);

    if (conn) {
        bt_conn_unref(conn);
    }

    atomic_set(&notify_size, ns);
    return ns;
}

static void refresh_notify_size(void) {
    (void)current_notify_size();
}

static int gatt_start_rx() {
    refresh_notify_size();
    handling_rx = true;
    return 0;
}

static int gatt_stop_rx(void) {
    handling_rx = false;
    return 0;
}

static uint8_t indicate_buffer[CONFIG_BT_L2CAP_TX_MTU - 3];

static void indicate_cb(struct bt_conn *conn, struct bt_gatt_indicate_params *params, uint8_t err);

static struct bt_gatt_indicate_params rpc_indicate_params = {
    .attr = &rpc_interface.attrs[1],
    .data = indicate_buffer,
    .func = indicate_cb,
};

static void notif_rpc_tx_cb(struct k_work *work);
static K_WORK_DEFINE(notify_tx_work, notif_rpc_tx_cb);

/*
 * Send a single indication carrying up to one MTU worth of bytes from tx_buf,
 * waiting for the previous indication to complete first. Used both from the
 * sync flush path (encoder context) and the work item (final drain).
 *
 * Returns 0 if an indication was queued or there was nothing to send, <0 on
 * error. The caller is responsible for managing the conn ref count.
 */
static int gatt_send_one(struct bt_conn *conn, k_timeout_t sem_timeout) {
    struct ring_buf *tx_buf = zmk_rpc_get_tx_buf();
    if (ring_buf_size_get(tx_buf) == 0) {
        return 0;
    }

    int ret = k_sem_take(&indicate_sem, sem_timeout);
    if (ret < 0) {
        return ret;
    }

    uint16_t notify_size = MIN(get_notify_size_for_conn(conn), sizeof(indicate_buffer));
    uint16_t added = 0;
    while (added < notify_size && ring_buf_size_get(tx_buf) > 0) {
        uint8_t *buf;
        int len = ring_buf_get_claim(tx_buf, &buf, notify_size - added);

        memcpy(indicate_buffer + added, buf, len);

        added += len;
        ring_buf_get_finish(tx_buf, len);
    }

    if (added == 0) {
        /* The other consumer (work item vs. sync flush) drained the buffer
         * while we waited for the semaphore; don't waste an indication round
         * trip on an empty payload. */
        k_sem_give(&indicate_sem);
        return 0;
    }

    rpc_indicate_params.len = added;

    int err = bt_gatt_indicate(conn, &rpc_indicate_params);
    if (err < 0) {
        LOG_ERR("Failed to send indication (%d)", err);
        k_sem_give(&indicate_sem);
        return err;
    }

    /* indicate_cb will release indicate_sem after ACK */
    return 0;
}

static void notif_rpc_tx_cb(struct k_work *work) {
    /* Only ever touched from this handler (single system workqueue thread). */
    static int consecutive_failures;

    struct bt_conn *conn = zmk_ble_active_profile_conn();
    struct ring_buf *tx_buf = zmk_rpc_get_tx_buf();

    if (!conn) {
        LOG_WRN("No active connection for queued data, dropping");
        ring_buf_reset(tx_buf);
        consecutive_failures = 0;
        return;
    }

    int err = gatt_send_one(conn, K_NO_WAIT);
    if (err == -EBUSY || err == -EAGAIN) {
        /* prior indication still in-flight; indicate_cb will resubmit us */
        consecutive_failures = 0;
    } else if (err < 0) {
        /* A half-dead link can keep failing the indicate while the conn object
         * still resolves; bound the immediate retries so this work item doesn't
         * spin on the system workqueue until the conn finally drops. */
        if (++consecutive_failures < 5) {
            LOG_ERR("Failed to drain tx_buf (%d), retrying", err);
            k_work_submit(&notify_tx_work);
        } else {
            LOG_ERR("Failed to drain tx_buf (%d) %d times in a row, dropping pending TX", err,
                    consecutive_failures);
            ring_buf_reset(tx_buf);
            consecutive_failures = 0;
        }
    } else {
        consecutive_failures = 0;
    }

    bt_conn_unref(conn);
}

struct gatt_write_state {
    size_t pending_notify;
};

static void indicate_cb(struct bt_conn *conn, struct bt_gatt_indicate_params *params, uint8_t err) {
    if (err) {
        LOG_WRN("Indication callback error: %d", err);
    }
    k_sem_give(&indicate_sem);
    k_work_submit(&notify_tx_work);
}

/*
 * Synchronously drain the tx_buf one indication at a time until tx_buf has room
 * for the encoder to make progress, or we hit an error / timeout.
 *
 * Called from gatt_tx_notify in encoder context when the ring_buf is filling up
 * faster than the work item can drain it. By waiting here for the indication
 * ACK we keep the ring_buf small enough that the encoder doesn't block on
 * rpc_tx_buffer_write claim failures, which lets us use the same modest TX_BUF
 * size on USB and BLE builds.
 */
static int gatt_sync_flush(void) {
    struct bt_conn *conn = zmk_ble_active_profile_conn();
    if (!conn) {
        struct ring_buf *tx_buf = zmk_rpc_get_tx_buf();
        ring_buf_reset(tx_buf);
        return -ENOTCONN;
    }

    int err = gatt_send_one(conn, K_MSEC(200));
    bt_conn_unref(conn);
    if (err == -EAGAIN || err == -EBUSY) {
        /* Prior indication never ACKed within the window — link likely stalled.
         * Drop the pending response so we don't loop forever in tx_notify.
         */
        struct ring_buf *tx_buf = zmk_rpc_get_tx_buf();
        ring_buf_reset(tx_buf);
        LOG_ERR("gatt_sync_flush: indicate_sem timed out, dropping pending TX");
    }
    return err;
}

static void gatt_tx_notify(struct ring_buf *tx_buf, size_t added, bool msg_done, void *user_data) {
    struct gatt_write_state *state = (struct gatt_write_state *)user_data;

    state->pending_notify += added;

    uint16_t ns = current_notify_size();

    if (added == 0 && ring_buf_size_get(tx_buf) > 0) {
        /* Encoder is stalled waiting for tx_buf room — drain in our context. */
        gatt_sync_flush();
        state->pending_notify = 0;
        return;
    }

    if (msg_done) {
        /* End of message: hand the remainder to the work item so this call
         * returns quickly and the encoder can finish.
         */
        k_work_submit(&notify_tx_work);
        state->pending_notify = 0;
        return;
    }

    if (state->pending_notify >= ns) {
        /* Got at least one indication worth of fresh bytes; drain now so the
         * ring_buf doesn't fill up and stall the encoder.
         */
        gatt_sync_flush();
        state->pending_notify = 0;
    }
}

static struct gatt_write_state tx_state = {};

static void *gatt_tx_user_data(void) {
    memset(&tx_state, 0, sizeof(tx_state));

    return &tx_state;
}

ZMK_RPC_TRANSPORT(gatt, ZMK_TRANSPORT_BLE, gatt_start_rx, gatt_stop_rx, gatt_tx_user_data,
                  gatt_tx_notify);

static int gatt_rpc_listener(const zmk_event_t *eh) {
    refresh_notify_size();

#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOCK_ON_DISCONNECT)
    struct bt_conn *conn = zmk_ble_active_profile_conn();

    if (!conn) {
        zmk_studio_core_lock();
    } else {
        bt_conn_unref(conn);
    }
#endif

    return 0;
}

ZMK_LISTENER(gatt_rpc_listener, gatt_rpc_listener);
ZMK_SUBSCRIPTION(gatt_rpc_listener, zmk_ble_active_profile_changed);
