#include <Arduino.h>
/**
 * Atom Echo S3R – Transkriptions-Pipeline Firmware
 *
 * Hardware:  M5Stack Atom VoiceS3R / Atom EchoS3R
 * MCU:       ESP32-S3 (Dual-Core LX7)
 * Codec:     ES8311  (via M5Unified)
 * Button:    GPIO 41
 * LED:       keine
 *
 * FreeRTOS-Architektur:
 *   Core 0 – audioTask : liest I2S-DMA, befüllt Buffer-Pool → filledQueue
 *   Core 1 – loop()    : leert filledQueue, sendet via WebSocket oder PSRAM
 *
 * WiFi-Einrichtung: Beim ersten Start öffnet das Gerät den Hotspot
 * "Atom-Transcription-Setup". Im Browser 192.168.4.1 aufrufen,
 * WLAN-Zugangsdaten + Server-IP eintragen.
 *
 * Zustände: IDLE → RECORDING → PROCESSING
 *                     ↕
 *                RECONNECTING
 */

#include <M5Unified.h>
#include <WiFiManager.h>
#include <WebSocketsClient.h>
#include <Preferences.h>

// =============================================================================
// Konstanten
// =============================================================================
#define SERVER_PORT                 8765

#define AUDIO_SAMPLE_RATE           16000
#define AUDIO_BUF_SAMPLES           512
#define AUDIO_BUF_BYTES             (AUDIO_BUF_SAMPLES * 2)

// Buffer-Pool: 16 × 1 KB = 16 KB im SRAM
// → ~512 ms Puffer zwischen audioTask und loop()
#define AUDIO_POOL_SIZE             16

#define BUTTON_PIN                  41
#define BUTTON_DEBOUNCE_MS          50
#define LONG_PRESS_MS               3000
#define PORTAL_RESET_HOLD_MS        5000

#define PSRAM_BUFFER_MAX_BYTES      (4 * 1024 * 1024)
#define WIFI_RECONNECT_INTERVAL_MS  2000
#define WIFI_RECONNECT_MAX_ATTEMPTS 30

// =============================================================================
// Zustandsmaschine
// =============================================================================
enum State { IDLE, RECORDING, RECONNECTING, PROCESSING };
volatile State currentState = IDLE;

char serverIP[40] = "192.168.x.x";
Preferences prefs;

// =============================================================================
// FreeRTOS Audio-Buffer-Pool + Queues
//
//  freeQueue  ──►  audioTask (befüllt)  ──►  filledQueue  ──►  loop() (sendet)
//                                                                    │
//                                              freeQueue  ◄──────────┘
// =============================================================================
static int16_t   audioPool[AUDIO_POOL_SIZE][AUDIO_BUF_SAMPLES];
static QueueHandle_t freeQueue   = nullptr;   // leere Puffer
static QueueHandle_t filledQueue = nullptr;   // befüllte Puffer, bereit zum Senden

volatile bool audioCapturing = false;   // steuert audioTask

void audioQueuesInit() {
    freeQueue   = xQueueCreate(AUDIO_POOL_SIZE, sizeof(int16_t*));
    filledQueue = xQueueCreate(AUDIO_POOL_SIZE, sizeof(int16_t*));
    // Alle Puffer in den Free-Pool legen
    for (int i = 0; i < AUDIO_POOL_SIZE; i++) {
        int16_t* ptr = audioPool[i];
        xQueueSend(freeQueue, &ptr, 0);
    }
}

// Gefüllte Queue leeren, Puffer zurück in Free-Pool
void drainFilledQueue() {
    int16_t* buf;
    while (xQueueReceive(filledQueue, &buf, 0) == pdTRUE)
        xQueueSend(freeQueue, &buf, 0);
}

// =============================================================================
// audioTask – läuft auf Core 0, hohe Priorität
// =============================================================================
void audioTask(void* /*param*/) {
    for (;;) {
        if (!audioCapturing) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        int16_t* buf = nullptr;
        // Freien Puffer holen (max. 50 ms warten)
        if (xQueueReceive(freeQueue, &buf, pdMS_TO_TICKS(50)) != pdTRUE)
            continue;

        // Blocking record: kehrt zurück wenn DMA-Puffer voll (~32 ms bei 512 Samples)
        if (M5.Mic.record(buf, AUDIO_BUF_SAMPLES, AUDIO_SAMPLE_RATE, true)
            && audioCapturing)
        {
            // Puffer in filledQueue – falls voll (Sender zu langsam), Puffer verwerfen
            if (xQueueSend(filledQueue, &buf, 0) != pdTRUE) {
                Serial.println("[AUDIO] filledQueue voll – Chunk verworfen");
                xQueueSend(freeQueue, &buf, 0);
            }
        } else {
            // Aufnahme gestoppt oder fehlgeschlagen → Puffer freigeben
            xQueueSend(freeQueue, &buf, 0);
        }
    }
}

// =============================================================================
// Mic starten / stoppen
// =============================================================================
void startCapture() {
    auto mic_cfg          = M5.Mic.config();
    mic_cfg.sample_rate   = AUDIO_SAMPLE_RATE;
    mic_cfg.stereo        = false;
    mic_cfg.dma_buf_count = 8;
    mic_cfg.dma_buf_len   = AUDIO_BUF_SAMPLES;
    M5.Mic.config(mic_cfg);
    M5.Mic.begin();
    audioCapturing = true;
    Serial.println("[MIC] Aufnahme gestartet");
}

void stopCapture() {
    audioCapturing = false;
    delay(60);          // audioTask beendet laufenden record()-Aufruf (~32 ms)
    M5.Mic.end();
    drainFilledQueue(); // restliche Puffer zurück in Free-Pool
    M5.Speaker.begin();
    Serial.println("[MIC] Aufnahme gestoppt");
}

// =============================================================================
// Pieptöne (nur wenn Speaker aktiv, d. h. Mic gestoppt)
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
// WiFi-Setup (WiFiManager + Server-IP-Parameter)
// =============================================================================
WiFiManager           wm;
char                  savedIP[40];
bool                  shouldSaveParams = false;
WiFiManagerParameter* paramServerIP   = nullptr;

void saveParamsCallback() { shouldSaveParams = true; }

void setupWiFi(bool forcePortal = false) {
    prefs.begin("atom", true);
    String ip = prefs.getString("server_ip", "192.168.x.x");
    prefs.end();
    ip.toCharArray(savedIP, sizeof(savedIP));

    delete paramServerIP;
    paramServerIP = new WiFiManagerParameter("server_ip", "Server-IP (N100)", savedIP, 39);
    wm.addParameter(paramServerIP);
    wm.setSaveParamsCallback(saveParamsCallback);
    wm.setConfigPortalTimeout(180);
    wm.setConnectTimeout(15);

    bool ok = forcePortal
        ? wm.startConfigPortal("Atom-Transcription-Setup")
        : wm.autoConnect("Atom-Transcription-Setup");

    if (!ok) {
        Serial.println("[WIFI] Verbindung fehlgeschlagen – Neustart");
        beepPattern(600, 80, 80, 5);
        delay(1000);
        ESP.restart();
    }

    if (shouldSaveParams) {
        const char* val = paramServerIP->getValue();
        prefs.begin("atom", false);
        prefs.putString("server_ip", val);
        prefs.end();
        strncpy(serverIP, val, sizeof(serverIP));
        Serial.printf("[WIFI] Server-IP gespeichert: %s\n", serverIP);
    } else {
        strncpy(serverIP, savedIP, sizeof(serverIP));
    }
    Serial.printf("[WIFI] Verbunden. IP: %s  Server: %s\n",
                  WiFi.localIP().toString().c_str(), serverIP);
}

// =============================================================================
// Server-Erreichbarkeitstest
// =============================================================================
bool serverReachable() {
    WiFiClient c;
    bool ok = c.connect(serverIP, SERVER_PORT, 2000);
    c.stop();
    return ok;
}

// =============================================================================
// PSRAM-Ringpuffer (für Offline-Pufferung bei WLAN-Abbruch)
// =============================================================================
uint8_t* psramBuf  = nullptr;
uint32_t psramHead = 0;
uint32_t psramTail = 0;
uint32_t psramUsed = 0;

void psramBufInit() {
    if (!psramFound()) { Serial.println("[PSRAM] Nicht verfügbar"); return; }
    psramBuf = (uint8_t*)ps_malloc(PSRAM_BUFFER_MAX_BYTES);
    if (psramBuf) Serial.printf("[PSRAM] %d MB reserviert\n",
                                 PSRAM_BUFFER_MAX_BYTES / (1024 * 1024));
    else          Serial.println("[PSRAM] Allokierung fehlgeschlagen");
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
        out[i]    = psramBuf[psramTail];
        psramTail = (psramTail + 1) % PSRAM_BUFFER_MAX_BYTES;
    }
    psramUsed -= n;
    return n;
}

// =============================================================================
// WebSocket
// =============================================================================
WebSocketsClient ws;
volatile bool wsConnected   = false;
volatile bool wsAckReceived = false;

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
    ws.begin(serverIP, SERVER_PORT, "/");
    ws.onEvent(wsEvent);
    ws.setReconnectInterval(0);
    unsigned long t = millis();
    while (!wsConnected && millis() - t < 3000) { ws.loop(); delay(10); }
    return wsConnected;
}

// =============================================================================
// Reconnect-Logik
// =============================================================================
int           reconnectAttempts   = 0;
unsigned long lastReconnectMillis = 0;

void handleReconnecting() {
    // Befüllte Queue → PSRAM (audioTask läuft weiter)
    int16_t* buf;
    while (xQueueReceive(filledQueue, &buf, 0) == pdTRUE) {
        psramBufWrite((uint8_t*)buf, AUDIO_BUF_BYTES);
        xQueueSend(freeQueue, &buf, 0);
    }

    unsigned long now = millis();
    if (now - lastReconnectMillis < WIFI_RECONNECT_INTERVAL_MS) return;
    lastReconnectMillis = now;
    reconnectAttempts++;
    Serial.printf("[RECONNECT] Versuch %d/%d\n", reconnectAttempts, WIFI_RECONNECT_MAX_ATTEMPTS);

    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();

    if (WiFi.status() == WL_CONNECTED && wsConnect()) {
        // PSRAM-Puffer zuerst senden, dann live weiterstreamen
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
        Serial.println("[RECONNECT] Timeout – Aufnahme abgebrochen");
        stopCapture();
        beepPattern(600, 80, 80, 5);
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
    Serial.println("[BOOT] Atom VoiceS3R startet");

    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Speaker.setVolume(200);
    M5.Speaker.begin();

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    psramBufInit();
    audioQueuesInit();

    // audioTask auf Core 0, Priorität 5 (höher als loop auf Core 1)
    xTaskCreatePinnedToCore(
        audioTask,          // Funktion
        "audioTask",        // Name (Debugging)
        4096,               // Stack-Größe in Bytes
        nullptr,            // Parameter
        5,                  // Priorität
        nullptr,            // Task-Handle (nicht benötigt)
        0                   // Core 0
    );
    Serial.println("[BOOT] audioTask gestartet (Core 0)");

    setupWiFi();

    if (serverReachable()) {
        Serial.println("[BOOT] Server erreichbar");
        beepPattern(1000, 80, 80, 2);   // 2× kurz → OK
    } else {
        Serial.println("[BOOT] Server NICHT erreichbar");
        beepPattern(600, 80, 80, 5);    // 5× kurz → Fehler
    }

    currentState = IDLE;
    Serial.println("[BOOT] Bereit");
}

// =============================================================================
// Loop (Core 1)
// =============================================================================
bool          lastButtonState  = HIGH;
unsigned long pressStartMillis = 0;
bool          longPressHandled = false;

void loop() {
    M5.update();
    ws.loop();

    // ---- Button ----
    bool btn = digitalRead(BUTTON_PIN);

    if (btn == LOW && lastButtonState == HIGH) {
        delay(BUTTON_DEBOUNCE_MS);
        if (digitalRead(BUTTON_PIN) == LOW) {
            pressStartMillis = millis();
            longPressHandled = false;
        }
    }

    // Langer Druck im IDLE (≥5s) → WiFi zurücksetzen
    if (btn == LOW && !longPressHandled && currentState == IDLE) {
        if (millis() - pressStartMillis >= PORTAL_RESET_HOLD_MS) {
            longPressHandled = true;
            Serial.println("[BTN] WiFi-Reset");
            beepPattern(1000, 200, 100, 2);
            wm.resetSettings();
            delay(500);
            ESP.restart();
        }
    }

    // Langer Druck im RECORDING (≥3s) → Aufnahme abbrechen
    if (btn == LOW && !longPressHandled && currentState == RECORDING) {
        if (millis() - pressStartMillis >= LONG_PRESS_MS) {
            longPressHandled = true;
            Serial.println("[BTN] Aufnahme abgebrochen");
            stopCapture();
            ws.disconnect();
            currentState = IDLE;
        }
    }

    // Kurzer Druck
    if (btn == HIGH && lastButtonState == LOW && !longPressHandled) {

        if (currentState == IDLE) {
            // Erst WS verbinden (Speaker noch aktiv), dann piepen, dann Mic starten
            Serial.println("[BTN] Start Aufnahme");
            if (wsConnect()) {
                beepPattern(1000, 100, 0, 1);   // 1× 1000 Hz → Aufnahme läuft
                M5.Speaker.end();
                psramBufReset();
                reconnectAttempts   = 0;
                lastReconnectMillis = 0;
                startCapture();
                currentState = RECORDING;
            } else {
                Serial.println("[BTN] WebSocket fehlgeschlagen");
                beepPattern(600, 80, 80, 5);    // 5× 600 Hz → Fehler
            }

        } else if (currentState == RECORDING) {
            Serial.println("[BTN] Stopp Aufnahme");
            stopCapture();              // audioTask pausiert, Mic aus, Speaker an
            ws.disconnect();
            beepPattern(1000, 80, 60, 3);   // 3× 1000 Hz → Verarbeitung läuft
            wsAckReceived = false;
            currentState  = PROCESSING;
        }
    }
    lastButtonState = btn;

    // ---- Audio senden (RECORDING) ----
    // filledQueue leeren und direkt per WebSocket senden
    if (currentState == RECORDING && wsConnected) {
        int16_t* buf;
        while (xQueueReceive(filledQueue, &buf, 0) == pdTRUE) {
            ws.sendBIN((uint8_t*)buf, AUDIO_BUF_BYTES);
            xQueueSend(freeQueue, &buf, 0);     // Puffer sofort zurückgeben
        }
    }

    // ---- PSRAM puffern + Reconnect (RECONNECTING) ----
    if (currentState == RECONNECTING)
        handleReconnecting();

    // ---- Warten auf ACK (PROCESSING) ----
    if (currentState == PROCESSING && wsAckReceived) {
        Serial.println("[PIPELINE] Server hat Job übernommen");
        currentState = IDLE;
    }
}
