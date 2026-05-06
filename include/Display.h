#pragma once
#include <Arduino.h>
#include <LiquidCrystal.h>   // standardní Arduino lib — žádný I²C backpack
#include "wavetables.h"      // WaveType
#include "Sequencer.h"       // SEQ_STEPS

// ═══════════════════════════════════════════════════════════════════
//  SCM1602-V3.1 — kontrolér SPLC780D (plně HD44780-kompatibilní)
//  Rozhraní: přímé paralelní, 4-bit mód
//
//  Pinout SCM1602-V3.1 (standardní 16-pinový konektor):
//
//  Pin │ Symbol │ Due pin  │ Popis
//  ────┼────────┼──────────┼──────────────────────────────────────
//   1  │ GND    │ GND      │ napájení GND
//   2  │ VCC    │ 3.3 V *  │ napájení logiky a LCD
//   3  │ V0     │ wiper    │ kontrast — trimr 10 kΩ (GND ↔ VCC)
//   4  │ RS     │ pin 24   │ Register Select (0=cmd, 1=data)
//   5  │ R/W    │ GND      │ pevně GND → pouze zápis
//   6  │ E      │ pin 25   │ Enable — data se čtou na falling edge
//   7  │ DB0    │ —        │ nepoužito (4-bit mód)
//   8  │ DB1    │ —        │ nepoužito
//   9  │ DB2    │ —        │ nepoužito
//  10  │ DB3    │ —        │ nepoužito
//  11  │ DB4    │ pin 26   │ datový bit 4
//  12  │ DB5    │ pin 27   │ datový bit 5
//  13  │ DB6    │ pin 28   │ datový bit 6
//  14  │ DB7    │ pin 29   │ datový bit 7
//  15  │ LED+   │ 3.3 V †  │ backlight anoda (s rezistorem)
//  16  │ LED-   │ GND      │ backlight katoda
//
//  * Napájení 3.3 V (ne 5 V):
//    Due GPIO výstupy = 3.3 V.  SPLC780D pracuje s VCC 2.7–5.5 V.
//    Při VCC = 3.3 V je V_IH(min) = 0.7 × 3.3 = 2.31 V.
//    Due 3.3 V výstupy (typicky 3.1–3.3 V) tento práh splňují.
//    → Žádný level-shifter není potřeba.
//    Pokud napájíš LCD z 5 V, přidej level-shifter 74LVC245 nebo
//    TXS0108E na RS, E, DB4–DB7 (Due 3.3 V → 5 V logika).
//
//  † Backlight rezistor:
//    Typický proud LEDky backlight = 15–20 mA.
//    R = (VCC - V_f) / I = (3.3 - 2.1) / 0.015 ≈ 80 Ω → použij 82 Ω.
//    Nebo ovládej brightness přes PWM (např. pin 30 → transistor BC547).
//
//  Výběr pinů — záměrně vyhýbá konfliktům s ostatním HW fáze 5:
//    74HC595 (LED pruh): pin 10 (RCLK), 11 (SER), 12 (SRCLK)
//    MPR121  (I²C):      pin 20 (SDA),  21 (SCL)
//    ADC     (poty):     A0–A3
//    TC3 ISR (Synthex):  interní → žádné GPIO
//    → Zvoleno: pin 24–29 (žádný jiný modul fáze 4/5 tyto nepoužívá)
// ═══════════════════════════════════════════════════════════════════

#define DISP_RS_PIN   24u
#define DISP_EN_PIN   25u
#define DISP_D4_PIN   26u
#define DISP_D5_PIN   27u
#define DISP_D6_PIN   28u
#define DISP_D7_PIN   29u

#define DISP_COLS     16u
#define DISP_ROWS      2u

// Po kolika ms bez aktivity se EDIT pohled přepne zpět na LIVE
#define DISP_EDIT_TIMEOUT_MS  3000u

// ─────────────────────────────────────────────────────────────────
//  Vlastní znaky v CGRAM (SPLC780D má 8 slotů, index 0–7)
//
//  CGRAM: 64 B celkem, každý znak = 5×8 px = 8 bytů (bit4–bit0 platné)
//  Znaky se zapíší jednou v begin() a zůstávají po celou dobu.
//  V řetězcích je použij jako char(DISP_CGRAM_xxx).
//
//  Slot 0 — PLAY ▶  (trojúhelník vpravo)
//  Slot 1 — STOP ■  (plný blok)
//  Slot 2 — ACTIVE  (aktivní krok sekvenceru, tučný blok ▮)
//  Slot 3 — HEAD    (playhead šipka ▸ tenčí varianta)
// ─────────────────────────────────────────────────────────────────
#define DISP_CGRAM_PLAY    0u
#define DISP_CGRAM_STOP    1u
#define DISP_CGRAM_ACTIVE  2u
#define DISP_CGRAM_HEAD    3u

// ─────────────────────────────────────────────────────────────────
//  Pohled displeje
// ─────────────────────────────────────────────────────────────────
enum class DisplayView : uint8_t {
    LIVE = 0,   // přehled všech 16 kroků + BPM + průběh + stav
    EDIT = 1,   // detail vybraného kroku (nota, amplituda, vlna)
};

// ─────────────────────────────────────────────────────────────────
//  Display
//
//  Architektura (change-detection):
//    Všechny render metody plní pouze char buffery _new0/_new1.
//    _writeBuf() pak porovná každý řádek se _old0/_old1 (strcmp).
//    LCD se přepisuje JEDINĚ pokud se obsah změnil →
//      - nulové blikání způsobené zbytečnými setCursor+print
//      - minimální I/O zátěž smyčky (paralelní zápis ~370 µs / řádek)
//
//  ── LIVE pohled ─────────────────────────────────────────────────
//  Řádek 0:  "120BPM BSW [▶]  "   (16 znaků)
//              BPM   průběh stav
//
//  Řádek 1:  16 znaků, každý = jeden krok sekvenceru:
//    char(DISP_CGRAM_ACTIVE) = aktivní krok ▮
//    '.'                     = neaktivní krok
//    char(DISP_CGRAM_HEAD)   = playhead na neaktivním kroku
//    '*'                     = playhead na aktivním kroku
//    '#'                     = vybraný krok pro editaci
//
//  ── EDIT pohled (zobrazí se 3 s po každém stisku klávesy) ───────
//  Řádek 0:  "S05  E4  AMP:512"
//  Řádek 1:  "WAVE:BSW  120BPM"
// ─────────────────────────────────────────────────────────────────
class Display {
public:
    Display();

    // Inicializace: konfigurace LCD, 4-bit mód, CGRAM zápis vlastních znaků.
    void begin();

    // ── Settery dat (plní interní buffery, LCD se nepřepíše ihned) ──

    // Data pro LIVE pohled:
    //   stepMask  = bitmaska aktivních kroků (bit N = krok N zapnutý)
    //   playhead  = index právě hraného kroku; 0xFF = sekvencer zastaven
    //   selected  = index kroku vybraného pro editaci
    void setLiveData(uint16_t bpm, WaveType wave,
                     bool isPlaying, uint16_t stepMask,
                     uint8_t playhead, uint8_t selected);

    // Data pro EDIT pohled:
    //   noteName  = textová zkratka noty max 3 znaky (např. "E4\0")
    //   amplitude = 0–4095
    void setEditData(uint8_t stepIdx, const char* noteName,
                     uint16_t amplitude, WaveType wave,
                     uint16_t bpm, bool stepActive);

    // ── Přepínání pohledů ────────────────────────────────────────
    void showLive();
    void showEdit();   // přepne na EDIT a resetuje timeout

    // ── Hlavní metoda — volat jednou za smyčku ───────────────────
    // Zkontroluje EDIT timeout, vyrenderuje buffery, zapíše na LCD.
    void flush();

private:
    LiquidCrystal _lcd;
    DisplayView   _view;

    // Nový obsah (generuje se v každém flush) a starý obsah (co je na LCD)
    char _new0[DISP_COLS + 1];
    char _new1[DISP_COLS + 1];
    char _old0[DISP_COLS + 1];
    char _old1[DISP_COLS + 1];

    // Live data
    uint16_t _bpm;
    WaveType _wave;
    bool     _isPlaying;
    uint16_t _stepMask;
    uint8_t  _playhead;
    uint8_t  _selected;

    // Edit data
    uint8_t  _editStep;
    char     _editNote[4];   // max "D5\0"
    uint16_t _editAmp;
    WaveType _editWave;
    uint16_t _editBpm;
    bool     _editActive;

    uint32_t _editEnteredAt;  // millis() při vstupu do EDIT pohledu

    // ── Privátní helpers ─────────────────────────────────────────
    void _defineCgram();      // zapsat 4 vlastní znaky do CGRAM jednou při begin()
    void _renderLive();       // naplnit _new0/_new1 pro LIVE pohled
    void _renderEdit();       // naplnit _new0/_new1 pro EDIT pohled
    void _writeBuf();         // porovnat a zapsat pouze změněné řádky na LCD

    static const char* _waveShort(WaveType w);    // "SIN","SAW","SQR","TRI","BSW","SMP"
    static void        _pad(char* buf, uint8_t targetLen);  // doplnit mezerami na 16 znaků
};
