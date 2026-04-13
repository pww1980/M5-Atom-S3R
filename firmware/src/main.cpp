/*
 * main.cpp – Atom VoiceS3R Aufnahmegerät
 *
 * Ablauf:
 *   1× Taste drücken → Aufnahme startet (1 Beep)
 *   1× Taste drücken → Aufnahme stoppt → WAV als HTTP-POST an Server
 *   3s lang drücken  → Aufnahme abbrechen
 *   5s lang drücken  → WiFi-Reset (Config-Portal)
 *
 * Segmentierung:
 *   Alle 30 Sekunden wird automatisch ein Segment hochgeladen.
 *   Beim Stopp wird das letzte Segment mit X-Final: 1 markiert.
 *   Der Server fügt alle Segmente zusammen und speichert ein WAV.
 */

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

// =============================================================================
// Konstanten
// =============================================================================
#define SERVER_PORT          8765
#define HTTP_UPLOAD_PATH     "/upload"

#define AUDIO_SAMPLE_RATE    16000
#define AUDIO_BUF_SAMPLES    512
#define AUDIO_BUF_BYTES      (AUDIO_BUF_SAMPLES * 2)
#define AUDIO_POOL_SIZE      64          // ~2s Puffer – reicht während HTTP-Upload

#define LONG_PRESS_MS        3000
#define PORTAL_RESET_HOLD_MS 5000
#define AUDIO_STOP_WAIT_MS   300

// Segmentgröße: 30s × 16000 Hz × 2 Bytes = 960 000 Bytes
#define SEGMENT_BYTES        (30 * AUDIO_SAMPLE_RATE * 2)
// PSRAM-Puffer: Segment + 64 KB Headroom für filledQueue-Drain beim Stopp
#define PSRAM_SEG_SIZE       (SEGMENT_BYTES + 64 * 1024)

#define UPLOAD_RETRIES       3
#define UPLOAD_RETRY_DELAY   600     // ms zwischen Retry-Versuchen

// =============================================================================
// EventGroup-Bits
// =============================================================================
static EventGroupHandle_t audioEvents = nullptr;
static constexpr EventBits_t EVT_AUDIO_IDLE    = (1 << 0);
static constexpr EventBits_t EVT_AUDIO_RUNNING = (1 << 1);
static constexpr EventBits_t EVT_STOP_REQUEST  = (1 << 2);

// =============================================================================
// Zustandsmaschine
// =============================================================================
enum State { IDLE, RECORDING, UPLOADING };
volatile State currentState = IDLE;

char serverIP[40] = "192.168.x.x";
Preferences prefs;

// =============================================================================
// Audio-Pool + Queues
// =============================================================================
static int16_t audioPool[AUDIO_POOL_SIZE][AUDIO_BUF_SAMPLES];
static QueueHandle_t freeQueue   = nullptr;
static QueueHandle_t filledQueue = nullptr;
static volatile bool audioCapturing = false;

// =============================================================================
// PSRAM-Segment-Puffer (linearer Append-Puffer)
// =============================================================================
static uint8_t*  psramBuf = nullptr;
static uint32_t  psramLen = 0;        // aktuell belegte Bytes

// =============================================================================
// Session / Upload
// =============================================================================
static char sessionId[32] = "";
static int  seqNum        = 0;
static bool finalSegment  = false;

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
    while (xQueueReceive(filledQueue, &buf, 0) == pdTRUE)
        xQueueSend(freeQueue, &buf, 0);
}

// =============================================================================
// PSRAM-Init
// =============================================================================
void psramBufInit() {
    if (!psramFound()) {
        Serial.println("[PSRAM] Nicht verfügbar – max. ~64KB Aufnahme möglich");
        return;
    }
    psramBuf = (uint8_t*)ps_malloc(PSRAM_SEG_SIZE);
    if (psramBuf) {
        Serial.printf("[PSRAM] %lu KB reserviert\n", (unsigned long)(PSRAM_SEG_SIZE / 1024));
    } else {
        Serial.println("[PSRAM] Allokierung fehlgeschlagen");
    }
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
        if (xQueueReceive(freeQueue, &buf, pdMS_TO_TICKS(50)) != pdTRUE)
            continue;

        bool ok = M5.Mic.record(buf, AUDIO_BUF_SAMPLES, AUDIO_SAMPLE_RATE, true);

        if (xEventGroupGetBits(audioEvents) & EVT_STOP_REQUEST) {
            xQueueSend(freeQueue, &buf, 0);
            audioCapturing = false;
            continue;
        }

        if (ok && audioCapturing) {
            if (xQueueSend(filledQueue, &buf, 0) != pdTRUE) {
                // filledQueue voll (passiert kurz während HTTP-Upload – akzeptabel)
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
        audioEvents, EVT_AUDIO_IDLE,
        pdFALSE, pdTRUE,
        pdMS_TO_TICKS(AUDIO_STOP_WAIT_MS)
    );
    if (!(bits & EVT_AUDIO_IDLE))
        Serial.println("[MIC] Warnung: audioTask nicht rechtzeitig idle");

    {
        unsigned long tStart = millis();
        while (M5.Mic.isRecording()) {
            if (millis() - tStart > 500) {
                Serial.println("[MIC] Warnung: isRecording() bleibt true");
                break;
            }
            M5.delay(1);
        }
    }

    M5.Mic.end();
    drainFilledQueue();
    M5.Speaker.begin();
    Serial.println("[MIC] Aufnahme gestoppt");
}

// =============================================================================
// WiFi
// =============================================================================
void saveParamsCallback() { shouldSaveParams = true; }

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
// HTTP-Hilfsfunktionen
// =============================================================================
void sendHello() {
    char url[80];
    snprintf(url, sizeof(url), "http://%s:%d/hello", serverIP, SERVER_PORT);

    HTTPClient http;
    http.begin(url);
    http.addHeader("X-Device-IP", WiFi.localIP().toString().c_str());
    http.setTimeout(3000);
    http.GET();
    http.end();
}

// =============================================================================
// HTTP-Upload
// =============================================================================
bool uploadSegment(bool isFinal) {
    if (!psramBuf || psramLen == 0) {
        Serial.println("[HTTP] Puffer leer – nichts zu senden");
        return true;
    }

    char url[80];
    snprintf(url, sizeof(url), "http://%s:%d%s", serverIP, SERVER_PORT, HTTP_UPLOAD_PATH);

    char seqStr[8];
    snprintf(seqStr, sizeof(seqStr), "%d", seqNum);

    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type",  "application/octet-stream");
    http.addHeader("X-Session-Id",  sessionId);
    http.addHeader("X-Seq-Num",     seqStr);
    http.addHeader("X-Final",       isFinal ? "1" : "0");
    http.setTimeout(15000);

    int code = http.POST(psramBuf, psramLen);
    http.end();

    if (code == 200) {
        Serial.printf("[HTTP] Segment %d OK (%lu Bytes, final=%d)\n",
                      seqNum, (unsigned long)psramLen, isFinal);
        seqNum++;
        psramLen = 0;
        return true;
    }

    Serial.printf("[HTTP] Segment %d FEHLER: HTTP %d\n", seqNum, code);
    return false;
}

// =============================================================================
// Zustandsaktionen
// =============================================================================
void beginRecording() {
    Serial.println("[BTN] Aufnahme startet");

    snprintf(sessionId, sizeof(sessionId), "sess_%lu", (unsigned long)millis());
    seqNum       = 0;
    psramLen     = 0;
    finalSegment = false;

    startCapture();
    beepPattern(1000, 100, 0, 1);
    currentState = RECORDING;
    Serial.printf("[BTN] Session: %s\n", sessionId);
}

void finishRecording() {
    Serial.println("[BTN] Aufnahme stoppt");
    stopCapture();

    // Verbleibende Chunks aus Queue noch in Puffer schreiben
    int16_t* buf;
    while (xQueueReceive(filledQueue, &buf, 0) == pdTRUE) {
        uint32_t space = PSRAM_SEG_SIZE - psramLen;
        if (space >= AUDIO_BUF_BYTES && psramBuf) {
            memcpy(psramBuf + psramLen, buf, AUDIO_BUF_BYTES);
            psramLen += AUDIO_BUF_BYTES;
        }
        xQueueSend(freeQueue, &buf, 0);
    }

    if (psramLen == 0) {
        Serial.println("[BTN] Puffer leer – nichts zu senden");
        beepPattern(600, 80, 80, 2);
        currentState = IDLE;
        return;
    }

    finalSegment = true;
    currentState = UPLOADING;
}

void abortRecording(const char* reason) {
    Serial.printf("[BTN] Abbruch: %s\n", reason);
    stopCapture();
    psramLen = 0;
    beepPattern(600, 80, 80, 3);
    currentState = IDLE;
}

void handleUploading() {
    bool ok = false;
    for (int i = 0; i < UPLOAD_RETRIES; i++) {
        ok = uploadSegment(finalSegment);
        if (ok) break;
        Serial.printf("[HTTP] Retry %d/%d\n", i + 1, UPLOAD_RETRIES);
        delay(UPLOAD_RETRY_DELAY * (i + 1));
    }

    if (!ok) {
        Serial.println("[HTTP] Upload endgültig fehlgeschlagen");
        beepPattern(600, 80, 80, 5);
        currentState = IDLE;
        return;
    }

    if (finalSegment) {
        beepPattern(1000, 80, 60, 3);   // 3 Beeps = Aufnahme abgeschlossen
        Serial.println("[HTTP] Aufnahme abgeschlossen");
        currentState = IDLE;
    } else {
        currentState = RECORDING;       // weiter aufnehmen
    }
    finalSegment = false;
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
    if (!audioEvents) {
        Serial.println("[BOOT] EventGroup fehlgeschlagen");
        while (true) delay(1000);
    }

    psramBufInit();
    audioQueuesInit();

    xTaskCreatePinnedToCore(audioTask, "audioTask", 4096, nullptr, 5, nullptr, 0);
    Serial.println("[BOOT] audioTask gestartet (Core 0)");

    WiFi.setSleep(false);   // VOR setupWiFi – verhindert Routing-Reset
    setupWiFi();

    if (serverReachable()) {
        Serial.println("[BOOT] Server erreichbar");
        sendHello();
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

    // --- Button-Handling -------------------------------------------------------

    // Langer Druck im IDLE (≥5s) → WiFi-Reset
    if (currentState == IDLE && M5.BtnA.wasReleaseFor(PORTAL_RESET_HOLD_MS)) {
        Serial.println("[BTN] WiFi-Reset");
        beepPattern(1000, 200, 100, 2);
        wm.resetSettings();
        delay(500);
        ESP.restart();
    }

    // Langer Druck im RECORDING (≥3s) → Aufnahme abbrechen
    if (currentState == RECORDING && M5.BtnA.wasReleaseFor(LONG_PRESS_MS)) {
        abortRecording("Langdruck");
        return;
    }

    // Kurzer Druck: Toggle Start/Stop
    if (M5.BtnA.wasReleased()) {
        if (currentState == IDLE) {
            beginRecording();
        } else if (currentState == RECORDING) {
            finishRecording();
        }
        // Im UPLOADING-Zustand Tastendruck ignorieren
    }

    // --- Audio in PSRAM schreiben (RECORDING) ----------------------------------
    if (currentState == RECORDING && psramBuf) {
        int16_t* buf;
        while (xQueueReceive(filledQueue, &buf, 0) == pdTRUE) {
            if (psramLen + AUDIO_BUF_BYTES <= PSRAM_SEG_SIZE) {
                memcpy(psramBuf + psramLen, buf, AUDIO_BUF_BYTES);
                psramLen += AUDIO_BUF_BYTES;
            }
            xQueueSend(freeQueue, &buf, 0);
        }

        // Segmentgrenze erreicht → automatisch hochladen
        if (psramLen >= SEGMENT_BYTES) {
            Serial.printf("[REC] Segment voll (%lu KB) – Upload\n",
                          (unsigned long)(psramLen / 1024));
            finalSegment = false;
            currentState = UPLOADING;
        }
    }

    // --- Segment hochladen (UPLOADING) -----------------------------------------
    if (currentState == UPLOADING) {
        handleUploading();
    }

    delay(2);
}
