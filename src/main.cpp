/*
 * main.cpp — Synthex + Step Sekvencer + LCD 1602 Display
 *
 * ═══════════════════════════════════════════════════════════════════
 *  PŘEHLED SYSTÉMU
 * ═══════════════════════════════════════════════════════════════════
 *
 *  Synthex   — 8hlasý wavetable syntezátor (ISR @ 44100 Hz)
 *  Sequencer — 16-krokový step sekvencer, 4 patterny
 *  Pots      — 4 potenciometry (volume, portamento, wavetype, tempo)
 *  Display   — HD44780 LCD 1602 (16×2), 4-bitový mód, @ 20 Hz
 *
 * ═══════════════════════════════════════════════════════════════════
 *  HARDWARE — Arduino Due / SAM3X8E
 * ═══════════════════════════════════════════════════════════════════
 *
 *  DAC výstup:     DAC1 (pin DAC1)  → audio
 *  Potenciometry:  A0 volume  A1 portamento  A2 wavetype  A3 tempo
 *  LCD:            RS=8 EN=9 D4=4 D5=5 D6=6 D7=7 (4-bitový mód)
 *  Transport:      Pin 10  PLAY/PAUSE (tlačítko, INPUT_PULLUP)
 *                  Pin 11  STOP       (tlačítko, INPUT_PULLUP)
 *                  Pin 12  PATTERN+   (tlačítko, INPUT_PULLUP)
 *
 * ═══════════════════════════════════════════════════════════════════
 *  SEKVENCER — PATTERNY
 * ═══════════════════════════════════════════════════════════════════
 *
 *  PAT 0  A3 pentatonická moll  (blues/funk groove)
 *  PAT 1  D4 dórská             (jazzový vzestup + sestup)
 *  PAT 2  C4 bluesová           (synkopy + blue note F#4)
 *  PAT 3  Prázdný               (pro live editaci)
 *
 * ═══════════════════════════════════════════════════════════════════
 *  OVLÁDÁNÍ POTENCIOMETRY
 * ═══════════════════════════════════════════════════════════════════
 *
 *  A0 VOLUME     → globální amplituda všech kroků (0–4095)
 *  A1 PORTAMENTO → glide čas sekvenceru (0–500 ms)
 *  A2 WAVETYPE   → globální přepis typu vlny (SINE/SAW/NOISE/…)
 *  A3 TEMPO      → tempo sekvenceru (80–500 ms/krok, ~30–187 BPM)
 *
 * ═══════════════════════════════════════════════════════════════════
 *  ARCHITEKTURA LOOP()
 * ═══════════════════════════════════════════════════════════════════
 *
 *  Bez delay(), bez blocking. Každá komponenta má vlastní timer.
 *
 *  Priority v loop():
 *    1. seq.update()      — krokový/gate timer (bezprostřední dopad na zvuk)
 *    2. pots.update()     — ADC + EMA každých 20 ms
 *    3. tlačítka          — transport každých 50 ms (debounce)
 *    4. disp.refresh()    — LCD překreslení každých 50 ms (~20 Hz)
 *    5. Serial (debug)    — volitelný výpis každou sekundu
 *
 * ═══════════════════════════════════════════════════════════════════
 *  PAMĚŤ (přibližné hodnoty, Arduino Due 512 KB Flash / 96 KB RAM)
 * ═══════════════════════════════════════════════════════════════════
 *
 *  wavetables.h (Flash .rodata):
 *    5 tabulek × 2048 × 2 B = 20 480 B (20 KB)
 *
 *  SeqStep pole (RAM .bss):
 *    4 × 16 × sizeof(SeqStep) ≈ 4 × 16 × 12 = 768 B
 *
 *  LCD 1602 nepotřebuje frame buffer (přímý zápis znaků):
 *    HD44780 DDRAM: 80 B (2 × 40 znaků) — žádná RAM na Due
 *
 *  Celkem RAM ≈ 768 + 1024 + stack + ostatní ≈ ~5 KB → v pohodě
 */

#include "Synthex.h"
#include "Sequencer.h"
#include "Display.h"
#include "Pots.h"
#include "MillisTimer.h"

// ─────────────────────────────────────────────
//  Tlačítka (INPUT_PULLUP — stisk = LOW)
// ─────────────────────────────────────────────
#define BTN_PLAY_PAUSE  10
#define BTN_STOP        11
#define BTN_PATTERN     12

// ─────────────────────────────────────────────
//  Globální reference na singletony
//  (zapsáno zde pro přehlednost; getInstance() je inlinováno)
// ─────────────────────────────────────────────
static Synthex&   engine = Synthex::getInstance();
static Sequencer& seq    = Sequencer::getInstance();
static Pots&      pots   = Pots::getInstance();
static Display&   disp   = Display::getInstance();

// ─────────────────────────────────────────────
//  Čtení tlačítek s debouncem
//  Vrátí true pokud bylo tlačítko práve stisknuto (sestupná hrana).
// ─────────────────────────────────────────────
static bool _buttonPressed(uint8_t pin, uint8_t& lastState) {
    const uint8_t current = digitalRead(pin);
    const bool    pressed = (current == LOW) && (lastState == HIGH);
    lastState = current;
    return pressed;
}

// ─────────────────────────────────────────────
//  setup()
// ─────────────────────────────────────────────
void setup() {
    delay(100);   // stabilizace napájení

    Serial.begin(115200);
    Serial.println(F("=== Synthex Sequencer v5 ==="));

    // ── Tlačítka ──────────────────────────────
    pinMode(BTN_PLAY_PAUSE, INPUT_PULLUP);
    pinMode(BTN_STOP,       INPUT_PULLUP);
    pinMode(BTN_PATTERN,    INPUT_PULLUP);

    // ── Synthex engine ────────────────────────
    engine.begin();
    Serial.println(F("[OK] Synthex engine"));

    // ── Potenciometry ─────────────────────────
    pots.begin();
    Serial.println(F("[OK] Pots"));

    // ── Sekvencer ─────────────────────────────
    seq.begin(engine);

    // Globální přepisy z potenciometrů (první čtení)
    seq.useGlobalWave(true,  pots.waveType());
    seq.useGlobalAmplitude(true, pots.amplitude());
    seq.setTempoMs(pots.tempoMs());
    engine.setPortamento(pots.portamento());

    // Spusť sekvencer
    seq.play();
    Serial.println(F("[OK] Sequencer — PLAYING"));

    // ── Display ───────────────────────────────
    if (disp.begin()) {
        Serial.println(F("[OK] Display LCD 1602"));
    } else {
        Serial.println(F("[WARN] Display begin failed"));
    }

    Serial.print(F("Tempo: "));
    Serial.print(seq.tempoMs());
    Serial.print(F(" ms/step = "));
    Serial.print(seq.bpm());
    Serial.println(F(" BPM"));
}

// ─────────────────────────────────────────────
//  loop()
// ─────────────────────────────────────────────
void loop() {
    // ── 1. Sekvencer — musí být co nejdříve v loop() ──────────
    //       processSample() běží v ISR, ale step-advance je zde.
    seq.update();

    // ── 2. Potenciometry (každých 20 ms) ──────────────────────
    if (pots.update()) {
        seq.setTempoMs(pots.tempoMs());
        seq.useGlobalWave(true, pots.waveType());
        seq.useGlobalAmplitude(true, pots.amplitude());
        engine.setPortamento(pots.portamento());
    }

    // ── 3. Tlačítka (každých 50 ms = debounce interval) ───────
    static MillisTimer btnTimer(50u, true);
    static uint8_t     lastPlay = HIGH;
    static uint8_t     lastStop = HIGH;
    static uint8_t     lastPat  = HIGH;

    if (btnTimer.expired()) {
        if (_buttonPressed(BTN_PLAY_PAUSE, lastPlay)) {
            if (seq.state() == SeqState::PLAYING) {
                seq.pause();
                Serial.println(F("PAUSED"));
            } else {
                seq.play();
                Serial.println(F("PLAYING"));
            }
        }

        if (_buttonPressed(BTN_STOP, lastStop)) {
            seq.stop();
            Serial.println(F("STOPPED"));
        }

        if (_buttonPressed(BTN_PATTERN, lastPat)) {
            const uint8_t next = (seq.currentPattern() + 1u) % SEQ_PATTERNS;
            seq.selectPattern(next);
            Serial.print(F("PATTERN: "));
            Serial.println(next + 1u);
        }
    }

    // ── 4. Display refresh (každých 50 ms = 20 Hz) ────────────
    static MillisTimer dispTimer(50u, true);
    if (dispTimer.expired()) {
        disp.refresh(seq, pots);
    }

    // ── 5. Debug výpis přes Serial (každou sekundu) ───────────
    //       Odkomentuj pro ladění; produkčně zakomentuj.
    //
    // static MillisTimer dbgTimer(1000u, true);
    // if (dbgTimer.expired()) {
    //     Serial.print(F("Step:"));
    //     Serial.print(seq.currentStep());
    //     Serial.print(F("  BPM:"));
    //     Serial.print(seq.bpm());
    //     Serial.print(F("  ISR:"));
    //     Serial.println(engine.getIsrCount());
    // }
}
