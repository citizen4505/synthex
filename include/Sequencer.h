/*
 * Sequencer.h — 16-krokový step sekvencer pro Synthex engine
 *
 * ═══════════════════════════════════════════════════════════════════
 *  ARCHITEKTURA
 * ═══════════════════════════════════════════════════════════════════
 *
 *  Paměť:  SEQ_PATTERNS × SEQ_STEPS kroků (4 × 32 = 128 SeqStep)
 *          Každý krok: nota (MIDI + Hz), amplitude, waveType, gate%, active
 *
 *  Timing (bez blokování):
 *    _stepTimer (autoReset=true)  → každých tempoMs přejdi na další krok
 *    _gateTimer (autoReset=false) → po gateMs zavolej noteOff (gate-off)
 *
 *  Tok jednoho kroku:
 *    1. _stepTimer.expired() → _triggerStep()
 *       • active && freq > 0  → noteOn(SEQ_VOICE, freq, amp, wave)
 *                               + nastav _gateTimer na tempoMs*gate%/100
 *       • rest nebo inactive  → rovnou noteOff (ticho bez artefaktů)
 *    2. _gateTimer.expired() → _gateOff() → noteOff(SEQ_VOICE)
 *
 *  Gate:
 *    Hodnota 5–95 % zajišťuje mezery mezi notami (artikulace).
 *    100 % = legato (noteOff těsně před dalším noteOn).
 *    Při step-time 125 ms a gate 75 %: note zní 93 ms, ticho 32 ms.
 *
 *  Globální přepisy:
 *    useGlobalWave(true, wt)   → všechny kroky hrají wt (přepíše krok)
 *    useGlobalAmplitude(true)  → všechny kroky hrají _globalAmp
 *    Vhodné pro přímé ovládání z potenciometrů (Pots.h).
 *
 *  Voice stealing:
 *    Sekvencer používá pevně SEQ_VOICE (hlas 0). Ostatní hlasy 1–7
 *    zůstávají volné pro manuální noteOn/unison/arpeggio z main.cpp.
 *
 * ═══════════════════════════════════════════════════════════════════
 *  BPM KONVENCE
 * ═══════════════════════════════════════════════════════════════════
 *
 *  Každý krok = 1/16 noty (šestnáctinová nota) v 4/4 taktu.
 *  BPM (čtvrťová nota) = 15 000 / tempoMs
 *
 *  tempoMs  │  BPM
 *  ─────────┼──────
 *    80 ms  │  187
 *   125 ms  │  120
 *   187 ms  │   80
 *   250 ms  │   60
 *   500 ms  │   30
 *
 * ═══════════════════════════════════════════════════════════════════
 *  HARDWARE
 * ═══════════════════════════════════════════════════════════════════
 *
 *  Arduino Due / SAM3X8E — 84 MHz ARM Cortex-M3
 *  Žádné ISR závislosti — Sequencer::update() se volá z loop().
 *
 * ═══════════════════════════════════════════════════════════════════
 *  PŘÍKLAD POUŽITÍ
 * ═══════════════════════════════════════════════════════════════════
 *
 *  // setup()
 *  Sequencer& seq = Sequencer::getInstance();
 *  seq.begin(engine);           // propoj s Synthex enginem
 *  seq.loadDemoPatterns();      // nahraj ukázkové patterny
 *  seq.play();
 *
 *  // loop()
 *  seq.setTempoMs(pots.tempoMs());
 *  seq.useGlobalWave(true, pots.waveType());
 *  seq.useGlobalAmplitude(true, pots.amplitude());
 *  seq.update();                // musí být v každé iteraci loop()
 */

#pragma once
#include <Arduino.h>
#include "wavetables.h"
#include "MillisTimer.h"

// ─────────────────────────────────────────────
//  Konfigurace — uprav podle potřeby
// ─────────────────────────────────────────────

#define SEQ_STEPS          32u    // počet kroků na pattern
#define SEQ_PATTERNS        4u    // počet patternů v paměti
#define SEQ_VOICE           0u    // hlas Synthex vyhrazený pro sekvencer
#define SEQ_DEFAULT_TEMPO 250u    // výchozí délka kroku [ms] → 60 BPM

// ─────────────────────────────────────────────
//  Jeden krok sekvenceru
// ─────────────────────────────────────────────
struct SeqStep {
    float    freq;          // Hz; 0.0 = pauza (rest)
    uint8_t  midiNote;      // MIDI číslo 0–127 (0 = rest); jen pro display
    uint16_t amplitude;     // 0–4095; ignoruje se pokud useGlobalAmp = true
    WaveType waveType;      // typ vlny;  ignoruje se pokud useGlobalWave = true
    uint8_t  gatePercent;   // 5–95 %: část kroku, po kterou nota zní
    bool     active;        // false = krok vynechán, bez výstupu

    // Defaultní konstruktor — prázdný krok, připravený na ruční nastavení
    SeqStep()
        : freq(0.0f), midiNote(0u), amplitude(512u),
          waveType(WaveType::SINE), gatePercent(75u), active(false) {}
};

// ─────────────────────────────────────────────
//  Stav transportu
// ─────────────────────────────────────────────
enum class SeqState : uint8_t {
    STOPPED = 0,
    PLAYING = 1,
    PAUSED  = 2
};

// ─────────────────────────────────────────────
//  Dopředná deklarace — Synthex.h includujeme až v Sequencer.cpp
//  aby se zabránilo kruhovým závislostem
// ─────────────────────────────────────────────
class Synthex;

// ─────────────────────────────────────────────
//  Třída Sequencer (Singleton)
// ─────────────────────────────────────────────
class Sequencer {
public:
    // Singleton — stejný vzor jako Synthex a Pots
    static Sequencer& getInstance();

    // ─────────────────────────────────────────
    //  Inicializace — volej v setup() po engine.begin()
    // ─────────────────────────────────────────
    void begin(Synthex& engine);

    // Hlavní update — volej v každé iteraci loop()
    // Spravuje časování kroků a gate-off.
    void update();

    // ─────────────────────────────────────────
    //  Transport
    // ─────────────────────────────────────────

    // play() → PLAYING; pokud byl STOPPED, resetuje krok na 0
    void play();

    // pause() ↔ PAUSED / PLAYING; zachovává pozici v sekvenci
    void pause();

    // stop()  → STOPPED; resetuje krok na 0, pošle noteOff
    void stop();

    SeqState state() const { return _state; }

    // ─────────────────────────────────────────
    //  Výběr patternu (0 až SEQ_PATTERNS-1)
    //  Přepnutí nastane při dalším kroku (bez kliknutí).
    // ─────────────────────────────────────────
    void    selectPattern(uint8_t idx);
    uint8_t currentPattern() const { return _curPattern; }

    // ─────────────────────────────────────────
    //  Tempo
    // ─────────────────────────────────────────

    // Nastav délku kroku v ms (platí od příštího kroku)
    void setTempoMs(uint32_t ms);

    // Aktuální délka kroku
    uint32_t tempoMs() const { return _stepTimer.interval(); }

    // Přibližné BPM: 1 krok = 1/16 noty → BPM = 15 000 / tempoMs
    uint16_t bpm() const { return static_cast<uint16_t>(15000u / _stepTimer.interval()); }

    // ─────────────────────────────────────────
    //  Přímý přístup ke krokům
    // ─────────────────────────────────────────

    // Referenční přístup pro plnou editaci
    SeqStep&       step(uint8_t pat, uint8_t stepIdx);
    const SeqStep& step(uint8_t pat, uint8_t stepIdx) const;

    // Zkratka: nastav notu v aktuálním patternu
    void setNote(uint8_t stepIdx, uint8_t midiNote,
                 uint16_t amp = 512u, uint8_t gatePercent = 75u,
                 WaveType wt  = WaveType::SINE);

    // Nastav krok jako pauzu (freq=0, active=true)
    void setRest(uint8_t stepIdx);

    // Přepni active flag daného kroku
    void toggleActive(uint8_t stepIdx);

    // ─────────────────────────────────────────
    //  Globální přepisy parametrů
    //  (např. ovládané z Pots — přepíší hodnoty v SeqStep)
    // ─────────────────────────────────────────

    // Pokud en=true, všechny kroky hrajou wt (ignoruje SeqStep.waveType)
    void useGlobalWave(bool en, WaveType wt = WaveType::SINE);

    // Pokud en=true, všechny kroky hrajou amp (ignoruje SeqStep.amplitude)
    void useGlobalAmplitude(bool en, uint16_t amp = 512u);

    WaveType globalWave()    const { return _globalWave; }
    uint16_t globalAmp()     const { return _globalAmp; }
    bool     globalWaveEn()  const { return _useGlobalWave; }
    bool     globalAmpEn()   const { return _useGlobalAmp; }

    // ─────────────────────────────────────────
    //  Stav pro Display (read-only přístup)
    // ─────────────────────────────────────────

    // Index aktuálně přehrávaného kroku (0–SEQ_STEPS-1)
    uint8_t currentStep() const { return _curStep; }

    // True pokud krok v aktuálním patternu má notu (ne rest, ne inactive)
    bool stepHasNote(uint8_t stepIdx) const;

    // True pokud krok je zapnutý (active flag)
    bool stepIsActive(uint8_t stepIdx) const;

    // True pokud gate právě zní (note je on)
    bool isGateOn() const { return _gateActive; }

    // ─────────────────────────────────────────
    //  Továrna na vzorové patterny
    //  Naplní všechny 4 patterny melodickými vzory pro demo.
    //  Volej před play() nebo kdykoli — přehrávání se nepreruší.
    // ─────────────────────────────────────────
    void loadDemoPatterns();

private:
    // Zakázání veřejné konstrukce a kopírování
    Sequencer();
    Sequencer(const Sequencer&)            = delete;
    Sequencer& operator=(const Sequencer&) = delete;

    // ── Interní kroky ────────────────────────
    // Spustí aktuální krok: noteOn nebo skip (rest/inactive)
    void _triggerStep();

    // Ukončí gate: noteOff a _gateActive = false
    void _gateOff();

    // Pomocná: vrátí efektivní waveType a amplitude pro krok
    WaveType _effectiveWave(const SeqStep& s) const;
    uint16_t _effectiveAmp(const SeqStep& s)  const;

    // ── Data ─────────────────────────────────
    SeqStep  _patterns[SEQ_PATTERNS][SEQ_STEPS];
    uint8_t  _curPattern;     // aktivní pattern (0–SEQ_PATTERNS-1)
    uint8_t  _curStep;        // aktuální krok  (0–SEQ_STEPS-1)
    SeqState _state;


    // Timery — inicializovány v konstruktoru
    MillisTimer _stepTimer;   // perioda kroku (autoReset=true)
    MillisTimer _gateTimer;   // gate-off timeout (autoReset=false)
    bool        _gateActive;  // probíhá gate (čeká se na _gateTimer)?

    // Globální přepisy
    bool     _useGlobalWave;
    WaveType _globalWave;
    bool     _useGlobalAmp;
    uint16_t _globalAmp;

    // Reference na Synthex engine (nastavena v begin())
    Synthex* _engine;
};
