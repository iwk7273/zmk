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
#define MACROS_NOTIFICATION(type, ...) ZMK_RPC_NOTIFICATION(macros, type, __VA_ARGS__)

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
    default:
        return -EINVAL;
    }
}

static void fill_proto_macro(const struct zmk_user_macro_slot *slot, zmk_macros_Macro *proto) {
    proto->slot_index = slot->slot_index;
    proto->behavior_id =
        slot->behavior_local_id == UINT16_MAX ? -1 : (int32_t)slot->behavior_local_id;
    strlcpy(proto->name, slot->name, sizeof(proto->name));
    proto->enabled = slot->enabled;
    proto->steps_count = slot->step_count;
    for (uint8_t i = 0; i < slot->step_count; i++) {
        zmk_macros_MacroStep *step = &proto->steps[i];
        step->action = step_action_to_proto(slot->steps[i].action);
        if (slot->steps[i].action != ZMK_USER_MACRO_STEP_WAIT) {
            step->has_binding = true;
            step->binding.behavior_id = slot->steps[i].behavior_local_id;
            step->binding.param1 = slot->steps[i].param1;
            step->binding.param2 = slot->steps[i].param2;
        }
        step->wait_ms = slot->steps[i].wait_ms;
    }
    proto->dirty = slot->dirty;
}

static bool encode_macros(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    ARG_UNUSED(arg);

    for (uint8_t i = 0; i < zmk_user_macro_get_slot_count(); i++) {
        struct zmk_user_macro_slot slot;
        int ret = zmk_user_macro_get_slot(i, &slot);
        if (ret < 0) {
            LOG_WRN("Failed to get user macro slot %d (%d)", i, ret);
            continue;
        }

        zmk_macros_Macro macro = zmk_macros_Macro_init_zero;
        fill_proto_macro(&slot, &macro);
        if (!pb_encode_tag_for_field(stream, field) ||
            !pb_encode_submessage(stream, &zmk_macros_Macro_msg, &macro)) {
            return false;
        }
    }
    return true;
}

static zmk_macros_MacroState macro_state_msg(void) {
    zmk_macros_MacroState state = zmk_macros_MacroState_init_zero;
    state.schema_version = 1;
    state.max_macros = zmk_user_macro_get_slot_count();
    state.max_steps_per_macro = zmk_user_macro_get_max_steps();
    state.macros.funcs.encode = encode_macros;
    state.dirty = zmk_user_macro_check_unsaved_changes() > 0;
    return state;
}

static zmk_studio_Response get_macro_state(const zmk_studio_Request *req) {
    ARG_UNUSED(req);
    return MACROS_RESPONSE(get_macro_state, macro_state_msg());
}

static int proto_macro_to_slot(const zmk_macros_Macro *proto,
                               struct zmk_user_macro_slot *slot) {
    if (!proto || !slot || proto->slot_index >= zmk_user_macro_get_slot_count()) {
        return -ENOENT;
    }

    memset(slot, 0, sizeof(*slot));
    slot->slot_index = proto->slot_index;
    strlcpy(slot->name, proto->name, sizeof(slot->name));
    slot->enabled = proto->enabled;
    slot->step_count = proto->steps_count;

    for (uint8_t i = 0; i < proto->steps_count; i++) {
        const zmk_macros_MacroStep *proto_step = &proto->steps[i];
        int action = step_action_from_proto(proto_step->action);
        if (action < 0) {
            return action;
        }

        slot->steps[i].action = action;
        slot->steps[i].wait_ms = proto_step->wait_ms;
        if (action == ZMK_USER_MACRO_STEP_WAIT) {
            if (proto_step->has_binding) {
                return -EINVAL;
            }
            continue;
        }
        if (!proto_step->has_binding || proto_step->binding.behavior_id < 0 ||
            proto_step->binding.behavior_id > UINT16_MAX) {
            return -ENODEV;
        }
        slot->steps[i].behavior_local_id = proto_step->binding.behavior_id;
        slot->steps[i].param1 = proto_step->binding.param1;
        slot->steps[i].param2 = proto_step->binding.param2;
    }
    return 0;
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
    case -EINVAL:
        return zmk_macros_SetMacroErrorCode_SET_MACRO_ERR_INVALID_STEP;
    default:
        return zmk_macros_SetMacroErrorCode_SET_MACRO_ERR_GENERIC;
    }
}

static zmk_studio_Response set_macro(const zmk_studio_Request *req) {
    const zmk_macros_SetMacroRequest *set_req = &req->subsystem.macros.request_type.set_macro;
    zmk_macros_SetMacroResponse resp = zmk_macros_SetMacroResponse_init_zero;
    struct zmk_user_macro_slot slot;

    int ret = proto_macro_to_slot(&set_req->macro, &slot);
    if (ret >= 0) {
        ret = zmk_user_macro_set_slot(&slot);
    }
    if (ret < 0) {
        resp.which_result = zmk_macros_SetMacroResponse_err_tag;
        resp.result.err = map_errno_to_set_macro_err(ret);
        return MACROS_RESPONSE(set_macro, resp);
    }

    struct zmk_user_macro_slot updated;
    ret = zmk_user_macro_get_slot(slot.slot_index, &updated);
    if (ret < 0) {
        resp.which_result = zmk_macros_SetMacroResponse_err_tag;
        resp.result.err = zmk_macros_SetMacroErrorCode_SET_MACRO_ERR_GENERIC;
        return MACROS_RESPONSE(set_macro, resp);
    }

    resp.which_result = zmk_macros_SetMacroResponse_ok_tag;
    resp.result.ok.has_macro = true;
    fill_proto_macro(&updated, &resp.result.ok.macro);
    resp.result.ok.dirty = zmk_user_macro_check_unsaved_changes() > 0;
    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
        .notification =
            MACROS_NOTIFICATION(unsaved_changes_status_changed, resp.result.ok.dirty)});
    return MACROS_RESPONSE(set_macro, resp);
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
    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
        .notification = MACROS_NOTIFICATION(unsaved_changes_status_changed, false)});
    return MACROS_RESPONSE(save_changes, resp);
}

static zmk_studio_Response discard_changes(const zmk_studio_Request *req) {
    ARG_UNUSED(req);
    if (zmk_user_macro_discard_changes() < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }
    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
        .notification = MACROS_NOTIFICATION(unsaved_changes_status_changed, false)});
    return MACROS_RESPONSE(discard_changes, true);
}

static int macros_settings_reset(void) {
    return zmk_user_macro_reset_settings();
}

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(macros, macros_settings_reset);

ZMK_RPC_SUBSYSTEM_HANDLER(macros, get_macro_state, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(macros, set_macro, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(macros, check_unsaved_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(macros, save_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(macros, discard_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);
