#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "AudioTools.h"
#include <Wire.h>

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

void init_es8388_codec() {
    Wire.begin(33, 32); 
    auto write_reg = [](uint8_t reg, uint8_t val) {
        Wire.beginTransmission(0x10); 
        Wire.write(reg);
        Wire.write(val);
        Wire.endTransmission();
    };
    
    write_reg(0x00, 0x80); // 1. Trigger system reset
    delay(15);             // CRITICAL FIX: Gives the hardware time to reboot
    
    write_reg(0x00, 0x00); // 2. Exit standby mode
    write_reg(0x02, 0x00); // 3. Fully power analog logic lines
    write_reg(0x04, 0x3C); // 4. Activate ADC circuits
    write_reg(0x09, 0x88); // 5. Forcibly route to onboard MIC1/MIC2 lines
    write_reg(0x0A, 0xBB); // 6. Boost Microphone pre-amps (+15dB)
    write_reg(0x0C, 0x4C); // 7. Frame format configuration (Standard 16-Bit)
    
    Serial.println("ES8388 Microphone registers fully active.");
}

// FIXED DASHBOARD UI CODE
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Cyber-Bio | Neural Audio Sync</title>
    <style>
        body { background: #030712; color: #00eaff; font-family: sans-serif; text-align: center; padding: 20px; }
        .card { background: rgba(0,234,255,0.05); border: 1px solid #00eaff; padding: 25px; border-radius: 12px; max-width: 400px; margin: 40px auto; }
        .slider { width: 100%; margin: 20px 0; accent-color: #00eaff; }
        h2 { text-transform: uppercase; letter-spacing: 2px; }
        button { background: #00eaff; border: none; color: #030712; padding: 15px 30px; font-weight: bold; cursor: pointer; border-radius: 4px; font-size: 16px; width: 100%; box-shadow: 0 0 15px rgba(0,234,255,0.3); }
        button:active { background: #00b7cc; }
    </style>
</head>
<body>
    <div class="card">
        <h2>Neural Audio Hub</h2>
        <p>Live MIC1/MIC2 Status: <span id="status" style="color:#ff3b3b; font-weight:bold;">DISCONNECTED</span></p>
        
        <!-- CRITICAL CORRECTION: Touch interaction must instantly resume Context -->
        <button id="audio-init" onclick="startAudioContext()">TAP TO ACTIVATE STREAM</button>
        
        <div style="margin-top:30px;">
            <label>DIGITAL ATTENUATION: <span id="vol-val">100%</span></label>
            <input type="range" min="0" max="250" value="100" class="slider" id="volSlider" oninput="updateVolume(this.value)">
        </div>
    </div>
<script>
    let ws, audioCtx;
    
    function startAudioContext() {
        // Force creation inside a user-triggered function chain to unlock mobile hardware speakers
        if (!audioCtx) {
            audioCtx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 44100 });
        }
        
        // Explicitly wake up mobile speaker rails
        audioCtx.resume().then(() => {
            console.log("Phone Audio Context successfully unmuted.");
            setupWebSocket();
        });
    }

    function setupWebSocket() {
        if (ws && ws.readyState === WebSocket.OPEN) return;

        ws = new WebSocket("ws://192.168.4.1/audioStream");
        ws.binaryType = 'arraybuffer';
        
        ws.onopen = () => { 
            document.getElementById('status').innerText = "LIVE (STREAMING)"; 
            document.getElementById('status').style.color = "#00ff00"; 
            document.getElementById('audio-init').style.background = "#222";
            document.getElementById('audio-init').style.color = "#00eaff";
            document.getElementById('audio-init').innerText = "STREAM IN PROGRESS";
        };
        
        ws.onclose = () => { 
            document.getElementById('status').innerText = "OFFLINE (RETRYING...)"; 
            document.getElementById('status').style.color = "#ff3b3b";
        };
        
        ws.onmessage = (event) => {
            let rawData = new Int16Array(event.data);
            let audioBuffer = audioCtx.createBuffer(1, rawData.length, 44100);
            let channelData = audioBuffer.getChannelData(0);
            
            // Map raw numbers to floating point samples
            for (let i = 0; i < rawData.length; i++) {
                channelData[i] = rawData[i] / 32768.0; 
            }
            
            let source = audioCtx.createBufferSource();
            source.buffer = audioBuffer;
            source.connect(audioCtx.destination);
            source.start(0);
        };
    }

    function updateVolume(val) {
        document.getElementById('vol-val').innerText = val + "%";
        fetch(`/setVol?val=${val / 100.0}`);
    }
</script>
</body>
</html>
)rawliteral";

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {}

void setup() {
    Serial.begin(115200);
    
    pinMode(PA_ENABLE_GPIO, OUTPUT);
    digitalWrite(PA_ENABLE_GPIO, HIGH);
    
    init_es8388_codec();

    WiFi.softAP("Cyber-Bio-Haptic-Portal", "12345678");
    Serial.print("Portal Live! Link: http://");
    Serial.println(WiFi.softAPIP());

    auto config = i2s_mic_input.defaultConfig(RX_MODE);
    config.sample_rate = 44100;
    config.bits_per_sample = 16;
    config.channels = 1; 
    config.pin_bck = I2S_BCLK;
    config.pin_ws = I2S_WS;
    config.pin_data = I2S_DATA_IN;
    i2s_mic_input.begin(config);

    ws.onEvent(onEvent);
    server.addHandler(&ws);

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html", index_html);
    });

    server.on("/setVol", HTTP_GET, [](AsyncWebServerRequest *request){
        if (request->hasParam("val")) {
            volume_multiplier = request->getParam("val")->value().toFloat();
        }
        request->send(200, "text/plain", "OK");
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
                if (val < -32768) val = -32768;
                samples[i] = (int16_t)val;
            }
            ws.binaryAll(raw_buffer, bytes_read);
        }
    }
}
