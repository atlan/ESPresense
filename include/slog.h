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
// AUTOMATISCH (kein Extra-Code nötig): Boot-Report + 5-min-Heartbeat +
// Absturzkontext (Phase + letzte Logzeile, siehe crash_* unten).
//
// Core-Dump-Backtrace (wie im Kanon) ist hier NICHT möglich:
// partitions_singleapp.csv hat keine coredump-Partition, und nachrüsten liesse
// sie sich nur per USB-Reflash JEDES Knotens - die Partitionstabelle liegt bei
// 0x8000 und wird von OTA nicht angefasst. (Platz wäre da: zwischen app1 und
// eeprom liegen 128 KB brach, 0x3d0000..0x3f0000. Wer die Flotte ohnehin einmal
// per USB anfasst, kann die Partition also mitnehmen.)
// Ersatz ohne Partitionsänderung: RTC-NOINIT-Kontext - er überlebt PANIC/WDT
// und sagt, WAS der Knoten tat, als er starb. Kein Backtrace, aber die Frage,
// die nach einem Absturzschwarm zählt (2026-07-28: 15 Knoten auf einmal).
//
// Ein stiller Hänger wie der floor_2/office_2-Vorfall vom 2026-07-19
// (Netzwerk-Stack eingefroren, KEIN Reset) zeigt sich ohnehin nicht über
// reset_reason, sondern über den ausbleibenden Heartbeat ("Gerät still"-Alert).
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

// ── Absturzkontext, der einen PANIC überlebt ────────────────────────────────
// Eine Core-Dump-Partition hat diese Flotte nicht, und sie lässt sich auch
// nicht nachrüsten: die Partitionstabelle liegt bei 0x8000 und wird von OTA
// nicht angefasst - jeder Knoten bräuchte einen USB-Reflash (18 Stück, in 18
// Räumen). RTC-Speicher mit NOINIT übersteht dagegen jeden Reset ausser dem
// Stromausfall. Das liefert keinen Backtrace, aber die Antwort auf die Frage,
// die nach einem Absturzschwarm zählt: WAS tat der Knoten, als er starb.
// (Dasselbe Mittel hat 2026-06 bei maat die Absturzphase gezeigt.)
// ⚠ NUR DEKLARIERT, definiert in src/slog_crash.cpp. `RTC_NOINIT_ATTR inline`
// funktioniert NICHT: inline-Variablen landen als COMDAT im .bss, das
// section-Attribut faellt still weg. Am fertigen Binary geprueft - die Symbole
// lagen bei 0x3ffc… (DRAM, wird beim Boot genullt) statt im .rtc_noinit bei
// 0x50000400. Der Absturzkontext waere bei jedem Reset spurlos verschwunden,
// ohne dass irgendetwas gemeckert haette.
#define SLOG_CRASH_MAGIC 0x5C0DE001u
extern RTC_NOINIT_ATTR uint32_t crash_magic;
extern RTC_NOINIT_ATTR char     crash_phase[24];
extern RTC_NOINIT_ATTR char     crash_last[112];
extern RTC_NOINIT_ATTR uint32_t crash_uptime;

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

// Aktuelle Phase vermerken - überlebt einen PANIC. Absichtlich das Billigste,
// was geht (ein strncpy in RTC-RAM, kein Lock, keine Allokation): das darf auch
// im BLE-Scan-Task und in MQTT-Callbacks stehen, ohne etwas zu kosten.
inline void slog_phase(const char *p) {
    if (!p) return;
    strncpy(slog_detail::crash_phase, p, sizeof(slog_detail::crash_phase) - 1);
    slog_detail::crash_phase[sizeof(slog_detail::crash_phase) - 1] = 0;
    slog_detail::crash_uptime = (uint32_t)(millis() / 1000);
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
// Bei abnormalem Reset zusätzlich der Absturzkontext aus dem RTC-Speicher
// (siehe crash_* oben) - das ersetzt den fehlenden Core-Dump so weit es geht.
inline void slog_boot() {
    slog_detail::boot_done = true;          // markiert Auto-Report als erledigt
    slog_detail::ensure_started();
    esp_reset_reason_t r = esp_reset_reason();
    bool abnormal = (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
                     r == ESP_RST_WDT || r == ESP_RST_BROWNOUT);

    // Kontext AUSLESEN, bevor irgendetwas ihn überschreibt. Gültig nur, wenn die
    // Marke steht - nach einem Stromausfall ist RTC-RAM Zufall, kein Zustand.
    bool ctx_ok = (slog_detail::crash_magic == SLOG_CRASH_MAGIC);
    char phase[sizeof(slog_detail::crash_phase)];
    char last[sizeof(slog_detail::crash_last)];
    uint32_t up = slog_detail::crash_uptime;
    if (ctx_ok) {
        memcpy(phase, slog_detail::crash_phase, sizeof(phase)); phase[sizeof(phase) - 1] = 0;
        memcpy(last,  slog_detail::crash_last,  sizeof(last));  last[sizeof(last) - 1]  = 0;
        for (char *c = phase; *c; ++c) if (*c < 32 || *c > 126) { *c = 0; break; }
        for (char *c = last;  *c; ++c) if (*c < 32 || *c > 126) { *c = 0; break; }
    }

    slog(abnormal ? SLOG_ERROR : SLOG_INFO,
         "boot reset=%s heap=%u minheap=%u ver=%s ip=%s rssi=%d",
         slog_detail::reset_str(r), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
         SLOG_FW_VERSION, WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());

    // Eigene Zeile, damit die bestehende Auswertung ("boot reset") unverändert
    // greift und der Kontext trotzdem durchsuchbar ist ("crashctx").
    if (abnormal && ctx_ok && phase[0])
        slog(SLOG_ERROR, "crashctx phase=%s uptime=%lus last=\"%s\"",
             phase, (unsigned long)up, last);

    // Frisch aufsetzen: ab jetzt gilt der Kontext dieses Laufs.
    slog_detail::crash_magic  = SLOG_CRASH_MAGIC;
    slog_detail::crash_phase[0] = 0;
    slog_detail::crash_last[0]  = 0;
    slog_detail::crash_uptime = 0;
    slog_phase("boot");
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
            if (*s) {
                // Letzte vollständige Zeile in den absturzfesten Speicher. Vor dem
                // Senden, nicht danach: stirbt der Knoten IM Sendepfad, ist genau
                // diese Zeile die interessante.
                strncpy(slog_detail::crash_last, s, sizeof(slog_detail::crash_last) - 1);
                slog_detail::crash_last[sizeof(slog_detail::crash_last) - 1] = 0;
                slog_detail::crash_uptime = (uint32_t)(millis() / 1000);
                slog(slog_detail::guess_level(out), "%s", out);
            }
        }
    }
}

// Bequemlichkeits-Ueberladung fuer bereits null-terminierte C-Strings
// (z.B. das Ergebnis von vsnprintf in LoggerVprintf).
inline void slog_feed(const char *frag) {
    if (!frag) return;
    slog_feed(frag, strlen(frag));
}
