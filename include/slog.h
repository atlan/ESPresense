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
//   void setup() {
//     slog_crash_capture();          // ERSTE Anweisung: Absturzkontext retten
//     ...
//     slog_set_name(id.c_str());     // sobald `id` bekannt ist — ab hier fuellt
//                                    // sich der Absturz-Ring, auch ohne WLAN
//     slog_init(); slog_boot();      // sobald das Netz steht
//   }
// Bestehenden Logger anzapfen: in Logger::write()/LoggerVprintf() slog_feed(...).
//
// AUTOMATISCH (kein Extra-Code nötig): Boot-Report + 5-min-Heartbeat +
// Absturzkontext (Phase + letzte Logzeile, siehe crash_* unten).
//
// ★ Core-Dump-Backtrace (wie im Kanon) ist hier NICHT möglich - und zwar aus
// einem tieferen Grund als der fehlenden Partition. Am 2026-07-28 nachgemessen:
// dieses Projekt baut auf tasmota/platform-espressif32 2023.07 (Arduino 2.x),
// dessen sdkconfig.h CONFIG_ESP_COREDUMP_ENABLE_TO_NONE setzt. Die mitgelieferte
// libespcoredump.a enthält NULL Definitionen von esp_core_dump_image_check() -
// die Implementierung ist gar nicht kompiliert, der Panic-Handler schreibt also
// nie einen Dump. Eine coredump-Partition (Platz wäre da: 128 KB brach zwischen
// app1 und eeprom, 0x3d0000..0x3f0000) würde daran NICHTS ändern.
// Voraussetzung wäre ein Wechsel auf pioarduino/Arduino 3.x wie bei horus & Co.
// - ein eigenes Vorhaben mit echtem Risiko (NimBLE, AsyncTCP, Upstream-Merges),
// nicht "eine Zeile in der CSV".
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
// Fehlgeschlagene UDP-Sendungen seit dem Start. Steht im Heartbeat, damit
// "Zeile fehlt" von "Zeile nie geschrieben" unterscheidbar wird.
inline volatile uint32_t tx_fail = 0;
// Pause zwischen den nachgereichten crashtail-Zeilen. Am 03.08.2026 kamen von acht
// Zeilen nur 1 bzw. 2 bzw. 5 an. Gegenprobe vom Mac: derselbe Schwall erreicht Loki
// vollstaendig, die Sammelstelle ist also unschuldig — es ist der Knoten, der sie
// unmittelbar nach dem WLAN-Aufbau nicht alle los wird.
#ifndef SLOG_CRASHTAIL_GAP_MS
#define SLOG_CRASHTAIL_GAP_MS 40
#endif

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
//
// ⚠⚠ MAGIC MITZIEHEN, wenn sich das Layout aendert. Ein OTA endet in ESP.restart(),
// und RTC-NOINIT ueberlebt genau das: die neue Firmware findet den RTC-Block der
// ALTEN vor. Bei gleicher Marke wuerde sie fremde Bytes nach neuem Layout deuten
// und einen frei erfundenen Absturzkontext melden. 002 = Ringpuffer + zwei
// getrennte Zeitstempel (03.08.2026).
#define SLOG_CRASH_MAGIC 0x5C0DE002u

// Wie viele der letzten Logzeilen einen PANIC ueberleben sollen.
// ⚠ Kostet RTC-NOINIT *und* dieselbe Menge DRAM (die Kopie, siehe snap_* unten).
#ifndef SLOG_CRASH_RING_N
#define SLOG_CRASH_RING_N 8
#endif
#define SLOG_CRASH_LINE 108          // Nutzlaenge je Zeile; mit `at` = 112 B/Eintrag

struct crash_line_t {
    uint32_t at;                     // Uptime in s, als die Zeile geschrieben wurde
    char     txt[SLOG_CRASH_LINE];
};

extern RTC_NOINIT_ATTR uint32_t     crash_magic;
extern RTC_NOINIT_ATTR char         crash_phase[24];
// ★ ZWEI Zeitstempel statt einem. Vorher setzten slog_phase() UND jede geloggte
// Zeile dasselbe `crash_uptime` — der Wert hiess also "das spaetere von beidem"
// und war nicht deutbar. Am 03.08.2026 fuehrte genau das in die Irre: gemeldet
// wurde `phase=net-up uptime=15s`, und 15 s war der Zeitpunkt der Phase, waehrend
// die letzte Zeile aus einem ganz anderen Lauf stammte.
extern RTC_NOINIT_ATTR uint32_t     crash_phase_at;   // Phase betreten
extern RTC_NOINIT_ATTR uint32_t     crash_last_at;    // letzte Zeile geschrieben
extern RTC_NOINIT_ATTR uint32_t     crash_ring_seq;   // Schreibzaehler, nie zurueckgesetzt
extern RTC_NOINIT_ATTR crash_line_t crash_ring[SLOG_CRASH_RING_N];

// ── Kopie im DRAM ───────────────────────────────────────────────────────────
// ★★ WARUM eine Kopie: bis 03.08.2026 raeumte erst `slog_boot()` den RTC-Block
// auf — die Funktion also, die bei einem fruehen Absturz gar nicht mehr laeuft.
// Folge: ein Knoten meldete Phase und Zeitstempel aus dem AKTUELLEN Start und
// die letzte Logzeile aus dem VORHERIGEN, ohne dass man das ansehen konnte. Das
// liest sich wie ein Befund und ist keiner. Jetzt wird der Block ganz am Anfang
// von setup() nach DRAM gerettet und sofort neu aufgesetzt; gemeldet wird
// ausschliesslich aus der Kopie. Danach kann nichts mehr zwei Laeufe mischen.
inline bool         snap_done  = false;   // Rettung erledigt (idempotent)
inline bool         snap_valid = false;   // Marke stand → Inhalt ist echter Zustand
inline char         snap_phase[24] = {0};
inline uint32_t     snap_phase_at = 0;
inline uint32_t     snap_last_at  = 0;
inline uint32_t     snap_n = 0;                        // gueltige Eintraege, alt → neu
inline crash_line_t snap_ring[SLOG_CRASH_RING_N] = {};

inline void hb_task(void *) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SLOG_HEARTBEAT_MS));
        if (WiFi.status() == WL_CONNECTED)
            // Einheitliches Fleet-Heartbeat-Schema (siehe arkon-infra/esp/STANDARDS.md).
            slog(SLOG_DEBUG, "heartbeat uptime=%lus free_int=%u free_ext=%u min_int=%u rssi=%d ip=%s ver=%s txfail=%lu",
                 (unsigned long)(millis() / 1000),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (int)WiFi.RSSI(),
                 WiFi.localIP().toString().c_str(),
                 SLOG_FW_VERSION,
                 (unsigned long)tx_fail);
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
    slog_detail::crash_phase_at = (uint32_t)(millis() / 1000);
}

// Text aus dem RTC-Speicher saeubern: nach einem Stromausfall steht dort Zufall,
// und ein abgeschnittener strncpy kann mitten in einem Zeichen enden. Ab dem
// ersten nicht druckbaren Byte wird abgeschnitten.
inline void slog_sanitize(char *s) {
    for (char *c = s; *c; ++c)
        if (*c < 32 || *c > 126) { *c = 0; break; }
}

// ★★ Absturzkontext retten und den RTC-Block fuer DIESEN Lauf neu aufsetzen.
// GANZ AN DEN ANFANG von setup() — vor allem, was loggen koennte. Idempotent;
// slog_boot() ruft sie notfalls selbst auf, damit der Kanon-Vertrag
// (set_name/init/boot genuegt) weiter gilt.
inline void slog_crash_capture() {
    if (slog_detail::snap_done) return;
    slog_detail::snap_done = true;

    // ⚠ Nur lesen, wenn die Marke steht: nach einem Stromausfall ist RTC-RAM
    // Zufall, kein Zustand. Und die Marke traegt die Layout-Version (s.o.).
    slog_detail::snap_valid = (slog_detail::crash_magic == SLOG_CRASH_MAGIC);
    if (slog_detail::snap_valid) {
        memcpy(slog_detail::snap_phase, slog_detail::crash_phase, sizeof(slog_detail::snap_phase));
        slog_detail::snap_phase[sizeof(slog_detail::snap_phase) - 1] = 0;
        slog_sanitize(slog_detail::snap_phase);
        slog_detail::snap_phase_at = slog_detail::crash_phase_at;
        slog_detail::snap_last_at  = slog_detail::crash_last_at;

        // Ring in Leserichtung alt → neu aufloesen. `seq` zaehlt Schreibvorgaenge,
        // steht also auf dem naechsten zu schreibenden Platz.
        uint32_t seq = slog_detail::crash_ring_seq;
        uint32_t n   = seq < SLOG_CRASH_RING_N ? seq : SLOG_CRASH_RING_N;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t idx = (seq - n + i) % SLOG_CRASH_RING_N;
            slog_detail::snap_ring[i] = slog_detail::crash_ring[idx];
            slog_detail::snap_ring[i].txt[SLOG_CRASH_LINE - 1] = 0;
            slog_sanitize(slog_detail::snap_ring[i].txt);
        }
        slog_detail::snap_n = n;
    }

    slog_detail::crash_magic    = SLOG_CRASH_MAGIC;
    slog_detail::crash_phase[0] = 0;
    slog_detail::crash_phase_at = 0;
    slog_detail::crash_last_at  = 0;
    slog_detail::crash_ring_seq = 0;
    for (uint32_t i = 0; i < SLOG_CRASH_RING_N; ++i) {
        slog_detail::crash_ring[i].at     = 0;
        slog_detail::crash_ring[i].txt[0] = 0;
    }
    slog_phase("boot");
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
        // ★ Rueckgabewerte AUSWERTEN. Bis 04.08.2026 wurden sie verworfen — ein
        // fehlgeschlagener Versand war damit von "nie geschrieben" nicht zu
        // unterscheiden, und genau diese Frage stand bei den verlorenen
        // crashtail-Zeilen im Raum. Der Zaehler geht in den Heartbeat.
        bool ok = slog_detail::udp.beginPacket(SLOG_HOST, SLOG_PORT) == 1;
        if (ok) ok = slog_detail::udp.write((const uint8_t *)pkt, n) == (size_t)n;
        if (ok) ok = slog_detail::udp.endPacket() == 1;
        if (!ok) slog_detail::tx_fail++;
    }
    xSemaphoreGive(slog_detail::udp_mtx);
}

// Boot-Report: reset-Grund + Diagnose; error-Level bei abnormalem Reset.
// Bei abnormalem Reset zusätzlich der Absturzkontext - das ersetzt den fehlenden
// Core-Dump so weit es geht. Gemeldet wird aus der DRAM-Kopie (slog_crash_capture),
// NICHT aus dem RTC-Block: der gehoert ab setup() schon wieder diesem Lauf.
inline void slog_boot() {
    slog_detail::boot_done = true;          // markiert Auto-Report als erledigt
    slog_crash_capture();                   // idempotent - falls setup() sie ausliess
    slog_detail::ensure_started();
    esp_reset_reason_t r = esp_reset_reason();
    bool abnormal = (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
                     r == ESP_RST_WDT || r == ESP_RST_BROWNOUT);

    slog(abnormal ? SLOG_ERROR : SLOG_INFO,
         "boot reset=%s heap=%u minheap=%u ver=%s ip=%s rssi=%d",
         slog_detail::reset_str(r), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
         SLOG_FW_VERSION, WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());

    if (!abnormal || !slog_detail::snap_valid) return;

    // Eigene Zeile, damit die bestehende Auswertung ("boot reset") unverändert
    // greift und der Kontext trotzdem durchsuchbar ist ("crashctx").
    // `last=` ist der juengste Ringeintrag; leer heisst "es gab keine Logzeile in
    // diesem Lauf" — eine ehrliche Aussage, keine Zeile von vorletztem Mal.
    const char *last = slog_detail::snap_n ? slog_detail::snap_ring[slog_detail::snap_n - 1].txt : "";
    if (slog_detail::snap_phase[0])
        slog(SLOG_ERROR, "crashctx phase=%s phase_at=%lus last_at=%lus last=\"%s\"",
             slog_detail::snap_phase, (unsigned long)slog_detail::snap_phase_at,
             (unsigned long)slog_detail::snap_last_at, last);

    // Die letzten Zeilen vor dem Absturz nachreichen, alt → neu. Sie sind der
    // eigentliche Grund fuer den Ring: slog() verwirft alles, solange kein WLAN
    // steht (siehe dort), und genau die Vorfaelle, die interessieren, spielen im
    // Netzausfall. Was live nie ankam, kommt so beim naechsten Start nach.
    for (uint32_t i = 0; i < slog_detail::snap_n; ++i) {
        char *t = slog_detail::snap_ring[i].txt;
        if (!t[0]) continue;
        // ⚠ Entzerren, sonst geht der groesste Teil verloren (siehe SLOG_CRASHTAIL_GAP_MS).
        // Der Preis ist eine Drittelsekunde beim Start; der Absturzkontext ist genau die
        // Information, an der man nicht sparen will. Weit unter dem 60-s-Watchdog.
        if (i) delay(SLOG_CRASHTAIL_GAP_MS);
        // ⚠ seshat-alert.py zaehlt Reboots ueber das Vorkommen von "boot reset=".
        // Eine nachgereichte Zeile darf diese Zaehlung nicht faelschen.
        if (char *p = strstr(t, "boot reset")) p[4] = '-';
        slog(SLOG_ERROR, "crashtail %lu/%lu at=%lus \"%s\"",
             (unsigned long)(i + 1), (unsigned long)slog_detail::snap_n,
             (unsigned long)slog_detail::snap_ring[i].at, t);
    }
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
                // Zeile in den absturzfesten Ring. Vor dem Senden, nicht danach:
                // stirbt der Knoten IM Sendepfad, ist genau diese Zeile die
                // interessante. Kurzer kritischer Abschnitt, weil hier mehrere
                // Tasks schreiben (loop, scanTask, arduino_events) und ein
                // zerrissener Zaehler den ganzen Ring falsch ausrichten wuerde.
                portENTER_CRITICAL(&slog_detail::mux);
                uint32_t slot = slog_detail::crash_ring_seq % SLOG_CRASH_RING_N;
                strncpy(slog_detail::crash_ring[slot].txt, s, SLOG_CRASH_LINE - 1);
                slog_detail::crash_ring[slot].txt[SLOG_CRASH_LINE - 1] = 0;
                slog_detail::crash_ring[slot].at = (uint32_t)(millis() / 1000);
                slog_detail::crash_ring_seq++;
                slog_detail::crash_last_at = slog_detail::crash_ring[slot].at;
                portEXIT_CRITICAL(&slog_detail::mux);
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
