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
RTC_NOINIT_ATTR uint32_t crash_magic;
RTC_NOINIT_ATTR char     crash_phase[24];
RTC_NOINIT_ATTR char     crash_last[112];
RTC_NOINIT_ATTR uint32_t crash_uptime;
}
