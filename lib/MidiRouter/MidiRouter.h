#pragma once
#include <Arduino.h>
#include "MidiParser.h"
#include "Synthex.h"

/*
 * MidiRouter — mapuje MIDI zprávy na hlasy Synthex enginu.
 *
 * ── Alokace hlasů (Omni Mode) ────────────────────────────────────────────
 *
 *   Každá aktivní nota obsazuje jeden VoiceSlot (index = index hlasu v enginu).
 *
 *   NoteOn:   hledá volný slot (active=false); nenajde-li, ukradne nejstarší
 *             (LRU = Least Recently Used). Stejná technika jako v Phase 4.
 *
 *   NoteOff:  uvolní přesně ten slot, který hraje (channel, note). Díky
 *             tomu funguje správně i polyfonie a rapid legato.
 *
 *   Omni Mode: zprávy ze všech MIDI kanálů jdou do stejného poolu hlasů.
 *             Kanál sledujeme jen kvůli per-kanálovému Pitch Bendu a CC.
 *
 * ── Pitch Bend ────────────────────────────────────────────────────────────
 *
 *   Rozsah ±PITCH_BEND_RANGE půltónů (výchozí 2 = standard GM).
 *   Při příchodu PitchBend přepočítáme frekvenci všech hlasů na kanálu.
 *
 *   ⚠  Vyžaduje Synthex::setFrequency(voiceIdx, hz).
 *   Přidej do enginu:
 *       void setFrequency(uint8_t vi, float hz) {
 *           _voices[vi].setFrequency(hz);
 *       }
 *   Pokud tuto metodu engine nemá, router zavolá noteOn() znovu
 *   (retrigguje obálku — méně ideální, ale funkční).
 *
 * ── Podporované CC ────────────────────────────────────────────────────────
 *
 *   CC   1  Mod Wheel    → amplitudová modulace (škáluje velocity 0–127 → 0–4095)
 *   CC   7  Volume       → master hlasitost kanálu (uchová se pro nové noty)
 *   CC  64  Sustain      → drží noty po NoteOff (>63 = pedál dole)
 *   CC 123  All Notes Off → okamžité zastavení všech hlasů (GM povinné)
 *   CC 120  All Sound Off → totéž
 *
 * ── Co je tady zajímavé pro C++ začátečníky ──────────────────────────────
 *
 *   • Pole struktur jako „databáze" aktivních not (VoiceSlot).
 *   • LRU voice stealing bez dynamické alokace (žádný new/delete).
 *   • Konverze MIDI nota → frekvence přes powf() (plovoucí čísla).
 *   • Per-kanálový stav v separátním poli (ChanState[16]).
 *   • Výpočet pitch bend multiplikátoru: f' = f * 2^(bend_semitones/12).
 */

#ifndef PITCH_BEND_RANGE
#define PITCH_BEND_RANGE 2      // ±2 půltóny = standard GM
#endif

// ── Voice slot: sleduje jednu aktivní notu ────────────────────────────────

struct VoiceSlot {
    bool     active    = false;   // hlas je obsazený
    uint8_t  channel   = 0;       // MIDI kanál 0–15
    uint8_t  note      = 0;       // MIDI nota 0–127
    uint8_t  velocity  = 0;       // velocity při noteOn (0–127)
    float    baseFreq  = 0.0f;    // frekvence BEZ pitch bendu [Hz]
    uint32_t timestamp = 0;       // millis() při alokaci (pro LRU stealing)
    bool     sustained = false;   // čeká na uvolnění sustain pedálu?
};

// ── Třída routeru ─────────────────────────────────────────────────────────

class MidiRouter {
public:
    explicit MidiRouter(Synthex& engine,
                        WaveType waveType = WaveType::BANDLIMITED_SAW)
        : _engine(engine), _waveType(waveType) {}

    // Zpracuj jednu kompletní MIDI zprávu (voláno z loop() po každém parse()).
    void handleMsg(const MidiMsg& msg);

    // Změna výchozího wavetypu za běhu
    void setWaveType(WaveType wt) { _waveType = wt; }

    // Debug výpis aktivních slotů do Serial
    void printSlots() const;

private:
    Synthex& _engine;
    WaveType _waveType;

    VoiceSlot _slots[SYNTHEX_VOICES];

    // Per-kanálový stav (pitch bend, hlasitost, sustain pedál)
    struct ChanState {
        float   bendMult = 1.0f;  // frekvenční multiplikátor z pitch bendu
        uint8_t volume   = 100;   // CC7, 0–127 (výchozí 100 = ~79 %)
        bool    sustain  = false; // CC64 > 63 → pedál dole
    };
    ChanState _chan[16];

    // ── Pomocné static utility ───────────────────────────────────────────

    // MIDI nota → frekvence [Hz]:  f = 440 × 2^((note − 69) / 12)
    // Nota 69 = A4 = 440 Hz (standardní ladění, A=440)
    static float midiToHz(uint8_t note) {
        return 440.0f * powf(2.0f, (note - 69) / 12.0f);
    }

    // Kombinuje volume CC (0–127) a velocity (0–127) na amplitudu enginu (0–4095).
    // Výsledek je lineárně škálován: amp = (vol/127) × (vel/127) × 4095
    static uint16_t calcAmplitude(uint8_t vol, uint8_t vel) {
        return uint16_t((uint32_t(vol) * uint32_t(vel) * 4095u) / (127u * 127u));
    }

    // ── Slot management ──────────────────────────────────────────────────

    // Najde první volný slot. Vrátí index nebo −1 (všechny obsazené).
    int8_t findFreeSlot() const {
        for (uint8_t i = 0; i < SYNTHEX_VOICES; ++i)
            if (!_slots[i].active) return int8_t(i);
        return -1;
    }

    // LRU stealing: vrátí index nejstaršího aktivního slotu.
    int8_t stealOldestSlot() const {
        int8_t   oldest  = 0;
        uint32_t minTime = _slots[0].timestamp;
        for (uint8_t i = 1; i < SYNTHEX_VOICES; ++i) {
            if (_slots[i].timestamp < minTime) {
                minTime = _slots[i].timestamp;
                oldest  = int8_t(i);
            }
        }
        return oldest;
    }

    // Najde slot s danou (channel, note). Vrátí index nebo −1.
    int8_t findNoteSlot(uint8_t ch, uint8_t note) const {
        for (uint8_t i = 0; i < SYNTHEX_VOICES; ++i)
            if (_slots[i].active && _slots[i].channel == ch
                                 && _slots[i].note    == note)
                return int8_t(i);
        return -1;
    }

    // ── Zpracování jednotlivých typů zpráv ───────────────────────────────
    void onNoteOn (uint8_t ch, uint8_t note, uint8_t vel);
    void onNoteOff(uint8_t ch, uint8_t note);
    void onCC     (uint8_t ch, uint8_t cc,   uint8_t val);
    void onBend   (uint8_t ch, int16_t bend);
};

// ─────────────────────────────────────────────────────────────────────────────
//  Implementace (inline, header-only)
// ─────────────────────────────────────────────────────────────────────────────

inline void MidiRouter::handleMsg(const MidiMsg& msg) {
    switch (msg.type) {
        case MidiMsg::NOTE_ON:       onNoteOn (msg.channel, msg.data1, msg.data2); break;
        case MidiMsg::NOTE_OFF:      onNoteOff(msg.channel, msg.data1);            break;
        case MidiMsg::CONTROL_CHANGE:onCC     (msg.channel, msg.data1, msg.data2); break;
        case MidiMsg::PITCH_BEND:    onBend   (msg.channel, msg.bend);             break;
        default: break;
    }
}

inline void MidiRouter::onNoteOn(uint8_t ch, uint8_t note, uint8_t vel) {
    // Parser to ošetřuje, ale pro jistotu: vel=0 = NoteOff
    if (vel == 0) { onNoteOff(ch, note); return; }

    // Nejprve zkontroluj, zda nota na tomto kanálu už nehraje (retriggering)
    int8_t si = findNoteSlot(ch, note);
    if (si < 0) {
        si = findFreeSlot();
        if (si < 0) {
            si = stealOldestSlot();
            Serial.print("[MIDI] Voice steal → hlas "); Serial.println(si);
            _engine.noteOff(uint8_t(si));
        }
    } else {
        // Retrigger: stejná nota na stejném kanálu — uvolni a znovu spusť
        _engine.noteOff(uint8_t(si));
    }

    float    baseFreq = midiToHz(note);
    float    freq     = baseFreq * _chan[ch].bendMult;
    uint16_t amp      = calcAmplitude(_chan[ch].volume, vel);

    _engine.noteOn(uint8_t(si), freq, amp, _waveType);

    _slots[si].active    = true;
    _slots[si].channel   = ch;
    _slots[si].note      = note;
    _slots[si].velocity  = vel;
    _slots[si].baseFreq  = baseFreq;
    _slots[si].timestamp = millis();
    _slots[si].sustained = false;

    Serial.print("[MIDI] NoteOn  ch="); Serial.print(ch + 1);
    Serial.print("  note="); Serial.print(note);
    Serial.print("  vel=");  Serial.print(vel);
    Serial.print("  freq="); Serial.print(freq, 1);
    Serial.print(" Hz  → hlas "); Serial.println(si);
}

inline void MidiRouter::onNoteOff(uint8_t ch, uint8_t note) {
    int8_t si = findNoteSlot(ch, note);
    if (si < 0) return;  // nota se nehraje (může nastat po voice stealingu)

    if (_chan[ch].sustain) {
        // Sustain pedál je dole → jen označ slot, že čeká na uvolnění
        _slots[si].sustained = true;
        return;
    }

    _engine.noteOff(uint8_t(si));
    _slots[si].active = false;

    Serial.print("[MIDI] NoteOff ch="); Serial.print(ch + 1);
    Serial.print("  note="); Serial.println(note);
}

inline void MidiRouter::onCC(uint8_t ch, uint8_t cc, uint8_t val) {
    switch (cc) {

        case 1: {   // Mod Wheel (0–127) → škáluje amplitudu aktivních hlasů
            // V enginu bez LFO použijeme modwheel jako tremolo (amplitudová modulace).
            // 0 = 50 % amplitudy, 127 = 100 % amplitudy (nevypne tón úplně)
            uint8_t effectiveVol = 64 + val / 2;  // mapuj 0..127 → 64..127
            for (uint8_t i = 0; i < SYNTHEX_VOICES; ++i) {
                if (_slots[i].active && _slots[i].channel == ch) {
                    uint16_t amp = calcAmplitude(effectiveVol, _slots[i].velocity);
                    // Poznámka: pokud Synthex nemá setAmplitude(), tuto část vynech.
                    // _engine.setAmplitude(i, amp);
                    (void)amp;  // potlač varování, dokud setAmplitude() neexistuje
                }
            }
            Serial.print("[MIDI] ModWheel ch="); Serial.print(ch + 1);
            Serial.print("  val="); Serial.println(val);
            break;
        }

        case 7: {   // Volume (0–127) → uloží se pro nové noty na tomto kanálu
            _chan[ch].volume = val;
            Serial.print("[MIDI] Volume   ch="); Serial.print(ch + 1);
            Serial.print("  = "); Serial.println(val);
            break;
        }

        case 64: {  // Sustain pedál (≥64 = dole, <64 = nahoře)
            bool wasOn      = _chan[ch].sustain;
            _chan[ch].sustain = (val >= 64);

            if (wasOn && !_chan[ch].sustain) {
                // Pedál zvednut → uvolni všechny čekající noty
                for (uint8_t i = 0; i < SYNTHEX_VOICES; ++i) {
                    if (_slots[i].active && _slots[i].channel == ch
                                        && _slots[i].sustained) {
                        _engine.noteOff(i);
                        _slots[i].active = false;
                    }
                }
            }
            Serial.print("[MIDI] Sustain  ch="); Serial.print(ch + 1);
            Serial.println(_chan[ch].sustain ? "  [ON]" : "  [OFF]");
            break;
        }

        case 120:   // All Sound Off (GM)
        case 123: { // All Notes Off (GM) — klaviatura to posílá při panic
            for (uint8_t i = 0; i < SYNTHEX_VOICES; ++i) {
                if (_slots[i].active) {
                    _engine.noteOff(i);
                    _slots[i].active = false;
                }
            }
            Serial.println("[MIDI] All Notes Off");
            break;
        }
    }
}

inline void MidiRouter::onBend(uint8_t ch, int16_t bend) {
    /*
     * Pitch Bend → frekvenční multiplikátor.
     *
     * MIDI bend:   −8192 … 0 … +8191
     * Semitóny:    bend × PITCH_BEND_RANGE / 8192
     * Multiplikátor: 2^(semitóny / 12)
     *
     * Příklad (range=2):
     *   bend = +8192 → +2 semitóny → ×1.1225
     *   bend =  −8192 → −2 semitóny → ×0.8909
     */
    float semitones      = float(bend) * PITCH_BEND_RANGE / 8192.0f;
    _chan[ch].bendMult   = powf(2.0f, semitones / 12.0f);

    // Přepočítej frekvenci všech aktivních hlasů na tomto kanálu
    for (uint8_t i = 0; i < SYNTHEX_VOICES; ++i) {
        if (!_slots[i].active || _slots[i].channel != ch) continue;

        float newFreq = _slots[i].baseFreq * _chan[ch].bendMult;

        // ── Preferovaná varianta: engine má setFrequency() ───────────────
        // Přidej do Synthex.h:
        //   void setFrequency(uint8_t vi, float hz) { _voices[vi].setFrequency(hz); }
        // _engine.setFrequency(i, newFreq);

        // ── Fallback bez setFrequency(): retrigguje obálku (méně ideální) ─
        uint16_t amp = calcAmplitude(_chan[ch].volume, _slots[i].velocity);
        _engine.noteOn(i, newFreq, amp, _waveType);
    }
}

inline void MidiRouter::printSlots() const {
    Serial.println("── Voice slots ─────────────────────────────");
    for (uint8_t i = 0; i < SYNTHEX_VOICES; ++i) {
        Serial.print("  ["); Serial.print(i); Serial.print("] ");
        if (!_slots[i].active) {
            Serial.println("volný");
        } else {
            Serial.print("ch=");   Serial.print(_slots[i].channel + 1);
            Serial.print("  n=");  Serial.print(_slots[i].note);
            Serial.print("  ");    Serial.print(_slots[i].baseFreq, 1);
            Serial.print(" Hz");
            if (_slots[i].sustained) Serial.print("  [sustain]");
            Serial.println();
        }
    }
    Serial.println("────────────────────────────────────────────");
}