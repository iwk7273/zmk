/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/sys/util.h>

#include <pb_encode.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/stdlib.h>
#include <zmk/studio/rpc.h>

ZMK_RPC_SUBSYSTEM(ble_profiles)

#define BLE_PROFILES_RESPONSE(type, ...) ZMK_RPC_RESPONSE(ble_profiles, type, __VA_ARGS__)

BUILD_ASSERT(sizeof(((zmk_ble_profiles_Profile *)0)->address) >= BT_ADDR_LE_STR_LEN);

static bool encode_profiles(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    ARG_UNUSED(arg);

    for (uint8_t index = 0; index < ZMK_BLE_PROFILE_COUNT; index++) {
        zmk_ble_profiles_Profile profile = zmk_ble_profiles_Profile_init_zero;
        profile.index = index;
        strlcpy(profile.name, zmk_ble_profile_name(index), sizeof(profile.name));
        strlcpy(profile.host_label, zmk_ble_host_label(index), sizeof(profile.host_label));
        profile.open = zmk_ble_profile_is_open(index);
        if (!profile.open) {
            bt_addr_le_to_str(zmk_ble_profile_address(index), profile.address,
                              sizeof(profile.address));
        }
        profile.connected = zmk_ble_profile_is_connected(index);
        profile.active = index == zmk_ble_active_profile_index();

        if (!pb_encode_tag_for_field(stream, field) ||
            !pb_encode_submessage(stream, &zmk_ble_profiles_Profile_msg, &profile)) {
            return false;
        }
    }
    return true;
}

static zmk_studio_Response get_profiles(const zmk_studio_Request *req) {
    ARG_UNUSED(req);
    zmk_ble_profiles_GetProfilesResponse response = zmk_ble_profiles_GetProfilesResponse_init_zero;
    response.slot_count = ZMK_BLE_PROFILE_COUNT;
    response.profiles.funcs.encode = encode_profiles;
    response.active_index = zmk_ble_active_profile_index();
    response.preferred_transport = (zmk_ble_profiles_Transport)zmk_endpoint_get_preferred_transport();
    response.selected_transport = (zmk_ble_profiles_Transport)zmk_endpoint_get_selected().transport;
    return BLE_PROFILES_RESPONSE(get_profiles, response);
}

static zmk_ble_profiles_MutationError map_profile_error(int err) {
    switch (err) {
    case 0:
        return zmk_ble_profiles_MutationError_MUTATION_OK;
    case -ERANGE:
        return zmk_ble_profiles_MutationError_MUTATION_INVALID_INDEX;
    case -EINVAL:
        return zmk_ble_profiles_MutationError_MUTATION_INVALID_NAME;
    case -ENOSPC:
        return zmk_ble_profiles_MutationError_MUTATION_SAVE_FAILED;
    default:
        return zmk_ble_profiles_MutationError_MUTATION_APPLY_FAILED;
    }
}

static zmk_studio_Response set_name(const zmk_studio_Request *req) {
    const zmk_ble_profiles_SetNameRequest *input =
        &req->subsystem.ble_profiles.request_type.set_name;
    zmk_ble_profiles_MutationResponse response = zmk_ble_profiles_MutationResponse_init_zero;
    if (input->index >= ZMK_BLE_PROFILE_COUNT) {
        response.error = zmk_ble_profiles_MutationError_MUTATION_INVALID_INDEX;
    } else if (input->name_utf8.size == 0 || input->name_utf8.size > CONFIG_BT_DEVICE_NAME_MAX ||
               memchr(input->name_utf8.bytes, '\0', input->name_utf8.size) != NULL) {
        response.error = zmk_ble_profiles_MutationError_MUTATION_INVALID_NAME;
    } else {
        char name[CONFIG_BT_DEVICE_NAME_MAX + 1];
        memcpy(name, input->name_utf8.bytes, input->name_utf8.size);
        name[input->name_utf8.size] = '\0';
        response.error = map_profile_error(zmk_ble_set_profile_name(input->index, name));
    }
    return BLE_PROFILES_RESPONSE(set_name, response);
}

static zmk_studio_Response set_host_label(const zmk_studio_Request *req) {
    const zmk_ble_profiles_SetHostLabelRequest *input =
        &req->subsystem.ble_profiles.request_type.set_host_label;
    zmk_ble_profiles_MutationResponse response = zmk_ble_profiles_MutationResponse_init_zero;
    if (input->index >= ZMK_BLE_PROFILE_COUNT) {
        response.error = zmk_ble_profiles_MutationError_MUTATION_INVALID_INDEX;
    } else if (input->host_label_utf8.size > ZMK_BLE_HOST_LABEL_MAX_LENGTH ||
               memchr(input->host_label_utf8.bytes, '\0', input->host_label_utf8.size) != NULL) {
        response.error = zmk_ble_profiles_MutationError_MUTATION_INVALID_HOST_LABEL;
    } else {
        char label[ZMK_BLE_HOST_LABEL_MAX_LENGTH + 1];
        memcpy(label, input->host_label_utf8.bytes, input->host_label_utf8.size);
        label[input->host_label_utf8.size] = '\0';
        int err = zmk_ble_set_host_label(input->index, label);
        response.error = err == -EINVAL
                             ? zmk_ble_profiles_MutationError_MUTATION_INVALID_HOST_LABEL
                             : map_profile_error(err);
    }
    return BLE_PROFILES_RESPONSE(set_host_label, response);
}

static zmk_studio_Response select_profile(const zmk_studio_Request *req) {
    uint32_t index = req->subsystem.ble_profiles.request_type.select_profile.index;
    zmk_ble_profiles_MutationResponse response = zmk_ble_profiles_MutationResponse_init_zero;
    response.error = index >= ZMK_BLE_PROFILE_COUNT
                         ? zmk_ble_profiles_MutationError_MUTATION_INVALID_INDEX
                         : map_profile_error(zmk_ble_prof_select(index));
    return BLE_PROFILES_RESPONSE(select_profile, response);
}

static zmk_studio_Response unpair_profile(const zmk_studio_Request *req) {
    uint32_t index = req->subsystem.ble_profiles.request_type.unpair_profile.index;
    zmk_ble_profiles_MutationResponse response = zmk_ble_profiles_MutationResponse_init_zero;
    response.error = index >= ZMK_BLE_PROFILE_COUNT
                         ? zmk_ble_profiles_MutationError_MUTATION_INVALID_INDEX
                         : map_profile_error(zmk_ble_unpair_profile(index));
    return BLE_PROFILES_RESPONSE(unpair_profile, response);
}

static zmk_studio_Response set_preferred_transport(const zmk_studio_Request *req) {
    zmk_ble_profiles_Transport transport =
        req->subsystem.ble_profiles.request_type.set_preferred_transport.transport;
    zmk_ble_profiles_MutationResponse response = zmk_ble_profiles_MutationResponse_init_zero;
    if (transport != zmk_ble_profiles_Transport_TRANSPORT_USB &&
        transport != zmk_ble_profiles_Transport_TRANSPORT_BLE) {
        response.error = zmk_ble_profiles_MutationError_MUTATION_INVALID_TRANSPORT;
    } else {
        int err = zmk_endpoint_set_preferred_transport((enum zmk_transport)transport);
        response.error = err ? zmk_ble_profiles_MutationError_MUTATION_APPLY_FAILED
                             : zmk_ble_profiles_MutationError_MUTATION_OK;
    }
    return BLE_PROFILES_RESPONSE(set_preferred_transport, response);
}

ZMK_RPC_SUBSYSTEM_HANDLER(ble_profiles, get_profiles, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(ble_profiles, set_name, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(ble_profiles, set_host_label, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(ble_profiles, select_profile, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(ble_profiles, unpair_profile, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(ble_profiles, set_preferred_transport, ZMK_STUDIO_RPC_HANDLER_SECURED);
