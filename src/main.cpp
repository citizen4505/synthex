/*
 * main.cpp — demo fáze 4 + integrace potenciometrů (Pots)
 *
 * ════════════════════════════════════════════════════════════════════
 *  Hardware — 4 potenciometry (10 kΩ lin., napájení 3,3 V!):
 *
 *   A0  VOLUME      otočení CW → hlasitěji  (0 – 4095)
 *   A1  PORTAMENTO  CCW = vypnuto, CW → až 500 ms glide
 *   A2  WAVETYPE    šest zón: SINE / SAW / NOISE / TRIANGLE /
 *                             BANDLIMITED_SAW / SAMPLE
 *   A3  TEMPO       CCW = 500 ms/krok (pomalé),
 *                   CW  =  80 ms/krok (rychlé)
 *
 * ════════════════════════════════════════════════════════════════════
 *  Tok programu
 *
 *  setup()
 *    1. engine.begin()      — inicializace DAC + TC3 ISR @ 44 100 Hz
 *    2. pots.begin()        — 12-bit ADC, první čtení všech kanálů,
 *                             přepočet výstupních parametrů
 *    3. Serial.begin()      — debug výpis při změně parametrů
 *
 *  loop()  [tight loop, bez delay()]
 *    ① pots.update()
 *         — každých 20 ms přečte ADC, aktualizuje EMA filtry
 *         — vrátí true pokud se cokoliv změnilo
 *         — při změně: hned propaguj portamento a tempo do enginu
 *
 *    ② eventTimer.expired()
 *         — sekvencer kroku běží na intervalu řízeném potem TEMPO
 *         — interval se aktualizuje při každé změně potu (viz ①)
 *
 *    ③ Sekvence not
 *         — noteOn používá aktuální pots.amplitude() a pots.waveType()
 *         — portamento je live: otáčení potu se projeví ihned na
 *           příštím noteOn (engine sleduje _portaTimeMs globálně)
 *
 * ════════════════════════════════════════════════════════════════════
 *  Bezpečné amplitudy pro 8 hlasů:
 *   max_safe ≈ 4095 / (8 × 0,7) ≈ 730
 *   Pots.amplitude() vrací 0–4095; při plném potu a 8 hlasech dojde
 *   k saturaci — engine to clampuje, ale zvuk bude ořezaný.
 *   Pro demo s jedním hlasem (hlas 0) je celý rozsah v pořádku.
 */

#include "Synthex.h"
#include "MillisTimer.h"
#include "Pots.h"

// ─────────────────────────────────────────────
//  Globální reference na singletonové objekty
// ─────────────────────────────────────────────
Synthex& engine = Synthex::getInstance();
Pots&    pots   = Pots::getInstance();

// ─────────────────────────────────────────────
//  Tónová tabulka — chromatická stupnice C0–D#1
//  Index 0 je záměrně 0 Hz (ticho / pauza).
// ─────────────────────────────────────────────
static const float CHROMATIC[] = {
    0.0f,       // 0  ticho
    16.35f,     // 1  C0
    17.32f,     // 2  C#0/Db0
    18.35f,     // 3  D0
    19.45f,     // 4  D#0/Eb0
    20.60f,     // 5  E0
    21.83f,     // 6  F0
    23.12f,     // 7  F#0/Gb0
    24.50f,     // 8  G0
    25.96f,     // 9  G#0/Ab0
    27.50f,     // 10 A0
    29.14f,     // 11 A#0/Bb0
    30.87f,     // 12 H0
    32.70f,     // 13 C1
    34.64f,     // 14 C#1/Db1
    36.69f,     // 15 D1
    38.87f      // 16 D#1/Eb1
};

// ─────────────────────────────────────────────
//  Melodická sekvence — indexy do CHROMATIC[]
//  Délka musí být ≤ 255 (uint8_t step čítač).
// ─────────────────────────────────────────────
static const uint8_t MELODY[] = {
    16, 4, 9, 4, 16, 4, 9, 4,
    16, 4, 9, 7, 16, 4, 9, 4,
    16,16, 9,16, 16,16, 9,16,
    16,16, 9, 9, 16,16, 9, 9
};
static constexpr uint8_t MELODY_LEN = sizeof(MELODY) / sizeof(MELODY[0]);

// ─────────────────────────────────────────────
//  Pomocná funkce — vypíše název WaveType
//  (pouze pro Serial debug, nevolá se v ISR)
// ─────────────────────────────────────────────
static const char* waveTypeName(WaveType w) {
    switch (w) {
        case WaveType::SINE:            return "SINE";
        case WaveType::SAW:             return "SAW";
        case WaveType::SQUARE:          return "NOISE";
        case WaveType::TRIANGLE:        return "TRIANGLE";
        case WaveType::BANDLIMITED_SAW: return "BL_SAW";
        case WaveType::SAMPLE:          return "SAMPLE";
        default:                        return "?";
    }
}

// ─────────────────────────────────────────────
//  setup()
// ─────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    // Krátká prodleva — napájení se ustálí, Serial se připraví.
    delay(200);

    // 1. Audio engine — DAC + TC3 přerušení @ 44 100 Hz
    engine.begin();

    // 2. Potenciometry — 12-bit ADC, EMA inicializace z reálných hodnot.
    //    Po begin() jsou pots.amplitude(), pots.waveType() atd. platné
    //    okamžitě — žádný "studený start" od nuly.
    pots.begin();

    // Počáteční stav parametrů do Serial monitoru
    Serial.println(F("=== Synthex start ==="));
    Serial.print(F("  amplitude  : ")); Serial.println(pots.amplitude());
    Serial.print(F("  portamento : ")); Serial.print(pots.portamento(), 1);
    Serial.println(F(" ms"));
    Serial.print(F("  waveType   : ")); Serial.println(waveTypeName(pots.waveType()));
    Serial.print(F("  tempo      : ")); Serial.print(pots.tempoMs());
    Serial.println(F(" ms/krok"));
    Serial.println(F("====================="));

    // Aplikuj počáteční hodnotu portamenta z potu
    engine.setPortamento(pots.portamento());
}

// ─────────────────────────────────────────────
//  loop()
// ─────────────────────────────────────────────
void loop() {
    // ── Stavové proměnné sekvenceru ───────────────────────────
    // Inicializují se jednou při prvním volání loop().
    // eventTimer.interval() se průběžně mění podle potu TEMPO.
    static MillisTimer eventTimer(250u, false);
    static uint8_t     step = 0u;

    // ════════════════════════════════════════════════════════════
    //  ① Čtení potenciometrů (každých 20 ms uvnitř update())
    //
    //  Pokud se cokoliv změnilo, ihned propaguj do enginu:
    //    • portamento → engine.setPortamento()
    //    • tempo      → eventTimer.setInterval()
    //    • amplitude / waveType se čtou přímo v noteOn() níže,
    //      takže žádná explicitní propagace není potřeba.
    // ════════════════════════════════════════════════════════════
    if (pots.update()) {
        // Portamento — živá změna, platí pro příští noteOn()
        engine.setPortamento(pots.portamento());

        // Tempo — aktualizuj interval sekvenceru okamžitě.
        // setInterval() změní délku příštího čekání bez resetu
        // aktuálního cyklu → sekvence neskočí.
        eventTimer.setInterval(pots.tempoMs());

        // Debug výpis — viditelné v Serial monitoru
        Serial.print(F("POT  amp="));
        Serial.print(pots.amplitude());
        Serial.print(F("  porta="));
        Serial.print(pots.portamento(), 0);
        Serial.print(F("ms  wave="));
        Serial.print(waveTypeName(pots.waveType()));
        Serial.print(F("  tempo="));
        Serial.print(pots.tempoMs());
        Serial.println(F("ms"));
    }

    // ════════════════════════════════════════════════════════════
    //  ② Sekvencer — krok se spustí po uplynutí eventTimer
    // ════════════════════════════════════════════════════════════
    if (!eventTimer.expired()) return;
    eventTimer.reset();

    step++;

    // ── Přehrání noty ─────────────────────────────────────────
    //  Hodnoty amplitudy a typu vlny se čtou v okamžiku noteOn:
    //  otočení potu se projeví na příštím kroku sekvenceru.
    //
    //  Hlas 0 hraje melodii s portamentem.
    //  Portamento je globální → plynulý glide, pokud pot A1 > deadband.
    if (step >= 1u && step <= MELODY_LEN) {
        const uint8_t noteIdx = MELODY[step - 1u];
        const float   freq    = CHROMATIC[noteIdx];

        if (freq > 0.0f) {
            // Nota — použij aktuální hodnoty potů
            engine.noteOn(0u, freq, pots.amplitude(), pots.waveType());
        } else {
            // Index 0 = ticho / pauza
            engine.noteOff(0u);
        }
    }

    // ── Konec melodie → noteOff ───────────────────────────────
    if (step == MELODY_LEN + 1u) {
        engine.noteOff(0u);
    }

    // ════════════════════════════════════════════════════════════
    //  Reset sekvence
    //  Přidáme jeden krok ticha (MELODY_LEN + 2) před opakováním,
    //  aby bylo slyšet oddělení smyček i při rychlém tempu.
    // ════════════════════════════════════════════════════════════
    if (step > MELODY_LEN + 2u) {
        // Utiš všechny hlasy pro čistý start
        for (uint8_t i = 0u; i < SYNTHEX_VOICES; ++i) {
            engine.noteOff(i);
        }
        step = 0u;
        Serial.println(F("--- smycka ---"));
    }
}
