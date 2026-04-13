#include <Arduino.h>
#include <M5Unified.h>
#include <WiFiManager.h>
#include <WebSocketsClient.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

// =============================================================================
// Konstanten
// =============================================================================
#define SERVER_PORT                 8765

#define AUDIO_SAMPLE_RATE           16000
#define AUDIO_BUF_SAMPLES           512
#define AUDIO_BUF_BYTES             (AUDIO_BUF_SAMPLES * 2)
#define AUDIO_POOL_SIZE             16

#define LONG_PRESS_MS               3000
#define PORTAL_RESET_HOLD_MS        5000

#define PSRAM_BUFFER_MAX_BYTES      (4 * 1024 * 1024)
#define WIFI_RECONNECT_INTERVAL_MS  2000
#define WIFI_RECONNECT_MAX_ATTEMPTS 30
#define PROCESSING_TIMEOUT_MS       30000
#define AUDIO_STOP_WAIT_MS          300
#define WS_CONNECT_TIMEOUT_MS       3000

// =============================================================================
// EventGroup Bits
// =============================================================================
static EventGroupHandle_t audioEvents = nullptr;
static constexpr EventBits_t EVT_AUDIO_IDLE    = (1 << 0);
static constexpr EventBits_t EVT_AUDIO_RUNNING = (1 << 1);
static constexpr EventBits_t EVT_STOP_REQUEST  = (1 << 2);

// =============================================================================
// Zustandsmaschine
// =============================================================================
enum State { IDLE, RECORDING, RECONNECTING, PROCESSING };
volatile State currentState = IDLE;

char serverIP[40] = "192.168.x.x";
Preferences prefs;

// =============================================================================
// Audio-Puffer + Queues
// =============================================================================
static int16_t audioPool[AUDIO_POOL_SIZE][AUDIO_BUF_SAMPLES];
static QueueHandle_t freeQueue   = nullptr;
static QueueHandle_t filledQueue = nullptr;
static volatile bool audioCapturing = false;

// =============================================================================
// PSRAM
// =============================================================================
uint8_t* psramBuf  = nullptr;
uint32_t psramHead = 0;
uint32_t psramTail = 0;
uint32_t psramUsed = 0;

// =============================================================================
// WebSocket
// =============================================================================
WebSocketsClient ws;
volatile bool wsConnected   = false;
volatile bool wsAckReceived = false;

// =============================================================================
// Reconnect / Processing
// =============================================================================
int reconnectAttempts = 0;
unsigned long lastReconnectMillis = 0;
unsigned long processingStartMillis = 0;

// =============================================================================
// WiFiManager
// =============================================================================
WiFiManager wm;
char savedIP[40];
bool shouldSaveParams = false;
WiFiManagerParameter* paramServerIP = nullptr;

// =============================================================================
// Hilfsfunktionen
// =============================================================================
void beepPattern(uint16_t freqHz, uint32_t tonMs, uint32_t pauseMs, uint8_t count) {
    for (uint8_t i = 0; i < count; ++i) {
        M5.Speaker.tone(freqHz, tonMs);
        delay(tonMs);
        M5.Speaker.stop();
        if (i + 1 < count) delay(pauseMs);
    }
}

void audioQueuesInit() {
    freeQueue   = xQueueCreate(AUDIO_POOL_SIZE, sizeof(int16_t*));
    filledQueue = xQueueCreate(AUDIO_POOL_SIZE, sizeof(int16_t*));

    for (int i = 0; i < AUDIO_POOL_SIZE; ++i) {
        int16_t* ptr = audioPool[i];
        xQueueSend(freeQueue, &ptr, 0);
    }
}

void drainFilledQueue() {
    int16_t* buf;
    while (xQueueReceive(filledQueue, &buf, 0) == pdTRUE) {
        xQueueSend(freeQueue, &buf, 0);
    }
}

// =============================================================================
// PSRAM-Ringpuffer
// =============================================================================
void psramBufInit() {
    if (!psramFound()) {
        Serial.println("[PSRAM] Nicht verfügbar");
        return;
    }
    psramBuf = (uint8_t*)ps_malloc(PSRAM_BUFFER_MAX_BYTES);
    if (psramBuf) {
        Serial.printf("[PSRAM] %d MB reserviert\n", PSRAM_BUFFER_MAX_BYTES / (1024 * 1024));
    } else {
        Serial.println("[PSRAM] Allokierung fehlgeschlagen");
    }
}

void psramBufReset() {
    psramHead = psramTail = psramUsed = 0;
}

void psramDropOldest(uint32_t len) {
    uint32_t drop = (len > psramUsed) ? psramUsed : len;
    psramTail = (psramTail + drop) % PSRAM_BUFFER_MAX_BYTES;
    psramUsed -= drop;
}

void psramBufWrite(const uint8_t* data, uint32_t len) {
    if (!psramBuf || len == 0) return;

    while ((PSRAM_BUFFER_MAX_BYTES - psramUsed) < len) {
        psramDropOldest(AUDIO_BUF_BYTES);
        if (psramUsed == 0 && len > PSRAM_BUFFER_MAX_BYTES) {
            data += (len - PSRAM_BUFFER_MAX_BYTES);
            len = PSRAM_BUFFER_MAX_BYTES;
            break;
        }
    }

    uint32_t firstPart = min(len, PSRAM_BUFFER_MAX_BYTES - psramHead);
    memcpy(&psramBuf[psramHead], data, firstPart);

    uint32_t secondPart = len - firstPart;
    if (secondPart > 0) {
        memcpy(psramBuf, data + firstPart, secondPart);
    }

    psramHead = (psramHead + len) % PSRAM_BUFFER_MAX_BYTES;
    psramUsed += len;
}

uint32_t psramBufRead(uint8_t* out, uint32_t maxLen) {
    if (!psramBuf || psramUsed == 0 || maxLen == 0) return 0;

    uint32_t toRead = min(psramUsed, maxLen);
    uint32_t firstPart = min(toRead, PSRAM_BUFFER_MAX_BYTES - psramTail);

    memcpy(out, &psramBuf[psramTail], firstPart);

    uint32_t secondPart = toRead - firstPart;
    if (secondPart > 0) {
        memcpy(out + firstPart, psramBuf, secondPart);
    }

    psramTail = (psramTail + toRead) % PSRAM_BUFFER_MAX_BYTES;
    psramUsed -= toRead;
    return toRead;
}

// =============================================================================
// WebSocket
// =============================================================================
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
            if (length >= 3 && strncmp((char*)payload, "ACK", 3) == 0) {
                wsAckReceived = true;
                Serial.println("[WS] ACK empfangen");
            }
            break;

        default:
            break;
    }
}

bool wsConnect() {
    wsConnected = false;
    ws.begin(serverIP, SERVER_PORT, "/");
    ws.onEvent(wsEvent);
    ws.setReconnectInterval(0);

    unsigned long start = millis();
    while (!wsConnected && (millis() - start < WS_CONNECT_TIMEOUT_MS)) {
        ws.loop();
        delay(10);
    }
    return wsConnected;
}

// =============================================================================
// Audio-Task (Core 0)
// =============================================================================
void audioTask(void* /*param*/) {
    xEventGroupSetBits(audioEvents, EVT_AUDIO_IDLE);
    xEventGroupClearBits(audioEvents, EVT_AUDIO_RUNNING);

    for (;;) {
        if (!audioCapturing) {
            xEventGroupSetBits(audioEvents, EVT_AUDIO_IDLE);
            xEventGroupClearBits(audioEvents, EVT_AUDIO_RUNNING | EVT_STOP_REQUEST);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        xEventGroupClearBits(audioEvents, EVT_AUDIO_IDLE);
        xEventGroupSetBits(audioEvents, EVT_AUDIO_RUNNING);

        int16_t* buf = nullptr;
        if (xQueueReceive(freeQueue, &buf, pdMS_TO_TICKS(50)) != pdTRUE) {
            continue;
        }

        bool ok = M5.Mic.record(buf, AUDIO_BUF_SAMPLES, AUDIO_SAMPLE_RATE, true);

        if (xEventGroupGetBits(audioEvents) & EVT_STOP_REQUEST) {
            xQueueSend(freeQueue, &buf, 0);
            audioCapturing = false;
            continue;
        }

        if (ok && audioCapturing) {
            if (xQueueSend(filledQueue, &buf, 0) != pdTRUE) {
                Serial.println("[AUDIO] filledQueue voll – Chunk verworfen");
                xQueueSend(freeQueue, &buf, 0);
            }
        } else {
            xQueueSend(freeQueue, &buf, 0);
        }
    }
}

// =============================================================================
// Mic / Speaker
// =============================================================================
void startCapture() {
    auto mic_cfg = M5.Mic.config();
    mic_cfg.sample_rate   = AUDIO_SAMPLE_RATE;
    mic_cfg.stereo        = false;
    mic_cfg.dma_buf_count = 8;
    mic_cfg.dma_buf_len   = AUDIO_BUF_SAMPLES;
    M5.Mic.config(mic_cfg);

    xEventGroupClearBits(audioEvents, EVT_STOP_REQUEST | EVT_AUDIO_IDLE);
    M5.Speaker.end();
    M5.Mic.begin();
    audioCapturing = true;

    Serial.println("[MIC] Aufnahme gestartet");
}

void stopCapture() {
    audioCapturing = false;
    xEventGroupSetBits(audioEvents, EVT_STOP_REQUEST);

    EventBits_t bits = xEventGroupWaitBits(
        audioEvents,
        EVT_AUDIO_IDLE,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(AUDIO_STOP_WAIT_MS)
    );

    if (!(bits & EVT_AUDIO_IDLE)) {
        Serial.println("[MIC] Warnung: audioTask wurde nicht rechtzeitig idle");
    }

    while (M5.Mic.isRecording()) {
        M5.delay(1);
    }

    M5.Mic.end();
    drainFilledQueue();
    M5.Speaker.begin();

    Serial.println("[MIC] Aufnahme gestoppt");
}

// =============================================================================
// WiFi
// =============================================================================
void saveParamsCallback() {
    shouldSaveParams = true;
}

void setupWiFi(bool forcePortal = false) {
    shouldSaveParams = false;

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
        serverIP[sizeof(serverIP) - 1] = '\0';
        Serial.printf("[WIFI] Server-IP gespeichert: %s\n", serverIP);
    } else {
        strncpy(serverIP, savedIP, sizeof(serverIP));
        serverIP[sizeof(serverIP) - 1] = '\0';
    }

    Serial.printf("[WIFI] Verbunden. IP: %s  Server: %s\n",
                  WiFi.localIP().toString().c_str(), serverIP);
}

bool serverReachable() {
    WiFiClient c;
    bool ok = c.connect(serverIP, SERVER_PORT, 2000);
    c.stop();
    return ok;
}

// =============================================================================
// Zustandsaktionen
// =============================================================================
void beginRecording() {
    Serial.println("[BTN] Start Aufnahme");

    if (!wsConnect()) {
        Serial.println("[BTN] WebSocket fehlgeschlagen");
        beepPattern(600, 80, 80, 5);
        return;
    }

    beepPattern(1000, 100, 0, 1);
    psramBufReset();
    wsAckReceived = false;
    reconnectAttempts = 0;
    lastReconnectMillis = 0;

    startCapture();
    currentState = RECORDING;
}

void finishRecording() {
    Serial.println("[BTN] Stopp Aufnahme");

    stopCapture();

    // ACK-Flag VOR dem Senden von DONE zurücksetzen.
    // Würde man es danach setzen, könnte ein sofort eintreffendes ACK
    // in ws.loop() gesetzt und danach wieder gelöscht werden (Race Condition).
    wsAckReceived = false;
    processingStartMillis = millis();

    if (wsConnected) {
        ws.sendTXT("DONE");     // Server erwartet "DONE" → schreibt WAV, sendet ACK
        ws.loop();
    }

    beepPattern(1000, 80, 60, 3);
    currentState = PROCESSING;
}

void abortRecording(const char* reason) {
    Serial.printf("[BTN] Aufnahme abgebrochen (%s)\n", reason);
    stopCapture();
    ws.disconnect();
    currentState = IDLE;
}

// =============================================================================
// Reconnect
// =============================================================================
void handleReconnecting() {
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
        uint8_t tmp[AUDIO_BUF_BYTES];
        uint32_t n;

        while ((n = psramBufRead(tmp, AUDIO_BUF_BYTES)) > 0) {
            ws.sendBIN(tmp, n);
            ws.loop();
        }

        reconnectAttempts = 0;
        currentState = RECORDING;
        Serial.println("[RECONNECT] Wiederverbunden");
        return;
    }

    if (reconnectAttempts >= WIFI_RECONNECT_MAX_ATTEMPTS) {
        Serial.println("[RECONNECT] Timeout – Aufnahme abgebrochen");
        stopCapture();
        ws.disconnect();
        beepPattern(600, 80, 80, 5);
        reconnectAttempts = 0;
        currentState = IDLE;
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

    audioEvents = xEventGroupCreate();
    if (audioEvents == nullptr) {
        Serial.println("[BOOT] EventGroup konnte nicht erstellt werden");
        while (true) { delay(1000); }
    }

    psramBufInit();
    audioQueuesInit();

    xTaskCreatePinnedToCore(audioTask, "audioTask", 4096, nullptr, 5, nullptr, 0);
    Serial.println("[BOOT] audioTask gestartet (Core 0)");

    setupWiFi();

    if (serverReachable()) {
        Serial.println("[BOOT] Server erreichbar");
        beepPattern(1000, 80, 80, 2);
    } else {
        Serial.println("[BOOT] Server NICHT erreichbar");
        beepPattern(600, 80, 80, 5);
    }

    currentState = IDLE;
    Serial.println("[BOOT] Bereit");
}

// =============================================================================
// Loop (Core 1)
// =============================================================================
void loop() {
    M5.update();
    ws.loop();

    // Langer Druck im IDLE (≥5s) → WiFi-Reset
    if (currentState == IDLE && M5.BtnA.wasReleaseFor(PORTAL_RESET_HOLD_MS)) {
        Serial.println("[BTN] WiFi-Reset");
        beepPattern(1000, 200, 100, 2);
        wm.resetSettings();
        delay(500);
        ESP.restart();
    }

    // Langer Druck im RECORDING/RECONNECTING (≥3s) → Aufnahme abbrechen
    if ((currentState == RECORDING || currentState == RECONNECTING) &&
        M5.BtnA.wasReleaseFor(LONG_PRESS_MS)) {
        abortRecording("Langdruck");
        return;
    }

    // Kurzer Druck
    if (M5.BtnA.wasReleased()) {
        if (currentState == IDLE) {
            beginRecording();
        } else if (currentState == RECORDING || currentState == RECONNECTING) {
            finishRecording();
        }
    }

    // Audio streamen (RECORDING)
    if (currentState == RECORDING && wsConnected) {
        int16_t* buf;
        while (xQueueReceive(filledQueue, &buf, 0) == pdTRUE) {
            ws.sendBIN((uint8_t*)buf, AUDIO_BUF_BYTES);
            xQueueSend(freeQueue, &buf, 0);
        }
    }

    // PSRAM puffern + Reconnect (RECONNECTING)
    if (currentState == RECONNECTING) {
        handleReconnecting();
    }

    // Warten auf ACK (PROCESSING)
    if (currentState == PROCESSING) {
        if (wsAckReceived) {
            Serial.println("[PIPELINE] Server hat Job übernommen");
            ws.disconnect();
            currentState = IDLE;
        } else if (millis() - processingStartMillis > PROCESSING_TIMEOUT_MS) {
            Serial.println("[PIPELINE] Timeout – kein ACK vom Server");
            ws.disconnect();
            beepPattern(600, 80, 80, 3);
            currentState = IDLE;
        }
    }

    delay(2);
}
