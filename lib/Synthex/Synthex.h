#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <wavetables.h>   // <- pre-generované tabulky v Flash

// ─────────────────────────────────────────────
//  Konfigurace enginu
// ─────────────────────────────────────────────
#define SYNTHEX_SAMPLE_RATE     44100u
#define SYNTHEX_VOICES          4u
#define SYNTHEX_DAC_RESOLUTION  12u
#define SYNTHEX_DAC_MAX         4095u
#define SYNTHEX_DAC_MID         2048u

// Fázový akumulátor — 32-bitový, horních 11 bitů = index (log2(2048) = 11)
#define SYNTHEX_PHASE_SHIFT     (32u - 11u)

// WaveType a SYNTHEX_WAVETABLE_SIZE jsou definovány v wavetables.h

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
              phaseIncrement(0), amplitude(SYNTHEX_DAC_MID),
              waveType(WaveType::SINE) {}
};

// ─────────────────────────────────────────────
//  Hlavní třída enginu
// ─────────────────────────────────────────────
class Synthex {
public:
    static Synthex& getInstance();

    // Inicializace — jen DAC + Timer
    void begin();

    void noteOn(uint8_t voiceIdx, float freqHz,
                uint16_t amplitude = SYNTHEX_DAC_MID,
                WaveType wave = WaveType::SINE);

    void noteOff(uint8_t voiceIdx);

    // Volá se výhradně z ISR
    void processSample();

    static uint32_t freqToIncrement(float freqHz);

    uint32_t getIsrCount() const { return _isrCount; }

private:
    Synthex();
    Synthex(const Synthex&)            = delete;
    Synthex& operator=(const Synthex&) = delete;

    void _initDACC();
    void _initTimer();

    // Wavetables jsou v Flash (wavetables.h)
    // Přístup: SYNTHEX_TABLES[(uint8_t)waveType][phaseIndex]

    Voice    _voices[SYNTHEX_VOICES];
    uint32_t _isrCount;
    uint32_t _lfsrState;    // LFSR pro WaveType::NOISE
};

#ifdef __cplusplus
extern "C" { void TC3_Handler(); }
#endif
