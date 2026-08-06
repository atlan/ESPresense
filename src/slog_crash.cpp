// Definition des absturzfesten Kontexts aus slog.h.
//
// Warum eine eigene Uebersetzungseinheit: `RTC_NOINIT_ATTR inline` im Header
// funktioniert nicht - inline-Variablen werden als COMDAT emittiert und das
// section-Attribut geht dabei verloren; die Symbole landen im .bss und werden
// beim Boot genullt. Genau EINE echte Definition erhaelt das Attribut.
//
// Gegenprobe nach jeder Aenderung:
//   xtensa-esp32-elf-nm -S .pio/build/esp32/firmware.elf | grep crash_
// Die Adressen muessen im .rtc_noinit-Bereich (0x5000xxxx) liegen, NICHT bei
// 0x3ffcxxxx.
#include "slog.h"

namespace slog_detail {
RTC_NOINIT_ATTR uint32_t     crash_magic;
RTC_NOINIT_ATTR char         crash_phase[24];
RTC_NOINIT_ATTR uint32_t     crash_phase_at;
RTC_NOINIT_ATTR uint32_t     crash_last_at;
RTC_NOINIT_ATTR uint32_t     crash_ring_seq;
RTC_NOINIT_ATTR crash_line_t crash_ring[SLOG_CRASH_RING_N];
RTC_NOINIT_ATTR char         crash_item[40];
RTC_NOINIT_ATTR uint32_t     crash_item_at;
RTC_NOINIT_ATTR uint8_t      crash_seen_addr[6];
RTC_NOINIT_ATTR uint32_t     crash_seen_at;
}
