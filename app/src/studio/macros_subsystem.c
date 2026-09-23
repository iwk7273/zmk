/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/studio/rpc.h>
#include <zmk/stdlib.h>
#include <zmk/user_macros.h>

#include <pb_encode.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

ZMK_RPC_SUBSYSTEM(macros)

#define MACROS_RESPONSE(type, ...) ZMK_RPC_RESPONSE(macros, type, __VA_ARGS__)

static zmk_studio_Notification macro_notification;
static struct zmk_user_macro_slot slot_scratch;
static zmk_macros_SetMacroResponse set_macro_resp;
static zmk_macros_GetMacroResponse get_macro_resp;
static zmk_macros_ResetMacroResponse reset_macro_resp;

static void notify_macros_unsaved(bool dirty) {
    memset(&macro_notification, 0, sizeof(macro_notification));
    macro_notification.which_subsystem = zmk_studio_Notification_macros_tag;
    macro_notification.subsystem.macros.which_notification_type =
        zmk_macros_Notification_unsaved_changes_status_changed_tag;
    macro_notification.subsystem.macros.notification_type.unsaved_changes_status_changed = dirty;
    zmk_rpc_send_notification(&macro_notification);
}

static zmk_macros_MacroStepAction step_action_to_proto(uint8_t action) {
    switch (action) {
    case ZMK_USER_MACRO_STEP_TAP:
        return zmk_macros_MacroStepAction_MACRO_STEP_ACTION_TAP;
    case ZMK_USER_MACRO_STEP_PRESS:
        return zmk_macros_MacroStepAction_MACRO_STEP_ACTION_PRESS;
    case ZMK_USER_MACRO_STEP_RELEASE:
        return zmk_macros_MacroStepAction_MACRO_STEP_ACTION_RELEASE;
    case ZMK_USER_MACRO_STEP_WAIT:
        return zmk_macros_MacroStepAction_MACRO_STEP_ACTION_WAIT;
    case ZMK_USER_MACRO_STEP_TEXT:
        return zmk_macros_MacroStepAction_MACRO_STEP_ACTION_TEXT;
    default:
        return zmk_macros_MacroStepAction_MACRO_STEP_ACTION_UNSPECIFIED;
    }
}

static int step_action_from_proto(zmk_macros_MacroStepAction action) {
    switch (action) {
    case zmk_macros_MacroStepAction_MACRO_STEP_ACTION_TAP:
        return ZMK_USER_MACRO_STEP_TAP;
    case zmk_macros_MacroStepAction_MACRO_STEP_ACTION_PRESS:
        return ZMK_USER_MACRO_STEP_PRESS;
    case zmk_macros_MacroStepAction_MACRO_STEP_ACTION_RELEASE:
        return ZMK_USER_MACRO_STEP_RELEASE;
    case zmk_macros_MacroStepAction_MACRO_STEP_ACTION_WAIT:
        return ZMK_USER_MACRO_STEP_WAIT;
    case zmk_macros_MacroStepAction_MACRO_STEP_ACTION_TEXT:
        return ZMK_USER_MACRO_STEP_TEXT;
    default:
        return -EINVAL;
    }
}

static void fill_proto_macro(const struct zmk_user_macro_slot *slot, zmk_macros_Macro *proto,
                             bool include_steps) {
    proto->slot_index = slot->slot_index;
    proto->behavior_id =
        slot->behavior_local_id == UINT16_MAX ? -1 : (int32_t)slot->behavior_local_id;
    strlcpy(proto->name, slot->name, sizeof(proto->name));
    proto->enabled = slot->enabled;
    proto->dirty = slot->dirty;
    proto->encoded_size = slot->encoded_size;
    proto->steps_count = 0;
    proto->packed_keys.size = 0;

    if (!include_steps) {
        return;
    }

    proto->steps_count = slot->step_count;
    for (uint8_t i = 0; i < slot->step_count; i++) {
        zmk_macros_MacroStep *step = &proto->steps[i];
        step->action = step_action_to_proto(slot->steps[i].action);
        step->wait_ms = slot->steps[i].wait_ms;
        step->packed_keys_offset = slot->steps[i].packed_off;
        step->packed_keys_length = slot->steps[i].packed_len;
        step->has_binding = false;
        if (slot->steps[i].action == ZMK_USER_MACRO_STEP_TAP ||
            slot->steps[i].action == ZMK_USER_MACRO_STEP_PRESS ||
            slot->steps[i].action == ZMK_USER_MACRO_STEP_RELEASE) {
            step->has_binding = true;
            step->binding.behavior_id = slot->steps[i].behavior_local_id;
            step->binding.param1 = slot->steps[i].param1;
            step->binding.param2 = 0;
        }
    }
    proto->packed_keys.size = slot->packed_len;
    memcpy(proto->packed_keys.bytes, slot->packed_keys, slot->packed_len);
}

static bool encode_macros(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    ARG_UNUSED(arg);

    for (uint8_t i = 0; i < zmk_user_macro_get_slot_count(); i++) {
        struct zmk_user_macro_slot slot;
        int ret = zmk_user_macro_get_summary(i, &slot);
        if (ret < 0) {
            LOG_WRN("Failed to get user macro slot %d (%d)", i, ret);
            continue;
        }

        zmk_macros_Macro macro = zmk_macros_Macro_init_zero;
        fill_proto_macro(&slot, &macro, false);
        if (!pb_encode_tag_for_field(stream, field) ||
            !pb_encode_submessage(stream, &zmk_macros_Macro_msg, &macro)) {
            return false;
        }
    }
    return true;
}

static zmk_macros_MacroState macro_state_msg(void) {
    zmk_macros_MacroState state = zmk_macros_MacroState_init_zero;
    state.schema_version = 2;
    state.max_macros = zmk_user_macro_get_slot_count();
    state.max_steps_per_macro = zmk_user_macro_get_max_steps();
    state.macros.funcs.encode = encode_macros;
    state.dirty = zmk_user_macro_check_unsaved_changes() > 0;
    state.tap_ms = zmk_user_macro_get_tap_ms();
    state.pool_bytes_total = zmk_user_macro_get_pool_total();
    state.pool_bytes_used = zmk_user_macro_get_pool_used();
    state.max_macro_bytes = zmk_user_macro_get_max_bytes();
    return state;
}

static zmk_studio_Response get_macro_state(const zmk_studio_Request *req) {
    ARG_UNUSED(req);
    return MACROS_RESPONSE(get_macro_state, macro_state_msg());
}

static zmk_macros_SetMacroErrorCode map_errno_to_set_macro_err(int err) {
    switch (err) {
    case -ENOENT:
        return zmk_macros_SetMacroErrorCode_SET_MACRO_ERR_INVALID_SLOT;
    case -ENAMETOOLONG:
        return zmk_macros_SetMacroErrorCode_SET_MACRO_ERR_INVALID_NAME;
    case -ENODEV:
        return zmk_macros_SetMacroErrorCode_SET_MACRO_ERR_INVALID_BEHAVIOR;
    case -EBUSY:
        return zmk_macros_SetMacroErrorCode_SET_MACRO_ERR_BUSY;
    case -ENOSPC:
        return zmk_macros_SetMacroErrorCode_SET_MACRO_ERR_NO_SPACE;
    case -EINVAL:
        return zmk_macros_SetMacroErrorCode_SET_MACRO_ERR_INVALID_STEP;
    default:
        return zmk_macros_SetMacroErrorCode_SET_MACRO_ERR_GENERIC;
    }
}

static int proto_macro_to_slot(const zmk_macros_Macro *proto, struct zmk_user_macro_slot *slot) {
    if (!proto || !slot || proto->slot_index >= zmk_user_macro_get_slot_count()) {
        return -ENOENT;
    }
    if (proto->packed_keys.size > zmk_user_macro_get_max_bytes()) {
        return -ENOSPC;
    }

    memset(slot, 0, sizeof(*slot));
    slot->slot_index = proto->slot_index;
    strlcpy(slot->name, proto->name, sizeof(slot->name));
    slot->enabled = proto->enabled;
    slot->step_count = proto->steps_count;
    slot->packed_len = proto->packed_keys.size;
    memcpy(slot->packed_keys, proto->packed_keys.bytes, proto->packed_keys.size);

    for (uint8_t i = 0; i < proto->steps_count; i++) {
        const zmk_macros_MacroStep *proto_step = &proto->steps[i];
        int action = step_action_from_proto(proto_step->action);
        if (action < 0) {
            return action;
        }

        slot->steps[i].action = action;
        slot->steps[i].wait_ms = proto_step->wait_ms;
        slot->steps[i].packed_off = proto_step->packed_keys_offset;
        slot->steps[i].packed_len = proto_step->packed_keys_length;
        if (action == ZMK_USER_MACRO_STEP_WAIT) {
            if (proto_step->has_binding || proto_step->packed_keys_length != 0) {
                return -EINVAL;
            }
            continue;
        }
        if (action == ZMK_USER_MACRO_STEP_TEXT) {
            if (proto_step->has_binding || proto_step->packed_keys_length == 0 ||
                (uint32_t)proto_step->packed_keys_offset + proto_step->packed_keys_length >
                    proto->packed_keys.size) {
                return -EINVAL;
            }
            continue;
        }
        if (!proto_step->has_binding || proto_step->binding.behavior_id < 0 ||
            proto_step->binding.behavior_id > UINT16_MAX || proto_step->packed_keys_length != 0) {
            return -ENODEV;
        }
        slot->steps[i].behavior_local_id = proto_step->binding.behavior_id;
        slot->steps[i].param1 = proto_step->binding.param1;
    }
    return 0;
}

static int fill_macro_ok(uint8_t slot_index, zmk_macros_SetMacroOk *ok) {
    int ret = zmk_user_macro_get_slot(slot_index, &slot_scratch);
    if (ret < 0) {
        return ret;
    }
    ok->has_macro = true;
    fill_proto_macro(&slot_scratch, &ok->macro, true);
    ok->dirty = zmk_user_macro_check_unsaved_changes() > 0;
    notify_macros_unsaved(ok->dirty);
    return 0;
}

static zmk_studio_Response get_macro(const zmk_studio_Request *req) {
    uint32_t slot_index = req->subsystem.macros.request_type.get_macro.slot_index;
    int ret = zmk_user_macro_get_slot(slot_index, &slot_scratch);
    if (ret < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    memset(&get_macro_resp, 0, sizeof(get_macro_resp));
    get_macro_resp.has_macro = true;
    fill_proto_macro(&slot_scratch, &get_macro_resp.macro, true);
    return MACROS_RESPONSE(get_macro, get_macro_resp);
}

static zmk_studio_Response set_macro(const zmk_studio_Request *req) {
    const zmk_macros_SetMacroRequest *set_req = &req->subsystem.macros.request_type.set_macro;
    memset(&set_macro_resp, 0, sizeof(set_macro_resp));

    int ret = proto_macro_to_slot(&set_req->macro, &slot_scratch);
    if (ret >= 0) {
        ret = zmk_user_macro_set_slot(&slot_scratch);
    }
    if (ret < 0) {
        set_macro_resp.which_result = zmk_macros_SetMacroResponse_err_tag;
        set_macro_resp.result.err = map_errno_to_set_macro_err(ret);
        return MACROS_RESPONSE(set_macro, set_macro_resp);
    }
    set_macro_resp.which_result = zmk_macros_SetMacroResponse_ok_tag;
    if (fill_macro_ok(slot_scratch.slot_index, &set_macro_resp.result.ok) < 0) {
        set_macro_resp.which_result = zmk_macros_SetMacroResponse_err_tag;
        set_macro_resp.result.err = zmk_macros_SetMacroErrorCode_SET_MACRO_ERR_GENERIC;
    }
    return MACROS_RESPONSE(set_macro, set_macro_resp);
}

static zmk_studio_Response set_tap_ms(const zmk_studio_Request *req) {
    uint32_t tap_ms = req->subsystem.macros.request_type.set_tap_ms.tap_ms;
    int ret = zmk_user_macro_set_tap_ms(tap_ms);
    if (ret < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }
    zmk_macros_SetTapMsResponse resp = zmk_macros_SetTapMsResponse_init_zero;
    resp.tap_ms = zmk_user_macro_get_tap_ms();
    resp.dirty = zmk_user_macro_check_unsaved_changes() > 0;
    notify_macros_unsaved(resp.dirty);
    return MACROS_RESPONSE(set_tap_ms, resp);
}

static zmk_studio_Response reset_macro(const zmk_studio_Request *req) {
    uint32_t slot_index = req->subsystem.macros.request_type.reset_macro.slot_index;
    memset(&reset_macro_resp, 0, sizeof(reset_macro_resp));
    int ret = zmk_user_macro_reset_slot(slot_index);
    if (ret < 0) {
        reset_macro_resp.which_result = zmk_macros_ResetMacroResponse_err_tag;
        reset_macro_resp.result.err = map_errno_to_set_macro_err(ret);
        return MACROS_RESPONSE(reset_macro, reset_macro_resp);
    }
    reset_macro_resp.which_result = zmk_macros_ResetMacroResponse_ok_tag;
    if (fill_macro_ok(slot_index, &reset_macro_resp.result.ok) < 0) {
        reset_macro_resp.which_result = zmk_macros_ResetMacroResponse_err_tag;
        reset_macro_resp.result.err = zmk_macros_SetMacroErrorCode_SET_MACRO_ERR_GENERIC;
    }
    return MACROS_RESPONSE(reset_macro, reset_macro_resp);
}

static zmk_studio_Response check_unsaved_changes(const zmk_studio_Request *req) {
    ARG_UNUSED(req);
    return MACROS_RESPONSE(check_unsaved_changes,
                           zmk_user_macro_check_unsaved_changes() > 0);
}

static void map_errno_to_save_resp(int err, zmk_macros_SaveChangesResponse *resp) {
    resp->which_result = zmk_macros_SaveChangesResponse_err_tag;
    if (err == -ENOTSUP) {
        resp->result.err =
            zmk_macros_SaveChangesErrorCode_SAVE_CHANGES_ERR_NOT_SUPPORTED;
    } else if (err == -ENOSPC) {
        resp->result.err = zmk_macros_SaveChangesErrorCode_SAVE_CHANGES_ERR_NO_SPACE;
    } else {
        resp->result.err = zmk_macros_SaveChangesErrorCode_SAVE_CHANGES_ERR_GENERIC;
    }
}

static zmk_studio_Response save_changes(const zmk_studio_Request *req) {
    ARG_UNUSED(req);
    zmk_macros_SaveChangesResponse resp = zmk_macros_SaveChangesResponse_init_zero;
    resp.which_result = zmk_macros_SaveChangesResponse_ok_tag;
    resp.result.ok = true;

    int ret = zmk_user_macro_save_changes();
    if (ret < 0) {
        map_errno_to_save_resp(ret, &resp);
        return MACROS_RESPONSE(save_changes, resp);
    }
    notify_macros_unsaved(false);
    return MACROS_RESPONSE(save_changes, resp);
}

static zmk_studio_Response discard_changes(const zmk_studio_Request *req) {
    ARG_UNUSED(req);
    if (zmk_user_macro_discard_changes() < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }
    notify_macros_unsaved(false);
    return MACROS_RESPONSE(discard_changes, true);
}

static int macros_settings_reset(void) {
    return zmk_user_macro_reset_settings();
}

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(macros, macros_settings_reset);

ZMK_RPC_SUBSYSTEM_HANDLER(macros, get_macro_state, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(macros, get_macro, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(macros, set_macro, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(macros, set_tap_ms, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(macros, reset_macro, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(macros, check_unsaved_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(macros, save_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(macros, discard_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);
