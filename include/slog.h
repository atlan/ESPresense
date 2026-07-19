// slog.h — leichtgewichtiges zentrales Logging (Syslog/UDP → seshat/Loki)
//
// Wire-Format an Alloy (seshat:514): RFC5424-UDP, MSG = "<host> <level> <text>"
//   level ∈ debug|info|warn|error|crit  (Alloy zieht host+level als Loki-Labels)
//
// ABWEICHUNG vom kanonischen slog.h (arkon-infra/esp/slog.h): dort ist der
// Loki-host-Label ein Compile-Time-#define SLOG_NAME, weil jedes Projekt dort
// EIN Gerät = EIN Firmware-Build ist. ESPresense ist eine Fleet aus ~18
// baugleichen Nodes, die alle dieselbe Firmware flashen und sich erst zur
// Laufzeit per Config (WiFiSettings "room" -> id) unterscheiden. Deshalb hier
// slog_set_name(id) zur Laufzeit statt eines Makros — Rest des Vertrags
// (Format, Boot-/Heartbeat-Verhalten) ist identisch zum Kanon.
//
// Nutzung im Projekt:
//   #include "slog.h"
//   // sobald `id` bekannt ist (main.cpp, nach id = slugify(room)):
//   slog_set_name(id.c_str());
//   slog_init();
//   slog_boot();
// Bestehenden Logger anzapfen: in Logger::write()/LoggerVprintf() slog_feed(...).
//
// AUTOMATISCH (kein Extra-Code nötig): Boot-Report + 5-min-Heartbeat.
// Core-Dump-Backtrace (wie im Kanon) ist hier NICHT aktiv: die aktuelle
// partitions_singleapp.csv hat keine coredump-Partition (bräuchte einen
// vollen USB-Reflash aller Nodes, nicht nur OTA) - reset_reason allein
// (PANIC/WDT/BROWNOUT vs. normal) reicht aber schon, um "abgestürzt vs.
// sauber neugestartet" zu unterscheiden. Ein stiller Hänger wie der
// floor_2/office_2-Vorfall vom 2026-07-19 (Netzwerk-Stack eingefroren, KEIN
// Reset) zeigt sich ohnehin nicht über reset_reason, sondern darüber, dass
// der Heartbeat ausbleibt ("Gerät still"-Alert in seshat-alert.py) - das ist
// hier bereits der Haupt-Nutzen, auch ohne Core-Dump-Partition.
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <ctype.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <cstring>

#ifndef SLOG_HEARTBEAT_MS
#define SLOG_HEARTBEAT_MS 300000   // 5 min Lebenszeichen (für "Gerät still"-Erkennung)
#endif

#ifndef SLOG_HOST
#define SLOG_HOST "10.10.20.50"   // seshat
#endif
#ifndef SLOG_PORT
#define SLOG_PORT 514
#endif
#ifndef SLOG_APP
#define SLOG_APP "espresense"
#endif
#ifndef SLOG_FW_VERSION
#  ifdef FIRMWARE
#    define SLOG_FW_VERSION FIRMWARE "-" __DATE__
#  else
#    define SLOG_FW_VERSION __DATE__
#  endif
#endif
#ifndef SLOG_NAME_MAXLEN
#define SLOG_NAME_MAXLEN 40
#endif

enum { SLOG_CRIT = 2, SLOG_ERROR = 3, SLOG_WARN = 4, SLOG_INFO = 6, SLOG_DEBUG = 7 };

inline void slog(int sev, const char *fmt, ...);   // fwd

namespace slog_detail {
inline WiFiUDP        udp;
inline portMUX_TYPE   mux = portMUX_INITIALIZER_UNLOCKED;
inline char           line[192];
inline int            linelen = 0;   // Zugriff nur unter portENTER_CRITICAL(&mux)
inline bool           udp_started = false;
inline bool           boot_done   = false;
inline SemaphoreHandle_t udp_mtx  = nullptr;   // serialisiert UDP-Versand (WiFiUDP NICHT thread-safe!)
inline char           name[SLOG_NAME_MAXLEN]  = "espresense-unnamed";
inline bool           name_set = false;

inline void hb_task(void *) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SLOG_HEARTBEAT_MS));
        if (WiFi.status() == WL_CONNECTED)
            // Einheitliches Fleet-Heartbeat-Schema (siehe arkon-infra/esp/STANDARDS.md).
            slog(SLOG_DEBUG, "heartbeat uptime=%lus free_int=%u free_ext=%u min_int=%u rssi=%d ip=%s ver=%s",
                 (unsigned long)(millis() / 1000),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (int)WiFi.RSSI(),
                 WiFi.localIP().toString().c_str(),
                 SLOG_FW_VERSION);
    }
}

inline void ensure_started() {
    if (!udp_started) {
        if (!udp_mtx) udp_mtx = xSemaphoreCreateMutex();
        udp.begin(0);
        udp_started = true;
        xTaskCreate(hb_task, "slog_hb", 3072, nullptr, 1, nullptr);   // Auto-Heartbeat
    }
}

inline const char *lvlword(int s) {
    switch (s) {
        case SLOG_CRIT:  return "crit";
        case SLOG_ERROR: return "error";
        case SLOG_WARN:  return "warn";
        case SLOG_DEBUG: return "debug";
        default:         return "info";
    }
}
inline const char *reset_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}
inline bool icontains(const char *h, const char *n) {
    for (const char *p = h; *p; ++p) {
        const char *a = p, *b = n;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { ++a; ++b; }
        if (!*b) return true;
    }
    return false;
}
inline int guess_level(const char *m) {
    if (icontains(m, "panic") || icontains(m, "error") || icontains(m, "fail") ||
        icontains(m, "timeout") || icontains(m, "crash") || icontains(m, "abort")) return SLOG_ERROR;
    if (icontains(m, "warn")) return SLOG_WARN;
    return SLOG_INFO;
}
} // namespace slog_detail

inline void slog_boot();   // fwd

// Muss VOR dem ersten slog()/slog_init() gesetzt werden, sonst laeuft der
// Host unter "espresense-unnamed" (Fallback, kein Absturz).
inline void slog_set_name(const char *n) {
    if (!n || !*n) return;
    strncpy(slog_detail::name, n, sizeof(slog_detail::name) - 1);
    slog_detail::name[sizeof(slog_detail::name) - 1] = 0;
    slog_detail::name_set = true;
}

// Optional explizit; sonst startet slog() sich selbst beim ersten Aufruf.
inline void slog_init() {
    slog_detail::ensure_started();
}

// Direkte strukturierte Lognachricht
inline void slog(int sev, const char *fmt, ...) {
    if (WiFi.status() != WL_CONNECTED) return;
    slog_detail::ensure_started();
    if (!slog_detail::boot_done) {          // Boot-Report automatisch einmalig
        slog_detail::boot_done = true;       // zuerst setzen → keine Rekursion
        slog_boot();                         // verschachteltes slog() — VOR dem Lock
    }
    // Puffer STATISCH (durch udp_mtx geschützt) statt ~500 B Stack: sonst Stack-Overflow
    // in kleinen Tasks (arduino_events bei WiFi-Events → "Stack canary watchpoint" PANIC).
    // Der Mutex serialisiert zugleich den Versand (WiFiUDP ist NICHT thread-safe).
    if (!slog_detail::udp_mtx ||
        xSemaphoreTake(slog_detail::udp_mtx, pdMS_TO_TICKS(50)) != pdTRUE) return;
    static char msg[200];
    static char pkt[300];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof(msg), fmt, ap); va_end(ap);
    int pri = 16 * 8 + (sev & 7);   // facility local0
    int n = snprintf(pkt, sizeof(pkt), "<%d>1 - %s %s - - - %s %s %s",
                     pri, slog_detail::name, SLOG_APP, slog_detail::name, slog_detail::lvlword(sev), msg);
    if (n > 0) {
        if (n > (int)sizeof(pkt)) n = sizeof(pkt);
        slog_detail::udp.beginPacket(SLOG_HOST, SLOG_PORT);
        slog_detail::udp.write((const uint8_t *)pkt, n);
        slog_detail::udp.endPacket();
    }
    xSemaphoreGive(slog_detail::udp_mtx);
}

// Boot-Report: reset-Grund + Diagnose; error-Level bei abnormalem Reset.
// (Kein Core-Dump-Backtrace hier - siehe Kommentar am Dateianfang.)
inline void slog_boot() {
    slog_detail::boot_done = true;          // markiert Auto-Report als erledigt
    slog_detail::ensure_started();
    esp_reset_reason_t r = esp_reset_reason();
    bool abnormal = (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
                     r == ESP_RST_WDT || r == ESP_RST_BROWNOUT);
    slog(abnormal ? SLOG_ERROR : SLOG_INFO,
         "boot reset=%s heap=%u minheap=%u ver=%s ip=%s rssi=%d",
         slog_detail::reset_str(r), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
         SLOG_FW_VERSION, WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
}

// Logger-Hook: Fragmente sammeln, vollständige Zeilen weiterleiten.
// In Logger::write()/LoggerVprintf() einsetzen: slog_feed(buf);
// Laengenbasierte Variante zuerst - sicher fuer Puffer, die NICHT
// null-terminiert sind (z.B. Print::write(const uint8_t*, size_t)).
inline void slog_feed(const char *frag, size_t len) {
    if (!frag || len == 0) return;
    if (!slog_detail::name_set) return;   // vor slog_set_name() noch nichts senden
    char out[192];
    for (size_t i = 0; i < len; ++i) {
        char c = frag[i];
        bool ready = false;
        portENTER_CRITICAL(&slog_detail::mux);
        if (c == '\n' || c == '\r') {
            if (slog_detail::linelen > 0) {
                int n = slog_detail::linelen;
                memcpy(out, (const void *)slog_detail::line, n);
                out[n] = 0;
                slog_detail::linelen = 0;
                ready = true;
            }
        } else if (slog_detail::linelen < (int)sizeof(slog_detail::line) - 1) {
            slog_detail::line[slog_detail::linelen++] = c;
        } else {  // Überlauf → flush + neu beginnen
            int n = slog_detail::linelen;
            memcpy(out, (const void *)slog_detail::line, n);
            out[n] = 0;
            slog_detail::line[0] = c;
            slog_detail::linelen = 1;
            ready = true;
        }
        portEXIT_CRITICAL(&slog_detail::mux);
        if (ready) {
            const char *s = out;
            while (*s == ' ' || *s == '\t' || *s == '.') ++s;   // reine Punkt/Whitespace-Zeilen überspringen
            if (*s) slog(slog_detail::guess_level(out), "%s", out);
        }
    }
}

// Bequemlichkeits-Ueberladung fuer bereits null-terminierte C-Strings
// (z.B. das Ergebnis von vsnprintf in LoggerVprintf).
inline void slog_feed(const char *frag) {
    if (!frag) return;
    slog_feed(frag, strlen(frag));
}
