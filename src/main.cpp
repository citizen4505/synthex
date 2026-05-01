/*
 * main.cpp — Synthex demo: ADSR obálka (Fáze 3)
 *
 * Hardware: SAM3X8E (Arduino Due), 84 MHz
 * DAC výstup: pin DAC1 (PA3)
 *
 * Tři ADSR presety demonstrující rozdílný charakter zvuku:
 *
 *   Hlas 0 — Piano:    krátký attack, výrazný decay, nízký sustain, střední release
 *   Hlas 1 — Organ:    okamžitý attack, žádný decay, plný sustain, krátký release
 *   Hlas 2 — Pad:      dlouhý attack, pomalý decay, vysoký sustain, dlouhý release
 *
 * Melodie: C4–E4–G4 (kvinta) hraje v kánonu na všech třech hlasech,
 *          každý s jiným zvukovým charakterem.
 */
#include "Synthex.h"
#include "MillisTimer.h"

// ─────────────────────────────────────────────
//  ADSR presety
//  Parametry: (attack ms, decay ms, sustain 0–4095, release ms)
// ─────────────────────────────────────────────
struct Preset {
    uint16_t attackMs;
    uint16_t decayMs;
    uint16_t sustainLevel;
    uint16_t releaseMs;
    WaveType wave;
    const char* name;
};

static const Preset PRESETS[3] = {
    //  A      D     S      R      tvar          název
    {   10,  300,  800,  400,  WaveType::BANDLIMITED_SAW,  "Piano"  },
    {    0,    0, 4095,   80,  WaveType::TRIANGLE,          "Organ"  },
    {  600,  800, 3200, 1200,  WaveType::SINE,              "Pad"    },
};

// Frekvence not (Hz) — C4, E4, G4, C5
static const float NOTES[] = { 261.63f, 329.63f, 392.00f, 523.25f };
static const uint8_t NOTE_COUNT = 4;

// Amplitudy presetů (0–4095) — organ tišší, aby nepřebíjel
static const uint16_t AMPLITUDES[3] = { 2800, 1800, 2200 };

Synthex& engine = Synthex::getInstance();

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) {}   // čekej na Serial (max 3 s)
    delay(100); // stabilizace DAC
    
    engine.begin();

// Nastav ADSR pro každý hlas
    for (uint8_t v = 0; v < 3; ++v) {
        const Preset& p = PRESETS[v];
        engine.setAdsr(v, p.attackMs, p.decayMs, p.sustainLevel, p.releaseMs);
        Serial.print("Voice ");
        Serial.print(v);
        Serial.print(" — ");
        Serial.print(p.name);
        Serial.print("  A="); Serial.print(p.attackMs);
        Serial.print(" D="); Serial.print(p.decayMs);
        Serial.print(" S="); Serial.print(p.sustainLevel);
        Serial.print(" R="); Serial.println(p.releaseMs);
    }

    Serial.println("=== Synthex — Fáze 3: ADSR ===");
}

// ─────────────────────────────────────────────
void loop() {
    // Kánon: každý hlas hraje notu 300 ms za předchozím
    //  Hlas 0: note on  @ 0 ms, note off @ 400 ms
    //  Hlas 1: note on  @ 300 ms, note off @ 700 ms
    //  Hlas 2: note on  @ 600 ms, note off @ 1000 ms
    //  Pauza do 1600 ms, pak opakuj s další notou

    static MillisTimer seqTimer(100, false);
    static uint8_t  stage    = 0;
    static uint8_t  noteIdx  = 0;

    if (!seqTimer.expired()) return;
    seqTimer.reset();
    ++stage;

    switch (stage) {

        // ── Hlas 0 ON ──────────────────────────────────────
        case 1:
            engine.noteOn(0, NOTES[noteIdx], AMPLITUDES[0], PRESETS[0].wave);
            Serial.print("[+0 ms] V0 ON  ");
            Serial.print(NOTES[noteIdx]); Serial.println(" Hz");
            break;

        // ── Hlas 1 ON ──────────────────────────────────────
        case 4:
            engine.noteOn(1, NOTES[(noteIdx + 2) % NOTE_COUNT], AMPLITUDES[1], PRESETS[1].wave);
            Serial.print("[+300 ms] V1 ON  ");
            Serial.print(NOTES[(noteIdx + 2) % NOTE_COUNT]); Serial.println(" Hz");
            break;

        // ── Hlas 0 OFF ─────────────────────────────────────
        case 5:
            engine.noteOff(0);
            Serial.println("[+400 ms] V0 OFF → Release");
            break;

        // ── Hlas 2 ON ──────────────────────────────────────
        case 7:
            engine.noteOn(2, NOTES[(noteIdx + 1) % NOTE_COUNT], AMPLITUDES[2], PRESETS[2].wave);
            Serial.print("[+600 ms] V2 ON  ");
            Serial.print(NOTES[(noteIdx + 1) % NOTE_COUNT]); Serial.println(" Hz");
            break;

        // ── Hlas 1 OFF ─────────────────────────────────────
        case 8:
            engine.noteOff(1);
            Serial.println("[+700 ms] V1 OFF → Release");
            break;

        // ── Hlas 2 OFF ─────────────────────────────────────
        case 11:
            engine.noteOff(2);
            Serial.println("[+1000 ms] V2 OFF → Release");
            break;

        // ── Pauza, pak další nota ───────────────────────────
        case 17:
            noteIdx = (noteIdx + 1) % NOTE_COUNT;
            stage   = 0;
            Serial.println("--- next ---");
            break;

        default:
            break;
    }
}
