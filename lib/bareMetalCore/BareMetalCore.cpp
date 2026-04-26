/*
 * BareMetalCore.cpp
 * Wavetable syntéza — tabulky čteny přímo z Flash (wavetables.h)
 * Arduino Due / SAM3X8E, 84 MHz ARM Cortex-M3
 */

#include "BareMetalCore.h"
#include <cstring>

// ─────────────────────────────────────────────
//  Singleton
// ─────────────────────────────────────────────
BareMetalCore& BareMetalCore::getInstance() {
    static BareMetalCore instance;
    return instance;
}

BareMetalCore::BareMetalCore()
    : _isrCount(0), _lfsrState(0xACE1u) {}

// ─────────────────────────────────────────────
//  begin() — žádné generování, jen hardware init
// ─────────────────────────────────────────────
void BareMetalCore::begin() {
    _initDACC();
    _initTimer();
}

// ─────────────────────────────────────────────
//  Ovládání hlasů
// ─────────────────────────────────────────────
void BareMetalCore::noteOn(uint8_t idx, float freqHz,
                           uint16_t amplitude, WaveType wave) {
    if (idx >= BMC_VOICES) return;
    __disable_irq();
    _voices[idx].phaseIncrement = freqToIncrement(freqHz);
    _voices[idx].amplitude      = amplitude;
    _voices[idx].waveType       = wave;
    _voices[idx].phaseAccum     = 0;
    _voices[idx].active         = true;
    __enable_irq();
}

void BareMetalCore::noteOff(uint8_t idx) {
    if (idx >= BMC_VOICES) return;
    __disable_irq();
    _voices[idx].active = false;
    __enable_irq();
}

// ─────────────────────────────────────────────
//  ISR — 44100× za sekundu
// ─────────────────────────────────────────────
void BareMetalCore::processSample() {
    int32_t mix = 0;

    for (uint8_t i = 0; i < BMC_VOICES; ++i) {
        Voice& v = _voices[i];
        if (!v.active) continue;

        v.phaseAccum += v.phaseIncrement;

        int16_t sample;

        if (v.waveType == WaveType::SQUARE) {
            // 32-bit Galoisův LFSR — nezávisí na phaseAccum
            _lfsrState ^= _lfsrState >> 1;
            _lfsrState ^= _lfsrState << 5;
            _lfsrState ^= _lfsrState >> 14;
            sample = static_cast<int16_t>(_lfsrState & 0x0FFF) - 2048;
        } else {
            // Přímé čtení z Flash — žádná kopie v RAM
            // Horních 11 bitů akumulátoru = index 0–2047
            const uint16_t tableIdx = (v.phaseAccum >> BMC_PHASE_SHIFT)
                                      & (BMC_WAVETABLE_SIZE - 1u);
            sample = BMC_TABLES[static_cast<uint8_t>(v.waveType)][tableIdx];
        }

        mix += (static_cast<int32_t>(sample) * v.amplitude) >> 12;
    }

    // Unsigned offset pro DAC (0–4095)
    mix += BMC_DAC_MID;
    if (mix < 0)           mix = 0;
    if (mix > BMC_DAC_MAX) mix = BMC_DAC_MAX;

    DACC->DACC_CDR = static_cast<uint32_t>(mix);
    ++_isrCount;
}

// ─────────────────────────────────────────────
//  Pomocné funkce
// ─────────────────────────────────────────────
uint32_t BareMetalCore::freqToIncrement(float freqHz) {
    // phaseIncrement = freqHz × (2^32 / sampleRate)
    constexpr float k = static_cast<float>(1ULL << 32) / BMC_SAMPLE_RATE;
    return static_cast<uint32_t>(freqHz * k);
}

// ─────────────────────────────────────────────
//  Inicializace DACC (12-bit DAC, kanál 1 = pin DAC1)
// ─────────────────────────────────────────────
void BareMetalCore::_initDACC() {
    PMC->PMC_PCER1 |= (1u << (ID_DACC - 32));
    DACC->DACC_CR   = DACC_CR_SWRST;
    DACC->DACC_MR   = DACC_MR_REFRESH(8)
                    | DACC_MR_USER_SEL_CHANNEL0
                    | DACC_MR_STARTUP_8;
    DACC->DACC_CHER = DACC_CHER_CH0;
    DACC->DACC_CDR  = BMC_DAC_MID;     // ticho jako výchozí stav
}

// ─────────────────────────────────────────────
//  Inicializace TC1/CH0 (TC3) @ 44100 Hz
//  MCK/2 = 42 MHz → RC = 42 000 000 / 44 100 ≈ 952
// ─────────────────────────────────────────────
void BareMetalCore::_initTimer() {
    PMC->PMC_PCER0 |= (1u << ID_TC3);

    TcChannel* ch = &TC1->TC_CHANNEL[0];
    ch->TC_CCR = TC_CCR_CLKDIS;
    ch->TC_IDR = 0xFFFFFFFF;
    (void)ch->TC_SR;

    ch->TC_CMR = TC_CMR_TCCLKS_TIMER_CLOCK1
               | TC_CMR_WAVE
               | TC_CMR_WAVSEL_UP_RC;

    constexpr uint32_t RC = (84000000u / 2u) / BMC_SAMPLE_RATE;
    ch->TC_RC  = RC;
    ch->TC_IER = TC_IER_CPCS;

    NVIC_SetPriority(TC3_IRQn, 0);
    NVIC_EnableIRQ(TC3_IRQn);

    ch->TC_CCR = TC_CCR_CLKEN | TC_CCR_SWTRG;
}

// ─────────────────────────────────────────────
//  ISR wrapper
// ─────────────────────────────────────────────
extern "C" void TC3_Handler() {
    (void)TC1->TC_CHANNEL[0].TC_SR;    // potvrzení přerušení
    BareMetalCore::getInstance().processSample();
}
