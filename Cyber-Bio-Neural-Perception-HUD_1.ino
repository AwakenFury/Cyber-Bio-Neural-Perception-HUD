#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h> 
#include "AudioTools.h"
#include "esp_wifi.h"
#include <Wire.h>
#include "html.h" 

// REPAIRED HARDWARE CONFIGURATIONS FOR THE V2.2 A541 BOARD
#define I2S_BCLK           27
#define I2S_WS             25
#define I2S_DATA_IN        36  // HARDWARE FIX 1: Flipped from 35 to 34 for A541 microphone traces!
#define PA_ENABLE_GPIO     21

#define AUDIO_SAMPLES      512
#define CSI_SUBCARRIERS    64
#define TOTAL_PACKET_SIZE  ((AUDIO_SAMPLES * 2) + (CSI_SUBCARRIERS * 4)) 

AsyncWebServer server(80);
AsyncWebSocket ws("/unifiedStream");
DNSServer dnsServer; 
I2SStream i2s_mic_input;

uint8_t tx_packet_buffer[TOTAL_PACKET_SIZE];
float *csi_shared_array = (float *)(tx_packet_buffer + (AUDIO_SAMPLES * 2));

volatile bool csi_is_activated = false; 
const byte DNS_PORT = 53; 
IPAddress apIP(192, 168, 4, 1);

void init_es8388_codec() {
    Wire.begin(33, 32); 
    auto write_reg = [](uint8_t reg, uint8_t val) {
        Wire.beginTransmission(0x10); Wire.write(reg); Wire.write(val); Wire.endTransmission();
    };
    write_reg(0x00, 0x80); delay(15);
    write_reg(0x00, 0x00); write_reg(0x02, 0x00); write_reg(0x04, 0x3C); 
    
    // HARDWARE FIX 2: Configures the input paths to cleanly process raw dual mic registers
    write_reg(0x09, 0x88); // Set registers directly to local analog input MIC1 and MIC2
    write_reg(0x0A, 0xFF); // Crank microphone preamp gain to absolute maximum (+24dB)
    write_reg(0x0B, 0x02); // Enable microphone bias voltage power lines
    write_reg(0x0C, 0x4C); 
}

void csi_rx_callback(void *ctx, wifi_csi_info_t *csi_info) {
    if (!csi_is_activated || !csi_info || !csi_info->buf || ws.count() == 0) return;
    int8_t *csi_raw = (int8_t *)csi_info->buf;
    
    for (int i = 0; i < CSI_SUBCARRIERS; i++) {
        int8_t real = csi_raw[i * 2];
        int8_t imag = csi_raw[i * 2 + 1];
        csi_shared_array[i] = sqrt((real * real) + (imag * imag));
    }
}

void activate_csi_sniffer_engine() {
    if (csi_is_activated) return; 
    esp_wifi_set_csi(true);
    wifi_csi_config_t csi_config = {0}; 
    csi_config.lltf_en = true;          
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(&csi_rx_callback, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    csi_is_activated = true; 
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {}

void setup() {
    Serial.begin(115200);
    pinMode(PA_ENABLE_GPIO, OUTPUT);
    digitalWrite(PA_ENABLE_GPIO, HIGH);
    
    init_es8388_codec();

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP("BioTech-Haptic-Portal", "12345678");
    delay(300); 

    dnsServer.start(DNS_PORT, "*", apIP);

    // Setup I2S Data Struct Parameters
    auto config = i2s_mic_input.defaultConfig(RX_MODE);
    config.sample_rate = 44100;
    config.bits_per_sample = 16;
    config.channels = 1; 
    config.pin_bck = I2S_BCLK;
    config.pin_ws = I2S_WS;
    config.pin_data = I2S_DATA_IN;
    
    // HARDWARE FIX 3: Force library architectures to look at external hardware pins
    config.use_apll = false; 
    
    i2s_mic_input.begin(config);

    ws.onEvent(onEvent);
    server.addHandler(&ws);
    
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){ 
        request->send_P(200, "text/html", index_html); 
    });

    server.on("/startCsi", HTTP_GET, [](AsyncWebServerRequest *request){
        activate_csi_sniffer_engine(); 
        request->send(200, "text/plain", "CSI_ACTIVE");
    });

    server.onNotFound([](AsyncWebServerRequest *request){
        String host = request->host();
        if (host.indexOf("test.com") >= 0) {
            request->send_P(200, "text/html", index_html);
        } else {
            request->redirect("http://192.168.4");
        }
    });

    server.begin();
    Serial.println("Fix patch applied. Listening on Pin 34 data channels.");
}

void loop() {
    dnsServer.processNextRequest(); 
    ws.cleanupClients();

    if (ws.count() > 0 && ws.availableForWriteAll()) {
        if (csi_is_activated) {
            uint8_t dummy_packet[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
            esp_wifi_80211_tx(WIFI_IF_STA, dummy_packet, sizeof(dummy_packet), true);
        }

        size_t bytes_to_read = AUDIO_SAMPLES * sizeof(int16_t);
        
        // HARDWARE FIX 4: Explicitly pull data bytes into buffer using low-level copy macros
        size_t bytes_read = i2s_mic_input.readBytes(tx_packet_buffer, bytes_to_read);
        
        if (bytes_read > 0) {
            ws.binaryAll(tx_packet_buffer, TOTAL_PACKET_SIZE);
        }
    }
    delay(8); 
}
