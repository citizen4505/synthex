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
#define SYNTHEX_PHASE_SHIFT     (32u - 11u)         // = 21

// Lineární interpolace — dolních 8 bitů frakce z akumulátoru
#define SYNTHEX_FRAC_SHIFT      (SYNTHEX_PHASE_SHIFT - 8u)  // = 13

// ADSR obálka — Q16 fixed-point, plná škála = 4095 << 16
// Proč Q16? Celočíselné přírůstky s dostatečnou rozlišovací schopností
// bez float v ISR; výstup envAccum >> 16 dává přímo 0–4095.
#define SYNTHEX_ENV_FULL        (static_cast<int32_t>(4095u << 16))  // = 268,369,920

// WaveType a SYNTHEX_WAVETABLE_SIZE jsou definovány v wavetables.h

// ─────────────────────────────────────────────
//  ADSR stavový automat
//
//  Přechody:
//    noteOn()  → IDLE → ATTACK → DECAY → SUSTAIN
//    noteOff() →                         SUSTAIN → RELEASE → IDLE
//
//  Speciální případ: noteOff() během ATTACK nebo DECAY okamžitě
//  přejde do RELEASE z aktuální hladiny — žádný click.
// ─────────────────────────────────────────────
enum class AdsrState : uint8_t {
    IDLE    = 0,   // hlas neaktivní, nepřispívá do mixu
    ATTACK  = 1,   // náběh:  envAccum: 0 → FULL         (attackMs ms)
    DECAY   = 2,   // pokles: envAccum: FULL → susTarget  (decayMs ms)
    SUSTAIN = 3,   // výdrž:  envAccum drží susTarget dokud nepřijde noteOff()
    RELEASE = 4,   // doznívání: envAccum: current → 0   (releaseMs ms)
};

// ─────────────────────────────────────────────
//  Parametry ADSR obálky (nastavuje se přes setAdsr())
// ─────────────────────────────────────────────
struct AdsrParams {
    uint16_t attackMs     =  10;    // doba náběhu  (ms)
    uint16_t decayMs      = 100;    // doba poklesu (ms)
    uint16_t sustainLevel = 3072;   // hladina výdrže, 0–4095 (výchozí 75 %)
    uint16_t releaseMs    = 200;    // doba doznívání (ms)
};

// ─────────────────────────────────────────────
//  Jeden hlas syntezátoru
// ─────────────────────────────────────────────
struct Voice {
    // ── Stavové příznaky ─────────────────────────────────────────────────────
    volatile bool      active;      // hlas právě hraje (nebo doznívá)
    volatile AdsrState adsrState;   // aktuální fáze obálky

    // ── Generátor (fázový akumulátor + parametry) ─────────────────────────────
    volatile uint32_t  phaseAccum;
    volatile uint32_t  phaseIncrement;
    volatile uint16_t  amplitude;   // velocity / hlasitost (0–4095, 12-bit škála)
    WaveType           waveType;

    // ── ADSR obálka — Q16 fixed-point ────────────────────────────────────────
    //
    //  Plná škála:  SYNTHEX_ENV_FULL = 4095 << 16 = 268 369 920
    //  Výstup ISR:  envLevel = envAccum >> 16   (0–4095)
    //
    //  Schéma envAccum [31 ......... 16][15 ........ 0]
    //                   └── envLevel ──┘└── frakce ──┘
    //
    //  envStep je podepsaný přírůstek na vzorek:
    //    Attack:  kladný  (envAccum roste 0 → FULL)
    //    Decay:   záporný (envAccum klesá FULL → susTarget)
    //    Sustain: nula    (envAccum drží susTarget)
    //    Release: záporný (envAccum klesá current → 0)
    //
    volatile int32_t   envAccum;      // aktuální hladina obálky (Q16)
    volatile int32_t   envStep;       // přírůstek / vzorek (Q16, signed)
    volatile uint32_t  envCountdown;  // vzorků do konce aktuální fáze

    // ── Předpočítané hodnoty pro přechody fází ────────────────────────────────
    // Zapisuje noteOn() / noteOff() (mimo ISR), čte processSample() (ISR).
    // Díky tomu ISR neprovádí žádné dělení ani float operace.
    volatile int32_t   decStep;       // Q16/vzorek pro fázi Decay
    volatile uint32_t  decSamples;    // délka fáze Decay [vzorky]
    volatile int32_t   susTarget;     // Q16 cílová hladina Sustain
    volatile uint32_t  relSamples;    // délka fáze Release [vzorky]

    // ── Parametry obálky ─────────────────────────────────────────────────────
    AdsrParams adsr;    // nastavitelné přes setAdsr(); platí od příštího noteOn()

    Voice()
        : active(false), adsrState(AdsrState::IDLE),
          phaseAccum(0), phaseIncrement(0),
          amplitude(SYNTHEX_DAC_MID), waveType(WaveType::SINE),
          envAccum(0), envStep(0), envCountdown(0),
          decStep(0), decSamples(1u), susTarget(0),
          relSamples(static_cast<uint32_t>(200u * SYNTHEX_SAMPLE_RATE / 1000u))
    {}
};

// ─────────────────────────────────────────────
//  Hlavní třída enginu
// ─────────────────────────────────────────────
class Synthex {
public:
    static Synthex& getInstance();

    // Inicializace — DAC + Timer
    void begin();

    // Nastaví ADSR parametry pro daný hlas (platné od příštího noteOn)
    void setAdsr(uint8_t voiceIdx,
                 uint16_t attackMs,
                 uint16_t decayMs,
                 uint16_t sustainLevel,
                 uint16_t releaseMs);

    // noteOn: předpočítá ADSR kroky, spustí Attack fázi
    void noteOn(uint8_t voiceIdx, float freqHz,
                uint16_t amplitude = SYNTHEX_DAC_MID,
                WaveType wave = WaveType::SINE);

    // noteOff: spustí Release fázi z aktuální hladiny; active=false nastaví ISR
    void noteOff(uint8_t voiceIdx);

    // Volá se výhradně z ISR (TC3_Handler) — 44100× za sekundu
    void processSample();

    static uint32_t freqToIncrement(float freqHz);

    uint32_t getIsrCount() const { return _isrCount; }

private:
    Synthex();
    Synthex(const Synthex&)            = delete;
    Synthex& operator=(const Synthex&) = delete;

    void _initDACC();
    void _initTimer();

    Voice    _voices[SYNTHEX_VOICES];
    uint32_t _isrCount;
    uint32_t _lfsrState;    // Galoisův LFSR pro WaveType::SQUARE (noise)
};

#ifdef __cplusplus
extern "C" { void TC3_Handler(); }
#endif
