/*
 * main.cpp — demo fáze 4
 *
 * Sekvence (každý oddíl trvá ~4 s, pak se smyčka opakuje):
 *
 *   A) Portamento melody
 *      Jeden hlas klouže lineárně přes pentatonickou stupnici (50 ms glide).
 *      Hlas se neresetuje → slyšíš plynulý legato glide bez kliků.
 *
 *   B) Unison akordy
 *      noteOnUnison() spustí 3 hlasy rozladěné o ±10 centů.
 *      Výsledek: charakteristické „chorus" zahušťování tónu.
 *      Celá skupina se zastaví jediným noteOffById().
 *
 *   C) Rychlá arpeggio — voice stealing
 *      noteOnAuto() spouští notu každých 80 ms bez noteOff.
 *      Po 8 not jsou všechny hlasy plné → engine krade nejstarší.
 *      V Sériové konzoli uvidíš, který hlas byl ukraden.
 *
 * Amplituda je záměrně nízká (150/4095) — při 8 aktivních hlasech
 * součet může saturovat DAC (4095), engine to clampuje, ale
 * pro čistý mix přizpůsob amplitude počtu souběžných hlasů:
 *   max_safe = 4095 / (SYNTHEX_VOICES * ~0.7)  ≈ 730 pro 8 hlasů
 */

#include "Synthex.h"
#include "MillisTimer.h"

Synthex& engine = Synthex::getInstance();

// ── Pentatonická stupnice A dur ──────────────────────────────────────────────
static const float PENTA[] = {
    220.0f,   // A3
    261.6f,   // C4
    293.7f,   // D4
    329.6f,   // E4
    392.0f,   // G4
    440.0f,   // A4
    523.3f,   // C5
    587.3f,   // D5
};
static constexpr uint8_t PENTA_LEN = sizeof(PENTA) / sizeof(PENTA[0]);

// ── Drobná melodie pro portamento sekci (indexy do PENTA) ────────────────────
static const uint8_t MELODY[] = { 0, 2, 4, 5, 4, 2, 1, 0, 3, 5, 7, 6 };
static constexpr uint8_t MELODY_LEN = sizeof(MELODY) / sizeof(MELODY[0]);

// ─────────────────────────────────────────────
//  setup
// ─────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) {}
    delay(100);

    engine.begin();

    Serial.println("╔══════════════════════════════════╗");
    Serial.println("║   Synthex — Fáze 4 · Polyfonie   ║");
    Serial.println("╚══════════════════════════════════╝");
    Serial.print("Hlasy:       "); Serial.println(SYNTHEX_VOICES);
    Serial.print("Sample rate: "); Serial.print(SYNTHEX_SAMPLE_RATE);
    Serial.println(" Hz");
    Serial.println();
    Serial.println("Sekvence:  A) Portamento  B) Unison  C) Voice stealing");
    Serial.println("────────────────────────────────────");
}

// ─────────────────────────────────────────────
//  loop
// ─────────────────────────────────────────────
void loop() {
    static MillisTimer eventTimer(333, false);   // přepínač událostí
    static uint8_t     step       = 0;
    static uint8_t     lastNoteId = 0;

    if (!eventTimer.expired()) return;
    eventTimer.reset();

    step++;

    // ════════════════════════════════════════════════════
    //  A) PORTAMENTO MELODY  (kroky 1–12, ~4 s)
    //     50ms glide, hlas 0, BANDLIMITED_SAW
    // ════════════════════════════════════════════════════
    if (step == 1) {
        engine.setPortamento(50.0f);
        Serial.println("\n── A) Portamento melody (50 ms glide) ──");
    }

    if (step >= 1 && step <= MELODY_LEN) {
        float freq = PENTA[ MELODY[step - 1] ];
        engine.noteOn(0, freq, 350, WaveType::BANDLIMITED_SAW);

        Serial.print("  [porta] hlas 0 → ");
        Serial.print(freq, 1);
        Serial.println(" Hz");
    }

    if (step == MELODY_LEN + 1) {
        engine.noteOff(0);
        engine.setPortamento(0.0f);   // vypni portamento před unison sekcí
    }

    // ════════════════════════════════════════════════════
    //  B) UNISON AKORDY  (kroky 14–21, ~2.5 s)
    //     3 hlasy, ±10 centů detune
    // ════════════════════════════════════════════════════
    if (step == 14) {
        Serial.println("\n── B) Unison akordy (3 hlasy, 20 centů spread) ──");
    }

    if (step >= 14 && step <= 21) {
        // Zastav předchozí akord
        if (lastNoteId != 0) engine.noteOffById(lastNoteId);

        float freq = PENTA[(step - 14) % PENTA_LEN];

        // 3 hlasy: [-10, 0, +10] centů
        lastNoteId = engine.noteOnUnison(freq, 3, 20.0f, 220, WaveType::BANDLIMITED_SAW);

        Serial.print("  [unison] ");
        Serial.print(freq, 1);
        Serial.print(" Hz × 3 hlasy → noteId=");
        Serial.println(lastNoteId);
    }

    if (step == 22) {
        if (lastNoteId != 0) { engine.noteOffById(lastNoteId); lastNoteId = 0; }
    }

    // ════════════════════════════════════════════════════
    //  C) VOICE STEALING  (kroky 24–39, ~5 s)
    //     Spouštíme více not než je hlasů; engine krade.
    //
    //  Proč to funguje:
    //    noteOnAuto() volá _findFreeVoice() před každým noteOn.
    //    Po SYNTHEX_VOICES (8) souběžných not jsou všechny hlasy plné.
    //    Další noteOnAuto() ukradne nejstarší — v sériové konzoli uvidíš
    //    stejný index hlasu znovu pro novou notu.
    // ════════════════════════════════════════════════════
    if (step == 24) {
        Serial.println("\n── C) Voice stealing (noteOnAuto, bez noteOff) ──");
        Serial.println("     Hlasy 0–7 se postupně zaplní, pak se kradou.");
    }

    if (step >= 24 && step <= 39) {
        uint8_t pIdx = (step - 24) % PENTA_LEN;
        float   freq = PENTA[pIdx];
        uint8_t vi   = engine.noteOnAuto(freq, 150, WaveType::SINE);

        Serial.print("  [steal] noteOnAuto → hlas ");
        Serial.print(vi);
        Serial.print("  @ ");
        Serial.print(freq, 1);
        Serial.println(" Hz");
    }

    // ════════════════════════════════════════════════════
    //  Reset sekvence
    // ════════════════════════════════════════════════════
    if (step > 39) {
        for (uint8_t i = 0; i < SYNTHEX_VOICES; ++i) engine.noteOff(i);
        step = 0;
        lastNoteId = 0;
        Serial.println("\n════════ restart smyčky ════════");
    }
}
