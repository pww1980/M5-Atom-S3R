/**
 * Atom Echo S3R – Transkriptions-Pipeline Firmware
 *
 * Hardware:  M5Stack Atom VoiceS3R / Atom EchoS3R
 * MCU:       ESP32-S3
 * Codec:     ES8311  (via M5Unified)
 * LED:       LP5562  (I2C, SDA=45, SCL=0)
 * Button:    GPIO 41
 *
 * Zustände: IDLE → RECORDING → PROCESSING → DONE / ERROR / RECONNECTING
 */

#include <M5Unified.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <Wire.h>
#include <esp_psram.h>
#include <math.h>

// =============================================================================
// Konfigurationskonstanten
// =============================================================================
#define WIFI_SSID                   "..."
#define WIFI_PASSWORD               "..."
#define SERVER_IP                   "192.168.x.x"
#define SERVER_PORT                 8765

#define AUDIO_SAMPLE_RATE           16000
#define AUDIO_BUF_SAMPLES           512     // Samples pro Chunk
#define AUDIO_BUF_BYTES             (AUDIO_BUF_SAMPLES * 2)

#define BUTTON_PIN                  41
#define BUTTON_DEBOUNCE_MS          50
#define LONG_PRESS_MS               3000

#define PSRAM_BUFFER_MAX_BYTES      (4 * 1024 * 1024)   // 4 MB
#define WIFI_RECONNECT_INTERVAL_MS  2000
#define WIFI_RECONNECT_MAX_ATTEMPTS 30

// I2C (geteilt von ES8311 und LP5562)
#define I2C_SDA_PIN                 45
#define I2C_SCL_PIN                 0

// =============================================================================
// LP5562 RGB-LED (I2C)
// =============================================================================
#define LP5562_ADDR     0x30

#define LP5562_ENABLE   0x00  // [6]=CHIP_EN, [5]=LOG_EN
#define LP5562_OP_MODE  0x01  // [7:6]=ENG3, [5:4]=ENG2, [3:2]=ENG1  (11=direct)
#define LP5562_B_PWM    0x02
#define LP5562_G_PWM    0x03
#define LP5562_R_PWM    0x04
#define LP5562_R_CUR    0x05  // Stromstärke Rot   (0xAF = ~175/255)
#define LP5562_G_CUR    0x06
#define LP5562_B_CUR    0x07
#define LP5562_CONFIG   0x08  // [0]=INT_CLK_EN
#define LP5562_RESET    0x0D  // 0xFF = Reset

void lp5562Write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(LP5562_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

void lp5562Init() {
    lp5562Write(LP5562_RESET,   0xFF);
    delay(10);
    lp5562Write(LP5562_ENABLE,  0x40);  // CHIP_EN
    lp5562Write(LP5562_CONFIG,  0x01);  // interner Takt
    lp5562Write(LP5562_OP_MODE, 0xFF);  // alle Kanäle: Direct Control
    // Stromstärke (AF hex = gemäßigter Strom, verhindert Überhitzung)
    lp5562Write(LP5562_R_CUR,   0xAF);
    lp5562Write(LP5562_G_CUR,   0xAF);
    lp5562Write(LP5562_B_CUR,   0xAF);
    // LED aus
    lp5562Write(LP5562_R_PWM,   0x00);
    lp5562Write(LP5562_G_PWM,   0x00);
    lp5562Write(LP5562_B_PWM,   0x00);
}

void setLED(uint8_t r, uint8_t g, uint8_t b) {
    lp5562Write(LP5562_R_PWM, r);
    lp5562Write(LP5562_G_PWM, g);
    lp5562Write(LP5562_B_PWM, b);
}

void setLEDDimmed(uint8_t r, uint8_t g, uint8_t b, uint8_t pct = 10) {
    setLED((r * pct) / 100, (g * pct) / 100, (b * pct) / 100);
}

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
// LED-Animation (non-blocking)
// =============================================================================
unsigned long ledTimer    = 0;
bool          ledToggle   = false;
float         ledPulse    = 0.0f;
float         ledPulseDir = 0.02f;

void updateLEDAnimation() {
    unsigned long now = millis();
    switch (currentState) {
        case IDLE:
            setLEDDimmed(255, 255, 255, 10);        // Weiß, gedimmt
            break;

        case RECORDING:
            if (now - ledTimer >= 500) {
                ledTimer  = now;
                ledToggle = !ledToggle;
                ledToggle ? setLED(180, 0, 0) : setLED(0, 0, 0);  // Rot blinkend
            }
            break;

        case RECONNECTING:                          // Blau pulsierend (langsam)
            if (now - ledTimer >= 20) {
                ledTimer   = now;
                ledPulse  += ledPulseDir;
                if (ledPulse >= 1.0f || ledPulse <= 0.0f) ledPulseDir = -ledPulseDir;
                setLED(0, 0, (uint8_t)(ledPulse * 150));
            }
            break;

        case PROCESSING:                            // Gelb pulsierend
            if (now - ledTimer >= 20) {
                ledTimer   = now;
                ledPulse  += ledPulseDir;
                if (ledPulse >= 1.0f || ledPulse <= 0.0f) ledPulseDir = -ledPulseDir;
                uint8_t y = (uint8_t)(ledPulse * 200);
                setLED(y, y, 0);
            }
            break;

        case DONE:
            setLED(0, 180, 0);                      // Grün
            break;

        case ERROR_STATE:
            if (now - ledTimer >= 200) {
                ledTimer  = now;
                ledToggle = !ledToggle;
                ledToggle ? setLED(0, 0, 200) : setLED(0, 0, 0); // Blau schnell
            }
            break;
    }
}

// =============================================================================
// Pieptöne (M5.Speaker, nur außerhalb RECORDING)
// =============================================================================
void beepPattern(uint16_t freqHz, uint32_t tonMs, uint32_t pauseMs, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        M5.Speaker.tone(freqHz, tonMs);
        delay(tonMs);
        M5.Speaker.stop();
        if (i < count - 1) delay(pauseMs);
    }
}

// =============================================================================
// PSRAM-Ringpuffer
// =============================================================================
uint8_t* psramBuf  = nullptr;
uint32_t psramHead = 0;
uint32_t psramTail = 0;
uint32_t psramUsed = 0;

void psramBufInit() {
    if (!psramFound()) { Serial.println("[PSRAM] Nicht verfügbar"); return; }
    psramBuf = (uint8_t*)ps_malloc(PSRAM_BUFFER_MAX_BYTES);
    if (!psramBuf) Serial.println("[PSRAM] Allokierung fehlgeschlagen");
    else           Serial.printf("[PSRAM] %d MB reserviert\n", PSRAM_BUFFER_MAX_BYTES / (1024*1024));
}

void psramBufReset() { psramHead = psramTail = psramUsed = 0; }

void psramBufWrite(const uint8_t* data, uint32_t len) {
    if (!psramBuf) return;
    for (uint32_t i = 0; i < len; i++) {
        if (psramUsed >= PSRAM_BUFFER_MAX_BYTES) {
            psramTail = (psramTail + 1) % PSRAM_BUFFER_MAX_BYTES;
            psramUsed--;
        }
        psramBuf[psramHead] = data[i];
        psramHead = (psramHead + 1) % PSRAM_BUFFER_MAX_BYTES;
        psramUsed++;
    }
}

uint32_t psramBufRead(uint8_t* out, uint32_t maxLen) {
    if (!psramBuf) return 0;
    uint32_t n = (psramUsed < maxLen) ? psramUsed : maxLen;
    for (uint32_t i = 0; i < n; i++) {
        out[i]   = psramBuf[psramTail];
        psramTail = (psramTail + 1) % PSRAM_BUFFER_MAX_BYTES;
    }
    psramUsed -= n;
    return n;
}

// =============================================================================
// WebSocket
// =============================================================================
WebSocketsClient ws;
bool wsConnected   = false;
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
            if (currentState == RECORDING) currentState = RECONNECTING;
            break;
        case WStype_TEXT:
            if (length >= 3 && strncmp((char*)payload, "ACK", 3) == 0)
                wsAckReceived = true;
            break;
        default: break;
    }
}

bool wsConnect() {
    ws.begin(SERVER_IP, SERVER_PORT, "/");
    ws.onEvent(wsEvent);
    ws.setReconnectInterval(0);
    unsigned long t = millis();
    while (!wsConnected && millis() - t < 3000) { ws.loop(); delay(10); }
    return wsConnected;
}

// =============================================================================
// Server-Erreichbarkeitstest (TCP-only)
// =============================================================================
bool serverReachable() {
    WiFiClient c;
    bool ok = c.connect(SERVER_IP, SERVER_PORT, 2000);
    c.stop();
    return ok;
}

// =============================================================================
// Reconnect-Logik
// =============================================================================
int           reconnectAttempts   = 0;
unsigned long lastReconnectMillis = 0;
int16_t       audioBuf[AUDIO_BUF_SAMPLES];

void handleReconnecting() {
    // Aufnahme weiterlaufen lassen, in PSRAM puffern
    if (M5.Mic.record(audioBuf, AUDIO_BUF_SAMPLES, AUDIO_SAMPLE_RATE, false))
        psramBufWrite((uint8_t*)audioBuf, AUDIO_BUF_BYTES);

    unsigned long now = millis();
    if (now - lastReconnectMillis < WIFI_RECONNECT_INTERVAL_MS) return;
    lastReconnectMillis = now;
    reconnectAttempts++;
    Serial.printf("[RECONNECT] Versuch %d/%d\n", reconnectAttempts, WIFI_RECONNECT_MAX_ATTEMPTS);

    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();

    if (WiFi.status() == WL_CONNECTED && wsConnect()) {
        // PSRAM-Puffer zuerst senden
        uint8_t tmp[AUDIO_BUF_BYTES];
        uint32_t n;
        while ((n = psramBufRead(tmp, AUDIO_BUF_BYTES)) > 0) {
            ws.sendBIN(tmp, n);
            ws.loop();
        }
        currentState      = RECORDING;
        reconnectAttempts = 0;
        Serial.println("[RECONNECT] Wiederverbunden");
        return;
    }

    if (reconnectAttempts >= WIFI_RECONNECT_MAX_ATTEMPTS) {
        Serial.println("[RECONNECT] Timeout");
        M5.Mic.end();
        M5.Speaker.begin();
        beepPattern(600, 80, 80, 5);
        setLED(0, 0, 0);
        currentState      = IDLE;
        reconnectAttempts = 0;
    }
}

// =============================================================================
// Setup
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(300);

    // M5Unified initialisieren (verwaltet ES8311, I2S, I2C intern)
    auto cfg = M5.config();
    M5.begin(cfg);

    // LP5562 LED-Treiber initialisieren
    // Wire wurde von M5.begin() mit SDA=45, SCL=0 gestartet
    lp5562Init();
    setLED(0, 0, 0);
    Serial.println("[BOOT] LP5562 LED OK");

    // Button
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    // PSRAM
    psramBufInit();

    // Speaker vorbereiten (für Pieptöne)
    M5.Speaker.setVolume(200);
    M5.Speaker.begin();

    // WiFi
    Serial.printf("[BOOT] Verbinde mit %s ...\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long wt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wt < 15000) delay(250);

    if (WiFi.status() == WL_CONNECTED)
        Serial.printf("[BOOT] WiFi OK – IP: %s\n", WiFi.localIP().toString().c_str());
    else
        Serial.println("[BOOT] WiFi FEHLER");

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
bool          lastButtonState  = HIGH;
unsigned long pressStartMillis = 0;
bool          longPressHandled = false;

void loop() {
    M5.update();
    ws.loop();
    updateLEDAnimation();

    // ---- Button lesen ----
    bool btn = digitalRead(BUTTON_PIN);
    unsigned long now = millis();

    // Flanke: gedrückt
    if (btn == LOW && lastButtonState == HIGH) {
        delay(BUTTON_DEBOUNCE_MS);
        if (digitalRead(BUTTON_PIN) == LOW) {
            pressStartMillis = millis();
            longPressHandled = false;
        }
    }

    // Langer Druck während RECORDING → Abbruch
    if (btn == LOW && !longPressHandled && currentState == RECORDING) {
        if (millis() - pressStartMillis >= LONG_PRESS_MS) {
            longPressHandled = true;
            Serial.println("[BTN] Langer Druck – Aufnahme abgebrochen");
            M5.Mic.end();
            ws.disconnect();
            M5.Speaker.begin();
            currentState = IDLE;
        }
    }

    // Loslassen nach kurzem Druck
    if (btn == HIGH && lastButtonState == LOW && !longPressHandled) {

        if (currentState == IDLE) {
            Serial.println("[BTN] Start Aufnahme");
            M5.Speaker.end();   // Lautsprecher aus, bevor Mikrofon startet
            auto mic_cfg            = M5.Mic.config();
            mic_cfg.sample_rate     = AUDIO_SAMPLE_RATE;
            mic_cfg.stereo          = false;
            mic_cfg.dma_buf_count   = 8;
            mic_cfg.dma_buf_len     = AUDIO_BUF_SAMPLES;
            M5.Mic.config(mic_cfg);
            M5.Mic.begin();

            if (wsConnect()) {
                M5.Speaker.begin();
                beepPattern(1000, 100, 0, 1);
                M5.Speaker.end();
                psramBufReset();
                reconnectAttempts   = 0;
                lastReconnectMillis = 0;
                currentState        = RECORDING;
            } else {
                Serial.println("[BTN] WebSocket Verbindung fehlgeschlagen");
                M5.Mic.end();
                M5.Speaker.begin();
                currentState = ERROR_STATE;
                delay(3000);
                currentState = IDLE;
            }

        } else if (currentState == RECORDING) {
            Serial.println("[BTN] Stopp Aufnahme");
            // Letzten Chunk senden
            if (M5.Mic.record(audioBuf, AUDIO_BUF_SAMPLES, AUDIO_SAMPLE_RATE, false) && wsConnected)
                ws.sendBIN((uint8_t*)audioBuf, AUDIO_BUF_BYTES);
            M5.Mic.end();
            ws.disconnect();
            delay(100);
            M5.Speaker.begin();
            beepPattern(1000, 80, 60, 3);
            wsAckReceived = false;
            currentState  = PROCESSING;
        }
    }
    lastButtonState = btn;

    // ---- Audio streamen (RECORDING) ----
    if (currentState == RECORDING && wsConnected) {
        if (M5.Mic.record(audioBuf, AUDIO_BUF_SAMPLES, AUDIO_SAMPLE_RATE, false))
            ws.sendBIN((uint8_t*)audioBuf, AUDIO_BUF_BYTES);
    }

    // ---- Puffern + Reconnect (RECONNECTING) ----
    if (currentState == RECONNECTING)
        handleReconnecting();

    // ---- Warten auf ACK (PROCESSING) ----
    if (currentState == PROCESSING && wsAckReceived) {
        currentState = DONE;
        unsigned long doneStart = millis();
        while (millis() - doneStart < 2000) { updateLEDAnimation(); delay(10); }
        setLED(0, 0, 0);
        currentState = IDLE;
    }
}
