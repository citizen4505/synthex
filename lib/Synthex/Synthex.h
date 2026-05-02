#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "wavetables.h"

// ─────────────────────────────────────────────
//  Konfigurace enginu
// ─────────────────────────────────────────────
#define SYNTHEX_SAMPLE_RATE     44100u
#define SYNTHEX_VOICES          8u          // fáze 4: 4 → 8 hlasů
#define SYNTHEX_DAC_RESOLUTION  12u
#define SYNTHEX_DAC_MAX         4095u
#define SYNTHEX_DAC_MID         2048u

// Fázový akumulátor — 32-bitový, horních 11 bitů = index (log2(2048) = 11)
#define SYNTHEX_PHASE_SHIFT     (32u - 11u)   // = 21

// Lineární interpolace — bits [20:13] jako 8-bit frakce
#define SYNTHEX_FRAC_SHIFT      (SYNTHEX_PHASE_SHIFT - 8u)  // = 13

// Anti-click: 8 vzorků @ 44100 Hz ≈ 0.18 ms
#define SYNTHEX_FADE_STEPS      8u

// Maximální počet hlasů v jednom unison shluku
#define SYNTHEX_MAX_UNISON      4u

// ─────────────────────────────────────────────
//  Jeden hlas syntezátoru
// ─────────────────────────────────────────────
struct Voice {
    // ── Stav ─────────────────────────────────
    volatile bool     active;           // hlas hraje (nebo dojíždí fade-out)
    volatile bool     fadingOut;        // probíhá fade-out; active=false po dojezdu

    // ── Anti-click fade ───────────────────────
    // 0 = ticho, SYNTHEX_FADE_STEPS = plná amplituda
    volatile uint8_t  fadeStep;

    // ── Fázový akumulátor ─────────────────────
    volatile uint32_t phaseAccum;
    volatile uint32_t phaseIncrement;   // aktuální fázový přírůstek

    // ── Portamento ────────────────────────────
    // targetIncrement : cílová frekvence přepočtená na přírůstek
    // portaStep       : přírůstek k phaseIncrement za jeden vzorek
    //                   0 = okamžitá změna (portamento vypnuto nebo dosaženo cíle)
    // Pozitivní portaStep = stoupáme, negativní = klesáme.
    volatile uint32_t targetIncrement;
    volatile int32_t  portaStep;

    // ── Hlasitost ─────────────────────────────
    volatile uint16_t amplitude;        // 0–4095 (12-bit škála)
    WaveType          waveType;

    // ── Voice stealing & unison ───────────────
    // birthTime : logický čítač; nižší = starší = prioritní pro krádež
    // noteId    : 0 = samostatný hlas, >0 = patří do unison skupiny
    //             použij noteOffById(noteId) pro vypnutí celé skupiny
    uint32_t          birthTime;
    uint8_t           noteId;

    Voice()
        : active(false), fadingOut(false), fadeStep(0),
          phaseAccum(0), phaseIncrement(0),
          targetIncrement(0), portaStep(0),
          amplitude(SYNTHEX_DAC_MID), waveType(WaveType::SINE),
          birthTime(0), noteId(0) {}
};

// ─────────────────────────────────────────────
//  Hlavní třída enginu
// ─────────────────────────────────────────────
class Synthex {
public:
    static Synthex& getInstance();

    // ── Inicializace ──────────────────────────
    void begin();

    // ─────────────────────────────────────────────────────────────────
    //  API pro přehrávání
    // ─────────────────────────────────────────────────────────────────

    // Explicitní index — zpětně kompatibilní s fází 1–3.
    // Portamento se uplatní, pokud je hlas active a není fadingOut.
    void noteOn(uint8_t voiceIdx, float freqHz,
                uint16_t amplitude = SYNTHEX_DAC_MID,
                WaveType wave      = WaveType::SINE);

    // Auto-alokace s voice stealing — vrátí použitý index hlasu.
    // Strategie krádeže: 1) neaktivní → 2) nejtiší fading-out → 3) nejstarší
    uint8_t noteOnAuto(float freqHz,
                       uint16_t amplitude = SYNTHEX_DAC_MID,
                       WaveType wave      = WaveType::SINE);

    // Unison: unisonVoices hlasů rozladěných o ±detuneCents/2 kolem freqHz.
    // Vrátí noteId pro skupinové noteOffById(); při 1 hlasu = bez detune.
    //
    //   unisonVoices = 2 : [-d/2, +d/2]
    //   unisonVoices = 3 : [-d/2, 0, +d/2]
    //   unisonVoices = 4 : rovnoměrně od -d/2 do +d/2
    //
    uint8_t noteOnUnison(float freqHz,
                         uint8_t  unisonVoices = 2u,
                         float    detuneCents  = 10.0f,
                         uint16_t amplitude    = SYNTHEX_DAC_MID,
                         WaveType wave         = WaveType::SINE);

    // Fade-out jednoho hlasu (explicitní index)
    void noteOff(uint8_t voiceIdx);

    // Fade-out celé unison skupiny (pro ID vrácené noteOnUnison)
    void noteOffById(uint8_t noteId);

    // ─────────────────────────────────────────────────────────────────
    //  Parametry
    // ─────────────────────────────────────────────────────────────────

    // Portamento: lineární glide v ms; 0 = okamžitá změna frekvence.
    // Platí pro všechny hlasy globálně.
    void  setPortamento(float timeMs);
    float getPortamento() const { return _portaTimeMs; }

    // ── ISR ──────────────────────────────────
    void processSample();   // volá výhradně TC3_Handler

    // ── Diagnostika ──────────────────────────
    static uint32_t freqToIncrement(float freqHz);
    uint32_t        getIsrCount() const { return _isrCount; }

private:
    Synthex();
    Synthex(const Synthex&)            = delete;
    Synthex& operator=(const Synthex&) = delete;

    void    _initDACC();
    void    _initTimer();

    // Vybere hlas pro nový tón (voice stealing při plné polyfónii)
    uint8_t _findFreeVoice();

    // ── Data ─────────────────────────────────
    Voice    _voices[SYNTHEX_VOICES];
    uint32_t _isrCount;
    uint32_t _lfsrState;        // Galoisův LFSR pro WaveType::SQUARE (noise)

    float    _portaTimeMs;      // globální čas portamenta [ms]
    uint32_t _voiceClock;       // monotónní čítač; přičítá se při každém noteOn
    uint8_t  _nextNoteId;       // rotuje 1–255; 0 je rezervováno jako "bez skupiny"
};

#ifdef __cplusplus
extern "C" { void TC3_Handler(); }
#endif
