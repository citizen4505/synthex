/*
 * Display.h — HD44780 LCD 1602 (16×2) pro Synthex sekvencer, 4-bitový mód
 *
 * ═══════════════════════════════════════════════════════════════════
 *  HARDWARE
 * ═══════════════════════════════════════════════════════════════════
 *
 *  Displej:  LCD 1602, řadič HD44780 (nebo kompatibilní),
 *            16 sloupců × 2 řádky
 *
 *  Připojení na Arduino Due (3,3 V logika, LCD většinou 5 V tolerantní):
 *
 *    LCD pin │ Název │ Arduino Due
 *    ────────┼───────┼────────────
 *       1    │ VSS   │ GND
 *       2    │ VDD   │ 5 V  (Due má 5V pin — použij pro LCD napájení)
 *       3    │ V0    │ střed děliče/trimmeru 10 kΩ (kontrast)
 *       4    │ RS    │ pin LCD_RS  (výchozí: 8)
 *       5    │ RW    │ GND         (jen zápis — RW=0 natvrdo)
 *       6    │ EN    │ pin LCD_EN  (výchozí: 9)
 *       7-10 │ D0-D3 │ nepřipojeno (4-bitový mód)
 *      11    │ D4    │ pin LCD_D4  (výchozí: 10)
 *      12    │ D5    │ pin LCD_D5  (výchozí: 11)
 *      13    │ D6    │ pin LCD_D6  (výchozí: 12)
 *      14    │ D7    │ pin LCD_D7  (výchozí:  7)
 *      15    │ A     │ 5 V přes 220 Ω (backlight anoda)
 *      16    │ K     │ GND (backlight katoda)
 *
 *  POZOR — Arduino Due: logická úroveň 3,3 V.
 *  Většina HD44780 displejů je 5 V a akceptuje 3,3 V HIGH jako
 *  validní HIGH (Vih_min ~2,0 V). Nicméně pokud displej nereaguje,
 *  přidej 3,3 V→5 V level shifter nebo voltage divider na datové piny.
 *
 *  Piny 2,3,4 jsou obsazeny tlačítky (main.cpp).
 *  Pin 13 je LED → D7 je na pinu 7 (aby nedocházelo ke konfliktu).
 *
 * ═══════════════════════════════════════════════════════════════════
 *  ROZLOŽENÍ DISPLEJE  (16 × 2 znaků)
 * ═══════════════════════════════════════════════════════════════════
 *
 *  Řádek 0 — STEP GRID (1 znak na krok, 16 kroků):
 *
 *    col:  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
 *          █  _  █  █  .  ▣  _  █  █  .  .  █  _  █  █  .
 *                         ^
 *                         aktuální krok = CGRAM rámeček (CUR_NOTE/CUR_REST)
 *
 *  Klíč symbolů:
 *    CGRAM 0  ▣  = aktuální krok + nota  (rámeček s tečkou uvnitř)
 *    CGRAM 1  □  = aktuální krok + pauza nebo neaktivní (prázdný rámeček)
 *    CGRAM 2  █  = krok s notou          (plný blok, horní polovina)
 *    CGRAM 3  _  = krok s pauzou (rest)  (spodní čára)
 *    ASCII '.'   = neaktivní krok        (malá tečka — žádný CGRAM slot)
 *
 *  Takt: každý 4. sloupec (0, 4, 8, 12) je první beat.
 *  Vizuální skupiny po 4 kroky odpovídají 4/4 taktu.
 *
 *  Řádek 1 — INFO LINE (16 znaků pevný formát):
 *
 *    pozice:  0    1 2 3   4   5 6 7   8   9 0 1 2  13  14 15
 *    obsah:  [st] [B B B] [sp][N N N] [sp][W W W W][sp][ P][n]
 *
 *    Příklad:  ">120 A#3 BSAW P2"
 *              "| 80 C4  SAW  P1"
 *              ".187 --- SINE P3"
 *
 *    [st]  = stav transportu: '>' hraje, '|' pauza, '.' stop
 *    [BPM] = BPM 3 číslice (zarovnáno vpravo, mezery vlevo), 30–999
 *    [NOTE]= jméno noty aktuálního kroku: "A#3", "C4 ", "---"
 *    [WAVE]= typ vlny 4 znaky: "SINE", "SAW ", "SQRE", "TRI ", "BSAW"
 *    [P]   = zkratka "P" + číslo patternu 1–4
 *
 * ═══════════════════════════════════════════════════════════════════
 *  CUSTOM ZNAKY (CGRAM)
 * ═══════════════════════════════════════════════════════════════════
 *
 *  HD44780 CGRAM: 8 slotů (0–7), každý 5×8 pixelů (8. řádek = kurzor).
 *
 *  Slot  │ Vzor (5 sloupců × 7 řádků)  │ Použití
 *  ──────┼──────────────────────────────┼────────────────────────────
 *    0   │ ┌───┐  rámeček s tečkou      │ aktuální krok + nota (gate ↑)
 *        │ │ · │                        │
 *        │ └───┘                        │
 *    1   │ ┌───┐  prázdný rámeček       │ aktuální krok + pauza/off
 *        │ │   │                        │
 *        │ └───┘                        │
 *    2   │ █████  plný blok top         │ krok s notou
 *        │ █████                        │
 *        │ (prázdné dole)               │
 *    3   │ (prázdné nahoře)             │ krok s pauzou (rest)
 *        │ _____  spodní čára           │
 *
 * ═══════════════════════════════════════════════════════════════════
 *  TIMING A BLOKOVÁNÍ
 * ═══════════════════════════════════════════════════════════════════
 *
 *  HD44780 v 4-bitovém módu @ 270 kHz E-clock:
 *    setCursor()  ≈  40 µs  (2 nibble × ~20 µs)
 *    write(char)  ≈  37 µs
 *    celý řádek   ≈  16 × 37 + 2 × 40 ≈ 672 µs
 *    oba řádky    ≈  ~1,5 ms
 *
 *  lcd.clear()   ≈  1,5 ms + HD44780 interní čas mazání (~750 µs)
 *  → v refresh() NIKDY nepoužíváme lcd.clear() !
 *
 * ═══════════════════════════════════════════════════════════════════
 *  KNIHOVNA — žádná externí závislost
 * ═══════════════════════════════════════════════════════════════════
 *
 *  #include <LiquidCrystal.h>   — součást Arduino core
 *  Žádný Wire.h, žádný I2C, žádný Adafruit.
 */

#pragma once
#include <Arduino.h>
#include <LiquidCrystal.h>
#include "Sequencer.h"
#include "Pots.h"

// ─────────────────────────────────────────────
//  Pinout — uprav dle svého zapojení
// ─────────────────────────────────────────────

#define LCD_RS   8
#define LCD_EN   9
#define LCD_D4   4
#define LCD_D5   5
#define LCD_D6   6
#define LCD_D7   7   // vyhýbáme se pinu 13 (LED)

// ─────────────────────────────────────────────
//  Rozměry displeje
// ─────────────────────────────────────────────

#define LCD_COLS  16u
#define LCD_ROWS   2u

// ─────────────────────────────────────────────
//  CGRAM sloty
// ─────────────────────────────────────────────

#define LCD_CHAR_CUR_NOTE  0u   // aktuální krok + nota
#define LCD_CHAR_CUR_REST  1u   // aktuální krok + pauza / neaktivní
#define LCD_CHAR_NOTE      2u   // krok s notou
#define LCD_CHAR_REST      3u   // krok s pauzou

// ─────────────────────────────────────────────
//  Třída Display (Singleton)
// ─────────────────────────────────────────────
class Display {
public:
    // Singleton — stejný vzor jako Synthex, Sequencer, Pots
    static Display& getInstance();

    // Inicializace LCD — volej v setup()
    // Nastaví 4-bitový mód, rozměry, nahraje CGRAM.
    // Vrátí vždy true (LiquidCrystal nemá HW detekci displeje).
    bool begin();

    // Překresli displej — volej z loop() (nikdy z ISR!)
    // Přepisuje každý znak na místě — bez lcd.clear(), bez delay().
    void refresh(const Sequencer& seq, const Pots& pots);

    // Je displej inicializovaný?
    bool isReady() const { return _initialized; }

private:
    Display();
    Display(const Display&)            = delete;
    Display& operator=(const Display&) = delete;

    // ── Vykreslovací metody ───────────────────

    // Řádek 0: 16 symbolů step gridu
    void _drawStepRow(const Sequencer& seq);

    // Řádek 1: ">120 A#3 BSAW P2"
    void _drawInfoRow(const Sequencer& seq, const Pots& pots);

    // ── Pomocné statické funkce ───────────────

    // MIDI číslo → "A#3" / "C4 " / "---"
    // buf: min 5 B; výsledek je vždy 3 znaky + '\0'
    static void _midiToName(uint8_t midiNote, char* buf, uint8_t bufLen);

    // WaveType → přesně 4 znaky + '\0'
    // Např. BANDLIMITED_SAW → "BSAW", SAW → "SAW "
    static const char* _waveName(WaveType wt);

    // Stav transportu → ASCII znak
    // PLAYING → '>'  PAUSED → '|'  STOPPED → '.'
    static char _stateChar(SeqState st);

    // Zapíše vzory CGRAM do HD44780 (voláno jednou z begin())
    void _loadCustomChars();

    // ── Data ─────────────────────────────────
    LiquidCrystal _lcd;
    bool          _initialized;
};
