/******************************************************************************
 * Copyright (c) 2025 Marconatale Parise.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *****************************************************************************/
/**
 * @file http_hal.h
 * @brief HTTP server abstraction layer (HAL) for esp_http_server
 * This module provides a simple wrapper around esp_http_server to manage server lifecycle
 * and endpoint registration. It allows registering endpoints before starting the server and handles common response patterns.
 * @author Marconatale Parise   
 * @date 19 Feb 2026
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HASH_LEN 32

/**
 * @brief OTA HAL configuration
 *
 * 🔧 USER MODIFIABLE fields:
 * - url: HTTPS endpoint hosting the firmware binary
 * - keep_alive: keep-alive generally improves OTA stability
 * - skip_cn_check: debug only
 *
 * Notes:
 * - TLS server verification is controlled by Kconfig:
 *   - CONFIG_EXAMPLE_USE_CERT_BUNDLE (recommended)
 *   - otherwise fall back to embedded PEM (server_certs/ca_cert.pem)
 */
typedef struct {
    const char *url;        /*!< 🔧 USER MODIFIABLE: OTA firmware URL */
    bool keep_alive;        /*!< Enable HTTP keep-alive */
    bool skip_cn_check;     /*!< Debug only: skip CN check */
} ota_hal_cfg_t;


/* =========================
 * USER CONFIGURATION TABLE
 * ========================= */

#ifdef OTA_HAL_DEFINE_CFG
    ota_hal_cfg_t ota_cfg = {
        .url = CONFIG_FIRMWARE_UPGRADE_URL,
        .keep_alive = true,
        .skip_cn_check = false,
    };
#else
    extern ota_hal_cfg_t ota_cfg;
#endif


/**
 * @brief Initialize OTA HAL
 *
 * Checks that configuration is valid.
 * 
 * @return ESP_OK on success
 */
esp_err_t ota_hal_init(void);

/**
 * @brief Start OTA update (blocking)
 *
 * Assumes the network is already connected.
 * If OTA succeeds, this function calls esp_restart().
 *
 * @return ESP_OK if reboot scheduled, otherwise an error code
 */
esp_err_t ota_hal_start(void);

/**
 * @brief Mark running app as valid (cancel rollback) if pending verify
 *
 * Call early on boot (after self-test) to confirm the new firmware.
 *
 * @return ESP_OK on success
 */
esp_err_t ota_hal_mark_app_valid_if_needed(void);

#ifdef __cplusplus
}
#endif
