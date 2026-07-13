/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/combos.h>
#include <zmk/stdlib.h>
#include <zmk/studio/rpc.h>

#include <pb_encode.h>

ZMK_RPC_SUBSYSTEM(combos)

#define COMBOS_RESPONSE(type, ...) ZMK_RPC_RESPONSE(combos, type, __VA_ARGS__)
#define COMBOS_NOTIFICATION(type, ...) ZMK_RPC_NOTIFICATION(combos, type, __VA_ARGS__)

static void fill_proto_binding(const struct zmk_behavior_binding *binding,
                               zmk_keymap_BehaviorBinding *proto) {
    proto->behavior_id = zmk_behavior_get_local_id(binding->behavior_dev);
    proto->param1 = binding->param1;
    proto->param2 = binding->param2;
}

static void fill_proto_combo(const struct zmk_combo_slot *slot, zmk_combos_Combo *proto) {
    strlcpy(proto->combo_id, slot->combo_id, sizeof(proto->combo_id));
    proto->slot_index = slot->slot_index;
    proto->source = slot->source == ZMK_COMBO_SOURCE_USER
                        ? zmk_combos_ComboSource_COMBO_SOURCE_USER
                        : zmk_combos_ComboSource_COMBO_SOURCE_STOCK;
    proto->enabled = slot->enabled;
    proto->has_binding = true;
    fill_proto_binding(&slot->binding, &proto->binding);
    proto->key_positions_count = slot->key_position_count;
    for (uint8_t i = 0; i < slot->key_position_count; i++) {
        proto->key_positions[i] = slot->key_positions[i];
    }
    proto->layers_count = slot->layer_count;
    for (uint8_t i = 0; i < slot->layer_count; i++) {
        proto->layers[i] = slot->layers[i];
    }
    proto->timeout_ms = slot->timeout_ms;
    proto->require_prior_idle_ms = slot->require_prior_idle_ms;
    proto->slow_release = slot->slow_release;
    proto->dirty = slot->dirty;
}

static bool encode_combos(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    ARG_UNUSED(arg);

    for (uint8_t i = 0; i < zmk_combo_get_slot_count(); i++) {
        struct zmk_combo_slot slot;
        int ret = zmk_combo_get_slot(i, &slot);
        if (ret < 0) {
            LOG_WRN("Failed to get combo slot %d (%d)", i, ret);
            continue;
        }

        zmk_combos_Combo combo = zmk_combos_Combo_init_zero;
        fill_proto_combo(&slot, &combo);

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        if (!pb_encode_submessage(stream, &zmk_combos_Combo_msg, &combo)) {
            return false;
        }
    }

    return true;
}

static zmk_combos_ComboState combo_state_msg(void) {
    zmk_combos_ComboState state = zmk_combos_ComboState_init_zero;
    state.schema_version = 1;
    state.max_combos = zmk_combo_get_slot_count();
    state.max_keys_per_combo = zmk_combo_get_max_keys_per_combo();
    state.combos.funcs.encode = encode_combos;
    state.dirty = zmk_combo_check_unsaved_changes() > 0;
    return state;
}

static void map_errno_to_save_resp(int err, zmk_combos_SaveChangesResponse *resp) {
    resp->which_result = zmk_combos_SaveChangesResponse_err_tag;

    switch (err) {
    case -ENOTSUP:
        resp->result.err = zmk_combos_SaveChangesErrorCode_SAVE_CHANGES_ERR_NOT_SUPPORTED;
        break;
    case -ENOSPC:
        resp->result.err = zmk_combos_SaveChangesErrorCode_SAVE_CHANGES_ERR_NO_SPACE;
        break;
    default:
        resp->result.err = zmk_combos_SaveChangesErrorCode_SAVE_CHANGES_ERR_GENERIC;
        break;
    }
}

static zmk_combos_SetComboErrorCode map_errno_to_set_combo_err(int err) {
    switch (err) {
    case -EINVAL:
        return zmk_combos_SetComboErrorCode_SET_COMBO_ERR_INVALID_PARAMETERS;
    case -ENODEV:
        return zmk_combos_SetComboErrorCode_SET_COMBO_ERR_INVALID_BEHAVIOR;
    case -EBUSY:
        return zmk_combos_SetComboErrorCode_SET_COMBO_ERR_BUSY;
    case -ENOENT:
        return zmk_combos_SetComboErrorCode_SET_COMBO_ERR_INVALID_SLOT;
    default:
        return zmk_combos_SetComboErrorCode_SET_COMBO_ERR_GENERIC;
    }
}

static zmk_studio_Response get_combo_state(const zmk_studio_Request *req) {
    ARG_UNUSED(req);
    LOG_DBG("");
    return COMBOS_RESPONSE(get_combo_state, combo_state_msg());
}

static int proto_combo_to_slot(const zmk_combos_Combo *proto, struct zmk_combo_slot *slot) {
    if (!proto || !slot || proto->slot_index >= zmk_combo_get_slot_count()) {
        return -ENOENT;
    }

    memset(slot, 0, sizeof(*slot));
    strlcpy(slot->combo_id, proto->combo_id, sizeof(slot->combo_id));
    slot->slot_index = proto->slot_index;
    slot->source = proto->source == zmk_combos_ComboSource_COMBO_SOURCE_USER
                       ? ZMK_COMBO_SOURCE_USER
                       : ZMK_COMBO_SOURCE_STOCK;
    slot->enabled = proto->enabled;

    const char *behavior_name = zmk_behavior_find_behavior_name_from_local_id(proto->binding.behavior_id);
    if (slot->enabled && !behavior_name) {
        return -ENODEV;
    }
    slot->binding = (struct zmk_behavior_binding){
        .behavior_dev = behavior_name,
        .param1 = proto->binding.param1,
        .param2 = proto->binding.param2,
    };
    slot->key_position_count = proto->key_positions_count;
    for (uint8_t i = 0; i < proto->key_positions_count; i++) {
        slot->key_positions[i] = proto->key_positions[i];
    }
    slot->layer_count = proto->layers_count;
    for (uint8_t i = 0; i < proto->layers_count; i++) {
        slot->layers[i] = proto->layers[i];
    }
    slot->timeout_ms = proto->timeout_ms;
    slot->require_prior_idle_ms = proto->require_prior_idle_ms;
    slot->slow_release = proto->slow_release;
    return 0;
}

static zmk_studio_Response set_combo(const zmk_studio_Request *req) {
    LOG_DBG("");
    const zmk_combos_SetComboRequest *set_req = &req->subsystem.combos.request_type.set_combo;

    zmk_combos_SetComboResponse resp = zmk_combos_SetComboResponse_init_zero;
    struct zmk_combo_slot slot;
    int ret = proto_combo_to_slot(&set_req->combo, &slot);
    if (ret < 0) {
        resp.which_result = zmk_combos_SetComboResponse_err_tag;
        resp.result.err = map_errno_to_set_combo_err(ret);
        return COMBOS_RESPONSE(set_combo, resp);
    }

    ret = zmk_combo_set_slot(&slot);
    if (ret < 0) {
        resp.which_result = zmk_combos_SetComboResponse_err_tag;
        resp.result.err = map_errno_to_set_combo_err(ret);
        return COMBOS_RESPONSE(set_combo, resp);
    }

    struct zmk_combo_slot updated;
    ret = zmk_combo_get_slot(slot.slot_index, &updated);
    if (ret < 0) {
        resp.which_result = zmk_combos_SetComboResponse_err_tag;
        resp.result.err = zmk_combos_SetComboErrorCode_SET_COMBO_ERR_GENERIC;
        return COMBOS_RESPONSE(set_combo, resp);
    }

    resp.which_result = zmk_combos_SetComboResponse_ok_tag;
    resp.result.ok.has_combo = true;
    fill_proto_combo(&updated, &resp.result.ok.combo);
    resp.result.ok.dirty = zmk_combo_check_unsaved_changes() > 0;

    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
        .notification = COMBOS_NOTIFICATION(unsaved_changes_status_changed, resp.result.ok.dirty)});

    return COMBOS_RESPONSE(set_combo, resp);
}

static zmk_studio_Response check_unsaved_changes(const zmk_studio_Request *req) {
    ARG_UNUSED(req);
    LOG_DBG("");
    return COMBOS_RESPONSE(check_unsaved_changes, zmk_combo_check_unsaved_changes() > 0);
}

static zmk_studio_Response save_changes(const zmk_studio_Request *req) {
    ARG_UNUSED(req);
    LOG_DBG("");

    zmk_combos_SaveChangesResponse resp = zmk_combos_SaveChangesResponse_init_zero;
    resp.which_result = zmk_combos_SaveChangesResponse_ok_tag;
    resp.result.ok = true;

    int ret = zmk_combo_save_changes();
    if (ret < 0) {
        map_errno_to_save_resp(ret, &resp);
        return COMBOS_RESPONSE(save_changes, resp);
    }

    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
        .notification = COMBOS_NOTIFICATION(unsaved_changes_status_changed, false)});

    return COMBOS_RESPONSE(save_changes, resp);
}

static zmk_studio_Response discard_changes(const zmk_studio_Request *req) {
    ARG_UNUSED(req);
    LOG_DBG("");

    int ret = zmk_combo_discard_changes();
    if (ret < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
        .notification = COMBOS_NOTIFICATION(unsaved_changes_status_changed, false)});

    return COMBOS_RESPONSE(discard_changes, true);
}

static int combos_settings_reset(void) { return zmk_combo_reset_settings(); }

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(combos, combos_settings_reset);

ZMK_RPC_SUBSYSTEM_HANDLER(combos, get_combo_state, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(combos, set_combo, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(combos, check_unsaved_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(combos, save_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(combos, discard_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);
