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

// ADSR akumulátor — Q16 fixed-point; 4095 << 16 = plná amplituda
// Rozsah: 0 (ticho) … SYNTHEX_ADSR_FULL (maximum)
#define SYNTHEX_ADSR_FULL       (4095u << 16u)   // = 268 369 920

// ─────────────────────────────────────────────
//  Fáze ADSR obálky
// ─────────────────────────────────────────────
enum class AdsrPhase : uint8_t {
    IDLE    = 0,   // hlas neaktivní (před noteOn nebo po Release)
    ATTACK  = 1,   // náběh: adsrAccum  0 → FULL
    DECAY   = 2,   // pokles: adsrAccum FULL → sustainLevel << 16
    SUSTAIN = 3,   // drží: adsrAccum = sustainLevel << 16 (do noteOff)
    RELEASE = 4    // doznění: adsrAccum → 0; poté spustí anti-click fade-out
};

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

    // ── ADSR obálka ───────────────────────────
    // adsrPhase : aktuální fáze ATTACK / DECAY / SUSTAIN / RELEASE / IDLE
    // adsrAccum : Q16 akumulátor, 0 = ticho, SYNTHEX_ADSR_FULL = plná amplituda
    //             envLevel = adsrAccum >> 16  →  0–4095  (12-bitový multiplikátor)
    volatile AdsrPhase adsrPhase;
    volatile uint32_t  adsrAccum;

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
          adsrPhase(AdsrPhase::IDLE), adsrAccum(0u),
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

    // ─────────────────────────────────────────────────────────────────
    //  ADSR obálka — globální parametry pro všechny hlasy
    //
    //  attackMs  : náběh 0 → plná amplituda [ms]  (0 = okamžitý)
    //  decayMs   : pokles plná → sustain level [ms]
    //  sustain   : úroveň držení [0–4095]; 4095 = žádný pokles
    //  releaseMs : doznění sustain → 0 [ms]
    //
    //  Kroky jsou přepočítány ihned; lze volat za běhu.
    //  Výchozí hodnoty: A=5ms, D=50ms, S=4095, R=100ms
    // ─────────────────────────────────────────────────────────────────
    void setAdsr(float    attackMs,
                 float    decayMs,
                 uint16_t sustainLevel,
                 float    releaseMs);

    float    getAdsrAttack()  const { return _adsrAttackMs; }
    float    getAdsrDecay()   const { return _adsrDecayMs; }
    uint16_t getAdsrSustain() const { return _adsrSustain; }
    float    getAdsrRelease() const { return _adsrReleaseMs; }

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

    // ── ADSR parametry ────────────────────────
    // Časy (float) — pouze pro get, přepočet probíhá v setAdsr()
    float    _adsrAttackMs;
    float    _adsrDecayMs;
    float    _adsrReleaseMs;

    // Kroky (volatile — čte je ISR processSample)
    // Q16 přírůstek/úbytek adsrAccum za jeden vzorek:
    //   SYNTHEX_ADSR_FULL / (timeMs * SYNTHEX_SAMPLE_RATE / 1000)
    volatile uint16_t _adsrSustain;      // sustain level 0–4095
    volatile uint32_t _adsrAttackStep;
    volatile uint32_t _adsrDecayStep;
    volatile uint32_t _adsrReleaseStep;
};

#ifdef __cplusplus
extern "C" { void TC3_Handler(); }
#endif
