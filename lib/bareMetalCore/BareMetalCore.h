#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <wavetables.h>   // <- pre-generované tabulky v Flash

// ─────────────────────────────────────────────
//  Konfigurace enginu
// ─────────────────────────────────────────────
#define BMC_SAMPLE_RATE     44100u
#define BMC_VOICES          4u
#define BMC_DAC_RESOLUTION  12u
#define BMC_DAC_MAX         4095u
#define BMC_DAC_MID         2048u

// Fázový akumulátor — 32-bitový, horních 11 bitů = index (log2(2048) = 11)
#define BMC_PHASE_SHIFT     (32u - 11u)

// WaveType a BMC_WAVETABLE_SIZE jsou definovány v wavetables.h

// ─────────────────────────────────────────────
//  Jeden hlas syntezátoru
// ─────────────────────────────────────────────
struct Voice {
    volatile bool     active;
    volatile uint32_t phaseAccum;
    volatile uint32_t phaseIncrement;
    volatile uint16_t amplitude;        // 0–4095
    WaveType          waveType;

    Voice() : active(false), phaseAccum(0),
              phaseIncrement(0), amplitude(BMC_DAC_MID),
              waveType(WaveType::SINE) {}
};

// ─────────────────────────────────────────────
//  Hlavní třída enginu
// ─────────────────────────────────────────────
class BareMetalCore {
public:
    static BareMetalCore& getInstance();

    // Inicializace — jen DAC + Timer
    void begin();

    void noteOn(uint8_t voiceIdx, float freqHz,
                uint16_t amplitude = BMC_DAC_MID,
                WaveType wave = WaveType::SINE);

    void noteOff(uint8_t voiceIdx);

    // Volá se výhradně z ISR
    void processSample();

    static uint32_t freqToIncrement(float freqHz);

    uint32_t getIsrCount() const { return _isrCount; }

private:
    BareMetalCore();
    BareMetalCore(const BareMetalCore&)            = delete;
    BareMetalCore& operator=(const BareMetalCore&) = delete;

    void _initDACC();
    void _initTimer();

    // Wavetables jsou v Flash (wavetables.h) — žádná kopie v RAM
    // Přístup: BMC_TABLES[(uint8_t)waveType][phaseIndex]

    Voice    _voices[BMC_VOICES];
    uint32_t _isrCount;
    uint32_t _lfsrState;    // LFSR pro WaveType::NOISE
};

#ifdef __cplusplus
extern "C" { void TC3_Handler(); }
#endif
