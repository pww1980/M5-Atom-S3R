/*
 * main.cpp – Atom VoiceS3R Aufnahmegerät
 *
 * Ablauf:
 *   1× Taste drücken → Aufnahme startet (1 Beep)
 *   1× Taste drücken → Aufnahme stoppt → WAV als HTTP-POST an Server
 *   3s lang drücken  → Aufnahme abbrechen
 *   5s lang drücken  → WiFi-Reset (Config-Portal)
 *
 * Segmentierung (Double-Buffer / Ping-Pong):
 *   Alle 30 Sekunden wird automatisch ein Segment hochgeladen.
 *   Der uploadTask läuft parallel zur Aufnahme – kein Audio-Gap.
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
#define AUDIO_POOL_SIZE      64          // ~2s Puffer während HTTP-Upload

#define LONG_PRESS_MS        3000
#define PORTAL_RESET_HOLD_MS 5000
#define AUDIO_STOP_WAIT_MS   300
#define BUTTON_PIN           41          // GPIO des Tasters (active-low)

// Segmentgröße: 30s × 16000 Hz × 2 Bytes = 960 000 Bytes
#define SEGMENT_BYTES        (30 * AUDIO_SAMPLE_RATE * 2)
// PSRAM-Puffer: Segment + 64 KB Headroom
#define PSRAM_SEG_SIZE       (SEGMENT_BYTES + 64 * 1024)

#define UPLOAD_RETRIES       3
#define UPLOAD_RETRY_DELAY   600         // ms zwischen Retry-Versuchen

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
// Double-Buffer (PSRAM Ping-Pong)
// =============================================================================
static uint8_t*     psramBuf[2] = {nullptr, nullptr};
static uint32_t     psramLen[2] = {0, 0};
static volatile int recBufIdx   = 0;     // Buffer der gerade beschrieben wird

// =============================================================================
// Session / Upload
// =============================================================================
static char sessionId[32] = "";
static int  seqNum        = 0;

struct UploadJob { int bufIdx; int seq; bool isFinal; };
static QueueHandle_t     uploadJobQueue = nullptr;
static SemaphoreHandle_t uploadDoneSem  = nullptr;
static volatile bool     uploadFinalOk  = false;

// =============================================================================
// Button-Zustand (muss vor setup() deklariert sein)
// =============================================================================
static bool     _btnPrev      = HIGH;
static uint32_t _btnPressedAt = 0;
static bool     _btnEvent     = false;
static uint32_t _btnHeldMs    = 0;

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
// PSRAM-Init (zwei Puffer)
// =============================================================================
void psramBufInit() {
    if (!psramFound()) {
        Serial.println("[PSRAM] Nicht verfügbar – max. ~64KB Aufnahme möglich");
        return;
    }
    for (int i = 0; i < 2; i++) {
        psramBuf[i] = (uint8_t*)ps_malloc(PSRAM_SEG_SIZE);
        if (psramBuf[i]) {
            Serial.printf("[PSRAM] Puffer %d: %lu KB reserviert\n",
                          i, (unsigned long)(PSRAM_SEG_SIZE / 1024));
        } else {
            Serial.printf("[PSRAM] Puffer %d: Allokierung fehlgeschlagen\n", i);
        }
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

        bool ok = M5.Mic.record(buf, AUDIO_BUF_SAMPLES, AUDIO_SAMPLE_RATE, false);

        if (xEventGroupGetBits(audioEvents) & EVT_STOP_REQUEST) {
            xQueueSend(freeQueue, &buf, 0);
            audioCapturing = false;
            continue;
        }

        if (ok && audioCapturing) {
            if (xQueueSend(filledQueue, &buf, 0) != pdTRUE)
                xQueueSend(freeQueue, &buf, 0);  // Queue voll: Frame verwerfen
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
    delay(150);          // DAC settle – verhindert Fiepen beim Umschalten Mic→Speaker
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

void sendLog(const char* msg) {
    char url[80];
    snprintf(url, sizeof(url), "http://%s:%d/log", serverIP, SERVER_PORT);
    HTTPClient http;
    http.begin(url);
    http.addHeader("X-Message", msg);
    http.setTimeout(2000);
    http.GET();
    http.end();
}

void sendStatus(const char* event) {
    char url[80];
    snprintf(url, sizeof(url), "http://%s:%d/status", serverIP, SERVER_PORT);
    HTTPClient http;
    http.begin(url);
    http.addHeader("X-Event",      event);
    http.addHeader("X-Session-Id", sessionId);
    http.setTimeout(3000);
    http.GET();
    http.end();
}

// =============================================================================
// HTTP-Upload
// =============================================================================
bool uploadSegment(int bufIdx, int seq, bool isFinal) {
    // Leeres nicht-finales Segment überspringen
    if (!psramBuf[bufIdx] || (!isFinal && psramLen[bufIdx] == 0)) {
        Serial.printf("[HTTP] Seg %d übersprungen (leer)\n", seq);
        return true;
    }

    char url[80];
    snprintf(url, sizeof(url), "http://%s:%d%s", serverIP, SERVER_PORT, HTTP_UPLOAD_PATH);

    char seqStr[8];
    snprintf(seqStr, sizeof(seqStr), "%d", seq);

    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type",  "application/octet-stream");
    http.addHeader("X-Session-Id",  sessionId);
    http.addHeader("X-Seq-Num",     seqStr);
    http.addHeader("X-Final",       isFinal ? "1" : "0");
    http.setTimeout(15000);

    int code = http.POST(psramBuf[bufIdx], psramLen[bufIdx]);
    http.end();

    if (code == 200) {
        Serial.printf("[HTTP] Seg %d OK (%lu Bytes, final=%d)\n",
                      seq, (unsigned long)psramLen[bufIdx], isFinal);
        return true;
    }

    Serial.printf("[HTTP] Seg %d FEHLER: HTTP %d\n", seq, code);
    return false;
}

// =============================================================================
// Upload-Task (Core 1) – läuft parallel zur Aufnahme
// =============================================================================
void uploadTask(void* /*param*/) {
    for (;;) {
        UploadJob job;
        xQueueReceive(uploadJobQueue, &job, portMAX_DELAY);

        bool ok = false;
        for (int i = 0; i < UPLOAD_RETRIES; i++) {
            ok = uploadSegment(job.bufIdx, job.seq, job.isFinal);
            if (ok) break;
            Serial.printf("[HTTP] Retry %d/%d (Seg %d)\n", i + 1, UPLOAD_RETRIES, job.seq);
            vTaskDelay(pdMS_TO_TICKS(UPLOAD_RETRY_DELAY * (i + 1)));
        }

        if (!ok)
            Serial.printf("[HTTP] Seg %d endgültig fehlgeschlagen\n", job.seq);

        psramLen[job.bufIdx] = 0;   // Puffer freigeben

        if (job.isFinal) {
            uploadFinalOk = ok;
            xSemaphoreGive(uploadDoneSem);
        }
    }
}

// =============================================================================
// Zustandsaktionen
// =============================================================================
void beginRecording() {
    Serial.println("[BTN] Aufnahme startet");
    snprintf(sessionId, sizeof(sessionId), "sess_%lu", (unsigned long)millis());
    sendStatus("recording_start");
    seqNum      = 0;
    recBufIdx   = 0;
    psramLen[0] = 0;
    psramLen[1] = 0;

    startCapture();
    beepPattern(1000, 100, 0, 1);
    currentState = RECORDING;
    Serial.printf("[BTN] Session: %s\n", sessionId);
}

void finishRecording() {
    Serial.println("[BTN] Aufnahme stoppt");
    sendStatus("recording_stop");
    stopCapture();

    // Verbleibende Chunks aus Queue in aktuellen Puffer schreiben
    int16_t* buf;
    while (xQueueReceive(filledQueue, &buf, 0) == pdTRUE) {
        uint32_t space = PSRAM_SEG_SIZE - psramLen[recBufIdx];
        if (space >= AUDIO_BUF_BYTES && psramBuf[recBufIdx]) {
            memcpy(psramBuf[recBufIdx] + psramLen[recBufIdx], buf, AUDIO_BUF_BYTES);
            psramLen[recBufIdx] += AUDIO_BUF_BYTES;
        }
        xQueueSend(freeQueue, &buf, 0);
    }

    // Finales Segment einreihen (leeres Segment signalisiert Session-Ende)
    UploadJob job = { recBufIdx, seqNum++, true };
    xQueueSend(uploadJobQueue, &job, portMAX_DELAY);
    currentState = UPLOADING;
}

void abortRecording(const char* reason) {
    Serial.printf("[BTN] Abbruch: %s\n", reason);
    stopCapture();
    // Ausstehende Upload-Jobs verwerfen
    UploadJob dummy;
    while (xQueueReceive(uploadJobQueue, &dummy, 0) == pdTRUE) {}
    psramLen[0] = 0;
    psramLen[1] = 0;
    beepPattern(600, 80, 80, 3);
    currentState = IDLE;
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

    uploadJobQueue = xQueueCreate(3, sizeof(UploadJob));
    uploadDoneSem  = xSemaphoreCreateBinary();

    // INPUT_PULLUP: interner Pull-up aktivieren.
    // M5.begin() setzt den Pin bereits auf Pull-up; pinMode(INPUT) würde ihn
    // wieder deaktivieren → GPIO bleibt nach dem Loslassen LOW → kein HIGH-Flankenwechsel.
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    delay(50);
    _btnPrev = digitalRead(BUTTON_PIN);
    Serial.printf("[BOOT] Button GPIO %d, Startzustand: %s\n",
                  BUTTON_PIN, _btnPrev == HIGH ? "HIGH" : "LOW");

    xTaskCreatePinnedToCore(audioTask,  "audioTask",  4096, nullptr, 5, nullptr, 0);
    xTaskCreatePinnedToCore(uploadTask, "uploadTask", 8192, nullptr, 2, nullptr, 1);
    Serial.println("[BOOT] Tasks gestartet (audioTask Core0, uploadTask Core1)");

    WiFi.setSleep(false);
    setupWiFi();

    delay(500);

    bool reachable = false;
    for (int attempt = 1; attempt <= 5 && !reachable; attempt++) {
        reachable = serverReachable();
        if (!reachable) {
            Serial.printf("[BOOT] Server nicht erreichbar (Versuch %d/5)\n", attempt);
            delay(1000);
        }
    }

    if (reachable) {
        Serial.println("[BOOT] Server erreichbar");
        sendHello();
        char logMsg[64];
        snprintf(logMsg, sizeof(logMsg), "Boot OK – Button GPIO %d Startzustand: %s",
                 BUTTON_PIN, _btnPrev == HIGH ? "HIGH" : "LOW");
        sendLog(logMsg);
        beepPattern(1000, 80, 80, 2);
    } else {
        Serial.println("[BOOT] Server NICHT erreichbar");
        beepPattern(600, 80, 80, 5);
    }

    currentState = IDLE;
    Serial.println("[BOOT] Bereit");
}

// =============================================================================
// Button – direktes GPIO-Reading (umgeht M5Unified-Board-Detection)
// =============================================================================
void updateButton() {
    bool raw = digitalRead(BUTTON_PIN);
    _btnEvent = false;

    if (raw == _btnPrev) return;

    uint32_t now = millis();

    if (_btnPrev == HIGH && raw == LOW) {
        // Flanke: offen → gedrückt
        _btnPressedAt = now;
    } else if (_btnPrev == LOW && raw == HIGH) {
        // Flanke: gedrückt → losgelassen
        _btnHeldMs = now - _btnPressedAt;
        _btnEvent  = true;
        // Kein sendLog hier – würde loop() bis zu 2s blockieren
    }
    _btnPrev = raw;
}

// =============================================================================
// Loop (Core 1)
// =============================================================================
void loop() {
    M5.update();
    updateButton();

    // --- Button-Handling -------------------------------------------------------
    if (_btnEvent) {
        Serial.printf("[BTN] Release nach %lums\n", (unsigned long)_btnHeldMs);

        if (_btnHeldMs >= PORTAL_RESET_HOLD_MS && currentState == IDLE) {
            Serial.println("[BTN] WiFi-Reset");
            beepPattern(1000, 200, 100, 2);
            wm.resetSettings();
            delay(500);
            ESP.restart();

        } else if (_btnHeldMs >= LONG_PRESS_MS && currentState == RECORDING) {
            abortRecording("Langdruck");

        } else {
            if (currentState == IDLE) {
                beginRecording();
            } else if (currentState == RECORDING) {
                finishRecording();
            }
            // Im UPLOADING-Zustand Tastendruck ignorieren
        }
    }

    // --- Audio in PSRAM schreiben (RECORDING) ----------------------------------
    if (currentState == RECORDING && psramBuf[recBufIdx]) {
        int16_t* buf;
        while (xQueueReceive(filledQueue, &buf, 0) == pdTRUE) {
            if (psramLen[recBufIdx] + AUDIO_BUF_BYTES <= PSRAM_SEG_SIZE) {
                memcpy(psramBuf[recBufIdx] + psramLen[recBufIdx], buf, AUDIO_BUF_BYTES);
                psramLen[recBufIdx] += AUDIO_BUF_BYTES;
            }
            xQueueSend(freeQueue, &buf, 0);
        }

        // Segmentgrenze → Puffer wechseln, Upload starten
        if (psramLen[recBufIdx] >= SEGMENT_BYTES) {
            int uploadBuf = recBufIdx;
            recBufIdx     = 1 - recBufIdx;
            psramLen[recBufIdx] = 0;

            UploadJob job = { uploadBuf, seqNum++, false };
            if (xQueueSend(uploadJobQueue, &job, pdMS_TO_TICKS(500)) != pdTRUE) {
                // uploadTask noch beschäftigt – Segment verwerfen
                Serial.println("[REC] WARNUNG: Upload-Queue voll – Segment verworfen");
                psramLen[uploadBuf] = 0;
            } else {
                Serial.printf("[REC] Seg %d → uploadTask (Buf %d)\n", job.seq, uploadBuf);
            }
        }
    }

    // --- Auf finalen Upload warten (UPLOADING) ---------------------------------
    if (currentState == UPLOADING) {
        if (xSemaphoreTake(uploadDoneSem, 0) == pdTRUE) {
            if (uploadFinalOk) {
                beepPattern(1000, 80, 60, 3);
                Serial.println("[HTTP] Aufnahme vollständig hochgeladen");
            } else {
                beepPattern(600, 80, 80, 5);
                Serial.println("[HTTP] Finaler Upload fehlgeschlagen");
            }
            currentState = IDLE;
        }
    }

    delay(2);
}
