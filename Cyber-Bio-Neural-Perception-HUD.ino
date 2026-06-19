#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "AudioTools.h"
#include <Wire.h>
#include "esp_wifi.h" // Required to tap into the physical layer Wi-Fi CSI matrix

#define I2S_BCLK           27
#define I2S_WS             25
#define I2S_DATA_IN        35
#define PA_ENABLE_GPIO     21

AsyncWebServer server(80);
AsyncWebSocket ws("/audioStream");
I2SStream i2s_mic_input;

float volume_multiplier = 1.0; 
#define CHUNK_SIZE 1024
uint8_t raw_buffer[CHUNK_SIZE]; 

// Simple CSI tracking variance filters
int16_t base_left_amplitude = 0;
int16_t base_right_amplitude = 0;
unsigned long last_csi_broadcast = 0;

// WI-FI CSI CALLBACK FUNCTION
void wifi_csi_rx_cb(void *ctx, wifi_csi_info_t *info) {
    if (!info || !info->buf) return;
    
    // Subcarrier spectrum index sampling 
    // Subcarriers 10-25 represent path reflections on one side of the antenna field
    // Subcarriers 35-50 represent path reflections on the opposing side
    int32_t left_sum = 0;
    int32_t right_sum = 0;
    
    for (int i = 10; i < 25; i++) left_sum += abs(info->buf[i*2]);     // Real component part
    for (int i = 35; i < 50; i++) right_sum += abs(info->buf[i*2]);   
    
    int16_t avg_left = left_sum / 15;
    int16_t avg_right = right_sum / 15;

    // Detect disturbances against environmental thresholds
    if (ws.count() > 0 && (millis() - last_csi_broadcast > 300)) { 
        if (abs(avg_left - base_left_amplitude) > 18) { // Deviation threshold
            ws.textAll("CSI_MOVE:LEFT");
            last_csi_broadcast = millis();
        } 
        else if (abs(avg_right - base_right_amplitude) > 18) {
            ws.textAll("CSI_MOVE:RIGHT");
            last_csi_broadcast = millis();
        }
    }
    
    // Slow environmental tracking adjustment
    base_left_amplitude = (base_left_amplitude * 0.95) + (avg_left * 0.05);
    base_right_amplitude = (base_right_amplitude * 0.95) + (avg_right * 0.05);
}

void init_es8388_codec() {
    Wire.begin(33, 32); 
    auto write_reg = [](uint8_t reg, uint8_t val) {
        Wire.beginTransmission(0x10); 
        Wire.write(reg); Wire.write(val); 
        Wire.endTransmission();
    };
    write_reg(0x00, 0x80); delay(15);             
    write_reg(0x00, 0x00); write_reg(0x02, 0x00); 
    write_reg(0x04, 0x3C); write_reg(0x09, 0x88); 
    write_reg(0x0A, 0xBB); write_reg(0x0C, 0x4C); 
    Serial.println("Codec Armed.");
}

// INLINE FRONTEND CODE
extern const char index_html[]; 

void setup() {
    Serial.begin(115200);
    pinMode(PA_ENABLE_GPIO, OUTPUT);
    digitalWrite(PA_ENABLE_GPIO, HIGH);
    
    init_es8388_codec();

    // Configure Wi-Fi SoftAP Mode with CSI Sub-Sensing Enabled
    WiFi.mode(WIFI_AP);
    WiFi.softAP("BioTech-Haptic-Portal", "12345678");
    
    // Initialize Espressif hardware CSI engine hooks
    // Initialize Espressif hardware CSI engine hooks (v2.0.x API)
    ESP_ERROR_CHECK(esp_wifi_set_csi(1));
    
    wifi_csi_config_t csi_config;
    csi_config.lltf_en = 1;
    csi_config.htltf_en = 1;
    csi_config.stbc_htltf2_en = 1;
    csi_config.ltf_merge_en = 1;
    csi_config.channel_filter_en = 1;
    
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    // FIXED: Correct v2.0.x function name mapping includes '_rx_'
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(&wifi_csi_rx_cb, NULL));


    auto config = i2s_mic_input.defaultConfig(RX_MODE);
    config.sample_rate = 44100;
    config.bits_per_sample = 16;
    config.channels = 1; 
    config.pin_bck = I2S_BCLK; config.pin_ws = I2S_WS; config.pin_data = I2S_DATA_IN;
    i2s_mic_input.begin(config);

    server.addHandler(&ws);
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html", index_html);
    });
    server.begin();
}

void loop() {
    ws.cleanupClients();
    if (ws.count() > 0 && ws.availableForWriteAll()) { 
        size_t bytes_read = i2s_mic_input.readBytes(raw_buffer, CHUNK_SIZE);
        if (bytes_read > 0) {
            int16_t *samples = (int16_t*)raw_buffer;
            size_t sample_count = bytes_read / 2;
            for (size_t i = 0; i < sample_count; i++) {
                int32_t val = samples[i] * volume_multiplier;
                if (val > 32767) val = 32767;
                else if (val < -32768) val = -32768;
                samples[i] = (int16_t)val;
            }
            ws.binaryAll(raw_buffer, bytes_read);
        }
    }
}

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Neural Fusion HUD</title>
    <style>
        body { background: #020617; color: #0df; font-family: monospace; padding: 20px; overflow: hidden; text-align: center; }
        #hud-container { position: relative; width: 340px; height: 340px; margin: 40px auto; border: 2px solid #0df; border-radius: 50%; box-shadow: 0 0 20px rgba(0,221,255,0.2); }
        .hud-arc { position: absolute; top:0; left:0; width:100%; height:100%; border-radius:50%; border: 4px dashed rgba(0,221,255,0.15); animation: rotate 20s linear infinite; }
        #hud-alert { position: absolute; top: 40%; width: 100%; font-size: 20px; font-weight: bold; text-shadow: 0 0 10px #0df; transition: 0.2s; }
        .flash-L { border-left: 12px solid #ff2a5f !important; background: rgba(255,42,95,0.1); }
        .flash-R { border-right: 12px solid #ff2a5f !important; background: rgba(255,42,95,0.1); }
        button { background: #0df; border: none; padding: 15px; font-weight: bold; cursor: pointer; width: 100%; max-width: 340px; border-radius: 4px; box-shadow: 0 0 15px rgba(0,255,255,0.4); }
        @keyframes rotate { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }
    </style>
</head>
<body>
    <h2>SYSTEM PERCEPTION HUD</h2>
    <button onclick="startFramework()">INITIALIZE SENSORY SYNC</button>
    
    <div id="hud-container">
        <div class="hud-arc" id="arc"></div>
        <div id="hud-alert">SYSTEM READY</div>
    </div>

<script>
    let ws, audioCtx, pannerNode, nextPlayTime = 0;

    function startFramework() {
        if (!audioCtx) {
            audioCtx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 44100 });
        }
        audioCtx.resume().then(() => {
            setupConnections();
        });
    }

    function setupConnections() {
        ws = new WebSocket("ws://" + window.location.hostname + "/audioStream");
        ws.binaryType = 'arraybuffer';
        
        ws.onopen = () => {
            document.getElementById("hud-alert").innerText = "SENSORS ONLINE";
            nextPlayTime = audioCtx.currentTime + 0.05;
        };
        
        ws.onmessage = (event) => {
            // LAYER 1: PARSE TEXT COMMANDS (CSI DIRECTIONAL RADAR)
            if (typeof event.data === "string") {
                handleRadarThreat(event.data);
                return;
            }

            // LAYER 2: SPATIAL AUDIO STREAM
            let rawData = new Int16Array(event.data);
            if (rawData.length === 0) return;

            let audioBuffer = audioCtx.createBuffer(1, rawData.length, 44100);
            let channelData = audioBuffer.getChannelData(0);
            for (let i = 0; i < rawData.length; i++) {
                channelData[i] = rawData[i] / 32768.0;
            }
            
            let source = audioCtx.createBufferSource();
            source.buffer = audioBuffer;

            // Connect to spatial processor
            if (!pannerNode) {
                pannerNode = audioCtx.createPanner();
                pannerNode.panningModel = 'HRTF';
                pannerNode.connect(audioCtx.destination);
            }
            source.connect(pannerNode);
            
            if (nextPlayTime < audioCtx.currentTime) nextPlayTime = audioCtx.currentTime + 0.01;
            source.start(nextPlayTime);
            nextPlayTime += audioBuffer.duration;
        };
    }

    function handleRadarThreat(command) {
        let alertText = document.getElementById("hud-alert");
        let container = document.getElementById("hud-container");
        
        if (command === "CSI_MOVE:LEFT") {
            // 1. HUD Visual Updates
            alertText.innerText = "<< THREAT LEFT";
            alertText.style.color = "#ff2a5f";
            container.className = "flash-L";
            
            // 2. Spatialize audio to the left ear (-1.5 on X axis)
            if (pannerNode) pannerNode.positionX.setValueAtTime(-1.5, audioCtx.currentTime);
            
            // 3. Trigger phone/gamepad haptic response
            triggerHaptic(true);
        } 
        else if (command === "CSI_MOVE:RIGHT") {
            alertText.innerText = "THREAT RIGHT >>";
            alertText.style.color = "#ff2a5f";
            container.className = "flash-R";
            
            if (pannerNode) pannerNode.positionX.setValueAtTime(1.5, audioCtx.currentTime);
            triggerHaptic(false);
        }

        // Clean up visual indicator after 250ms
        setTimeout(() => {
            container.className = "";
            alertText.style.color = "#0df";
        }, 250);
    }

    
    function triggerHaptic(isLeft) {
        // FIXED: Replaced invalid slice syntax with a proper Millisecond array [Left, Pause, Right]
        if ("vibrate" in navigator) {
            navigator.vibrate(isLeft ? [150, 50, 0] : [0, 50, 150]); 
        }
        
        let gamepads = navigator.getGamepads ? navigator.getGamepads() : [];
        for (let gp of gamepads) {
            if (gp && gp.vibrationActuator) {
                gp.vibrationActuator.playEffect("dual-rumble", {
                    startDelay: 0,
                    duration: 200,
                    weakMagnitude: isLeft ? 1.0 : 0.0,
                    strongMagnitude: isLeft ? 0.0 : 1.0
                });
            }
        }
    }


</script>
</body>
</html>
)rawliteral";
