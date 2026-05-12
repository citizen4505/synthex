/*
 * Sequencer.cpp — implementace 32-krokového step sekvenceru
 *
 * Závislosti:
 *   Synthex.h   — noteOn() / noteOff() API
 *   Scales.h    — noteFreq(), NOTE() makro, MIDI_xx konstanty
 *   MillisTimer — neblokovací časování kroků a gate
 */

#include "Sequencer.h"
#include "Synthex.h"   // tady, ne v .h — vyhne se kruhovým závislostem
#include "Scales.h"    // noteFreq(), NOTE(), MIDI_xx

// ─────────────────────────────────────────────
//  Singleton
// ─────────────────────────────────────────────
Sequencer& Sequencer::getInstance() {
    static Sequencer instance;
    return instance;
}

// ─────────────────────────────────────────────
//  Konstruktor
//  MillisTimer vyžaduje interval v konstruktoru — defaultní hodnoty.
//  Skutečné tempo nastavíme v begin() nebo setTempoMs().
// ─────────────────────────────────────────────
Sequencer::Sequencer()
    : _curPattern(0u), _curStep(0u), _state(SeqState::STOPPED),
      _stepTimer(SEQ_DEFAULT_TEMPO, true),
      _gateTimer(SEQ_DEFAULT_TEMPO * 75u / 100u, false),
      _gateActive(false),
      _useGlobalWave(false), _globalWave(WaveType::SINE),
      _useGlobalAmp(false),  _globalAmp(512u),
      _engine(nullptr)
{
    // Nulovej všechny kroky (default konstruktor SeqStep je zavolán automaticky)
}

// ─────────────────────────────────────────────
//  begin()
// ─────────────────────────────────────────────
void Sequencer::begin(Synthex& engine) {
    _engine = &engine;

    // Nahraj demo patterny hned při startu
    loadDemoPatterns();
}

// ─────────────────────────────────────────────
//  update() — volej každou iterací loop()
//
//  Logika:
//    1. Pokud STOPPED nebo PAUSED → nic
//    2. _stepTimer.expired() → _triggerStep() + posuň _curStep
//    3. _gateActive && _gateTimer.expired() → _gateOff()
//
//  Pořadí: nejdřív gateOff check, pak step check.
//  Důvod: při gate 95 % a krátkém tempu by se mohlo stát, že
//  gate timer vyprší právě ve stejném loop() cyklu jako step timer.
//  Tím zajistíme, že předchozí nota je ukončena před novou.
// ─────────────────────────────────────────────
void Sequencer::update() {
    if (_state != SeqState::PLAYING) return;
    if (_engine == nullptr) return;

    // ── Gate-off check ────────────────────────
    if (_gateActive && _gateTimer.expired()) {
        _gateOff();
    }

    // ── Krokový timer ─────────────────────────
    if (_stepTimer.expired()) {
        // Posuň na další krok (modulo = wrap na 0 po posledním kroku)
        _curStep = (_curStep + 1u) % static_cast<uint8_t>(SEQ_STEPS);

        // Okamžitě ukonči gate předchozího kroku pokud ještě zní
        // (zajistí staccato pokud gateTimer ještě nevypršel)
        if (_gateActive) {
            _gateOff();
        }

        _triggerStep();
    }
}

// ─────────────────────────────────────────────
//  _triggerStep()
//
//  Přečte aktuální krok a buď:
//    a) spustí noteOn + nastaví gateTimer (aktivní krok s notou)
//    b) zavolá noteOff (rest nebo inactive krok — zaručí ticho)
// ─────────────────────────────────────────────
void Sequencer::_triggerStep() {
    const SeqStep& s = _patterns[_curPattern][_curStep];

    if (!s.active || s.freq < 1.0f) {
        // Pauza nebo neaktivní krok → ticho
        _engine->noteOff(SEQ_VOICE);
        _gateActive = false;
        return;
    }

    // Efektivní parametry (globální přepisy)
    const WaveType wt  = _effectiveWave(s);
    const uint16_t amp = _effectiveAmp(s);

    // Spusť notu
    _engine->noteOn(SEQ_VOICE, s.freq, amp, wt);

    // Nastav gate-off timer
    // gateMs = tempoMs * gatePercent / 100; min 10 ms pro čistý noteOff
    const uint32_t gateMs = static_cast<uint32_t>(
        static_cast<uint32_t>(_stepTimer.interval())
        * static_cast<uint32_t>(s.gatePercent) / 100u);
    _gateTimer.setInterval((gateMs > 10u) ? gateMs : 10u);
    _gateTimer.reset();
    _gateActive = true;
}

// ─────────────────────────────────────────────
//  _gateOff()
// ─────────────────────────────────────────────
void Sequencer::_gateOff() {
    _engine->noteOff(SEQ_VOICE);
    _gateActive = false;
}

// ─────────────────────────────────────────────
//  Pomocné: efektivní parametry kroku
// ─────────────────────────────────────────────
WaveType Sequencer::_effectiveWave(const SeqStep& s) const {
    return _useGlobalWave ? _globalWave : s.waveType;
}

uint16_t Sequencer::_effectiveAmp(const SeqStep& s) const {
    return _useGlobalAmp ? _globalAmp : s.amplitude;
}

// ─────────────────────────────────────────────
//  Transport — play / pause / stop
// ─────────────────────────────────────────────
void Sequencer::play() {
    if (_state == SeqState::PLAYING) return;

    if (_state == SeqState::STOPPED) {
        // Začínáme znovu od prvního kroku
        // (krok 0 zazní při prvním expiry stepTimeru)
        _curStep = static_cast<uint8_t>(SEQ_STEPS - 1u);
        // Nastavím na poslední krok → po prvním expiry se přesune na 0
    }

    _state = SeqState::PLAYING;
    _stepTimer.reset();   // reset reference time → první krok po tempoMs
}

void Sequencer::pause() {
    if (_state == SeqState::PLAYING) {
        _state = SeqState::PAUSED;
        if (_gateActive) _gateOff();
    } else if (_state == SeqState::PAUSED) {
        _state = SeqState::PLAYING;
        _stepTimer.reset();
    }
}

void Sequencer::stop() {
    if (_gateActive) _gateOff();
    _engine->noteOff(SEQ_VOICE);
    _curStep   = 0u;
    _gateActive = false;
    _state     = SeqState::STOPPED;
}

// ─────────────────────────────────────────────
//  Výběr patternu
// ─────────────────────────────────────────────
void Sequencer::selectPattern(uint8_t idx) {
    if (idx >= SEQ_PATTERNS) return;
    _curPattern = idx;
    // Krok se neresruje — přechod nastane plynule v dalším cyklu
}

// ─────────────────────────────────────────────
//  Tempo
// ─────────────────────────────────────────────
void Sequencer::setTempoMs(uint32_t ms) {
    if (ms < 20u)   ms = 20u;    // min 20 ms/krok = ~750 BPM (pojistka)
    if (ms > 2000u) ms = 2000u;  // max 2000 ms/krok = 7.5 BPM
    _stepTimer.setInterval(ms);
}

// ─────────────────────────────────────────────
//  Přístup ke krokům
// ─────────────────────────────────────────────
SeqStep& Sequencer::step(uint8_t pat, uint8_t s) {
    // Clamp — bezpečný přístup i při chybném indexu
    if (pat >= SEQ_PATTERNS) pat = SEQ_PATTERNS - 1u;
    if (s   >= SEQ_STEPS)    s   = SEQ_STEPS    - 1u;
    return _patterns[pat][s];
}

const SeqStep& Sequencer::step(uint8_t pat, uint8_t s) const {
    if (pat >= SEQ_PATTERNS) pat = SEQ_PATTERNS - 1u;
    if (s   >= SEQ_STEPS)    s   = SEQ_STEPS    - 1u;
    return _patterns[pat][s];
}

// ─────────────────────────────────────────────
//  Zkrácené API pro editaci aktuálního patternu
// ─────────────────────────────────────────────
void Sequencer::setNote(uint8_t stepIdx, uint8_t midiNote,
                        uint16_t amp, uint8_t gatePercent, WaveType wt) {
    if (stepIdx >= SEQ_STEPS) return;
    SeqStep& s   = _patterns[_curPattern][stepIdx];
    s.midiNote   = midiNote;
    s.freq       = noteFreq(midiNote);
    s.amplitude  = amp;
    s.gatePercent = (gatePercent > 0u && gatePercent <= 95u) ? gatePercent : 75u;
    s.waveType   = wt;
    s.active     = true;
}

void Sequencer::setRest(uint8_t stepIdx) {
    if (stepIdx >= SEQ_STEPS) return;
    SeqStep& s = _patterns[_curPattern][stepIdx];
    s.freq     = 0.0f;
    s.midiNote = 0u;
    s.active   = true;  // krok je "aktivní" ale hraje ticho (= pauza)
}

void Sequencer::toggleActive(uint8_t stepIdx) {
    if (stepIdx >= SEQ_STEPS) return;
    _patterns[_curPattern][stepIdx].active ^= true;
}

// ─────────────────────────────────────────────
//  Globální přepisy
// ─────────────────────────────────────────────
void Sequencer::useGlobalWave(bool en, WaveType wt) {
    _useGlobalWave = en;
    _globalWave    = wt;
}

void Sequencer::useGlobalAmplitude(bool en, uint16_t amp) {
    _useGlobalAmp = en;
    _globalAmp    = amp;
}

// ─────────────────────────────────────────────
//  Read-only přístup pro Display
// ─────────────────────────────────────────────
bool Sequencer::stepHasNote(uint8_t stepIdx) const {
    if (stepIdx >= SEQ_STEPS) return false;
    const SeqStep& s = _patterns[_curPattern][stepIdx];
    return s.active && (s.freq > 1.0f);
}

bool Sequencer::stepIsActive(uint8_t stepIdx) const {
    if (stepIdx >= SEQ_STEPS) return false;
    return _patterns[_curPattern][stepIdx].active;
}

// ═══════════════════════════════════════════════════════════════════
//  loadDemoPatterns()
//
//  Naplní 4 patterny melodickými vzory z různých stupnic.
//  Všechny noty jsou ze Scales.h — midiNote + noteFreq().
//
//  PAT 0 — A3 pentatonická moll (blues/funk groove)
//  PAT 1 — D4 dórská (jazzový vzestup + sestup)
//  PAT 2 — C4 bluesová (synkopy + blue note)
//  PAT 3 — Prázdný pattern (uživatelský vstup)
// ═══════════════════════════════════════════════════════════════════
void Sequencer::loadDemoPatterns() {

    // ── Lokální helper lambda — naplní krok bez wt (použije se default) ──
    // Zapíše midiNote + noteFreq do daného patternu/kroku
    auto setN = [this](uint8_t pat, uint8_t s,
                       uint8_t midi, uint16_t amp = 512u,
                       uint8_t gate = 75u,
                       WaveType wt  = WaveType::SINE) {
        SeqStep& st = _patterns[pat][s];
        st.midiNote   = midi;
        st.freq       = noteFreq(midi);
        st.amplitude  = amp;
        st.gatePercent = gate;
        st.waveType   = wt;
        st.active     = true;
    };

    auto setR = [this](uint8_t pat, uint8_t s) {
        // Pauza — active=true, freq=0
        SeqStep& st = _patterns[pat][s];
        st.midiNote   = 0u;
        st.freq       = 0.0f;
        st.active     = true;
    };

    // Vymaž všechny patterny
    for (uint8_t p = 0u; p < SEQ_PATTERNS; ++p)
        for (uint8_t s = 0u; s < SEQ_STEPS; ++s)
            _patterns[p][s] = SeqStep();

    // ────────────────────────────────────────────────────────────
    //  PATTERN 0 — A3 pentatonická moll (blues groove)
    //
    //  Stupnice: A3 C4 D4 E4 G4  (MIDI: 57 60 62 64 67)
    //  Rytmus: akcentovaný downbeat + synkopy
    //
    //  Krok:  1   2   3   4   5   6   7   8   9  10  11  12  13  14  15  16
    //  Nota: A3  ---  D4  ---  E4  G4  ---  E4  A3  C4  D4  ---  G4  ---  E4  ---
    // ────────────────────────────────────────────────────────────
    {
        const uint8_t p = 0u;
        setN(p,  0, MIDI_A3, 600u, 80u);  // downbeat — hlasitější
        setR(p,  1);
        setN(p,  2, MIDI_D4, 512u, 70u);
        setR(p,  3);
        setN(p,  4, MIDI_E4, 512u, 75u);
        setN(p,  5, MIDI_G4, 450u, 60u);  // off-beat — kratší gate
        setR(p,  6);
        setN(p,  7, MIDI_E4, 512u, 75u);
        setN(p,  8, MIDI_A3, 600u, 80u);  // downbeat 2. části
        setN(p,  9, MIDI_C4, 500u, 70u);
        setN(p, 10, MIDI_D4, 512u, 75u);
        setR(p, 11);
        setN(p, 12, MIDI_G4, 512u, 75u);
        setR(p, 13);
        setN(p, 14, MIDI_E4, 512u, 90u);  // legato před koncem
        setR(p, 15);
    }

    // ────────────────────────────────────────────────────────────
    //  PATTERN 1 — D4 dórská stupnice (jazzový vzestup + sestup)
    //
    //  Stupnice: D4 E4 F4 G4 A4 B4 C5  (MIDI: 62 64 65 67 69 71 72)
    //  Vzestup v 1. části, sestup ve 2. části.
    //
    //  Všechny MIDI_xx konstanty jsou ze Scales.h — nemusíme je lokálně
    //  přepisovat (MIDI_F4=65, MIDI_B4=71, MIDI_C5=72, MIDI_D5=74 jsou tam).
    //
    //  Krok:  1   2   3   4   5   6   7   8   9  10  11  12  13  14  15  16
    //  Nota: D4  E4  F4  G4  A4  B4  C5  A4  G4  F4  E4  D4  F4  A4  D5  ---
    // ────────────────────────────────────────────────────────────
    {
        const uint8_t p = 1u;

        // Vzestup
        setN(p,  0, MIDI_D4,  512u, 80u);
        setN(p,  1, MIDI_E4,  512u, 80u);
        setN(p,  2, MIDI_F4,  512u, 80u);   // MIDI_F4 = 65 (ze Scales.h)
        setN(p,  3, MIDI_G4,  512u, 80u);
        setN(p,  4, MIDI_A4,  512u, 80u);
        setN(p,  5, MIDI_B4,  512u, 80u);   // MIDI_B4 = 71 (ze Scales.h)
        setN(p,  6, MIDI_C5,  600u, 80u);   // vrchol — trochu hlasitější
        setN(p,  7, MIDI_A4,  512u, 70u);
        // Sestup
        setN(p,  8, MIDI_G4,  512u, 80u);
        setN(p,  9, MIDI_F4,  512u, 80u);
        setN(p, 10, MIDI_E4,  512u, 80u);
        setN(p, 11, MIDI_D4,  600u, 80u);   // kořen — forte
        // Coda
        setN(p, 12, MIDI_F4,  450u, 60u);
        setN(p, 13, MIDI_A4,  450u, 60u);
        setN(p, 14, MIDI_D5,  700u, 90u);   // vysoké D — forte legato
        setR(p, 15);
    }

    // ────────────────────────────────────────────────────────────
    //  PATTERN 2 — C4 bluesová stupnice (synkopy + blue note)
    //
    //  Stupnice: C4 Eb4 F4 F#4 G4 Bb4  (MIDI: 60 63 65 66 67 70)
    //  Typický blues riff s akcentem na off-beat
    //
    //  Krok:  1   2   3   4   5   6   7   8   9  10  11  12  13  14  15  16
    //  Nota: C4  ---  Eb4 F4  F#4 F4  ---  G4  ---  Bb4 G4  ---  F4  Eb4 C4 ---
    // ────────────────────────────────────────────────────────────
    {
        const uint8_t p = 2u;
        // Scales.h: Ds = D# = Eb,  Fs = F# (blue note),  As = Bb
        // Konstanty MIDI_Ds4=63, MIDI_F4=65, MIDI_Fs4=66, MIDI_As4=70
        // jsou součástí Scales.h — lokální redefinice není potřeba.

        setN(p,  0, MIDI_C4,   600u, 85u);
        setR(p,  1);
        setN(p,  2, MIDI_Ds4,  512u, 70u);  // Eb4 = Ds4
        setN(p,  3, MIDI_F4,   512u, 75u);
        setN(p,  4, MIDI_Fs4,  450u, 55u);  // blue note (tritón) — kratší, tišší
        setN(p,  5, MIDI_F4,   512u, 70u);
        setR(p,  6);
        setN(p,  7, MIDI_G4,   550u, 85u);  // off-beat akcent — forte
        setR(p,  8);
        setN(p,  9, MIDI_As4,  512u, 75u);  // Bb4 = As4
        setN(p, 10, MIDI_G4,   512u, 70u);
        setR(p, 11);
        setN(p, 12, MIDI_F4,   450u, 65u);
        setN(p, 13, MIDI_Ds4,  450u, 70u);  // Eb4
        setN(p, 14, MIDI_C4,   600u, 90u);  // rozlišení — forte legato
        setR(p, 15);
    }

    // ────────────────────────────────────────────────────────────
    //  PATTERN 0 — kroky 16–31 (druhá polovina: A3 penta moll — variace)
    //
    //  Krok: 17  18  19  20  21  22  23  24  25  26  27  28  29  30  31  32
    //  Nota: E4  G4  A4  ---  G4  E4  D4  ---  A3  C4  E4  G4  A4  ---  E4 ---
    // ────────────────────────────────────────────────────────────
    {
        const uint8_t p = 0u;
        setN(p, 16, MIDI_E4,  512u, 75u);
        setN(p, 17, MIDI_G4,  450u, 60u);
        setN(p, 18, MIDI_A4,  600u, 80u);
        setR(p, 19);
        setN(p, 20, MIDI_G4,  512u, 70u);
        setN(p, 21, MIDI_E4,  512u, 75u);
        setN(p, 22, MIDI_D4,  480u, 70u);
        setR(p, 23);
        setN(p, 24, MIDI_A3,  600u, 80u);
        setN(p, 25, MIDI_C4,  500u, 70u);
        setN(p, 26, MIDI_E4,  512u, 75u);
        setN(p, 27, MIDI_G4,  512u, 70u);
        setN(p, 28, MIDI_A4,  650u, 85u);
        setR(p, 29);
        setN(p, 30, MIDI_E4,  512u, 90u);
        setR(p, 31);
    }

    // ────────────────────────────────────────────────────────────
    //  PATTERN 1 — kroky 16–31 (druhá polovina: dórská — vyšší fráze)
    //
    //  Krok: 17  18  19  20  21  22  23  24  25  26  27  28  29  30  31  32
    //  Nota: E4  G4  A4  B4  C5  D5  ---  C5  B4  A4  G4  F4  E4  D4  A3  ---
    // ────────────────────────────────────────────────────────────
    {
        const uint8_t p = 1u;
        setN(p, 16, MIDI_E4,  512u, 80u);
        setN(p, 17, MIDI_G4,  512u, 80u);
        setN(p, 18, MIDI_A4,  512u, 80u);
        setN(p, 19, MIDI_B4,  512u, 80u);
        setN(p, 20, MIDI_C5,  580u, 80u);
        setN(p, 21, MIDI_D5,  650u, 85u);
        setR(p, 22);
        setN(p, 23, MIDI_C5,  580u, 75u);
        setN(p, 24, MIDI_B4,  512u, 80u);
        setN(p, 25, MIDI_A4,  512u, 80u);
        setN(p, 26, MIDI_G4,  512u, 80u);
        setN(p, 27, MIDI_F4,  512u, 80u);
        setN(p, 28, MIDI_E4,  512u, 80u);
        setN(p, 29, MIDI_D4,  600u, 80u);
        setN(p, 30, MIDI_A3,  550u, 75u);
        setR(p, 31);
    }

    // ────────────────────────────────────────────────────────────
    //  PATTERN 2 — kroky 16–31 (druhá polovina: blues — turnaround)
    //
    //  Krok: 17  18  19  20  21  22  23  24  25  26  27  28  29  30  31  32
    //  Nota: C4  ---  G4  F#4 F4  ---  Eb4 C4  ---  Bb4 G4  ---  F4  Eb4 C4 ---
    // ────────────────────────────────────────────────────────────
    {
        const uint8_t p = 2u;
        setN(p, 16, MIDI_C4,   600u, 85u);
        setR(p, 17);
        setN(p, 18, MIDI_G4,   512u, 80u);
        setN(p, 19, MIDI_Fs4,  450u, 55u);  // blue note
        setN(p, 20, MIDI_F4,   512u, 70u);
        setR(p, 21);
        setN(p, 22, MIDI_Ds4,  512u, 70u);  // Eb4
        setN(p, 23, MIDI_C4,   512u, 80u);
        setR(p, 24);
        setN(p, 25, MIDI_As4,  512u, 75u);  // Bb4
        setN(p, 26, MIDI_G4,   512u, 70u);
        setR(p, 27);
        setN(p, 28, MIDI_F4,   450u, 65u);
        setN(p, 29, MIDI_Ds4,  450u, 70u);  // Eb4
        setN(p, 30, MIDI_C4,   650u, 90u);  // finální rozlišení
        setR(p, 31);
    }

    // ────────────────────────────────────────────────────────────
    //  PATTERN 3 — Prázdný (pro live editaci tlačítky / sériovou konzolí)
    //
    //  Všechny kroky jsou inactive (active=false) → kompletní ticho.
    //  Uživatel může zapnout/editovat přes toggleActive() a setNote().
    // ────────────────────────────────────────────────────────────
    // (Pattern 3 zůstane výchozí — všechny kroky inactive z konstruktoru)
}
