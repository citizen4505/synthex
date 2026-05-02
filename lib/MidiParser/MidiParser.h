#pragma once
#include <Arduino.h>

/*
 * MidiParser — stavový automat pro UART MIDI byte stream (31250 baud).
 *
 * ── Jak MIDI protokol funguje ──────────────────────────────────────────
 *
 * Každá zpráva = STATUS BYTE + 0–2 DATA BYTES:
 *
 *   Status byte:  bit7=1 | vyšší nibble=typ | nižší nibble=kanál (0–15)
 *   Data byte:    bit7=0 | 7 bitů hodnoty (0–127)
 *
 *   Typ zprávy       Status   Data1        Data2
 *   ──────────────────────────────────────────────────────────────
 *   Note Off         0x8n     nota         rychlost
 *   Note On          0x9n     nota         rychlost   (vel=0 → NoteOff)
 *   Poly Pressure    0xAn     nota         tlak        (ignorujeme)
 *   Control Change   0xBn     číslo CC     hodnota
 *   Program Change   0xCn     program      —           (1 datový bajt)
 *   Channel Pressure 0xDn     tlak         —           (ignorujeme)
 *   Pitch Bend       0xEn     LSB (7b)     MSB (7b)    (14-bit, střed=8192)
 *
 * Running Status:
 *   Pokud přijde datový bajt bez nového status byte, použije se poslední
 *   platný status. Klaviatury to využívají při rychlém legatu (šetří 1 bajt).
 *
 * Real-time (0xF8–0xFF):
 *   MIDI Clock, Start, Stop… mohou přijít KDYKOLI i uprostřed zprávy.
 *   Ignorujeme je, aniž bychom narušili running status nebo aktuální stav.
 *
 * SysEx (0xF0…0xF7):
 *   Výrobcové bloky libovolné délky — přeskakujeme celý blok.
 */

// ── Výstupní struktura jedné kompletní MIDI zprávy ──────────────────────

struct MidiMsg {
    enum Type : uint8_t {
        NONE,            // parser ještě neskončil / ignorovaná zpráva
        NOTE_OFF,        // data1=nota (0–127),  data2=rychlost
        NOTE_ON,         // data1=nota (0–127),  data2=rychlost (>0)
        CONTROL_CHANGE,  // data1=číslo CC,      data2=hodnota (0–127)
        PROGRAM_CHANGE,  // data1=číslo programu (0–127)
        PITCH_BEND,      // bend: −8192 (plně dolů) … +8191 (plně nahoru)
    };

    Type    type    = NONE;
    uint8_t channel = 0;    // MIDI kanál 0–15  (= zobrazovaný kanál 1–16 mínus 1)
    uint8_t data1   = 0;    // nota / číslo CC / číslo programu
    uint8_t data2   = 0;    // rychlost / hodnota CC
    int16_t bend    = 0;    // 14-bit signed, pouze pro PITCH_BEND
};

// ── Třída parseru ────────────────────────────────────────────────────────

class MidiParser {
public:
    /*
     * Zpracuje jeden bajt ze sériového streamu.
     *
     * Vrátí true  → zpráva je kompletní, `out` je platný.
     * Vrátí false → zpráva ještě není hotová (nebo je ignorovaná).
     *
     * Bezpečné vůči chybám: neznámé bajty parser nepokazí.
     */
    bool parse(uint8_t b, MidiMsg& out);

    // Reset — volej po výpadku linky nebo při re-sync
    void reset() { _state = State::IDLE; _status = 0; _d1 = 0; _ch = 0; }

private:
    enum class State : uint8_t {
        IDLE,    // čekáme na status byte
        DATA1,   // čekáme na první datový bajt
        DATA2,   // čekáme na druhý datový bajt
        SYSEX,   // uvnitř SysEx bloku — ignorujeme do 0xF7
    };

    State   _state  = State::IDLE;
    uint8_t _status = 0;    // running status (uložený horní nibble)
    uint8_t _ch     = 0;    // kanál z posledního status byte
    uint8_t _d1     = 0;    // buffer pro první datový bajt

    // Počet datových bajtů pro daný status nibble.
    static uint8_t dataCount(uint8_t sn) {
        switch (sn) {
            case 0x80: case 0x90: case 0xA0:
            case 0xB0: case 0xE0: return 2;
            case 0xC0: case 0xD0: return 1;
            default:               return 0;
        }
    }

    // Sestaví MidiMsg; ošetřuje NoteOn(vel=0) → NoteOff a 14-bit bend.
    static MidiMsg buildMsg(uint8_t sn, uint8_t ch, uint8_t d1, uint8_t d2) {
        MidiMsg m;
        m.channel = ch; m.data1 = d1; m.data2 = d2;
        switch (sn) {
            case 0x80: m.type = MidiMsg::NOTE_OFF; break;
            case 0x90:
                // NoteOn s velocity=0 je Running-Status NoteOff (MIDI spec §5.1)
                m.type = (d2 == 0) ? MidiMsg::NOTE_OFF : MidiMsg::NOTE_ON;
                break;
            case 0xB0: m.type = MidiMsg::CONTROL_CHANGE; break;
            case 0xC0: m.type = MidiMsg::PROGRAM_CHANGE; break;
            case 0xE0: {
                // 14-bit Pitch Bend:
                //   raw = d1(bity 0–6) | d2(bity 7–13),  rozsah 0..16383
                //   střed = 8192 → signed: raw − 8192 = −8192..+8191
                uint16_t raw = uint16_t(d1) | (uint16_t(d2) << 7);
                m.bend = int16_t(raw) - 8192;
                m.type = MidiMsg::PITCH_BEND;
                break;
            }
            default: m.type = MidiMsg::NONE; break;
        }
        return m;
    }
};

// ── Implementace (inline, header-only) ──────────────────────────────────

inline bool MidiParser::parse(uint8_t b, MidiMsg& out) {
    out.type = MidiMsg::NONE;

    // ── Real-time bajty (0xF8–0xFF) ──────────────────────────────────────
    // Mohou přijít kdykoli; ignorujeme je a nenarušíme running status.
    if (b >= 0xF8) return false;

    // ── Start SysEx (0xF0) ───────────────────────────────────────────────
    // Zruší running status — obsah SysEx může obsahovat jakékoli bajty.
    if (b == 0xF0) { _state = State::SYSEX; _status = 0; return false; }

    // ── Uvnitř SysEx → ignoruj vše do End Of SysEx (0xF7) ───────────────
    if (_state == State::SYSEX) {
        if (b == 0xF7) _state = State::IDLE;
        return false;
    }

    // ── Status byte (bit7 = 1) ───────────────────────────────────────────
    if (b & 0x80) {
        // System Common (0xF1–0xF7 bez SysEx/EOX): neznámá délka → resetuj
        if (b >= 0xF0) { _state = State::IDLE; _status = 0; return false; }

        _status = b & 0xF0;     // typ zprávy (horní nibble, running status)
        _ch     = b & 0x0F;     // kanál (dolní nibble)
        _state  = (dataCount(_status) > 0) ? State::DATA1 : State::IDLE;
        return false;
    }

    // ── Datový bajt (bit7 = 0) ───────────────────────────────────────────
    if (_state == State::IDLE || _status == 0) {
        // Datový bajt bez platného running status → zahoď.
        // Stává se po připojení klávesnice za chodu nebo po line error.
        return false;
    }

    if (_state == State::DATA1) {
        if (dataCount(_status) == 1) {
            // Jednobajtová zpráva (Program Change, Channel Pressure)
            out = buildMsg(_status, _ch, b, 0);
            // Running status: stav zůstane v DATA1 pro příští zprávu
            return (out.type != MidiMsg::NONE);
        }
        _d1    = b;
        _state = State::DATA2;
        return false;
    }

    if (_state == State::DATA2) {
        out    = buildMsg(_status, _ch, _d1, b);
        _state = State::DATA1;  // running status: čekáme na další d1
        return (out.type != MidiMsg::NONE);
    }

    return false;
}