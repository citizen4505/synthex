/*
 * Synthex.cpp
 * Fáze 4 — Rozšíření hlasů a polyfonie
 *
 *   • SYNTHEX_VOICES = 8
 *   • Voice stealing     — při plné polyfónii se ukrade nejstarší/nejtiší hlas
 *   • Portamento         — lineární glide phaseIncrement; integer aritmetika v ISR
 *   • Unison / Detune    — noteOnUnison() rozloží N hlasů symetricky v centech
 *
 * Arduino Due / SAM3X8E, 84 MHz ARM Cortex-M3
 */

#include "Synthex.h"
#include <cmath>       // powf — pouze v noteOnUnison(), NIKDY v ISR

// ─────────────────────────────────────────────
//  Singleton
// ─────────────────────────────────────────────
Synthex& Synthex::getInstance() {
    static Synthex instance;
    return instance;
}

Synthex::Synthex()
    : _isrCount(0), _lfsrState(0xACE1u),
      _portaTimeMs(0.0f), _voiceClock(0), _nextNoteId(1),
      _adsrAttackMs(5.0f), _adsrDecayMs(50.0f), _adsrReleaseMs(100.0f),
      _adsrSustain(4095u),
      _adsrAttackStep(0u), _adsrDecayStep(0u), _adsrReleaseStep(0u)
{
    // Předpočítej kroky pro výchozí parametry.
    // ISR ještě neběží → __disable_irq není nutný.
    constexpr float    SR   = static_cast<float>(SYNTHEX_SAMPLE_RATE);
    constexpr uint32_t FULL = SYNTHEX_ADSR_FULL;

    _adsrAttackStep  = static_cast<uint32_t>(FULL / (_adsrAttackMs  * SR / 1000.0f));
    _adsrDecayStep   = static_cast<uint32_t>(FULL / (_adsrDecayMs   * SR / 1000.0f));
    _adsrReleaseStep = static_cast<uint32_t>(FULL / (_adsrReleaseMs * SR / 1000.0f));
}

// ─────────────────────────────────────────────
//  begin()
// ─────────────────────────────────────────────
void Synthex::begin() {
    _initDACC();
    _initTimer();
}

// ─────────────────────────────────────────────
//  _findFreeVoice — výběr hlasu pro nový tón
//
//  Priorita:
//    1. Neaktivní hlas           → ideální, bez artefaktů
//    2. Nejtiší fading-out hlas  → krádež při doznívání je méně slyšitelná
//    3. Nejstarší aktivní hlas   → „oldest-note stealing" (klasická strategie)
// ─────────────────────────────────────────────
uint8_t Synthex::_findFreeVoice() {
    // 1. Hledej zcela neaktivní hlas
    for (uint8_t i = 0; i < SYNTHEX_VOICES; ++i) {
        if (!_voices[i].active) return i;
    }

    // 2a. Nejtiší fading-out hlas (fadeStep = 0 → úplné ticho)
    uint8_t bestFO  = 0xFF;
    uint8_t minFade = 0xFF;
    for (uint8_t i = 0; i < SYNTHEX_VOICES; ++i) {
        if (_voices[i].fadingOut && _voices[i].fadeStep < minFade) {
            minFade = _voices[i].fadeStep;
            bestFO  = i;
        }
    }
    if (bestFO != 0xFF) return bestFO;

    // 2b. Hlas v RELEASE fázi s nejnižším adsrAccum (nejblíže tichu)
    //     Je to lepší volba než ukrást plně znějící hlas.
    uint8_t  bestRel   = 0xFF;
    uint32_t minAccum  = UINT32_MAX;
    for (uint8_t i = 0; i < SYNTHEX_VOICES; ++i) {
        if (_voices[i].adsrPhase == AdsrPhase::RELEASE
            && _voices[i].adsrAccum < minAccum) {
            minAccum = _voices[i].adsrAccum;
            bestRel  = i;
        }
    }
    if (bestRel != 0xFF) return bestRel;

    // 3. Ukradni nejstarší aktivní hlas (nejnižší birthTime)
    uint8_t  oldest   = 0;
    uint32_t minBirth = _voices[0].birthTime;
    for (uint8_t i = 1; i < SYNTHEX_VOICES; ++i) {
        if (_voices[i].birthTime < minBirth) {
            minBirth = _voices[i].birthTime;
            oldest   = i;
        }
    }
    return oldest;
}

// ─────────────────────────────────────────────
//  noteOn — explicitní index hlasu
//
//  Portamento logika:
//    Pokud je hlas právě aktivní (a není fadingOut), NEresetujeme phaseAccum
//    a nastavíme portaStep tak, aby phaseIncrement lineárně doputovalo
//    z aktuální hodnoty na targetIncrement za _portaTimeMs milisekund.
//    Výsledek: plynulý legato glide bez fázového skoku.
//
//    Pokud portamento není aktivní (timeMs == 0) nebo hlas nestartuje
//    z aktivního stavu, phaseAccum se resetuje — čistý nový tón.
// ─────────────────────────────────────────────
void Synthex::noteOn(uint8_t idx, float freqHz,
                     uint16_t amplitude, WaveType wave) {
    if (idx >= SYNTHEX_VOICES) return;

    const uint32_t newInc = freqToIncrement(freqHz);

    __disable_irq();
    Voice& v = _voices[idx];

    if (_portaTimeMs > 0.0f && v.active && !v.fadingOut) {
        // ── Portamento: klouzej z aktuální frekvence na novou ──────────
        v.targetIncrement = newInc;

        // Kolik vzorků trvá glide?
        const uint32_t samples = static_cast<uint32_t>(
            _portaTimeMs * (SYNTHEX_SAMPLE_RATE / 1000.0f));

        if (samples > 1u) {
            // Přírůstek za vzorek — záměrně integer dělení (dostatečná přesnost)
            const int32_t diff = static_cast<int32_t>(newInc) -
                                 static_cast<int32_t>(v.phaseIncrement);
            v.portaStep = diff / static_cast<int32_t>(samples);

            // Pokud diff < samples (malý frekvenční skok), integer dělení zaokrouhlí
            // na 0 → ISR by nikdy nedosáhla cíle. Pojistka: min. ±1.
            if (v.portaStep == 0 && diff != 0) {
                v.portaStep = (diff > 0) ? 1 : -1;
            }
        } else {
            // Příliš krátký čas → přeskok
            v.phaseIncrement  = newInc;
            v.portaStep       = 0;
        }
        // phaseAccum záměrně NEresetujeme → legato kontinuita průběhu
    } else {
        // ── Nový tón (bez portamenta nebo hlas byl neaktivní) ──────────
        v.phaseIncrement  = newInc;
        v.targetIncrement = newInc;
        v.portaStep       = 0;
        v.phaseAccum      = 0;
    }

    v.amplitude = amplitude;
    v.waveType  = wave;
    v.fadeStep  = 0;        // fade-in začne od ticha
    v.fadingOut = false;
    v.active    = true;
    v.birthTime = _voiceClock++;
    v.noteId    = 0;        // noteOnUnison ho přepíše na skupinové ID
    v.adsrPhase = AdsrPhase::ATTACK;   // ADSR: vždy začínáme úplně od nuly
    v.adsrAccum = 0u;
    __enable_irq();
}

// ─────────────────────────────────────────────
//  noteOnAuto — auto-alokace s voice stealing
// ─────────────────────────────────────────────
uint8_t Synthex::noteOnAuto(float freqHz, uint16_t amplitude, WaveType wave) {
    // _findFreeVoice čte volatile data mimo ISR → krátký window neurčitosti
    // je akceptovatelný; nejhorší případ = dva hlasy ukradeny ve stejný okamžik.
    const uint8_t idx = _findFreeVoice();
    noteOn(idx, freqHz, amplitude, wave);
    return idx;
}

// ─────────────────────────────────────────────
//  noteOnUnison — N hlasů s detune kolem freqHz
//
//  Rozložení v centech (symetrické, rovnoměrné):
//
//    n=1 : [  0 ]
//    n=2 : [ -d/2,          +d/2 ]
//    n=3 : [ -d/2,    0,    +d/2 ]
//    n=4 : [ -d/2, -d/6, +d/6, +d/2 ]
//
//  Převod centů na Hz: f' = f × 2^(cents / 1200)
//  (počítáno v main-thread pomocí powf; NIKOLI v ISR)
//
//  Vrátí noteId (1–255). Hodnota 0 je rezervována jako "bez skupiny".
// ─────────────────────────────────────────────
uint8_t Synthex::noteOnUnison(float freqHz, uint8_t unisonVoices,
                               float detuneCents,
                               uint16_t amplitude, WaveType wave) {
    if (unisonVoices < 1u) unisonVoices = 1u;
    if (unisonVoices > SYNTHEX_MAX_UNISON) unisonVoices = SYNTHEX_MAX_UNISON;

    // Přiřaď skupinové ID
    const uint8_t id = _nextNoteId;
    _nextNoteId = (_nextNoteId == 255u) ? 1u : (_nextNoteId + 1u);

    for (uint8_t i = 0; i < unisonVoices; ++i) {
        // Offset v centech: rovnoměrně od -detuneCents/2 do +detuneCents/2
        // Speciální případ: 1 hlas → 0 centů (žádné rozladění)
        float offsetCents = 0.0f;
        if (unisonVoices > 1u) {
            offsetCents = -detuneCents * 0.5f
                         + static_cast<float>(i)
                           * (detuneCents / static_cast<float>(unisonVoices - 1u));
        }

        // f' = f × 2^(cents/1200)
        const float detunedFreq = freqHz * powf(2.0f, offsetCents / 1200.0f);

        const uint8_t voiceIdx = _findFreeVoice();
        noteOn(voiceIdx, detunedFreq, amplitude, wave);

        // noteOn nastaví noteId = 0; přepíš na skupinové ID
        __disable_irq();
        _voices[voiceIdx].noteId = id;
        __enable_irq();
    }

    return id;
}

// ─────────────────────────────────────────────
//  noteOff — spustí ADSR Release fázi
//
//  Nespouštíme fadingOut přímo — to by přeskočilo ADSR Release.
//  ADSR Release decrementuje adsrAccum → 0; teprve pak nastaví
//  fadingOut=true a předá řízení anti-click fade-out mechanismu.
// ─────────────────────────────────────────────
void Synthex::noteOff(uint8_t idx) {
    if (idx >= SYNTHEX_VOICES) return;
    __disable_irq();
    Voice& v = _voices[idx];
    if (v.active && !v.fadingOut
        && v.adsrPhase != AdsrPhase::RELEASE
        && v.adsrPhase != AdsrPhase::IDLE) {
        v.adsrPhase = AdsrPhase::RELEASE;
    }
    __enable_irq();
}

// ─────────────────────────────────────────────
//  noteOffById — spustí ADSR Release celé unison skupiny
// ─────────────────────────────────────────────
void Synthex::noteOffById(uint8_t noteId) {
    if (noteId == 0u) return;
    __disable_irq();
    for (uint8_t i = 0; i < SYNTHEX_VOICES; ++i) {
        Voice& v = _voices[i];
        if (v.active && !v.fadingOut
            && v.noteId == noteId
            && v.adsrPhase != AdsrPhase::RELEASE
            && v.adsrPhase != AdsrPhase::IDLE) {
            v.adsrPhase = AdsrPhase::RELEASE;
        }
    }
    __enable_irq();
}

// ─────────────────────────────────────────────
//  setPortamento — globální čas glide v ms (0 = vypnuto)
// ─────────────────────────────────────────────
void Synthex::setPortamento(float timeMs) {
    _portaTimeMs = (timeMs > 0.0f) ? timeMs : 0.0f;
}

// ─────────────────────────────────────────────
//  setAdsr — nastaví ADSR parametry pro všechny hlasy
//
//  Přepočet Q16 kroků:
//    step = SYNTHEX_ADSR_FULL / samples
//         = SYNTHEX_ADSR_FULL / (timeMs * SR / 1000)
//
//  Krok pro decay/release je odvozený z celého rozsahu (0→FULL),
//  takže skutečná délka decay se zkrátí úměrně hloubce poklesu,
//  a release se zkrátí úměrně výšce sustain. Toto je standardní
//  chování jednoduchých ADSR implementací (stejný přístup jako MCP4922).
//
//  Volej z hlavní smyčky, NIKDY z ISR.
// ─────────────────────────────────────────────
void Synthex::setAdsr(float attackMs, float decayMs,
                      uint16_t sustainLevel, float releaseMs) {
    _adsrAttackMs  = attackMs;
    _adsrDecayMs   = decayMs;
    _adsrReleaseMs = releaseMs;

    constexpr float    SR   = static_cast<float>(SYNTHEX_SAMPLE_RATE);
    constexpr uint32_t FULL = SYNTHEX_ADSR_FULL;

    // Převod ms → počet vzorků; pojistka: min. 1 vzorek
    const uint32_t atkSamp = static_cast<uint32_t>(attackMs  * SR / 1000.0f);
    const uint32_t decSamp = static_cast<uint32_t>(decayMs   * SR / 1000.0f);
    const uint32_t relSamp = static_cast<uint32_t>(releaseMs * SR / 1000.0f);

    const uint32_t newAtkStep  = (atkSamp > 1u) ? (FULL / atkSamp) : FULL;
    const uint32_t newDecStep  = (decSamp > 1u) ? (FULL / decSamp) : FULL;
    const uint32_t newRelStep  = (relSamp > 1u) ? (FULL / relSamp) : FULL;
    const uint16_t newSustain  = (sustainLevel > 4095u) ? 4095u : sustainLevel;

    // Atomická aktualizace — kroky čte ISR
    __disable_irq();
    _adsrSustain      = newSustain;
    _adsrAttackStep   = newAtkStep;
    _adsrDecayStep    = newDecStep;
    _adsrReleaseStep  = newRelStep;
    __enable_irq();
}

// ─────────────────────────────────────────────
//  processSample() — volá se 44100× za sekundu z TC3_Handler
//
//  Rozšíření fáze 4 oproti fázi 3:
//
//  0. Portamento update (PŘED posunem fázového akumulátoru):
//       if portaStep != 0:
//           remaining = targetIncrement - phaseIncrement   (signed)
//           if |remaining| ≤ |portaStep| → snap na target, portaStep = 0
//           else                         → phaseIncrement += portaStep
//
//  Portamento po celou dobu glide mění phaseIncrement o konstantní portaStep
//  za každý vzorek → lineární glide frekvence.
//
//  Unsigned aritmetika pro přičtení záporného portaStep:
//       phaseIncrement += (uint32_t)portaStep
//  funguje díky two's complement wrap-around (standardní pro unsigned v C++).
//  Příklad: 1000u + (uint32_t)(-100) = 1000u + 0xFFFFFF9Cu = 900u ✓
// ─────────────────────────────────────────────
void Synthex::processSample() {
    int32_t mix = 0;

    for (uint8_t i = 0; i < SYNTHEX_VOICES; ++i) {
        Voice& v = _voices[i];
        if (!v.active) continue;

        // ── 0. Portamento ─────────────────────────────────────
        //
        // Vše v integer aritmetice — žádný float v ISR.
        //
        if (v.portaStep != 0) {
            // signed rozdíl: kladný = stoupáme, záporný = klesáme
            const int32_t remaining = static_cast<int32_t>(v.targetIncrement)
                                    - static_cast<int32_t>(v.phaseIncrement);

            // Dosáhli jsme cíle nebo jsme ho přeskočili?
            const bool arrived = (v.portaStep > 0)
                                 ? (remaining <= v.portaStep)
                                 : (remaining >= v.portaStep);
            if (arrived) {
                v.phaseIncrement = v.targetIncrement;
                v.portaStep      = 0;
            } else {
                // Přičtení záporného int32_t přes unsigned wrap — viz komentář výše
                v.phaseIncrement += static_cast<uint32_t>(v.portaStep);
            }
        }

        // ── 1. Posun fázového akumulátoru ─────────────────────
        v.phaseAccum += v.phaseIncrement;

        // ── 2. Generování vzorku ──────────────────────────────
        int32_t sample;

        if (v.waveType == WaveType::SQUARE) {
            // Galoisův LFSR — white noise (nezávisí na phaseAccum)
            _lfsrState ^= _lfsrState >> 1;
            _lfsrState ^= _lfsrState << 5;
            _lfsrState ^= _lfsrState >> 14;
            sample = static_cast<int32_t>(
                         static_cast<int16_t>(_lfsrState & 0x0FFFu) - 2048);
        } else {
            // Wavetable lookup s lineární interpolací
            const uint16_t idx0 =
                static_cast<uint16_t>(v.phaseAccum >> SYNTHEX_PHASE_SHIFT)
                & static_cast<uint16_t>(SYNTHEX_WAVETABLE_SIZE - 1u);
            const uint16_t idx1 =
                (idx0 + 1u) & static_cast<uint16_t>(SYNTHEX_WAVETABLE_SIZE - 1u);
            const uint8_t  frac =
                static_cast<uint8_t>(v.phaseAccum >> SYNTHEX_FRAC_SHIFT);

            const int32_t s0 =
                SYNTHEX_TABLES[static_cast<uint8_t>(v.waveType)][idx0];
            const int32_t s1 =
                SYNTHEX_TABLES[static_cast<uint8_t>(v.waveType)][idx1];

            sample = s0 + (((s1 - s0) * static_cast<int32_t>(frac)) >> 8);
        }

        // ── 3. Anti-click fade-in / fade-out ──────────────────
        if (v.fadingOut) {
            if (v.fadeStep > 0u) {
                --v.fadeStep;
            } else {
                v.active = false;
                continue;
            }
        } else {
            if (v.fadeStep < SYNTHEX_FADE_STEPS) { ++v.fadeStep; }
        }

        // ── 4. ADSR obálka ────────────────────────────────────
        //
        // Pracujeme s Q16 akumulátorem (adsrAccum):
        //   rozsah 0 … SYNTHEX_ADSR_FULL  (= 4095 << 16)
        //   envLevel = adsrAccum >> 16  →  0–4095 (12-bit multiplikátor)
        //
        // State machine:
        //   ATTACK  : adsrAccum += attackStep  (0 → FULL)
        //   DECAY   : adsrAccum -= decayStep   (FULL → sustainLevel << 16)
        //   SUSTAIN : adsrAccum drží na sustainLevel << 16
        //   RELEASE : adsrAccum -= releaseStep (sustain → 0); poté fadingOut=true
        //
        // Unsigned aritmetika — hlídáme podtečení explicitní podmínkou.
        {
            constexpr uint32_t FULL    = SYNTHEX_ADSR_FULL;
            const     uint32_t SUSTAIN = static_cast<uint32_t>(_adsrSustain) << 16u;

            switch (v.adsrPhase) {

            case AdsrPhase::ATTACK:
                if (v.adsrAccum + _adsrAttackStep >= FULL) {
                    v.adsrAccum = FULL;
                    v.adsrPhase = AdsrPhase::DECAY;
                } else {
                    v.adsrAccum += _adsrAttackStep;
                }
                break;

            case AdsrPhase::DECAY:
                // Podtečení (adsrAccum < decayStep) nebo dosažení sustain úrovně
                if (_adsrDecayStep >= v.adsrAccum
                    || v.adsrAccum - _adsrDecayStep <= SUSTAIN) {
                    v.adsrAccum = SUSTAIN;
                    v.adsrPhase = AdsrPhase::SUSTAIN;
                } else {
                    v.adsrAccum -= _adsrDecayStep;
                }
                break;

            case AdsrPhase::SUSTAIN:
                // Sleduj změny sustain parametru za běhu
                v.adsrAccum = SUSTAIN;
                break;

            case AdsrPhase::RELEASE:
                if (_adsrReleaseStep >= v.adsrAccum) {
                    // Release dokončen: předej řízení anti-click fade-out
                    v.adsrAccum = 0u;
                    v.fadingOut = true;
                    v.adsrPhase = AdsrPhase::IDLE;
                    // fadingOut spustí dekrementaci fadeStep v příštím cyklu;
                    // envLevel = 0 → mix příspěvek je 0 po celou dobu fade-out
                } else {
                    v.adsrAccum -= _adsrReleaseStep;
                }
                break;

            case AdsrPhase::IDLE:
            default:
                break;
            }
        }

        // ── 5. Mix s ADSR obálkou ─────────────────────────────
        //
        // envLevel : 0–4095  (adsrAccum >> 16)
        // effAmp   : (v.amplitude × envLevel) >> 12  →  0–4095
        //
        // Overflow check:
        //   |sample|  ≤ 2292, effAmp ≤ 4094, fadeStep ≤ 8
        //   produkt   ≤ 2292 × 4094 × 8 ≈ 75 M < INT32_MAX/8 ✓   (stejné jako dřív)
        //   8 hlasů: 8 × 75 M = 600 M < INT32_MAX (2 147 M) ✓
        //
        const uint16_t envLevel = static_cast<uint16_t>(v.adsrAccum >> 16u);
        const int32_t  effAmp   = (static_cast<int32_t>(v.amplitude)
                                 * static_cast<int32_t>(envLevel)) >> 12;
        mix += (sample * effAmp * static_cast<int32_t>(v.fadeStep)) >> 15;
    }

    // DC offset pro DAC (0–4095); clamp pro případ saturace mixu
    mix += SYNTHEX_DAC_MID;
    if (mix < 0)                                        mix = 0;
    if (mix > static_cast<int32_t>(SYNTHEX_DAC_MAX))   mix = SYNTHEX_DAC_MAX;

    DACC->DACC_CDR = static_cast<uint32_t>(mix);
    ++_isrCount;
}

// ─────────────────────────────────────────────
//  freqToIncrement
//
//  phaseIncrement = freqHz × (2^32 / sampleRate)
//  Příklad: 440 Hz → 440 × 97556.5 ≈ 42 924 860
// ─────────────────────────────────────────────
uint32_t Synthex::freqToIncrement(float freqHz) {
    constexpr float k = static_cast<float>(1ULL << 32) / SYNTHEX_SAMPLE_RATE;
    return static_cast<uint32_t>(freqHz * k);
}

// ─────────────────────────────────────────────
//  DACC init — 12-bit DAC, kanál 1 (pin DAC1)
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
//  Timer init — TC1/CH0 (TC3) @ 44100 Hz
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
    (void)TC1->TC_CHANNEL[0].TC_SR;    // potvrzení přerušení
    Synthex::getInstance().processSample();
}
