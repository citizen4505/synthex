/*
 * Display.cpp — implementace LCD displeje pro Synthex
 *
 * Formaty radku (presne 16 znaku):
 *
 *   Radek 0: "%-8sVOL:%3u%%"
 *     priklad: "SINE    VOL: 75%"
 *              "BL-SAW  VOL:100%"
 *
 *   Radek 1: "P:%3ums  %3u BPM"
 *     priklad: "P:  0ms  240 BPM"
 *              "P:500ms   80 BPM"
 *
 * Vypocet BPM:
 *   BPM = 60 000 / tempoMs
 *   (tempoMs je delka jednoho kroku sekvenceru v ms)
 *
 * Proc snprintf misto String/print:
 *   Na Due je snprintf rychle a prediktabilni.
 *   String() alokuje heap — v embedded kontextu s ISR riziko fragmentace.
 *   Pevne char buffery na stacku jsou bezpecne a deterministicke.
 */

#include "Display.h"
#include <stdio.h>   // snprintf — dostupne na Due (arm-none-eabi-gcc, C99)

// ─────────────────────────────────────────────
//  Singleton
// ─────────────────────────────────────────────
Display& Display::getInstance() {
    static Display instance;
    return instance;
}

Display::Display()
    : _lcd(DISPLAY_PIN_RS, DISPLAY_PIN_EN,
           DISPLAY_PIN_D4, DISPLAY_PIN_D5,
           DISPLAY_PIN_D6, DISPLAY_PIN_D7),
      _refreshTimer(DISPLAY_REFRESH_MS, /*autoReset=*/true),
      _waveType(WaveType::SINE),
      _volume(2048u),
      _portaMs(0.0f),
      _tempoMs(250u),
      _initialized(false)
{}

// ─────────────────────────────────────────────
//  begin()
// ─────────────────────────────────────────────
void Display::begin() {
    _lcd.begin(DISPLAY_COLS, DISPLAY_ROWS);
    _lcd.noCursor();
    _lcd.noBlink();

    // Uvitaci zprava na ~1,5 s pred prvnim prekreslenim.
    // Pouzivame lcd.print() bez preformátovani — jen pro start.
    _lcd.setCursor(0, 0);
    _lcd.print("  SYNTHEX v4.0  ");
    _lcd.setCursor(0, 1);
    _lcd.print(" Arduino  Due   ");

    _initialized = true;

    // Prekresleni hned po zpozdeni (blokujici — pouze jednou v setup)
    delay(1500);
    _redraw();
}

// ─────────────────────────────────────────────
//  update() — neblokovaci, polluje timer
// ─────────────────────────────────────────────
bool Display::update() {
    if (!_initialized)            return false;
    if (!_refreshTimer.expired()) return false;
    _redraw();
    return true;
}

// ─────────────────────────────────────────────
//  forceRedraw() — okamzite prekresleni
// ─────────────────────────────────────────────
void Display::forceRedraw() {
    if (_initialized) _redraw();
}

// ─────────────────────────────────────────────
//  _redraw() — zapise oba radky
//
//  Vzdy prepseme vsechny znaky obou radku — zadne zbytky ze
//  stareho obsahu. Nepouzivame clear() → zadny zablesk.
// ─────────────────────────────────────────────
void Display::_redraw() {
    // +1 pro terminujici '\0'
    char line0[DISPLAY_COLS + 1u];
    char line1[DISPLAY_COLS + 1u];

    _formatLine0(line0);
    _formatLine1(line1);

    _lcd.setCursor(0, 0);
    _lcd.print(line0);

    _lcd.setCursor(0, 1);
    _lcd.print(line1);
}

// ─────────────────────────────────────────────
//  _formatLine0 — vlna + hlasitost
//
//  Format: "%-8sVOL:%3u%%"   → presne 16 znaku
//
//  Priklady:
//    "SINE    VOL: 75%"
//    "SAW     VOL:100%"
//    "BL-SAW  VOL:  0%"
//    "NOISE   VOL: 50%"
//
//  Vypocet procent:
//    volPct = volume * 100 / 4095
//    Pro volume=4095 → 100, pro volume=0 → 0.
//    Pouzivame uint32_t aby nasobeni nepreteklo uint16_t.
// ─────────────────────────────────────────────
void Display::_formatLine0(char* buf) const {
    const uint8_t volPct = static_cast<uint8_t>(
        static_cast<uint32_t>(_volume) * 100u / 4095u
    );

    // %-8s: nazev vlny zarovnany vlevo na 8 znaku (doplnen mezerami)
    // VOL:%3u%%: "VOL:" + 3-znakove cislo + procento = 8 znaku
    // Celkem: 8 + 8 = 16 znaku
    snprintf(buf, DISPLAY_COLS + 1u, "%-8sVOL:%3u%%",
             _waveTypeName(_waveType),
             static_cast<unsigned>(volPct));
}

// ─────────────────────────────────────────────
//  _formatLine1 — portamento + BPM
//
//  Format: "P:%3ums  %3u BPM"   → presne 16 znaku
//
//  Priklady:
//    "P:  0ms  240 BPM"   (portamento=0,  tempo=250 ms)
//    "P:500ms   80 BPM"   (portamento=500, tempo=750 ms)
//    "P: 80ms  120 BPM"   (portamento=80,  tempo=500 ms)
//
//  BPM = 60 000 / tempoMs
//  Rozsah tempoMs: 80–500 ms → BPM: 120–750 (3 mista staci).
//  Portamento: 0–500 ms → 3 mista staci.
// ─────────────────────────────────────────────
void Display::_formatLine1(char* buf) const {
    const uint32_t bpm      = (_tempoMs > 0u) ? (60000u / _tempoMs) : 0u;
    const uint32_t portaInt = static_cast<uint32_t>(_portaMs);

    // "P:" (2) + %3u (3) + "ms  " (4) + %3u (3) + " BPM" (4) = 16
    snprintf(buf, DISPLAY_COLS + 1u, "P:%3ums  %3u BPM",
             static_cast<unsigned>(portaInt),
             static_cast<unsigned>(bpm));
}

// ─────────────────────────────────────────────
//  _waveTypeName — textovy nazev WaveType
//
//  Maximalni delka: 6 znaku ("BL-SAW", "SAMPLE").
//  %-8s ve _formatLine0 doplni zbyvajici mezery automaticky.
//
//  WaveType::SQUARE v tomto enginu generuje LFSR white noise,
//  proto zobrazujeme "NOISE" misto "SQUARE" — presneji popisuje zvuk.
// ─────────────────────────────────────────────
const char* Display::_waveTypeName(WaveType wt) {
    switch (wt) {
        case WaveType::SINE:            return "SINE";
        case WaveType::SAW:             return "SAW";
        case WaveType::SQUARE:          return "NOISE";    // LFSR noise
        case WaveType::TRIANGLE:        return "TRI";
        case WaveType::BANDLIMITED_SAW: return "BL-SAW";
        case WaveType::SAMPLE:          return "SAMPLE";
        default:                        return "???";
    }
}