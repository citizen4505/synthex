/*
 * Synthex.cpp
 * Wavetable syntéza — tabulky čteny přímo z Flash (wavetables.h)
 * Arduino Due / SAM3X8E, 84 MHz ARM Cortex-M3
 *
 * Fáze 3 — ADSR obálka:
 *   • AdsrState stavový automat: IDLE → ATTACK → DECAY → SUSTAIN → RELEASE → IDLE
 *   • Obálka počítána v Q16 fixed-point → nulová float operace v ISR
 *   • noteOff() spustí Release z aktuální hladiny (funguje i během Attack/Decay)
 *   • setAdsr() nastavuje parametry per-hlas; platí od příštího noteOn()
 *   • Mix: (sample × amplitude × envLevel) >> 24 — dvoustupňový posun, bez overflow
 */

#include "Synthex.h"
#include <cstring>

// ─────────────────────────────────────────────
//  Singleton
// ─────────────────────────────────────────────
Synthex& Synthex::getInstance() {
    static Synthex instance;
    return instance;
}

Synthex::Synthex()
    : _isrCount(0), _lfsrState(0xACE1u) {}

// ─────────────────────────────────────────────
//  begin()
// ─────────────────────────────────────────────
void Synthex::begin() {
    _initDACC();
    _initTimer();
}

// ─────────────────────────────────────────────
//  setAdsr — nastaví parametry obálky pro daný hlas
//  Platí od příštího noteOn(); neovlivní právě hrající tón.
// ─────────────────────────────────────────────
void Synthex::setAdsr(uint8_t idx,
                      uint16_t attackMs,
                      uint16_t decayMs,
                      uint16_t sustainLevel,
                      uint16_t releaseMs) {
    if (idx >= SYNTHEX_VOICES) return;
    // Zápis mimo ISR — není nutný disable_irq (AdsrParams se čte jen v noteOn)
    AdsrParams& p     = _voices[idx].adsr;
    p.attackMs        = attackMs;
    p.decayMs         = decayMs;
    p.sustainLevel    = (sustainLevel > SYNTHEX_DAC_MAX) ? SYNTHEX_DAC_MAX : sustainLevel;
    p.releaseMs       = releaseMs;
}

// ─────────────────────────────────────────────
//  noteOn — předpočítá ADSR kroky a spustí Attack fázi
//
//  Předpočítané hodnoty (Q16 fixed-point):
//
//    atkStep = FULL / atkSamples       (kladný, 0 → FULL)
//    decStep = -(FULL - susTarget) / decSamples  (záporný, FULL → susTarget)
//    susTarget = sustainLevel << 16    (cílová hladina Sustain)
//    relSamples = releaseMs × 44.1     (délka Release — krok se počítá v noteOff)
//
//  Proč předpočítat mimo ISR?
//    Dělení (/) je na ARM Cortex-M3 cca 12 cyklů.
//    ISR běží 44100× za sekundu → každý zbytečný cyklus navíc je drahý.
//    Předpočítáme jednou při noteOn a ISR pak jen sčítá.
// ─────────────────────────────────────────────
void Synthex::noteOn(uint8_t idx, float freqHz,
                     uint16_t amplitude, WaveType wave) {
    if (idx >= SYNTHEX_VOICES) return;

    Voice& v            = _voices[idx];
    const AdsrParams& p = v.adsr;

    // Převod ms → vzorky (float mimo ISR, výsledek uložit jako uint32)
    constexpr float kMs = static_cast<float>(SYNTHEX_SAMPLE_RATE) / 1000.0f;

    const uint32_t atkSamples = (p.attackMs  > 0u)
                                ? static_cast<uint32_t>(p.attackMs  * kMs) : 0u;
    const uint32_t decSamples = (p.decayMs   > 0u)
                                ? static_cast<uint32_t>(p.decayMs   * kMs) : 0u;
    const uint32_t relSamples = (p.releaseMs > 0u)
                                ? static_cast<uint32_t>(p.releaseMs * kMs) : 0u;

    // Q16 cílová hladina Sustain
    const int32_t susTarget = static_cast<int32_t>(p.sustainLevel) << 16;

    // Q16 krok pro Decay: od FULL_SCALE dolů k susTarget
    const int32_t decStep = (decSamples > 0u)
        ? -((SYNTHEX_ENV_FULL - susTarget) / static_cast<int32_t>(decSamples))
        : -(SYNTHEX_ENV_FULL - susTarget);   // okamžitý decay

    __disable_irq();

    // ── Generátor ──────────────────────────────────────────
    v.phaseIncrement = freqToIncrement(freqHz);
    v.amplitude      = amplitude;
    v.waveType       = wave;
    v.phaseAccum     = 0;

    // ── Uložit předpočítané hodnoty pro ISR přechody ───────
    v.susTarget  = susTarget;
    v.decStep    = decStep;
    v.decSamples = (decSamples > 0u) ? decSamples : 1u;
    v.relSamples = (relSamples > 0u) ? relSamples : 1u;

    // ── Start obálky ───────────────────────────────────────
    if (atkSamples > 0u) {
        // Normální případ: náběh od ticha
        const int32_t atkStep = SYNTHEX_ENV_FULL / static_cast<int32_t>(atkSamples);
        v.envAccum     = 0;
        v.envStep      = atkStep;
        v.envCountdown = atkSamples;
        v.adsrState    = AdsrState::ATTACK;
    } else {
        // attackMs == 0: okamžitý náběh → rovnou do Decay
        v.envAccum     = SYNTHEX_ENV_FULL;
        v.envStep      = decStep;
        v.envCountdown = v.decSamples;
        v.adsrState    = AdsrState::DECAY;
    }

    v.active = true;

    __enable_irq();
}
// ─────────────────────────────────────────────
//  noteOff — spustí Release fázi z aktuální hladiny
//
//  Krok Release se počítá dynamicky z envAccum v okamžiku noteOff.
//  Díky tomu Release trvá vždy releaseMs ms bez ohledu na to,
//  v jaké fázi (Attack/Decay/Sustain) byl hlas při noteOff.
//
//  Ochrana před dělením nulou: pokud je envAccum ≤ 0, hlas
//  se rovnou deaktivuje — není co doznívat.
// ─────────────────────────────────────────────
void Synthex::noteOff(uint8_t idx) {
    if (idx >= SYNTHEX_VOICES) return;

    __disable_irq();
    Voice& v = _voices[idx];

    if (v.active
        && v.adsrState != AdsrState::RELEASE
        && v.adsrState != AdsrState::IDLE)
    {
        if (v.envAccum > 0 && v.relSamples > 0u) {
            // Q16 krok pro Release: od aktuální hladiny na 0 za relSamples vzorků
            int32_t relStep = -(v.envAccum / static_cast<int32_t>(v.relSamples));
            // Zaručit alespoň jeden krok dolů (pro případ zaokrouhlení na 0)
            if (relStep == 0) relStep = -1;
            v.envStep      = relStep;
            v.envCountdown = v.relSamples;
            v.adsrState    = AdsrState::RELEASE;
        } else {
            // Okamžité ztišení (releaseMs == 0 nebo hlas již tichý)
            v.envAccum  = 0;
            v.active    = false;
            v.adsrState = AdsrState::IDLE;
        }
    }

    __enable_irq();
}

// ─────────────────────────────────────────────
//  processSample() — volá se 44100× za sekundu z TC3_Handler
//
//  Výpočet vzorku (jeden hlas):
//
//  1. Fázový akumulátor:
//       phaseAccum += phaseIncrement   (wrap přes 2^32 = 1 perioda)
//
//  2. Wavetable lookup s lineární interpolací (stejný jako Fáze 2)
//
//  3. ADSR stavový automat:
//       envAccum += envStep   (Q16 přírůstek)
//       --envCountdown == 0   → přechod do další fáze
//
//  4. Mix (dvoustupňové škálování — bez int32 overflow):
//       scaledAmp = (amplitude × envLevel) >> 12   (0–4095)
//       mix      += (sample    × scaledAmp) >> 12  (≈ ±2291 na hlas)
//
//  Overflow proof:
//       amplitude max 4095, envLevel max 4095
//       4095 × 4095 = 16 769 025 → >> 12 = 4094              ✓ int32
//       sample max 2292 × scaledAmp max 4094 = 9 383 448      ✓ int32
//       4 hlasy × 2291 = 9 164 → po + DAC_MID = 11 212 → clamp ✓
// ─────────────────────────────────────────────
void Synthex::processSample() {
    int32_t mix = 0;

    for (uint8_t i = 0; i < SYNTHEX_VOICES; ++i) {
        Voice& v = _voices[i];
        if (!v.active) continue;

        // ── Posun fázového akumulátoru ────────────────────────
        v.phaseAccum += v.phaseIncrement;

        // ── Generování vzorku ─────────────────────────────────
        int32_t sample;

        if (v.waveType == WaveType::SQUARE) {
            // Galoisův LFSR — bílý šum (viz komentář Fáze 2)
            _lfsrState ^= _lfsrState >> 1;
            _lfsrState ^= _lfsrState << 5;
            _lfsrState ^= _lfsrState >> 14;
            sample = static_cast<int32_t>(
                         static_cast<int16_t>(_lfsrState & 0x0FFFu) - 2048);
        } else {
            const uint16_t idx0 = static_cast<uint16_t>(
                                      v.phaseAccum >> SYNTHEX_PHASE_SHIFT)
                                  & static_cast<uint16_t>(SYNTHEX_WAVETABLE_SIZE - 1u);
            const uint16_t idx1 = (idx0 + 1u)
                                  & static_cast<uint16_t>(SYNTHEX_WAVETABLE_SIZE - 1u);
            const uint8_t  frac = static_cast<uint8_t>(v.phaseAccum >> SYNTHEX_FRAC_SHIFT);

            const int32_t s0 = SYNTHEX_TABLES[static_cast<uint8_t>(v.waveType)][idx0];
            const int32_t s1 = SYNTHEX_TABLES[static_cast<uint8_t>(v.waveType)][idx1];
            sample = s0 + (((s1 - s0) * static_cast<int32_t>(frac)) >> 8);
        }

        // ── ADSR stavový automat ──────────────────────────────
        //
        //  Každá fáze:
        //    1. envAccum += envStep          (Q16 přírůstek)
        //    2. --envCountdown == 0          → přechod do další fáze
        //       nebo envAccum přesáhlo cíl  → přechod + korekce na cílovou hodnotu
        //
        //  Poznámka k `continue`:
        //    Uvnitř switch+for má `continue` sémanitku for-cyklu —
        //    přeskočí zbytek těla smyčky a přejde na další hlas.
        //    Používáme ho, když chceme hlas ztišit BEZ přidání do mixu.
        //
        switch (v.adsrState) {

            // ── IDLE: nemělo by nastat (active==false), pojistka ─
            case AdsrState::IDLE:
                v.active = false;
                continue;

            // ── ATTACK: lineární náběh 0 → FULL ─────────────────
            //   Přechod na DECAY: po uplynutí countdown
            //   nebo (pojistka) při přesahu FULL
            case AdsrState::ATTACK:
                v.envAccum += v.envStep;
                if (--v.envCountdown == 0u
                    || v.envAccum >= SYNTHEX_ENV_FULL)
                {
                    v.envAccum     = SYNTHEX_ENV_FULL;
                    // Přejdi do Decay (nebo rovnou do Sustain pokud decSamples == 1)
                    if (v.decSamples > 1u && v.decStep != 0) {
                        v.envStep      = v.decStep;
                        v.envCountdown = v.decSamples;
                        v.adsrState    = AdsrState::DECAY;
                    } else {
                        v.envAccum  = v.susTarget;
                        v.envStep   = 0;
                        v.adsrState = AdsrState::SUSTAIN;
                    }
                }
                break;

            // ── DECAY: lineární pokles FULL → susTarget ──────────
            //   Přechod na SUSTAIN: po uplynutí countdown
            //   nebo (pojistka) při podkročení susTarget
            case AdsrState::DECAY:
                v.envAccum += v.envStep;
                if (--v.envCountdown == 0u
                    || v.envAccum <= v.susTarget)
                {
                    v.envAccum  = v.susTarget;
                    v.envStep   = 0;
                    v.adsrState = AdsrState::SUSTAIN;
                }
                break;

            // ── SUSTAIN: envAccum drží susTarget; envStep == 0 ───
            //   Konec: pouze přes noteOff() → RELEASE
            case AdsrState::SUSTAIN:
                // Nic — ISR jen prochází touto větví, envAccum se nemění
                break;

            // ── RELEASE: lineární pokles current → 0 ─────────────
            //   Přechod na IDLE: envAccum ≤ 0 nebo countdown vyprší
            case AdsrState::RELEASE:
                v.envAccum += v.envStep;
                if (v.envAccum <= 0 || --v.envCountdown == 0u) {
                    v.envAccum  = 0;
                    v.active    = false;
                    v.adsrState = AdsrState::IDLE;
                    continue;   // žádný příspěvek do mixu
                }
                break;
        }

        // ── Výpočet a přidání do mixu ─────────────────────────
        //
        // envLevel:  envAccum >> 16   → 0–4095
        // scaledAmp: (amplitude × envLevel) >> 12  → 0–4095
        // příspěvek: (sample × scaledAmp) >> 12    → ≈ ±2291
        //
        const int32_t envLevel  = v.envAccum >> 16;
        const int32_t scaledAmp = (static_cast<int32_t>(v.amplitude)
                                   * envLevel) >> 12;
        mix += (sample * scaledAmp) >> 12;
    }

    // Unsigned offset pro DAC (0–4095); clamp pro případ přetečení mixu
    mix += SYNTHEX_DAC_MID;
    if (mix < 0)                                     mix = 0;
    if (mix > static_cast<int32_t>(SYNTHEX_DAC_MAX)) mix = SYNTHEX_DAC_MAX;

    DACC->DACC_CDR = static_cast<uint32_t>(mix);
    ++_isrCount;
}

// ─────────────────────────────────────────────
//  freqToIncrement — převod Hz → fázový přírůstek
// ─────────────────────────────────────────────
uint32_t Synthex::freqToIncrement(float freqHz) {
    constexpr float k = static_cast<float>(1ULL << 32) / SYNTHEX_SAMPLE_RATE;
    return static_cast<uint32_t>(freqHz * k);
}

// ─────────────────────────────────────────────
//  Inicializace DACC (12-bit DAC, kanál 1 = pin DAC1)
// ─────────────────────────────────────────────
void Synthex::_initDACC() {
    PMC->PMC_PCER1 |= (1u << (ID_DACC - 32));
    DACC->DACC_CR   = DACC_CR_SWRST;
    DACC->DACC_MR   = DACC_MR_REFRESH(8)
                    | DACC_MR_USER_SEL_CHANNEL1
                    | DACC_MR_STARTUP_8;
    DACC->DACC_CHER = DACC_CHER_CH1;
    DACC->DACC_CDR  = SYNTHEX_DAC_MID;
}

// ─────────────────────────────────────────────
//  Inicializace TC1/CH0 (TC3) @ 44100 Hz
//  MCK/2 = 42 MHz → RC = 42 000 000 / 44 100 ≈ 952
// ─────────────────────────────────────────────
void Synthex::_initTimer() {
    PMC->PMC_PCER0 |= (1u << ID_TC3);

    TcChannel* ch = &TC1->TC_CHANNEL[0];
    ch->TC_CCR = TC_CCR_CLKDIS;
    ch->TC_IDR = 0xFFFFFFFF;
    (void)ch->TC_SR;

    ch->TC_CMR = TC_CMR_TCCLKS_TIMER_CLOCK1
               | TC_CMR_WAVE
               | TC_CMR_WAVSEL_UP_RC;

    constexpr uint32_t RC = (84000000u / 2u) / SYNTHEX_SAMPLE_RATE;
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
    (void)TC1->TC_CHANNEL[0].TC_SR;
    Synthex::getInstance().processSample();
}
