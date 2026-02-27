#define OTA_HAL_DEFINE_CFG
#include "ota_hal.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"

#include "wifi.h"

#include <sys/socket.h>
#include <net/if.h>
#include "esp_netif.h"

#ifdef CONFIG_USE_CERT_BUNDLE
#include "esp_crt_bundle.h"
#endif

static const char *TAG = "ota_hal";

static bool s_inited;

#define OTA_URL_SIZE 256

static void stdio_prepare(void)
{
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
}

static void strip_newline(char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n--;
    }
}

static void print_sha256(const uint8_t *image_hash, const char *label)
{
    char hash_print[HASH_LEN * 2 + 1];
    hash_print[HASH_LEN * 2] = 0;
    for (int i = 0; i < HASH_LEN; ++i) {
        sprintf(&hash_print[i * 2], "%02x", image_hash[i]);
    }
    ESP_LOGI("OTA", "%s %s", label, hash_print);
}

static void get_sha256_of_partitions(void)
{
    uint8_t sha_256[HASH_LEN] = { 0 };
    esp_partition_t partition;

    /* SHA256 for bootloader */
    partition.address = ESP_BOOTLOADER_OFFSET;
    partition.size    = ESP_PARTITION_TABLE_OFFSET;
    partition.type    = ESP_PARTITION_TYPE_APP;
    esp_partition_get_sha256(&partition, sha_256);
    print_sha256(sha_256, "SHA-256 for bootloader:");

    /* SHA256 for running firmware */
    esp_partition_get_sha256(esp_ota_get_running_partition(), sha_256);
    print_sha256(sha_256, "SHA-256 for current firmware:");
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
            break;
        default:
            break;
    }
    return ESP_OK;
}

esp_err_t ota_hal_init()
{
    const ota_hal_cfg_t *cfg = &ota_cfg;
    if (!cfg || !cfg->url){
        ESP_LOGI(TAG, "OTA setting missing or No URL configured");
        s_inited = false;
        return ESP_ERR_INVALID_ARG;
    }
    //memset(&ota_cfg, 0, sizeof(ota_cfg));
    s_inited = true;
    
    return ESP_OK;
}

esp_err_t ota_hal_mark_app_valid_if_needed(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return ESP_FAIL;

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "App is PENDING_VERIFY -> marking VALID (cancel rollback)");
            return esp_ota_mark_app_valid_cancel_rollback();
        }
    }
    return ESP_OK;
}

esp_err_t ota_hal_start(void)
{
    if (!s_inited){
        ESP_LOGI(TAG, "OTA HAL not initialized");
        return ESP_ERR_INVALID_STATE;
    } 

    const char *url = ota_cfg.url;
    char url_buf[OTA_URL_SIZE] = {0};

#ifdef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL_FROM_STDIN
    if (strcmp(url, "FROM_STDIN") == 0) {
        stdio_prepare();
        printf("Enter OTA URL:\n");
        if (!fgets(url_buf, sizeof(url_buf), stdin)) {
            return ESP_FAIL;
        }
        strip_newline(url_buf);
        url = url_buf;
    }
#endif

#ifdef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF
    struct ifreq ifr = {0};
    esp_netif_t *netif = wifi_get_netif_sta();
    if (!netif) {
        ESP_LOGE(TAG, "Bind-if enabled but Wi-Fi netif is NULL");
        return ESP_ERR_INVALID_STATE;
    }
    esp_netif_get_netif_impl_name(netif, ifr.ifr_name);
    ESP_LOGI(TAG, "Bind interface name is %s", ifr.ifr_name);
#endif

    esp_http_client_config_t http_cfg = {
        .url = url,
        .event_handler = http_event_handler,
        .keep_alive_enable = ota_cfg.keep_alive,
        .buffer_size_tx = 8192,   // request line + headers
        .buffer_size    = 4096,   // response headers/data chunk (puoi anche lasciare 2048)
        .timeout_ms     = 30000,  // opzionale ma utile su rete “lenta”
#ifdef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF
        .if_name = &ifr,
#endif
    };

#ifdef CONFIG_USE_CERT_BUNDLE
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
#else
    /* Fallback: embed server_certs/ca_cert.pem with EMBED_TXTFILES in main/CMakeLists.txt */
    extern const uint8_t ca_cert_pem_start[] asm("_binary_ca_cert_pem_start");
    http_cfg.cert_pem = (const char *)ca_cert_pem_start;
#endif

#ifdef CONFIG_EXAMPLE_SKIP_COMMON_NAME_CHECK
    /* Match original example behavior: force skip when Kconfig says so */
    http_cfg.skip_cert_common_name_check = true;
#else
    /* Optional runtime override (keep default secure behavior if false) */
    if (ota_cfg.skip_cn_check) {
        http_cfg.skip_cert_common_name_check = true;
    }
#endif

    esp_https_ota_config_t https_ota_cfg = {
        .http_config = &http_cfg,
    };

    ESP_LOGI(TAG, "Attempting to download update from %s", url);
    esp_err_t ret = esp_https_ota(&https_ota_cfg);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA Succeed, Rebooting...");
        esp_restart();
        return ESP_OK; /* unreachable */
    }

    ESP_LOGE(TAG, "Firmware upgrade failed: %s", esp_err_to_name(ret));
    return ret;
}
