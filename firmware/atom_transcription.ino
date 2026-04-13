/**
 * Atom Echo S3R – Transkriptions-Pipeline Firmware
 * M5Stack Atom Echo S3R (ESP32-S3, ES7210 Codec)
 *
 * Zustände: IDLE → RECORDING → PROCESSING → DONE / ERROR / RECONNECTING
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <Adafruit_NeoPixel.h>
#include <esp_psram.h>

// =============================================================================
// Konfigurationskonstanten
// =============================================================================
#define WIFI_SSID                   "..."
#define WIFI_PASSWORD               "..."
#define SERVER_IP                   "192.168.x.x"
#define SERVER_PORT                 8765

#define I2S_SAMPLE_RATE             16000
#define I2S_DMA_BUF_LEN            512
#define I2S_DMA_BUF_CNT            8
#define ES7210_I2C_ADDR             0x40

#define BUTTON_PIN                  41
#define BUTTON_DEBOUNCE             50          // ms
#define LONG_PRESS_MS               3000

#define MIC_GAIN_DB                 24

#define PSRAM_BUFFER_MAX_BYTES      (4 * 1024 * 1024)   // 4 MB
#define WIFI_RECONNECT_INTERVAL_MS  2000
#define WIFI_RECONNECT_MAX_ATTEMPTS 30

#define LED_PIN                     35          // NeoPixel pin (Atom S3R)
#define LED_COUNT                   1

// =============================================================================
// Zustände
// =============================================================================
enum State {
    IDLE,
    RECORDING,
    RECONNECTING,
    PROCESSING,
    DONE,
    ERROR_STATE
};

State currentState = IDLE;

// =============================================================================
// LED
// =============================================================================
Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

void setLED(uint8_t r, uint8_t g, uint8_t b) {
    led.setPixelColor(0, led.Color(r, g, b));
    led.show();
}

void setLEDDimmed(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness = 25) {
    led.setPixelColor(0, led.Color(
        (r * brightness) / 255,
        (g * brightness) / 255,
        (b * brightness) / 255
    ));
    led.show();
}

// =============================================================================
// LED-Animationen (non-blocking via Millisekunden-Timer)
// =============================================================================
unsigned long ledTimer = 0;
bool ledToggle = false;
float ledPulse = 0.0f;
float ledPulseDir = 0.02f;

void updateLEDAnimation() {
    unsigned long now = millis();
    switch (currentState) {
        case IDLE:
            setLEDDimmed(255, 255, 255, 25);
            break;
        case RECORDING:
            if (now - ledTimer >= 500) {
                ledTimer = now;
                ledToggle = !ledToggle;
                if (ledToggle) setLED(180, 0, 0);
                else           setLED(0, 0, 0);
            }
            break;
        case RECONNECTING:
            // langsames Pulsieren blau
            if (now - ledTimer >= 20) {
                ledTimer = now;
                ledPulse += ledPulseDir;
                if (ledPulse >= 1.0f || ledPulse <= 0.0f) ledPulseDir = -ledPulseDir;
                uint8_t b = (uint8_t)(ledPulse * 180);
                setLED(0, 0, b);
            }
            break;
        case PROCESSING:
            if (now - ledTimer >= 20) {
                ledTimer = now;
                ledPulse += ledPulseDir;
                if (ledPulse >= 1.0f || ledPulse <= 0.0f) ledPulseDir = -ledPulseDir;
                uint8_t y = (uint8_t)(ledPulse * 200);
                setLED(y, y, 0);
            }
            break;
        case DONE:
            setLED(0, 200, 0);
            break;
        case ERROR_STATE:
            if (now - ledTimer >= 200) {
                ledTimer = now;
                ledToggle = !ledToggle;
                if (ledToggle) setLED(0, 0, 200);
                else           setLED(0, 0, 0);
            }
            break;
    }
}

// =============================================================================
// Pieptöne (I2S TX, nur außerhalb RECORDING)
// =============================================================================
#define I2S_TX_PORT     I2S_NUM_1

void i2sTxInit() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate          = I2S_SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 4,
        .dma_buf_len          = 256,
        .use_apll             = false,
        .tx_desc_auto_clear   = true,
    };
    i2s_pin_config_t pins = {
        .bck_io_num   = 5,
        .ws_io_num    = 6,
        .data_out_num = 38,
        .data_in_num  = I2S_PIN_NO_CHANGE
    };
    i2s_driver_install(I2S_TX_PORT, &cfg, 0, NULL);
    i2s_set_pin(I2S_TX_PORT, &pins);
}

void beep(uint16_t freqHz, uint32_t durationMs) {
    const uint32_t sampleRate = I2S_SAMPLE_RATE;
    const uint32_t totalSamples = (sampleRate * durationMs) / 1000;
    const uint32_t chunkSamples = 256;
    int16_t buf[chunkSamples];

    uint32_t sampleCount = 0;
    float phase = 0.0f;
    float phaseStep = 2.0f * M_PI * freqHz / (float)sampleRate;

    // Envelope: 10ms fade-in / 10ms fade-out
    uint32_t fadeLen = (sampleRate * 10) / 1000;

    while (sampleCount < totalSamples) {
        uint32_t chunk = min((uint32_t)chunkSamples, totalSamples - sampleCount);
        for (uint32_t i = 0; i < chunk; i++) {
            float env = 1.0f;
            if (sampleCount + i < fadeLen)
                env = (float)(sampleCount + i) / fadeLen;
            else if (sampleCount + i > totalSamples - fadeLen)
                env = (float)(totalSamples - sampleCount - i) / fadeLen;
            buf[i] = (int16_t)(sinf(phase) * 20000.0f * env);
            phase += phaseStep;
            if (phase > 2.0f * M_PI) phase -= 2.0f * M_PI;
        }
        size_t written = 0;
        i2s_write(I2S_TX_PORT, buf, chunk * 2, &written, portMAX_DELAY);
        sampleCount += chunk;
    }
    // Flush silence
    memset(buf, 0, sizeof(buf));
    size_t written = 0;
    i2s_write(I2S_TX_PORT, buf, chunkSamples * 2, &written, 100);
}

void beepPattern(uint16_t freqHz, uint32_t tonMs, uint32_t pauseMs, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        beep(freqHz, tonMs);
        if (i < count - 1) delay(pauseMs);
    }
}

// =============================================================================
// ES7210 Initialisierung
// =============================================================================
void es7210WriteReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES7210_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

void es7210Init() {
    Wire.begin();
    delay(10);
    // Reset
    es7210WriteReg(0x00, 0xFF);
    delay(20);
    es7210WriteReg(0x00, 0x41);
    delay(10);
    // Clock / sample rate 16 kHz
    es7210WriteReg(0x01, 0x20);
    es7210WriteReg(0x02, 0x00);
    es7210WriteReg(0x06, 0x00);
    es7210WriteReg(0x07, 0x20);
    // Mono, 16 Bit
    es7210WriteReg(0x11, 0x60);
    // MIC gain 24 dB
    uint8_t gainVal = 0x00;
    if (MIC_GAIN_DB >= 30) gainVal = 0x05;
    else if (MIC_GAIN_DB >= 27) gainVal = 0x04;
    else if (MIC_GAIN_DB >= 24) gainVal = 0x03;
    else gainVal = 0x02;
    es7210WriteReg(0x43, gainVal);  // MIC1
    es7210WriteReg(0x44, gainVal);  // MIC2
    // Power up
    es7210WriteReg(0x0B, 0xFF);
    es7210WriteReg(0x0C, 0xFF);
    delay(20);
}

// =============================================================================
// I2S RX (Mikrofon)
// =============================================================================
#define I2S_RX_PORT     I2S_NUM_0

void i2sRxInit() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate          = I2S_SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = I2S_DMA_BUF_CNT,
        .dma_buf_len          = I2S_DMA_BUF_LEN,
        .use_apll             = false,
        .tx_desc_auto_clear   = false,
    };
    i2s_pin_config_t pins = {
        .bck_io_num   = 5,
        .ws_io_num    = 6,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = 39
    };
    i2s_driver_install(I2S_RX_PORT, &cfg, 0, NULL);
    i2s_set_pin(I2S_RX_PORT, &pins);
}

void i2sRxStop() {
    i2s_stop(I2S_RX_PORT);
    i2s_driver_uninstall(I2S_RX_PORT);
}

// =============================================================================
// PSRAM-Ringpuffer
// =============================================================================
uint8_t* psramBuf = nullptr;
uint32_t psramHead = 0;    // Schreibzeiger
uint32_t psramTail = 0;    // Lesezeiger
uint32_t psramUsed = 0;

void psramBufInit() {
    psramBuf = (uint8_t*)ps_malloc(PSRAM_BUFFER_MAX_BYTES);
    psramHead = psramTail = psramUsed = 0;
}

void psramBufWrite(const uint8_t* data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (psramUsed >= PSRAM_BUFFER_MAX_BYTES) {
            // Älteste Bytes verwerfen (Ringpuffer)
            psramTail = (psramTail + 1) % PSRAM_BUFFER_MAX_BYTES;
            psramUsed--;
        }
        psramBuf[psramHead] = data[i];
        psramHead = (psramHead + 1) % PSRAM_BUFFER_MAX_BYTES;
        psramUsed++;
    }
}

uint32_t psramBufRead(uint8_t* out, uint32_t maxLen) {
    uint32_t n = min(maxLen, psramUsed);
    for (uint32_t i = 0; i < n; i++) {
        out[i] = psramBuf[psramTail];
        psramTail = (psramTail + 1) % PSRAM_BUFFER_MAX_BYTES;
    }
    psramUsed -= n;
    return n;
}

// =============================================================================
// WebSocket
// =============================================================================
WebSocketsClient ws;
bool wsConnected = false;
bool wsAckReceived = false;

void wsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            wsConnected = true;
            Serial.println("[WS] Verbunden");
            break;
        case WStype_DISCONNECTED:
            wsConnected = false;
            Serial.println("[WS] Getrennt");
            if (currentState == RECORDING) {
                currentState = RECONNECTING;
            }
            break;
        case WStype_TEXT:
            if (strncmp((char*)payload, "ACK", 3) == 0) {
                wsAckReceived = true;
            }
            break;
        default:
            break;
    }
}

bool wsConnect() {
    ws.begin(SERVER_IP, SERVER_PORT, "/");
    ws.onEvent(wsEvent);
    ws.setReconnectInterval(0);
    // Warte bis zu 3 Sekunden auf Verbindung
    unsigned long t = millis();
    while (!wsConnected && millis() - t < 3000) {
        ws.loop();
        delay(10);
    }
    return wsConnected;
}

// =============================================================================
// Server-Erreichbarkeitstest (TCP-only)
// =============================================================================
bool serverReachable() {
    WiFiClient client;
    return client.connect(SERVER_IP, SERVER_PORT, 2000);
}

// =============================================================================
// Button-Logik
// =============================================================================
bool lastButtonState = HIGH;
unsigned long pressStart = 0;
bool longPressHandled = false;

// =============================================================================
// Audio-Chunk-Buffer (Stack)
// =============================================================================
#define CHUNK_BYTES (I2S_DMA_BUF_LEN * 2)
int16_t audioChunk[I2S_DMA_BUF_LEN];

// =============================================================================
// Reconnect-Logik
// =============================================================================
int reconnectAttempts = 0;
unsigned long lastReconnectAttempt = 0;

void handleReconnecting() {
    unsigned long now = millis();
    if (now - lastReconnectAttempt >= WIFI_RECONNECT_INTERVAL_MS) {
        lastReconnectAttempt = now;
        reconnectAttempts++;
        Serial.printf("[RECONNECT] Versuch %d/%d\n", reconnectAttempts, WIFI_RECONNECT_MAX_ATTEMPTS);

        if (WiFi.status() != WL_CONNECTED) {
            WiFi.reconnect();
        } else if (wsConnect()) {
            // PSRAM-Puffer zuerst senden
            uint8_t tmpBuf[CHUNK_BYTES];
            uint32_t n;
            while ((n = psramBufRead(tmpBuf, CHUNK_BYTES)) > 0) {
                ws.sendBIN(tmpBuf, n);
                ws.loop();
            }
            currentState = RECORDING;
            reconnectAttempts = 0;
            Serial.println("[RECONNECT] Wiederverbunden, Puffer gesendet");
        }

        if (reconnectAttempts >= WIFI_RECONNECT_MAX_ATTEMPTS) {
            Serial.println("[RECONNECT] Timeout – Aufnahme abgebrochen");
            i2sRxStop();
            currentState = IDLE;
            reconnectAttempts = 0;
            beepPattern(600, 80, 80, 5);
        }
    }
}

// =============================================================================
// Setup
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("[BOOT] Atom Echo S3R startet...");

    // LED initialisieren
    led.begin();
    led.setBrightness(255);
    setLED(0, 0, 0);

    // Button
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    // PSRAM
    if (psramFound()) {
        psramBufInit();
        Serial.println("[BOOT] PSRAM OK");
    } else {
        Serial.println("[BOOT] WARN: PSRAM nicht gefunden");
    }

    // WiFi
    Serial.printf("[BOOT] Verbinde mit %s...\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long wt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wt < 15000) {
        delay(250);
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[BOOT] WiFi OK, IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("[BOOT] WiFi FEHLER");
    }

    // ES7210
    es7210Init();
    Serial.println("[BOOT] ES7210 OK");

    // I2S TX (Pieptöne)
    i2sTxInit();

    // I2S RX (Mikrofon) – initialisieren, aber erst bei Aufnahme nutzen
    i2sRxInit();

    // Server-Erreichbarkeit
    if (serverReachable()) {
        Serial.println("[BOOT] Server erreichbar");
        beepPattern(1000, 80, 80, 2);
        currentState = IDLE;
    } else {
        Serial.println("[BOOT] Server NICHT erreichbar");
        currentState = ERROR_STATE;
        beepPattern(600, 80, 80, 5);
        delay(2000);
        currentState = IDLE;
    }

    Serial.println("[BOOT] Bereit");
}

// =============================================================================
// Loop
// =============================================================================
void loop() {
    ws.loop();
    updateLEDAnimation();

    // --- Button lesen ---
    bool btn = digitalRead(BUTTON_PIN);
    unsigned long now = millis();

    if (btn == LOW && lastButtonState == HIGH) {
        // Flanke: gedrückt
        pressStart = now;
        longPressHandled = false;
        delay(BUTTON_DEBOUNCE);
        if (digitalRead(BUTTON_PIN) == HIGH) goto skipButton;
    }

    // Langer Druck während RECORDING → abbrechen
    if (btn == LOW && !longPressHandled && currentState == RECORDING) {
        if (now - pressStart >= LONG_PRESS_MS) {
            longPressHandled = true;
            Serial.println("[BTN] Langer Druck – Aufnahme abgebrochen");
            ws.disconnect();
            i2sRxStop();
            currentState = IDLE;
        }
    }

    if (btn == HIGH && lastButtonState == LOW && !longPressHandled) {
        // Loslassen nach kurzem Druck
        if (currentState == IDLE) {
            Serial.println("[BTN] Start Aufnahme");
            // I2S RX starten
            i2sRxInit();
            if (wsConnect()) {
                beep(1000, 100);
                currentState = RECORDING;
                psramHead = psramTail = psramUsed = 0;
                reconnectAttempts = 0;
            } else {
                i2sRxStop();
                Serial.println("[BTN] WebSocket Verbindung fehlgeschlagen");
                currentState = ERROR_STATE;
                delay(3000);
                currentState = IDLE;
            }
        } else if (currentState == RECORDING) {
            Serial.println("[BTN] Stopp Aufnahme");
            // Letzten Chunk senden
            size_t bytesRead = 0;
            i2s_read(I2S_RX_PORT, audioChunk, CHUNK_BYTES, &bytesRead, 100);
            if (bytesRead > 0 && wsConnected) {
                ws.sendBIN((uint8_t*)audioChunk, bytesRead);
            }
            i2sRxStop();
            ws.disconnect();
            delay(100);
            beepPattern(1000, 80, 60, 3);
            currentState = PROCESSING;
            wsAckReceived = false;
        }
    }

    skipButton:
    lastButtonState = btn;

    // --- Audio streamen (RECORDING) ---
    if (currentState == RECORDING && wsConnected) {
        size_t bytesRead = 0;
        esp_err_t err = i2s_read(I2S_RX_PORT, audioChunk, CHUNK_BYTES, &bytesRead, 10);
        if (err == ESP_OK && bytesRead > 0) {
            ws.sendBIN((uint8_t*)audioChunk, bytesRead);
        }
    }

    // --- Puffern bei RECONNECTING ---
    if (currentState == RECONNECTING) {
        size_t bytesRead = 0;
        i2s_read(I2S_RX_PORT, audioChunk, CHUNK_BYTES, &bytesRead, 10);
        if (bytesRead > 0 && psramBuf != nullptr) {
            psramBufWrite((uint8_t*)audioChunk, bytesRead);
        }
        handleReconnecting();
    }

    // --- Warten auf ACK (PROCESSING) ---
    if (currentState == PROCESSING) {
        if (wsAckReceived) {
            currentState = DONE;
            // DONE: 2 Sekunden grün, dann IDLE
            unsigned long doneStart = millis();
            while (millis() - doneStart < 2000) {
                updateLEDAnimation();
                delay(10);
            }
            currentState = IDLE;
        }
    }
}
