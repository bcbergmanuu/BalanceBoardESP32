#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_now.h"
#include "BalanceBoard.h"
#include "driver/gpio.h"


static const char *TAG = "BalanceBoard";
#define isSender            true
#define LED         2 
#define PD_SCK      18 
#define DOUT        19
#define GPIO_BIT_MASK_OUT ((1ULL<<LED) | (1ULL<<PD_SCK)) 
#define GPIO_BIT_MASK_IN (1ULL<<DOUT)
#define GAIN        3 // (1 = 128, 3 = 64, 2 = 32 (channel B))

//static uint8_t ReceiveAddress[] = {0x7c,0x9e,0xbd,0x66,0x60,0x81};

//mac #1
static uint8_t ReceiveAddress[] = {0x7c,0x9e,0xbd,0x61,0x28,0x84};
//mac #2
//static uint8_t ReceiveAddress[] = {0x7c,0x9e,0xbd,0x66,0x81,0xe5};



static void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (mac_addr == NULL) {
        ESP_LOGE(TAG, "Send cb arg error");
        return;
    }

    ESP_LOGI(TAG, "data send to "MACSTR"", MAC2STR(mac_addr));
}
static void espnow_recieve_cb(const uint8_t *mac_addr, const uint8_t *data, int len) {
    if(mac_addr == NULL) {
        ESP_LOGE(TAG, "receive arg error");
        return;
    }
    beat_data_t *structured_data = (beat_data_t*)malloc(sizeof(beat_data_t));
    
    memcpy(structured_data, data, sizeof(beat_data_t));

    ESP_LOGI(TAG, "Received: ");
    for(int x = 0; x<8; x++) {
        ESP_LOGI(TAG, "%u", structured_data->available_data[x]);
    }    
    ESP_LOGI(TAG, "\n");
    free(structured_data);
}


/* WiFi should start before using ESPNOW */
static void wifi_init(void)
{
    ESP_LOGI(TAG, "WIFI init\n");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(ESPNOW_WIFI_MODE) );
    ESP_ERROR_CHECK( esp_wifi_start());

#if CONFIG_ESPNOW_ENABLE_LONG_RANGE
    ESP_ERROR_CHECK( esp_wifi_set_protocol(ESPNOW_WIFI_IF, WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G|WIFI_PROTOCOL_11N|WIFI_PROTOCOL_LR) );
#endif
}

static esp_err_t espnow_init_procedure(void){    
    ESP_LOGI(TAG, "ESPNOW init\n");
    /* Initialize ESPNOW and register sending and receiving callback function. */
    ESP_ERROR_CHECK( esp_now_init() );
    ESP_ERROR_CHECK( esp_now_register_send_cb(espnow_send_cb) );    
    ESP_ERROR_CHECK( esp_now_register_recv_cb(espnow_recieve_cb));

    /* Set primary master key. */
    ESP_ERROR_CHECK( esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK) );        
    
    return ESP_OK;
}

static void NVS_init(void) {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );
}

static void add_peer() {
    esp_now_peer_info_t *peerInfo = (esp_now_peer_info_t*)malloc(sizeof(esp_now_peer_info_t));
    memcpy(peerInfo->peer_addr, ReceiveAddress, ESP_NOW_ETH_ALEN);
    peerInfo->channel = CONFIG_ESPNOW_CHANNEL;
    peerInfo->ifidx = ESPNOW_WIFI_IF;
    peerInfo->encrypt = false;
    
    // Add peer        
    if (esp_now_add_peer(peerInfo) != ESP_OK){
        ESP_LOGE(TAG, "Failed to add peer\n");
        return;
    } else {
        ESP_LOGI(TAG, "Peer added\n");
    }
    free(peerInfo);
}


static void send_data(void* arg) {
    if(!isSender) {
        return;
    }
    for(;;){
        vTaskDelay(2000 / portTICK_PERIOD_MS);    
        
        beat_data_t *myData = (beat_data_t*)malloc(sizeof(beat_data_t));
        uint8_t datatosend[] = {5,6,7,1,2,3,0,0};
        memcpy(myData->available_data, datatosend, sizeof(uint8_t)*8);                
        
        esp_err_t result = esp_now_send(ReceiveAddress, (uint8_t *)myData, sizeof(beat_data_t));
        if (result != ESP_OK) {        
            ESP_LOGE(TAG,"Send error %i\n", result);
        } else {
            ESP_LOGI(TAG, "data send\n");
        }
        free(myData);
    }
}



static void config_io_pins() {
    /*
    https://github.com/espressif/esp-idf/tree/master/examples/peripherals/gpio/generic_gpio
    */    
    gpio_config_t io_conf = {};
    //disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
     //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = GPIO_BIT_MASK_OUT;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //configure GPIO with the given settings
    gpio_config(&io_conf);

    io_conf = (gpio_config_t) 
            {.mode = GPIO_MODE_INPUT, .pin_bit_mask = GPIO_BIT_MASK_IN, .pull_up_en = 1};
    gpio_config(&io_conf);
}

static void toggle_hx711_clock() {
    usleep(1); 
    gpio_set_level(PD_SCK, 1);
    usleep(1);
    gpio_set_level(PD_SCK, 0);           
}

static float read_hx711() {
    
    //check if chip is ready
    if(gpio_get_level(DOUT) == 1)  {
        ESP_LOGI(TAG, "HX711 not ready");
        return 0;
    }

    uint32_t count = 1;

    //Toggle clockpin to start reading
    //then
    //Continue toggling clockpin to set gain factor for the next reading.    
    for(uint8_t i = 0; i < 24; i++) {        
        toggle_hx711_clock();
        count = count << 1;
        if(gpio_get_level(DOUT)) count++;
    }
    
    toggle_hx711_clock();    
	
    count = count^0x800000;
    count = 25219440 - count;
    float ret = 1.0*count/23120;

    return ret;
}

void app_main(void)
{
    NVS_init();       
    wifi_init();    
    espnow_init_procedure();
        
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "mac: "MACSTR"\n", MAC2STR(mac));
    
    if(isSender) {
        add_peer();    
    }

    //xTaskCreate(send_data, "data_send_continously", 2048, NULL, 10, NULL);    
    
    config_io_pins();
    for(;;) {        
        vTaskDelay(80 / portTICK_PERIOD_MS);   
        
        printf("%4.3fKg\n", read_hx711());
    }
    
    //never reach this code
    fflush(stdout);
    esp_restart();
}
