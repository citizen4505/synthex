/*
 * Hardware: Arduino Due (SAM3X8E, 84 MHz)
 * DAC výstup: pin DAC0 (PA2)
 *
 * Co se děje:
 *   - Voice 0: sinusový tón 440 Hz (A4)
 *   - Voice 1: pilový tón 261 Hz (C4), spustí se po 2 s
 *   - Voice 2: šum (noise), spustí se po 4 s, vypne po 6 s
 *   - Voice 3: trojúhelník 880 Hz (A5), spustí se po 6 s
 *
 * Každé 3 s se vypíše počet ISR volání → ověření stabilní 44100 Hz. 
 */

#include "BareMetalCore.h"

BareMetalCore& engine = BareMetalCore::getInstance();

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) {}   // čekej na Serial (max 3 s)

    Serial.println("=== BareMetalCore v1.0 ===");
    Serial.print("Sample rate : "); Serial.print(BMC_SAMPLE_RATE); Serial.println(" Hz");
    Serial.print("Voices      : "); Serial.println(BMC_VOICES);
    Serial.print("Wavetable   : "); Serial.print(BMC_WAVETABLE_SIZE); Serial.println(" samples");
    Serial.println("DAC output  : DAC1 (pin PA3)");
    Serial.println("-------------------------");

    engine.begin();

    // Voice 0: sinus 440 Hz, plná amplituda
    engine.noteOn(0, 440.0f, 2000, WaveType::SINE);
    Serial.println("[0s] Voice 0 ON — Sine 440 Hz");
}

void loop() {
    static uint32_t lastPrint  = 0;
    static uint32_t lastEvent  = 0;
    static uint8_t  eventStage = 0;

    uint32_t now = millis();

    // Časované události
    if (now - lastEvent >= 2000) {
        lastEvent = now;
        eventStage++;

        switch (eventStage) {
            case 1:
                engine.noteOn(1, 261.63f, 1800, WaveType::SAW);
                Serial.println("[2s] Voice 1 ON — Saw 261 Hz (C4)");
                break;

            case 2:
                engine.noteOn(2, 0.0f, 1500, WaveType::SQUARE);
                // Noise nepotřebuje frekvenci, phaseIncrement se ignoruje
                Serial.println("[4s] Voice 2 ON — Noise");
                break;

            case 3:
                engine.noteOff(2);
                engine.noteOn(3, 880.0f, 1600, WaveType::TRIANGLE);
                Serial.println("[6s] Voice 2 OFF, Voice 3 ON — Triangle 880 Hz (A5)");
                break;

            case 4:
                // Chord: 440 + 554 + 659 Hz (A dur)
                engine.noteOn(0, 440.0f,  2000, WaveType::SINE);
                engine.noteOn(1, 554.37f, 2000, WaveType::SINE);
                engine.noteOn(3, 659.25f, 2000, WaveType::SINE);
                Serial.println("[8s] Chord: A major (sine)");
                break;

            case 5:
                engine.noteOff(0);
                engine.noteOff(1);
                engine.noteOff(3);
                Serial.println("[10s] All voices OFF");
                eventStage = 0;   // restart smyčky
                lastEvent  = now;
                break;
        }
    }

    // Diagnostický výpis každé 3 s
    if (now - lastPrint >= 3000) {
        lastPrint = now;
        uint32_t isr = engine.getIsrCount();
        Serial.print("[diag] ISR count: ");
        Serial.print(isr);
        // Očekáváme ~44100 × (čas v sekundách)
        Serial.print("  (~");
        Serial.print(isr / (now / 1000));
        Serial.println(" Hz eff.)");
    }
}
