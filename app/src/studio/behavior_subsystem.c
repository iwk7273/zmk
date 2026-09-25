/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <pb_encode.h>
#include <zmk/studio/rpc.h>
#include <drivers/behavior.h>
#include <zmk/hid.h>
#include "fast_hash.h"

ZMK_RPC_SUBSYSTEM(behaviors)

#define BEHAVIOR_RESPONSE(type, ...) ZMK_RPC_RESPONSE(behaviors, type, __VA_ARGS__)

static bool encode_behavior_summaries(pb_ostream_t *stream, const pb_field_t *field,
                                      void *const *arg) {
    STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, beh) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        if (!pb_encode_varint(stream, beh->local_id)) {
            LOG_ERR("Failed to encode behavior ID");
            return false;
        }
    }

    return true;
}

zmk_studio_Response list_all_behaviors(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_behaviors_ListAllBehaviorsResponse beh_resp =
        zmk_behaviors_ListAllBehaviorsResponse_init_zero;
    beh_resp.behaviors.funcs.encode = encode_behavior_summaries;

    return BEHAVIOR_RESPONSE(list_all_behaviors, beh_resp);
}

struct encode_metadata_sets_state {
    const struct behavior_parameter_metadata_set *sets;
    size_t sets_len;
    size_t i;
};

static bool encode_value_description_name(pb_ostream_t *stream, const pb_field_t *field,
                                          void *const *arg) {
    struct behavior_parameter_value_metadata *state =
        (struct behavior_parameter_value_metadata *)*arg;

    if (!state->display_name) {
        return true;
    }

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, state->display_name, strlen(state->display_name));
}

static bool encode_value_description(pb_ostream_t *stream, const pb_field_t *field,
                                     void *const *arg) {
    struct encode_metadata_sets_state *state = (struct encode_metadata_sets_state *)*arg;

    const struct behavior_parameter_metadata_set *set = &state->sets[state->i];

    bool is_param1 = field->tag == zmk_behaviors_BehaviorBindingParametersSet_param1_tag;
    size_t values_len = is_param1 ? set->param1_values_len : set->param2_values_len;
    const struct behavior_parameter_value_metadata *values =
        is_param1 ? set->param1_values : set->param2_values;

    for (int val_i = 0; val_i < values_len; val_i++) {
        const struct behavior_parameter_value_metadata *val = &values[val_i];

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        zmk_behaviors_BehaviorParameterValueDescription desc =
            zmk_behaviors_BehaviorParameterValueDescription_init_zero;
        desc.name.funcs.encode = encode_value_description_name;
        desc.name.arg = (void *)val;

        switch (val->type) {
        case BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE:
            desc.which_value_type = zmk_behaviors_BehaviorParameterValueDescription_constant_tag;
            desc.value_type.constant = val->value;
            break;
        case BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE:
            desc.which_value_type = zmk_behaviors_BehaviorParameterValueDescription_range_tag;
            desc.value_type.range.min = val->range.min;
            desc.value_type.range.max = val->range.max;
            break;
        case BEHAVIOR_PARAMETER_VALUE_TYPE_NIL:
            desc.which_value_type = zmk_behaviors_BehaviorParameterValueDescription_nil_tag;
            break;
        case BEHAVIOR_PARAMETER_VALUE_TYPE_HID_USAGE:
            desc.which_value_type = zmk_behaviors_BehaviorParameterValueDescription_hid_usage_tag;
            desc.value_type.hid_usage.consumer_max = ZMK_HID_CONSUMER_MAX_USAGE;
            desc.value_type.hid_usage.keyboard_max = ZMK_HID_KEYBOARD_MAX_USAGE;
            break;
        case BEHAVIOR_PARAMETER_VALUE_TYPE_LAYER_ID:
            desc.which_value_type = zmk_behaviors_BehaviorParameterValueDescription_layer_id_tag;
            break;
        default:
            LOG_ERR("Unknown value description type %d", val->type);
            return false;
        }

        if (!pb_encode_submessage(stream, &zmk_behaviors_BehaviorParameterValueDescription_msg,
                                  &desc)) {
            LOG_WRN("Failed to encode submessage for set %d, value %d!", state->i, val_i);
            return false;
        }
    }

    return true;
}

static bool encode_metadata_sets(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    struct encode_metadata_sets_state *state = (struct encode_metadata_sets_state *)*arg;
    bool ret = true;

    LOG_DBG("Encoding the %d metadata sets with %p", state->sets_len, state->sets);

    for (int i = 0; i < state->sets_len; i++) {
        LOG_DBG("Encoding set %d", i);
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        state->i = i;
        zmk_behaviors_BehaviorBindingParametersSet msg =
            zmk_behaviors_BehaviorBindingParametersSet_init_zero;
        msg.param1.funcs.encode = encode_value_description;
        msg.param1.arg = state;
        msg.param2.funcs.encode = encode_value_description;
        msg.param2.arg = state;
        ret = pb_encode_submessage(stream, &zmk_behaviors_BehaviorBindingParametersSet_msg, &msg);
        if (!ret) {
            LOG_WRN("Failed to encode submessage for set %d", i);
            break;
        }
    }

    return ret;
}

static bool encode_behavior_name(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    struct zmk_behavior_ref *zbm = (struct zmk_behavior_ref *)*arg;

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, zbm->metadata.display_name, strlen(zbm->metadata.display_name));
}

static struct encode_metadata_sets_state state = {};

static bool populate_behavior_detail(uint32_t behavior_id,
                                     zmk_behaviors_GetBehaviorDetailsResponse *resp,
                                     struct encode_metadata_sets_state *metadata_state) {
    const char *behavior_name = zmk_behavior_find_behavior_name_from_local_id(behavior_id);
    if (!behavior_name) {
        return false;
    }

    const struct device *device = behavior_get_binding(behavior_name);
    struct zmk_behavior_ref *zbm = NULL;
    STRUCT_SECTION_FOREACH(zmk_behavior_ref, item) {
        if (item->device == device) {
            zbm = item;
            break;
        }
    }
    if (!zbm) {
        return false;
    }

    struct behavior_parameter_metadata desc = {0};
    if (behavior_get_parameter_metadata(device, &desc) < 0) {
        LOG_DBG("No parameter metadata for %s", zbm->metadata.display_name);
    }

    resp->id = behavior_id;
    resp->display_name.funcs.encode = encode_behavior_name;
    resp->display_name.arg = zbm;
    metadata_state->sets = desc.sets;
    metadata_state->sets_len = desc.sets_len;
    resp->metadata.funcs.encode = encode_metadata_sets;
    resp->metadata.arg = metadata_state;
    return true;
}

zmk_studio_Response get_behavior_details(const zmk_studio_Request *req) {
    uint32_t behavior_id = req->subsystem.behaviors.request_type.get_behavior_details.behavior_id;
    zmk_behaviors_GetBehaviorDetailsResponse resp =
        zmk_behaviors_GetBehaviorDetailsResponse_init_zero;
    if (!populate_behavior_detail(behavior_id, &resp, &state)) {
        LOG_WRN("No behavior found for ID %d", behavior_id);
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    return BEHAVIOR_RESPONSE(get_behavior_details, resp);
}

static bool encode_all_behavior_details(pb_ostream_t *stream, const pb_field_t *field,
                                        void *const *arg) {
    STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, beh) {
        zmk_behaviors_GetBehaviorDetailsResponse detail =
            zmk_behaviors_GetBehaviorDetailsResponse_init_zero;
        struct encode_metadata_sets_state metadata_state = {};
        if (!populate_behavior_detail(beh->local_id, &detail, &metadata_state) ||
            !pb_encode_tag_for_field(stream, field) ||
            !pb_encode_submessage(stream, &zmk_behaviors_GetBehaviorDetailsResponse_msg, &detail)) {
            return false;
        }
    }
    return true;
}

bool zmk_studio_behavior_fingerprint(uint64_t *fingerprint) {
    zmk_behaviors_AllBehaviorDetails details = zmk_behaviors_AllBehaviorDetails_init_zero;
    details.behaviors.funcs.encode = encode_all_behavior_details;
    pb_ostream_t stream = studio_fast_hash_stream(fingerprint);
    return pb_encode(&stream, &zmk_behaviors_AllBehaviorDetails_msg, &details);
}

zmk_studio_Response get_all_behavior_details(const zmk_studio_Request *req) {
    zmk_behaviors_AllBehaviorDetails details = zmk_behaviors_AllBehaviorDetails_init_zero;
    details.behaviors.funcs.encode = encode_all_behavior_details;
    return BEHAVIOR_RESPONSE(get_all_behavior_details, details);
}

ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, list_all_behaviors, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, get_behavior_details, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, get_all_behavior_details, ZMK_STUDIO_RPC_HANDLER_SECURED);
