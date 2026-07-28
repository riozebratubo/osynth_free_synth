#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "Wire.h"
#include "ProtocolBase.h"

#include "protocol/d2h.pb.h"
#include "protocol/h2d.pb.h"

namespace SimpleBLE {
namespace Dongl {
namespace Serial {

class Protocol : public ProtocolBase {
  public:
    Protocol(const std::string& device_path);
    ~Protocol();

    basic_WhoamiRsp basic_whoami();
    basic_ResetRsp basic_reset();
    basic_DfuStartRsp basic_dfu_start();

    simpleble_InitRsp simpleble_init();
    simpleble_ScanStartRsp simpleble_scan_start();
    simpleble_ScanStopRsp simpleble_scan_stop();
    simpleble_ConnectRsp simpleble_connect(simpleble_BluetoothAddressType address_type, const std::string& address);
    simpleble_DisconnectRsp simpleble_disconnect(uint16_t conn_handle);
    simpleble_ReadRsp simpleble_read(uint16_t conn_handle, uint16_t handle);
    simpleble_WriteRsp simpleble_write(uint16_t conn_handle, uint16_t handle, simpleble_WriteOperation operation, const std::vector<uint8_t>& data);

    // GAP commands
    sd_GapAddrSetRsp sd_gap_addr_set(bool has_addr, sd_types_BleGapAddr addr);
    sd_GapAddrGetRsp sd_gap_addr_get();
    sd_GapWhitelistSetRsp sd_gap_whitelist_set(pb_size_t wl_addrs_count, sd_types_BleGapAddr wl_addrs[8], uint8_t len);
    sd_GapDeviceIdentitiesSetRsp sd_gap_device_identities_set(pb_size_t id_keys_count, sd_types_BleGapIdKey id_keys[8], pb_size_t local_irks_count, sd_types_BleGapIrk local_irks[8], uint8_t len);
    sd_GapPrivacySetRsp sd_gap_privacy_set(bool has_privacy_params, sd_types_BleGapPrivacyParams privacy_params);
    sd_GapPrivacyGetRsp sd_gap_privacy_get();
    sd_GapAdvSetConfigureRsp sd_gap_adv_set_configure(uint8_t adv_handle, bool has_adv_data, sd_types_BleGapAdvData adv_data, bool has_adv_params, sd_types_BleGapAdvParams adv_params);
    sd_GapAdvStartRsp sd_gap_adv_start(uint8_t adv_handle, uint8_t conn_cfg_tag);
    sd_GapAdvStopRsp sd_gap_adv_stop(uint8_t adv_handle);
    sd_GapConnParamUpdateRsp sd_gap_conn_param_update(uint16_t conn_handle, bool has_conn_params, sd_types_BleGapConnParams conn_params);
    sd_GapDisconnectRsp sd_gap_disconnect(uint16_t conn_handle, uint8_t hci_ret_code_code);
    sd_GapTxPowerSetRsp sd_gap_tx_power_set(uint8_t role, uint16_t handle, int8_t tx_power);
    sd_GapAppearanceSetRsp sd_gap_appearance_set(uint16_t appearance);
    sd_GapAppearanceGetRsp sd_gap_appearance_get();
    sd_GapPpcpSetRsp sd_gap_ppcp_set(bool has_conn_params, sd_types_BleGapConnParams conn_params);
    sd_GapPpcpGetRsp sd_gap_ppcp_get();
    sd_GapDeviceNameSetRsp sd_gap_device_name_set(bool has_write_perm, sd_types_BleGapConnSecMode write_perm, sd_GapDeviceNameSetCmd_dev_name_t dev_name, uint16_t len);
    sd_GapDeviceNameGetRsp sd_gap_device_name_get();
    sd_GapAuthenticateRsp sd_gap_authenticate(uint16_t conn_handle, bool has_sec_params, sd_types_BleGapSecParams sec_params);
    sd_GapSecParamsReplyRsp sd_gap_sec_params_reply(uint16_t conn_handle, sd_types_BleGapSecStatus sec_ret_code, bool has_sec_params, sd_types_BleGapSecParams sec_params, bool has_sec_keyset, sd_types_BleGapSecKeyset sec_keyset);
    sd_GapAuthKeyReplyRsp sd_gap_auth_key_reply(uint16_t conn_handle, sd_types_BleGapAuthKeyType key_type, sd_GapAuthKeyReplyCmd_key_t key);
    sd_GapLescDhkeyReplyRsp sd_gap_lesc_dhkey_reply(uint16_t conn_handle, bool has_dhkey, sd_types_BleGapLescDhkey dhkey);
    sd_GapKeypressNotifyRsp sd_gap_keypress_notify(uint16_t conn_handle, sd_types_BleGapKpNotType kp_not);
    sd_GapLescOobDataGetRsp sd_gap_lesc_oob_data_get(uint16_t conn_handle, bool has_pk_own, sd_types_BleGapLescP256Pk pk_own);
    sd_GapLescOobDataSetRsp sd_gap_lesc_oob_data_set(uint16_t conn_handle, bool has_oobd_own, sd_types_BleGapLescOobData oobd_own, bool has_oobd_peer, sd_types_BleGapLescOobData oobd_peer);
    sd_GapEncryptRsp sd_gap_encrypt(uint16_t conn_handle, bool has_master_id, sd_types_BleGapMasterId master_id, bool has_enc_info, sd_types_BleGapEncInfo enc_info);
    sd_GapSecInfoReplyRsp sd_gap_sec_info_reply(uint16_t conn_handle, bool has_enc_info, sd_types_BleGapEncInfo enc_info, bool has_id_info, sd_types_BleGapIrk id_info, bool has_sign_info, sd_types_BleGapSignInfo sign_info);
    sd_GapConnSecGetRsp sd_gap_conn_sec_get(uint16_t conn_handle);
    sd_GapRssiStartRsp sd_gap_rssi_start(uint16_t conn_handle, uint8_t threshold_dbm, uint8_t skip_count);
    sd_GapRssiStopRsp sd_gap_rssi_stop(uint16_t conn_handle);
    sd_GapScanStartRsp sd_gap_scan_start(sd_types_BleGapScanParams scan_params);
    sd_GapScanStopRsp sd_gap_scan_stop();
    sd_GapConnectRsp sd_gap_connect(bool has_peer_addr, sd_types_BleGapAddr peer_addr, bool has_scan_params, sd_types_BleGapScanParams scan_params, bool has_conn_params, sd_types_BleGapConnParams conn_params, uint8_t conn_cfg_tag);
    sd_GapConnectCancelRsp sd_gap_connect_cancel();
    sd_GapRssiGetRsp sd_gap_rssi_get(uint16_t conn_handle);
    sd_GapPhyUpdateRsp sd_gap_phy_update(uint16_t conn_handle, bool has_phys, sd_BleGapPhysCmd phys);
    sd_GapDataLengthUpdateRsp sd_gap_data_length_update(uint16_t conn_handle, bool has_dl_params, sd_types_BleGapDataLengthParams dl_params, bool has_dl_limitation, sd_types_BleGapDataLengthLimitation dl_limitation);
    sd_GapQosChannelSurveyStartRsp sd_gap_qos_channel_survey_start(uint32_t interval_us);
    sd_GapQosChannelSurveyStopRsp sd_gap_qos_channel_survey_stop();
    sd_GapAdvAddrGetRsp sd_gap_adv_addr_get(uint8_t adv_handle);
    sd_GapNextConnEvtCounterGetRsp sd_gap_next_conn_evt_counter_get(uint16_t conn_handle);
    sd_GapConnEvtTriggerStartRsp sd_gap_conn_evt_trigger_start(uint16_t conn_handle, bool has_params, sd_types_BleGapConnEventTrigger params);
    sd_GapConnEvtTriggerStopRsp sd_gap_conn_evt_trigger_stop(uint16_t conn_handle);

    // GATTC commands
    sd_GattcPrimaryServicesDiscoverRsp sd_gattc_primary_services_discover(uint16_t conn_handle, uint16_t start_handle, bool has_srvc_uuid, sd_types_BleUuid srvc_uuid);
    sd_GattcRelationshipsDiscoverRsp sd_gattc_relationships_discover(uint16_t conn_handle, bool has_handle_range, sd_types_BleGattcHandleRange handle_range);
    sd_GattcCharacteristicsDiscoverRsp sd_gattc_characteristics_discover(uint16_t conn_handle, bool has_handle_range, sd_types_BleGattcHandleRange handle_range);
    sd_GattcDescriptorsDiscoverRsp sd_gattc_descriptors_discover(uint16_t conn_handle, bool has_handle_range, sd_types_BleGattcHandleRange handle_range);
    sd_GattcAttrInfoDiscoverRsp sd_gattc_attr_info_discover(uint16_t conn_handle, bool has_handle_range, sd_types_BleGattcHandleRange handle_range);
    sd_GattcCharValueByUuidReadRsp sd_gattc_char_value_by_uuid_read(uint16_t conn_handle, bool has_uuid, sd_types_BleUuid uuid, bool has_handle_range, sd_types_BleGattcHandleRange handle_range);
    sd_GattcReadRsp sd_gattc_read(uint16_t conn_handle, uint16_t handle, uint16_t offset);
    sd_GattcCharValuesReadRsp sd_gattc_char_values_read(uint16_t conn_handle, pb_callback_t handles, uint8_t handle_count);
    sd_GattcWriteRsp sd_gattc_write(uint16_t conn_handle, bool has_write_params, sd_types_BleGattcWriteParams write_params);
    sd_GattcHvConfirmRsp sd_gattc_hv_confirm(uint16_t conn_handle, uint16_t handle);
    sd_GattcExchangeMtuRequestRsp sd_gattc_exchange_mtu_request(uint16_t conn_handle, uint16_t client_rx_mtu);

    // GATTS commands
    sd_GattsServiceAddRsp sd_gatts_service_add(uint8_t type, bool has_uuid, sd_types_BleUuid uuid);
    sd_GattsIncludeAddRsp sd_gatts_include_add(uint16_t service_handle, uint16_t inc_srvc_handle);
    sd_GattsCharacteristicAddRsp sd_gatts_characteristic_add(uint16_t service_handle, bool has_char_md, sd_types_BleGattsCharMd char_md, bool has_attr_char_value, sd_types_BleGattsAttr attr_char_value);
    sd_GattsDescriptorAddRsp sd_gatts_descriptor_add(uint16_t char_handle, bool has_attr, sd_types_BleGattsAttr attr);
    sd_GattsValueSetRsp sd_gatts_value_set(uint16_t conn_handle, uint16_t handle, bool has_value, sd_types_BleGattsValue value);
    sd_GattsValueGetRsp sd_gatts_value_get(uint16_t conn_handle, uint16_t handle);
    sd_GattsHvxRsp sd_gatts_hvx(uint16_t conn_handle, bool has_hvx_params, sd_types_BleGattsHvxParams hvx_params);
    sd_GattsServiceChangedRsp sd_gatts_service_changed(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle);
    sd_GattsRwAuthorizeReplyRsp sd_gatts_rw_authorize_reply(uint16_t conn_handle, bool has_rw_authorize_reply_params, sd_types_BleGattsRwAuthorizeReplyParams rw_authorize_reply_params);
    sd_GattsSysAttrSetRsp sd_gatts_sys_attr_set(uint16_t conn_handle, sd_GattsSysAttrSetCmd_sys_attr_data_t sys_attr_data, uint16_t len, uint32_t flags);
    sd_GattsSysAttrGetRsp sd_gatts_sys_attr_get(uint16_t conn_handle, uint32_t flags);
    sd_GattsInitialUserHandleGetRsp sd_gatts_initial_user_handle_get();
    sd_GattsAttrGetRsp sd_gatts_attr_get(uint16_t handle);
    sd_GattsExchangeMtuReplyRsp sd_gatts_exchange_mtu_reply(uint16_t conn_handle, uint16_t server_rx_mtu);

};

}  // namespace Serial
}  // namespace Dongl
}  // namespace SimpleBLE
