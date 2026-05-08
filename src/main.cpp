/*
 * main.cpp — Synthex + Pots + Display
 *
 * Sekvence (step 1–MELODY_LEN, pak reset):
 *   Portamento melodie hraje automaticky.
 *   Vsechny parametry (vlna, hlasitost, portamento, tempo)
 *   jsou rizeny potenciometry v realnem case.
 *   LCD displej zobrazuje aktualni stav.
 *
 * Tok dat v loop():
 *
 *   Pots::update()          cteni ADC (max 1x za 20 ms)
 *       |
 *       +-> engine.setPortamento()   global glide
 *       |
 *   eventTimer (tempoMs)    krok sekvenceru
 *       |
 *       +-> engine.noteOn()          nota s aktualnimi pot hodnotami
 *       |
 *   Display::update()        obnova LCD (max 1x za 100 ms)
 */

#include "Synthex.h"
#include "MillisTimer.h"
#include "Pots.h"
#include "Display.h"

Synthex& engine  = Synthex::getInstance();
Pots&    pots    = Pots::getInstance();
Display& display = Display::getInstance();

// ── Melodie ─────────────────────────────────────────────────────
// Indexy do CHROMATIC[] — 0 = ticho, zbytek = noty
static const float CHROMATIC[] = {
    0.0f,    // 0  ticho
    16.35f,  // 1  C0
    17.32f,  // 2  C#0
    18.35f,  // 3  D0
    19.45f,  // 4  D#0
    20.60f,  // 5  E0
    21.83f,  // 6  F0
    23.12f,  // 7  F#0
    24.50f,  // 8  G0
    25.96f,  // 9  G#0
    27.50f,  // 10 A0
    29.14f,  // 11 A#0
    30.87f,  // 12 H0
    32.70f,  // 13 C1
    34.64f,  // 14 C#1
    36.69f,  // 15 D1
    38.87f   // 16 D#1
};

static const uint8_t MELODY[] = {
    16, 4, 9, 4, 16, 4, 9, 4,
    16, 4, 9, 7, 16, 4, 9, 4,
    16,16, 9,16, 16,16, 9,16,
    16,16, 9, 9, 16,16, 9, 9
};
static constexpr uint8_t MELODY_LEN = sizeof(MELODY) / sizeof(MELODY[0]);


// ─────────────────────────────────────────────
//  setup
// ─────────────────────────────────────────────
void setup() {
    delay(100);

    engine.begin();   // DAC + timer ISR — musi byt prvni
    pots.begin();     // ADC init + prvni cteni
    display.begin();  // LCD init + uvitaci zprava (blokuje 1,5 s)

    // Prvni zobrazeni aktualnich hodnot potu
    display.setWaveType  (pots.waveType());
    display.setVolume    (pots.amplitude());
    display.setPortamento(pots.portamento());
    display.setTempoMs   (pots.tempoMs());
    display.forceRedraw();
}

// ─────────────────────────────────────────────
//  loop
// ─────────────────────────────────────────────
void loop() {
    // ── 1. Cteni potenciometru ───────────────────────────────────
    // Pots::update() polluje ADC max 1x za POTS_POLL_MS (20 ms).
    // Vraci true pokud se nektery parametr zmenil.
    const bool potsChanged = pots.update();

    if (potsChanged) {
        // Portamento je globalni — nastavime okamzite
        engine.setPortamento(pots.portamento());

        // Predame nove hodnoty displeje (obnovi se v display.update())
        display.setWaveType  (pots.waveType());
        display.setVolume    (pots.amplitude());
        display.setPortamento(pots.portamento());
        display.setTempoMs   (pots.tempoMs());
    }

    // ── 2. Krok sekvenceru ──────────────────────────────────────
    // Casovac se dynamicky nastavuje z pot hodnoty tempoMs.
    // Pouzivame staticky MillisTimer — interval menime za behu
    // pres setInterval() pokud se tempo zmenilo.
    static MillisTimer eventTimer(250u, /*autoReset=*/false);
    static uint8_t     step = 0;

    // Synchronizace intervalu casovace s aktualni hodnotou potu
    if (potsChanged) {
        eventTimer.setInterval(pots.tempoMs());
    }

    if (eventTimer.expired()) {
        eventTimer.reset();
        step++;

        if (step == 1u) {
            // Zacatek sekvence — zapneme portamento z potu
            engine.setPortamento(pots.portamento());
        }

        if (step >= 1u && step <= MELODY_LEN) {
            // Zahraj notu — pouzij aktualni hodnoty potu
            const float    freq = CHROMATIC[ MELODY[step - 1u] ] * 4.0f;  // posuneme o 2 oktavy nahoru (aby bylo slyset)
            const uint16_t amp  = pots.amplitude();
            const WaveType wave = pots.waveType();
            engine.noteOn(0u, freq, amp, wave);
        }

        if (step == MELODY_LEN + 1u) {
            // Konec sekvence — vypni hlas a portamento
            engine.noteOff(0u);
            engine.setPortamento(0.0f);
        }

        // ── Reset po dokonceni sekvence ──────────────────────────
        if (step > MELODY_LEN + 2u) {
            for (uint8_t i = 0u; i < SYNTHEX_VOICES; ++i) {
                engine.noteOff(i);
            }
            step = 0u;
        }
    }

    // ── 3. Obnova displeje ──────────────────────────────────────
    // Display::update() polluje max 1x za DISPLAY_REFRESH_MS (100 ms).
    // Nepotrebuje zadne dalsi volani — hodnoty uz jsou nastaveny vyse.
    display.update();
}