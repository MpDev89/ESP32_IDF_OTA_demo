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
/** * @file main.c
 * @brief Main application entry point. Initializes Wi-Fi, GPIO, and starts tasks for normal operation and OTA handling.
 * 
 * This file contains the main application logic, including task definitions for normal operation (toggling an LED) and OTA handling. 
 * It also sets up GPIO interrupts for a button to trigger OTA updates.
 * 
 * @author Marconatale Parise
 * @date 28 Feb 2026
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "wifi.h"
#include "ota_hal.h"
#include "common.h"

typedef enum {
  SYS_RUN = 0,
  SYS_OTA_REQUESTED,
  SYS_OTA_PREPARE,
  SYS_OTA_RUNNING,
  SYS_OTA_FAILED
} sys_state_t;

#define TASKPER_TIME 100 //ms
#define TASKAPP_TIME CONFIG_TOGGLE_LED_FREQUENCY //ms
#define TASKOTA_TIME 500 //ms
#define GPIO_BTN     CONFIG_GPIO_BTN_PIN
#define GPIO_BTN_PIN_SEL  (1ULL<<GPIO_BTN)
#define GPIO_OUT    CONFIG_GPIO_OUT_PIN
#define GPIO_OUT_PIN_SEL  (1ULL<<GPIO_OUT)
static QueueHandle_t gpio_evt_queue = NULL;
bool toogle_led = false;
sys_state_t system_state = SYS_RUN;

static esp_err_t gpio_toggle(uint32_t gpio_num, bool* toogle);
static esp_err_t gpio_init(void);
static void peripherals_safe_outputs();


static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

void Task_per(void *pvParameters) {
    const TickType_t period = pdMS_TO_TICKS(TASKPER_TIME);
    TickType_t xLastWakeTime = xTaskGetTickCount();
    uint32_t io_num;
    while (true) {
        if(system_state == SYS_RUN) {
            // Normal operation code here
            if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
                const bool level = (gpio_get_level(io_num) != 0);
                uint8_t ev = level ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE;
                if (ev == GPIO_INTR_POSEDGE) {
                    LOG("Button pushed - Rising Edge Interrupt");
                    system_state = SYS_OTA_REQUESTED;
                }
            }
        }
        xLastWakeTime = xTaskGetTickCount();
        vTaskDelayUntil(&xLastWakeTime, period);
    }
}

void Task_app(void *pvParameters) {
    const TickType_t period = pdMS_TO_TICKS(TASKAPP_TIME);
    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (true) {
        if(system_state == SYS_RUN){
            ESP_ERROR_CHECK(gpio_toggle(GPIO_OUT, &toogle_led));
            toogle_led = !toogle_led;
        }
        xLastWakeTime = xTaskGetTickCount();
        vTaskDelayUntil(&xLastWakeTime, period);
    }
}

void Task_ota(void *pvParameters) {
    const TickType_t period = pdMS_TO_TICKS(TASKOTA_TIME);
    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (true) {
        switch(system_state){
            case SYS_OTA_REQUESTED:
                LOG("OTA requested, preparing...\n");
                ESP_ERROR_CHECK(ota_hal_init());
                system_state = SYS_OTA_PREPARE;
                break;
            case SYS_OTA_PREPARE:
                LOG("Starting OTA process...\n");
                peripherals_safe_outputs();
                system_state = SYS_OTA_RUNNING;
                break;
            case SYS_OTA_RUNNING:
                esp_err_t err = ota_hal_start();
                if (err != ESP_OK)system_state = SYS_OTA_FAILED;
                break;
            case SYS_OTA_FAILED:
                LOG("OTA failed, reverting to previous state...\n");
                // Handle OTA failure here
                ESP_ERROR_CHECK(gpio_init());
                system_state = SYS_RUN;
                break;
            case SYS_RUN:
            default:
                // Normal operation
                break;
        }
        xLastWakeTime = xTaskGetTickCount();
        vTaskDelayUntil(&xLastWakeTime, period);
    }   
}


void app_main(void)
{
    /* Initialize NVS — it is used to store PHY calibration data */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(gpio_init());

    ESP_ERROR_CHECK(wifi_init_connection());
    ESP_ERROR_CHECK(wifi_connect_sta());
    ESP_ERROR_CHECK(wifi_disable_powersave());
    /* If rollback is enabled, confirm the running image when needed */
    ESP_ERROR_CHECK(ota_hal_mark_app_valid_if_needed());

    xTaskCreatePinnedToCore(Task_app, "Task App", 2048, NULL, 1 , NULL, 1); //Core 1
    xTaskCreatePinnedToCore(Task_per, "Task Peripheral", 2048, NULL, 1 , NULL, 1); //Core 1
    xTaskCreatePinnedToCore(Task_ota, "Task OTA", 8192, NULL, 5 , NULL, 1); //Core 1
}

static esp_err_t gpio_init(void)
{
    gpio_config_t io_conf = {};
    //disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = GPIO_OUT_PIN_SEL;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //configure GPIO with the given settings
    gpio_config(&io_conf);

    //interrupt of rising edge
    #if CONFIG_GPIO_BTN_INTR_NEGEDGE
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    #elif CONFIG_GPIO_BTN_INTR_POSEDGE
    io_conf.intr_type = GPIO_INTR_POSEDGE;
    #else 
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    #endif;
    //bit mask of the pins, use GPIO4/5 here
    io_conf.pin_bit_mask = GPIO_BTN_PIN_SEL;
    //set as input mode
    io_conf.mode = GPIO_MODE_INPUT;
    //enable pull-up mode
    #if CONFIG_GPIO_BTN_PULLUP
    io_conf.pull_up_en = 1;
    io_conf.pull_down_en = 0;
    #elif CONFIG_GPIO_BTN_PULLDOWN
    io_conf.pull_up_en = 0;
    io_conf.pull_down_en = 1;
    #else
    io_conf.pull_up_en = 0;
    io_conf.pull_down_en = 0;
    #endif
    gpio_config(&io_conf);

    //change gpio interrupt type for one pin
    gpio_set_intr_type(GPIO_BTN, GPIO_INTR_POSEDGE);

    //create a queue to handle gpio event from isr
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));

    int isr_flags = 0;
    esp_err_t err = gpio_install_isr_service(isr_flags);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = gpio_isr_handler_add(GPIO_BTN, gpio_isr_handler, (void *)GPIO_BTN);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

static esp_err_t gpio_toggle(uint32_t gpio_num, bool* toogle){
    int level = (* toogle) ? 0 : 1;
    LOG("Toggling GPIO[%"PRIu32"] to level %d", gpio_num, level);
    return gpio_set_level(gpio_num, level);
}

static void peripherals_safe_outputs() {
    gpio_set_level(GPIO_OUT, 0);
    gpio_isr_handler_remove(GPIO_BTN);
    gpio_set_intr_type(GPIO_BTN, GPIO_INTR_DISABLE);
    ESP_LOGI("OTA", "Peripherals put in safe");
}
