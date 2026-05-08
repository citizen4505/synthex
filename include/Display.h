/*
 * Display.h — 16×2 LCD displej pro Synthex
 *
 * Hardware: HD44780 kompatibilní modul (1602), 4-bitové rozhraní.
 *
 * POZOR — Arduino Due pracuje na 3,3 V!
 *   Napájej Vdd displeje z 5 V pinu Due (pin "5V").
 *   Datové piny (RS, EN, D4–D7) jsou 3,3 V — většina 1602
 *   modulů to přijme. Kontrast nastav trimrem 10 kΩ mezi Vdd–VO–GND.
 *
 * Zapojení (výchozí, změň makra DISPLAY_PIN_* dle potřeby):
 *
 *   1602 pin │ Arduino Due
 *   ─────────┼───────────────────────────────
 *   VSS      │ GND
 *   VDD      │ 5V
 *   VO       │ střed trimru 10 kΩ (kontrast)
 *   RS       │ D8
 *   RW       │ GND  (pevně — pouze zápis)
 *   EN       │ D9
 *   D4–D7    │ D4–D7
 *   A (BL+)  │ 5V přes 100 Ω
 *   K (BL-)  │ GND
 *
 * Layout displeje:
 *
 *   +----------------+
 *   |BL-SAW  VOL: 75%|   <- nazev vlny (8 znaku) + hlasitost [%]
 *   |P:500ms  120 BPM|   <- portamento [ms]      + tempo [BPM]
 *   +----------------+
 *
 * Pouziti:
 *   // setup()
 *   Display::getInstance().begin();
 *
 *   // loop()
 *   Display& disp = Display::getInstance();
 *   disp.setWaveType(pots.waveType());
 *   disp.setVolume(pots.amplitude());
 *   disp.setPortamento(pots.portamento());
 *   disp.setTempoMs(pots.tempoMs());
 *   disp.update();
 *
 * Obnova bez blikani:
 *   Nepouzivame lcd.clear() (~1,6 ms, viditelny zablesk).
 *   Kazdy radek je vzdy formatovan na presne 16 znaku a prepsan
 *   pres setCursor() — zadne artefakty ze stare hodnoty.
 */

#pragma once
#include <Arduino.h>
#include <LiquidCrystal.h>
#include "wavetables.h"
#include "MillisTimer.h"

// ── Piny ────────────────────────────────────────────────────────
#define DISPLAY_PIN_RS        8
#define DISPLAY_PIN_EN        9
#define DISPLAY_PIN_D4        4
#define DISPLAY_PIN_D5        5
#define DISPLAY_PIN_D6        6
#define DISPLAY_PIN_D7        7

// ── Parametry displeje ──────────────────────────────────────────
#define DISPLAY_COLS          16u
#define DISPLAY_ROWS          2u

// Interval obnovy [ms].
// 100 ms = 10 fps — dostatecne pro indikaci potu, bez zbytecne zateze.
#define DISPLAY_REFRESH_MS    100u


class Display {
public:
    // Singleton — stejny vzor jako Synthex a Pots
    static Display& getInstance();

    // Inicializace HW. Volej v setup() az po Synthex::begin() a Pots::begin().
    void begin();

    // ── Settery — nastav hodnoty pred volanim update() ─────────
    void setWaveType  (WaveType wt)      { _waveType = wt;      }
    void setVolume    (uint16_t vol)     { _volume   = vol;     }  // 0–4095
    void setPortamento(float portaMs)    { _portaMs  = portaMs; }  // 0–500 ms
    void setTempoMs   (uint32_t ms)      { _tempoMs  = ms;      }  // ms/krok

    // Volej z kazde iterace loop().
    // Obnovi displej jen po uplynuti DISPLAY_REFRESH_MS.
    // Vraci true kdyz k obnove skutecne doslo (vhodne pro debug).
    bool update();

    // Okamzite prekresleni bez cekani na timer (napr. po staru).
    void forceRedraw();

private:
    Display();
    Display(const Display&)            = delete;
    Display& operator=(const Display&) = delete;

    void _redraw();
    void _formatLine0(char* buf) const;   // radek 0: vlna + hlasitost
    void _formatLine1(char* buf) const;   // radek 1: portamento + BPM

    // Textovy nazev WaveType (max 6 znaku, doplnen mezerami na 8)
    static const char* _waveTypeName(WaveType wt);

    LiquidCrystal _lcd;
    MillisTimer   _refreshTimer;

    WaveType  _waveType;
    uint16_t  _volume;
    float     _portaMs;
    uint32_t  _tempoMs;
    bool      _initialized;
};