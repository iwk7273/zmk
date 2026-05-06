/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <pb_encode.h>

#include <zmk/custom_feature.h>
#include <zmk/keymap.h>
#include <zmk/studio/rpc.h>

#define METEORITE_CONFIG_SCHEMA_VERSION 1
#define METEORITE_CONFIG_FEATURE_VERSION "1.0.0"

ZMK_RPC_SUBSYSTEM(meteorite)

#define METEORITE_RESPONSE(type, ...) ZMK_RPC_RESPONSE(meteorite, type, __VA_ARGS__)
#define METEORITE_NOTIFICATION(type, ...) ZMK_RPC_NOTIFICATION(meteorite, type, __VA_ARGS__)

enum meteorite_option_kind {
    METEORITE_OPTIONS_NONE,
    METEORITE_OPTIONS_TOGGLE,
    METEORITE_OPTIONS_CPI,
    METEORITE_OPTIONS_SCROLL_DIV,
    METEORITE_OPTIONS_ROTATION,
    METEORITE_OPTIONS_LAYERS,
    METEORITE_OPTIONS_OS_MODE,
};

enum meteorite_max_kind {
    METEORITE_MAX_FIXED,
    METEORITE_MAX_CPI,
    METEORITE_MAX_SCROLL_DIV,
    METEORITE_MAX_ROTATION,
    METEORITE_MAX_LAYER,
};

struct meteorite_field_desc {
    const char *id;
    const char *label;
    zmk_meteorite_ConfigFieldKind kind;
    const char *unit;
    int32_t min;
    int32_t max;
    int32_t step;
    enum meteorite_max_kind max_kind;
    bool read_only;
    const char *fixed_reason;
    enum meteorite_option_kind options;
};

static const struct meteorite_field_desc meteorite_fields[] = {
    {
        .id = "cpi_idx",
        .label = "CPI",
        .kind = zmk_meteorite_ConfigFieldKind_CONFIG_FIELD_KIND_INDEXED,
        .unit = "CPI",
        .min = 0,
        .step = 1,
        .max_kind = METEORITE_MAX_CPI,
        .options = METEORITE_OPTIONS_CPI,
    },
    {
        .id = "scroll_div",
        .label = "Scroll divisor",
        .kind = zmk_meteorite_ConfigFieldKind_CONFIG_FIELD_KIND_INDEXED,
        .min = 0,
        .step = 1,
        .max_kind = METEORITE_MAX_SCROLL_DIV,
        .options = METEORITE_OPTIONS_SCROLL_DIV,
    },
    {
        .id = "rotation_idx",
        .label = "Sensor rotation",
        .kind = zmk_meteorite_ConfigFieldKind_CONFIG_FIELD_KIND_INDEXED,
        .unit = "deg",
        .min = 0,
        .step = 1,
        .max_kind = METEORITE_MAX_ROTATION,
        .options = METEORITE_OPTIONS_ROTATION,
    },
    {
        .id = "scroll_h_rev",
        .label = "Horizontal scroll reverse",
        .kind = zmk_meteorite_ConfigFieldKind_CONFIG_FIELD_KIND_TOGGLE,
        .min = 0,
        .max = 1,
        .step = 1,
        .options = METEORITE_OPTIONS_TOGGLE,
    },
    {
        .id = "scroll_v_rev",
        .label = "Vertical scroll reverse",
        .kind = zmk_meteorite_ConfigFieldKind_CONFIG_FIELD_KIND_TOGGLE,
        .min = 0,
        .max = 1,
        .step = 1,
        .options = METEORITE_OPTIONS_TOGGLE,
    },
    {
        .id = "scaling_mode",
        .label = "Pointer scaling",
        .kind = zmk_meteorite_ConfigFieldKind_CONFIG_FIELD_KIND_TOGGLE,
        .min = 0,
        .max = 1,
        .step = 1,
        .options = METEORITE_OPTIONS_TOGGLE,
    },
    {
        .id = "scroll_scaling_mode",
        .label = "Scroll scaling",
        .kind = zmk_meteorite_ConfigFieldKind_CONFIG_FIELD_KIND_TOGGLE,
        .min = 0,
        .max = 1,
        .step = 1,
        .options = METEORITE_OPTIONS_TOGGLE,
    },
    {
        .id = "scroll_layer_1",
        .label = "Scroll layer 1",
        .kind = zmk_meteorite_ConfigFieldKind_CONFIG_FIELD_KIND_ENUM,
        .min = 0,
        .step = 1,
        .max_kind = METEORITE_MAX_LAYER,
        .read_only = true,
        .fixed_reason = "fixed to default scroll layer",
        .options = METEORITE_OPTIONS_LAYERS,
    },
    {
        .id = "scroll_layer_2",
        .label = "Scroll layer 2",
        .kind = zmk_meteorite_ConfigFieldKind_CONFIG_FIELD_KIND_ENUM,
        .min = 0,
        .step = 1,
        .max_kind = METEORITE_MAX_LAYER,
        .options = METEORITE_OPTIONS_LAYERS,
    },
    {
        .id = "os_mode",
        .label = "OS mode",
        .kind = zmk_meteorite_ConfigFieldKind_CONFIG_FIELD_KIND_ENUM,
        .min = 0,
        .max = 1,
        .step = 1,
        .options = METEORITE_OPTIONS_OS_MODE,
    },
};

static bool encode_string(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const char *value = (const char *)*arg;

    if (!value || value[0] == '\0') {
        return true;
    }

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, value, strlen(value));
}

static int32_t count_to_max(uint8_t count) { return count > 0 ? count - 1 : 0; }

static int32_t resolve_field_max(const struct meteorite_field_desc *desc) {
    switch (desc->max_kind) {
    case METEORITE_MAX_CPI:
        return count_to_max(zmk_custom_config_cpi_count());
    case METEORITE_MAX_SCROLL_DIV:
        return count_to_max(zmk_custom_config_scroll_div_count());
    case METEORITE_MAX_ROTATION:
        return count_to_max(zmk_custom_config_rotation_count());
    case METEORITE_MAX_LAYER:
        return count_to_max(zmk_custom_config_layer_count());
    case METEORITE_MAX_FIXED:
    default:
        return desc->max;
    }
}

static bool encode_option(pb_ostream_t *stream, const pb_field_t *field, int32_t value,
                          const char *label, int32_t display_value, const char *display_label) {
    zmk_meteorite_ConfigFieldOption option = zmk_meteorite_ConfigFieldOption_init_zero;

    option.value = value;
    option.display_value = display_value;

    option.label.funcs.encode = encode_string;
    option.label.arg = (void *)label;
    option.display_label.funcs.encode = encode_string;
    option.display_label.arg = (void *)display_label;

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_submessage(stream, &zmk_meteorite_ConfigFieldOption_msg, &option);
}

static bool encode_layer_option(pb_ostream_t *stream, const pb_field_t *field,
                                zmk_keymap_layer_index_t layer_index) {
    zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(layer_index);
    const char *name = NULL;

    if (layer_id != ZMK_KEYMAP_LAYER_ID_INVAL) {
        name = zmk_keymap_layer_name(layer_id);
    }

    char label[CONFIG_ZMK_KEYMAP_LAYER_NAME_MAX_LEN + 16];
    if (name && name[0] != '\0') {
        snprintf(label, sizeof(label), "%s", name);
    } else {
        snprintf(label, sizeof(label), "Layer %u", layer_index);
    }

    return encode_option(stream, field, layer_index, label, layer_index, label);
}

static bool encode_field_options(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const struct meteorite_field_desc *desc = (const struct meteorite_field_desc *)*arg;

    switch (desc->options) {
    case METEORITE_OPTIONS_TOGGLE:
        return encode_option(stream, field, 0, "Off", 0, "Off") &&
               encode_option(stream, field, 1, "On", 1, "On");
    case METEORITE_OPTIONS_CPI:
        for (uint8_t i = 0; i < zmk_custom_config_cpi_count(); i++) {
            uint16_t cpi = (i + 1) * 200;
            char label[16];
            snprintf(label, sizeof(label), "%u CPI", cpi);
            if (!encode_option(stream, field, i, label, cpi, label)) {
                return false;
            }
        }
        return true;
    case METEORITE_OPTIONS_SCROLL_DIV:
        for (uint8_t i = 0; i < zmk_custom_config_scroll_div_count(); i++) {
            uint16_t div = (i + 1) * 5;
            char label[16];
            snprintf(label, sizeof(label), "%u", div);
            if (!encode_option(stream, field, i, label, div, label)) {
                return false;
            }
        }
        return true;
    case METEORITE_OPTIONS_ROTATION:
        for (uint8_t i = 0; i < zmk_custom_config_rotation_count(); i++) {
            int16_t deg = zmk_custom_config_rotation_deg_at(i);
            char label[16];
            snprintf(label, sizeof(label), "%d deg", deg);
            if (!encode_option(stream, field, i, label, deg, label)) {
                return false;
            }
        }
        return true;
    case METEORITE_OPTIONS_LAYERS:
        for (zmk_keymap_layer_index_t i = 0; i < zmk_custom_config_layer_count(); i++) {
            if (!encode_layer_option(stream, field, i)) {
                return false;
            }
        }
        return true;
    case METEORITE_OPTIONS_OS_MODE:
        return encode_option(stream, field, 0, "Windows", 0, "Windows") &&
               encode_option(stream, field, 1, "Mac", 1, "Mac");
    case METEORITE_OPTIONS_NONE:
    default:
        return true;
    }
}

static bool encode_config_fields(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    ARG_UNUSED(arg);

    for (size_t i = 0; i < ARRAY_SIZE(meteorite_fields); i++) {
        const struct meteorite_field_desc *desc = &meteorite_fields[i];
        zmk_meteorite_ConfigField out = zmk_meteorite_ConfigField_init_zero;

        out.id.funcs.encode = encode_string;
        out.id.arg = (void *)desc->id;
        out.label.funcs.encode = encode_string;
        out.label.arg = (void *)desc->label;
        out.unit.funcs.encode = encode_string;
        out.unit.arg = (void *)desc->unit;
        out.fixed_reason.funcs.encode = encode_string;
        out.fixed_reason.arg = (void *)desc->fixed_reason;
        out.options.funcs.encode = encode_field_options;
        out.options.arg = (void *)desc;

        out.kind = desc->kind;
        out.min = desc->min;
        out.max = resolve_field_max(desc);
        out.step = desc->step;
        out.read_only = desc->read_only;

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        if (!pb_encode_submessage(stream, &zmk_meteorite_ConfigField_msg, &out)) {
            return false;
        }
    }

    return true;
}

static zmk_meteorite_ConfigValues config_values_from(const struct zmk_custom_config *cfg) {
    return (zmk_meteorite_ConfigValues){
        .cpi_idx = cfg->cpi_idx,
        .scroll_div = cfg->scroll_div,
        .rotation_idx = cfg->rotation_idx,
        .scroll_h_rev = cfg->scroll_h_rev,
        .scroll_v_rev = cfg->scroll_v_rev,
        .scaling_mode = cfg->scaling_mode,
        .scroll_scaling_mode = cfg->scroll_scaling_mode,
        .scroll_layer_1 = cfg->scroll_layer_1,
        .scroll_layer_2 = cfg->scroll_layer_2,
        .os_mode = cfg->os_mode,
    };
}

static struct zmk_custom_config custom_config_from_values(const zmk_meteorite_ConfigValues *values) {
    return (struct zmk_custom_config){
        .cpi_idx = values->cpi_idx,
        .scroll_div = values->scroll_div,
        .rotation_idx = values->rotation_idx,
        .scroll_h_rev = values->scroll_h_rev,
        .scroll_v_rev = values->scroll_v_rev,
        .scaling_mode = values->scaling_mode,
        .scroll_scaling_mode = values->scroll_scaling_mode,
        .scroll_layer_1 = values->scroll_layer_1,
        .scroll_layer_2 = values->scroll_layer_2,
        .os_mode = values->os_mode,
    };
}

static bool bool_value_is_valid(uint32_t value) { return value <= 1; }

static bool config_values_are_valid(const zmk_meteorite_ConfigValues *values) {
    const struct zmk_custom_config *defaults = zmk_custom_config_defaults_get();

    return values->cpi_idx < zmk_custom_config_cpi_count() &&
           values->scroll_div < zmk_custom_config_scroll_div_count() &&
           values->rotation_idx < zmk_custom_config_rotation_count() &&
           bool_value_is_valid(values->scroll_h_rev) &&
           bool_value_is_valid(values->scroll_v_rev) &&
           bool_value_is_valid(values->scaling_mode) &&
           bool_value_is_valid(values->scroll_scaling_mode) &&
           values->scroll_layer_1 < zmk_custom_config_layer_count() &&
           values->scroll_layer_1 == defaults->scroll_layer_1 &&
           values->scroll_layer_2 < zmk_custom_config_layer_count() &&
           bool_value_is_valid(values->os_mode);
}

static zmk_meteorite_ConfigState config_state_msg(void) {
    zmk_meteorite_ConfigState state = zmk_meteorite_ConfigState_init_zero;

    state.schema_version = METEORITE_CONFIG_SCHEMA_VERSION;
    state.current = config_values_from(zmk_custom_config_get());
    state.saved = config_values_from(zmk_custom_config_saved_get());
    state.defaults = config_values_from(zmk_custom_config_defaults_get());
    state.dirty = zmk_custom_config_check_unsaved_changes();

    state.firmware_feature_version.funcs.encode = encode_string;
    state.firmware_feature_version.arg = (void *)METEORITE_CONFIG_FEATURE_VERSION;
    state.fields.funcs.encode = encode_config_fields;

    return state;
}

static zmk_studio_Response get_config_state(const zmk_studio_Request *req) {
    ARG_UNUSED(req);
    LOG_DBG("");
    return METEORITE_RESPONSE(get_config_state, config_state_msg());
}

static zmk_studio_Response set_config(const zmk_studio_Request *req) {
    LOG_DBG("");
    const zmk_meteorite_SetConfigRequest *set_req =
        &req->subsystem.meteorite.request_type.set_config;

    if (!config_values_are_valid(&set_req->config)) {
        return METEORITE_RESPONSE(
            set_config,
            zmk_meteorite_SetConfigResponse_SET_CONFIG_RESP_ERR_INVALID_VALUE);
    }

    struct zmk_custom_config cfg = custom_config_from_values(&set_req->config);
    int ret = zmk_custom_config_set(&cfg);
    if (ret < 0) {
        return METEORITE_RESPONSE(
            set_config, zmk_meteorite_SetConfigResponse_SET_CONFIG_RESP_ERR_GENERIC);
    }

    return METEORITE_RESPONSE(set_config, zmk_meteorite_SetConfigResponse_SET_CONFIG_RESP_OK);
}

static zmk_studio_Response check_unsaved_changes(const zmk_studio_Request *req) {
    ARG_UNUSED(req);
    LOG_DBG("");
    return METEORITE_RESPONSE(check_unsaved_changes, zmk_custom_config_check_unsaved_changes());
}

static void map_errno_to_save_resp(int err, zmk_meteorite_SaveChangesResponse *resp) {
    resp->which_result = zmk_meteorite_SaveChangesResponse_err_tag;

    switch (err) {
    case -ENOTSUP:
        resp->result.err =
            zmk_meteorite_SaveChangesErrorCode_SAVE_CHANGES_ERR_NOT_SUPPORTED;
        break;
    case -ENOSPC:
        resp->result.err = zmk_meteorite_SaveChangesErrorCode_SAVE_CHANGES_ERR_NO_SPACE;
        break;
    default:
        resp->result.err = zmk_meteorite_SaveChangesErrorCode_SAVE_CHANGES_ERR_GENERIC;
        break;
    }
}

static zmk_studio_Response save_changes(const zmk_studio_Request *req) {
    ARG_UNUSED(req);
    LOG_DBG("");

    zmk_meteorite_SaveChangesResponse resp = zmk_meteorite_SaveChangesResponse_init_zero;
    resp.which_result = zmk_meteorite_SaveChangesResponse_ok_tag;
    resp.result.ok = true;

    int ret = zmk_custom_config_save();
    if (ret < 0) {
        LOG_WRN("Failed to save Meteorite custom config (%d)", ret);
        map_errno_to_save_resp(ret, &resp);
    }

    return METEORITE_RESPONSE(save_changes, resp);
}

static zmk_studio_Response discard_changes(const zmk_studio_Request *req) {
    ARG_UNUSED(req);
    LOG_DBG("");

    int ret = zmk_custom_config_discard();
    if (ret < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    return METEORITE_RESPONSE(discard_changes, true);
}

static int meteorite_settings_reset(void) { return zmk_custom_config_reset_settings(); }

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(meteorite, meteorite_settings_reset);

void zmk_custom_config_changed(const struct zmk_custom_config *cfg) {
    ARG_UNUSED(cfg);

    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
        .notification = METEORITE_NOTIFICATION(config_state_changed, config_state_msg())});

    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
        .notification = METEORITE_NOTIFICATION(unsaved_changes_status_changed,
                                               zmk_custom_config_check_unsaved_changes())});
}

ZMK_RPC_SUBSYSTEM_HANDLER(meteorite, get_config_state, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(meteorite, set_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(meteorite, check_unsaved_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(meteorite, save_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(meteorite, discard_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);
