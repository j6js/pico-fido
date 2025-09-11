/*
 * This file is part of the Pico FIDO distribution (https://github.com/polhenarejos/pico-fido).
 * Copyright (c) 2022 Pol Henarejos.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ctap2_cbor.h"
#include "fido.h"
#include "ctap.h"
#include "hid/ctap_hid.h"
#include "files.h"
#include "apdu.h"
#include "credential.h"
#include "pico_keys.h"
#include "random.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/chachapoly.h"
#include "mbedtls/sha256.h"
#include "file.h"

extern uint8_t keydev_dec[32];
extern bool has_keydev_dec;
extern void resetPersistentPinUvAuthToken();
extern void resetPinUvAuthToken();

int cbor_config(const uint8_t *data, size_t len) {
    CborParser parser;
    CborValue map;
    CborError error = CborNoError;
    uint64_t subcommand = 0, pinUvAuthProtocol = 0, vendorCommandId = 0, newMinPinLength = 0, vendorParamInt = 0;
    CborByteString pinUvAuthParam = { 0 }, vendorParamByteString = { 0 };
    CborCharString minPinLengthRPIDs[32] = { 0 };
    size_t resp_size = 0, raw_subpara_len = 0, minPinLengthRPIDs_len = 0;
    CborEncoder encoder;
    //CborEncoder mapEncoder;
    uint8_t *raw_subpara = NULL;
    const bool *forceChangePin = NULL;

    CBOR_CHECK(cbor_parser_init(data, len, 0, &parser, &map));
    uint64_t val_c = 1;
    CBOR_PARSE_MAP_START(map, 1)
    {
        uint64_t val_u = 0;
        CBOR_FIELD_GET_UINT(val_u, 1);
        if (val_c <= 1 && val_c != val_u) {
            CBOR_ERROR(CTAP2_ERR_MISSING_PARAMETER);
        }
        if (val_u < val_c) {
            CBOR_ERROR(CTAP2_ERR_INVALID_CBOR);
        }
        val_c = val_u + 1;
        if (val_u == 0x01) {
            CBOR_FIELD_GET_UINT(subcommand, 1);
        }
        else if (val_u == 0x02) {
            uint64_t subpara = 0;
            raw_subpara = (uint8_t *) cbor_value_get_next_byte(&_f1);
            CBOR_PARSE_MAP_START(_f1, 2)
            {
                if (subcommand == 0xFF) { // Vendor
                    CBOR_FIELD_GET_UINT(subpara, 2);
                    if (subpara == 0x01) {
                        CBOR_FIELD_GET_UINT(vendorCommandId, 2);
                    }
                    else if (subpara == 0x02) {
                        CBOR_FIELD_GET_BYTES(vendorParamByteString, 2);
                    }
                    else if (subpara == 0x03) {
                        CBOR_FIELD_GET_UINT(vendorParamInt, 2);
                    }
                }
                else if (subcommand == 0x03) { // Extensions
                    CBOR_FIELD_GET_UINT(subpara, 2);
                    if (subpara == 0x01) {
                        CBOR_FIELD_GET_UINT(newMinPinLength, 2);
                    }
                    else if (subpara == 0x02) {
                        CBOR_PARSE_ARRAY_START(_f2, 3)
                        {
                            CBOR_FIELD_GET_TEXT(minPinLengthRPIDs[minPinLengthRPIDs_len], 3);
                            minPinLengthRPIDs_len++;
                            if (minPinLengthRPIDs_len >= 32) {
                                CBOR_ERROR(CTAP2_ERR_KEY_STORE_FULL);
                            }
                        }
                        CBOR_PARSE_ARRAY_END(_f2, 3);
                    }
                    else if (subpara == 0x03) {
                        CBOR_FIELD_GET_BOOL(forceChangePin, 2);
                    }
                }
            }
            CBOR_PARSE_MAP_END(_f1, 2);
            raw_subpara_len = cbor_value_get_next_byte(&_f1) - raw_subpara;
        }
        else if (val_u == 0x03) {
            CBOR_FIELD_GET_UINT(pinUvAuthProtocol, 1);
        }
        else if (val_u == 0x04) {
            CBOR_FIELD_GET_BYTES(pinUvAuthParam, 1);
        }
    }
    CBOR_PARSE_MAP_END(map, 1);

    cbor_encoder_init(&encoder, ctap_resp->init.data + 1, CTAP_MAX_CBOR_PAYLOAD, 0);

    if (pinUvAuthParam.present == false) {
        CBOR_ERROR(CTAP2_ERR_PUAT_REQUIRED);
    }
    if (pinUvAuthProtocol == 0) {
        CBOR_ERROR(CTAP2_ERR_MISSING_PARAMETER);
    }
    if (pinUvAuthProtocol != 1 && pinUvAuthProtocol != 2) {
        CBOR_ERROR(CTAP1_ERR_INVALID_PARAMETER);
    }

    uint8_t *verify_payload = (uint8_t *) calloc(1, 32 + 1 + 1 + raw_subpara_len);
    memset(verify_payload, 0xff, 32);
    verify_payload[32] = 0x0d;
    verify_payload[33] = (uint8_t)subcommand;
    memcpy(verify_payload + 34, raw_subpara, raw_subpara_len);
    error = verify((uint8_t)pinUvAuthProtocol, paut.data, verify_payload, (uint16_t)(32 + 1 + 1 + raw_subpara_len), pinUvAuthParam.data);
    free(verify_payload);
    if (error != CborNoError) {
        CBOR_ERROR(CTAP2_ERR_PIN_AUTH_INVALID);
    }

    if (!(paut.permissions & CTAP_PERMISSION_ACFG)) {
        CBOR_ERROR(CTAP2_ERR_PIN_AUTH_INVALID);
    }

    if (subcommand == 0xFF) {
#ifndef ENABLE_EMULATION
        const bool is_phy = (vendorCommandId == CTAP_CONFIG_PHY_VIDPID ||
            vendorCommandId == CTAP_CONFIG_PHY_LED_GPIO ||
            vendorCommandId == CTAP_CONFIG_PHY_LED_BTNESS ||
            vendorCommandId == CTAP_CONFIG_PHY_OPTS);
#endif
        if (vendorCommandId == CTAP_CONFIG_AUT_DISABLE){
            if (!file_has_data(ef_keydev_enc)) {
                CBOR_ERROR(CTAP2_ERR_NOT_ALLOWED);
            }
            if (has_keydev_dec == false) {
                CBOR_ERROR(CTAP2_ERR_PIN_AUTH_INVALID);
            }
            file_put_data(ef_keydev, keydev_dec, sizeof(keydev_dec));
            mbedtls_platform_zeroize(keydev_dec, sizeof(keydev_dec));
            file_put_data(ef_keydev_enc, NULL, 0); // Set ef to 0 bytes
            low_flash_available();
        }
        else if (vendorCommandId == CTAP_CONFIG_AUT_ENABLE) {
            if (!file_has_data(ef_keydev)) {
                CBOR_ERROR(CTAP2_ERR_NOT_ALLOWED);
            }
            if (mse.init == false) {
                CBOR_ERROR(CTAP2_ERR_NOT_ALLOWED);
            }

            mbedtls_chachapoly_context chatx;
            int ret = mse_decrypt_ct(vendorParamByteString.data, vendorParamByteString.len);
            if (ret != 0) {
                CBOR_ERROR(CTAP1_ERR_INVALID_PARAMETER);
            }

            uint8_t key_dev_enc[12 + 32 + 16];
            random_gen(NULL, key_dev_enc, 12);
            mbedtls_chachapoly_init(&chatx);
            mbedtls_chachapoly_setkey(&chatx, vendorParamByteString.data);
            ret = mbedtls_chachapoly_encrypt_and_tag(&chatx, file_get_size(ef_keydev), key_dev_enc, NULL, 0, file_get_data(ef_keydev), key_dev_enc + 12, key_dev_enc + 12 + file_get_size(ef_keydev));
            mbedtls_chachapoly_free(&chatx);
            if (ret != 0) {
                CBOR_ERROR(CTAP1_ERR_INVALID_PARAMETER);
            }

            file_put_data(ef_keydev_enc, key_dev_enc, sizeof(key_dev_enc));
            mbedtls_platform_zeroize(key_dev_enc, sizeof(key_dev_enc));
            file_put_data(ef_keydev, key_dev_enc, file_get_size(ef_keydev)); // Overwrite ef with 0
            file_put_data(ef_keydev, NULL, 0); // Set ef to 0 bytes
            low_flash_available();
        }

#ifndef ENABLE_EMULATION
        else if (vendorCommandId == CTAP_CONFIG_PHY_VIDPID) {
            phy_data.vid = (vendorParamInt >> 16) & 0xFFFF;
            phy_data.pid = vendorParamInt & 0xFFFF;
            phy_data.vidpid_present = true;
        }
        else if (vendorCommandId == CTAP_CONFIG_PHY_LED_GPIO) {
            phy_data.led_gpio = (uint8_t)vendorParamInt;
            phy_data.led_gpio_present = true;
        }
        else if (vendorCommandId == CTAP_CONFIG_PHY_LED_BTNESS) {
            phy_data.led_brightness = (uint8_t)vendorParamInt;
            phy_data.led_brightness_present = true;
        }
        else if (vendorCommandId == CTAP_CONFIG_PHY_OPTS) {
            phy_data.opts = (uint16_t)vendorParamInt;
        }
        else {
            CBOR_ERROR(CTAP2_ERR_UNSUPPORTED_OPTION);
        }
        if (is_phy && phy_save() != PICOKEY_OK) {
            CBOR_ERROR(CTAP2_ERR_PROCESSING);
        }
#endif
        else if (vendorCommandId == CTAP_CONFIG_EA_UPLOAD) {
            if (vendorParamByteString.present == false) {
                CBOR_ERROR(CTAP2_ERR_MISSING_PARAMETER);
            }
            file_t *ef_ee_ea = search_by_fid(EF_EE_DEV_EA, NULL, SPECIFY_EF);
            if (ef_ee_ea) {
                file_put_data(ef_ee_ea, vendorParamByteString.data, (uint16_t)vendorParamByteString.len);
            }
            low_flash_available();
        }
        else {
            CBOR_ERROR(CTAP2_ERR_INVALID_SUBCOMMAND);
        }
        goto err;
    }
    else if (subcommand == 0x03) {
        uint8_t currentMinPinLen = 4;
        file_t *ef_minpin = search_by_fid(EF_MINPINLEN, NULL, SPECIFY_EF);
        if (file_has_data(ef_minpin)) {
            currentMinPinLen = *file_get_data(ef_minpin);
        }
        if (newMinPinLength == 0) {
            newMinPinLength = currentMinPinLen;
        }
        else if (newMinPinLength > 0 && newMinPinLength < currentMinPinLen) {
            CBOR_ERROR(CTAP2_ERR_PIN_POLICY_VIOLATION);
        }
        if (forceChangePin == ptrue && !file_has_data(ef_pin)) {
            CBOR_ERROR(CTAP2_ERR_PIN_NOT_SET);
        }
        if (file_has_data(ef_pin) && file_get_data(ef_pin)[1] < newMinPinLength) {
            forceChangePin = ptrue;
        }
        if (forceChangePin) {
            resetPersistentPinUvAuthToken();
            resetPinUvAuthToken();
        }
        uint8_t *dataf = (uint8_t *) calloc(1, 2 + minPinLengthRPIDs_len * 32);
        dataf[0] = (uint8_t)newMinPinLength;
        dataf[1] = forceChangePin == ptrue ? 1 : 0;
        for (size_t m = 0; m < minPinLengthRPIDs_len; m++) {
            mbedtls_sha256((uint8_t *) minPinLengthRPIDs[m].data, minPinLengthRPIDs[m].len, dataf + 2 + m * 32, 0);
        }
        file_put_data(ef_minpin, dataf, (uint16_t)(2 + minPinLengthRPIDs_len * 32));
        low_flash_available();
        free(dataf);
        goto err; //No return
    }
    else if (subcommand == 0x01) {
        set_opts(get_opts() | FIDO2_OPT_EA);
        goto err;
    }
    else {
        CBOR_ERROR(CTAP2_ERR_UNSUPPORTED_OPTION);
    }
    //CBOR_CHECK(cbor_encoder_close_container(&encoder, &mapEncoder));
    //resp_size = cbor_encoder_get_buffer_size(&encoder, ctap_resp->init.data + 1);

err:
    CBOR_FREE_BYTE_STRING(pinUvAuthParam);
    CBOR_FREE_BYTE_STRING(vendorParamByteString);
    for (size_t i = 0; i < minPinLengthRPIDs_len; i++) {
        CBOR_FREE_BYTE_STRING(minPinLengthRPIDs[i]);
    }

    if (error != CborNoError) {
        if (error == CborErrorImproperValue) {
            return CTAP2_ERR_CBOR_UNEXPECTED_TYPE;
        }
        return error;
    }
    res_APDU_size = (uint16_t)resp_size;
    return 0;
}
