/*
 * Scales.h — hudební stupnice a mapování kláves pro Synthex engine
 *
 * ═══════════════════════════════════════════════════════════════════
 *  PŘEHLED OBSAHU
 * ═══════════════════════════════════════════════════════════════════
 *
 *  1. MIDI nota → frekvence (Hz) — vzorec: 440 × 2^((n-69)/12)
 *     • noteFreq(midiNote)     — constexpr funkce, vhodná pro pole
 *     • NOTE(semitone, octave) — makro pro MIDI číslo noty
 *     • pojmenované konstanty  — C4, Ds3, A4 …
 *
 *  2. Vzory stupnic (semitónové intervaly od kořene)
 *     • ScalePattern struct + předdefinované vzory
 *       SCALE_CHROMATIC, SCALE_MAJOR, SCALE_NATURAL_MINOR,
 *       SCALE_PENTATONIC_MAJOR, SCALE_PENTATONIC_MINOR,
 *       SCALE_BLUES, SCALE_DORIAN, SCALE_PHRYGIAN,
 *       SCALE_MIXOLYDIAN, SCALE_WHOLE_TONE, SCALE_DIMINISHED
 *
 *  3. Hotové frekvenční tabulky
 *     • buildScale()       — naplní float pole podle vzoru a kořene
 *     • Předdefinované pole pro nejčastější použití v loop()
 *       SCALE_C4_MAJOR, SCALE_A3_MINOR_PENTA …
 *
 *  4. Akordové makro
 *     • CHORD_MAJOR(root), CHORD_MINOR(root), CHORD_DOM7(root)
 *
 *  5. SILENCE — frekvence 0.0 Hz (note off / pauza)
 *
 * ═══════════════════════════════════════════════════════════════════
 *  NOTOVÁ KONVENCE
 * ═══════════════════════════════════════════════════════════════════
 *
 *  MIDI čísla (standard):
 *     C-1 = 0   C0 = 12   C1 = 24   C2 = 36
 *     C3 = 48   C4 = 60 (střední C)  A4 = 69 (ladičkové 440 Hz)
 *     C5 = 72   C8 = 108  (nejvyšší na klavíru)
 *
 *  Semitóny v oktávě (0–11):
 *     C=0  C#=1  D=2  D#=3  E=4  F=5  F#=6  G=7  G#=8  A=9  A#=10  B=11
 *
 *  Pojmenování v kódu:
 *     "s" za názvem = křížek (#):   Cs = C#,  Ds = D#,  Fs = F# …
 *     "b" za názvem = béčko (b):    Db = Db,  Eb = Eb  (alias pro #)
 *
 * ═══════════════════════════════════════════════════════════════════
 *  PŘÍKLADY POUŽITÍ
 * ═══════════════════════════════════════════════════════════════════
 *
 *  #include "Scales.h"
 *
 *  // — Přímé frekvence jednotlivých not —
 *  engine.noteOn(0, noteFreq(NOTE(A, 4)));        // A4 = 440 Hz
 *  engine.noteOn(0, noteFreq(NOTE(C, 5)));        // C5 = 523.25 Hz
 *  engine.noteOn(0, noteFreq(NOTE(Ds, 3)));       // D#3 = 155.56 Hz
 *
 *  // — Pojmenované konstanty —
 *  engine.noteOn(0, MIDI_C4,  512, WaveType::SINE);
 *  engine.noteOn(1, MIDI_E4,  512, WaveType::SINE);
 *  engine.noteOn(2, MIDI_G4,  512, WaveType::SINE);   // C dur akord
 *
 *  // — Iterace přes stupnici —
 *  static uint8_t step = 0;
 *  engine.noteOn(0, SCALE_C4_MAJOR[step % SCALE_MAJOR.len]);
 *  step++;
 *
 *  // — Vlastní stupnice z libovolného kořene —
 *  float myScale[12];
 *  uint8_t len = buildScale(NOTE(D, 4), SCALE_PENTATONIC_MINOR, myScale);
 *  // → D4 pentatonická moll: D4, F4, G4, A4, C5
 *
 *  // — Akordové noty (offsety v semitónech) —
 *  const uint8_t* ch = CHORD_MAJOR(NOTE(G, 3));
 *  // výsledek: MIDI čísla tónů G dur akordu
 */

#pragma once
#include <stdint.h>
#include <math.h>    // powf — pouze v noteFreq(), NIKDY nevolej z ISR

// ═══════════════════════════════════════════════════════════════════
//  1. ZÁKLADNÍ KONSTANTA — LADICÍ VÝŠKA
// ═══════════════════════════════════════════════════════════════════

// Frekvence A4 v Hz (standardní ladění).
// Změn na 432.0f nebo 415.0f pro historická ladění.
#ifndef SCALES_A4_HZ
#define SCALES_A4_HZ   440.0f
#endif

// Ticho / pauza — předej do noteOn() jako freq pro okamžité ztišení
#define SCALES_SILENCE   0.0f

// ═══════════════════════════════════════════════════════════════════
//  2. MIDI NOTA → FREKVENCE
// ═══════════════════════════════════════════════════════════════════

// NOTE(semitone, octave) → MIDI číslo noty
// Příklady: NOTE(A,4)=69  NOTE(C,4)=60  NOTE(Fs,3)=54
//
// Semitóny (0–11):
#define C   0
#define Cs  1   // C# / Db
#define Db  1
#define D   2
#define Ds  3   // D# / Eb
#define Eb  3
#define E   4
#define F   5
#define Fs  6   // F# / Gb
#define Gb  6
#define G   7
#define Gs  8   // G# / Ab
#define Ab  8
#define A   9
#define As  10  // A# / Bb
#define Bb  10
#define B   11

// Sestaví MIDI číslo z semitónu a oktávy.
// POZOR: NOTE() je makro — může být použito jako inicializátor pole.
// Vzorec: (octave+1)*12 + semitone  →  C-1=0, C0=12, C4=60, A4=69
#define NOTE(semitone, octave)   ((uint8_t)(((octave) + 1u) * 12u + (semitone)))

// Převede MIDI číslo noty na frekvenci v Hz.
// Vzorec (12-TET, stejné temperování):
//   f(n) = A4 × 2^((n - 69) / 12)
// NIKDY nevolej z ISR — powf() je pomalé!
static inline float noteFreq(uint8_t midiNote) {
    return SCALES_A4_HZ * powf(2.0f, (static_cast<float>(midiNote) - 69.0f) / 12.0f);
}

// ═══════════════════════════════════════════════════════════════════
//  3. POJMENOVANÉ MIDI KONSTANTY (nejpoužívanější noty)
// ═══════════════════════════════════════════════════════════════════
//
//  Frekvence (noteFreq()):
//    MIDI_C4 → 261.63 Hz    MIDI_A4 → 440.00 Hz
//    MIDI_E4 → 329.63 Hz    MIDI_G4 → 392.00 Hz
//
//  Použití jako index:
//    engine.noteOn(0, noteFreq(MIDI_A4));   // 440 Hz

// Oktáva 2
static constexpr uint8_t MIDI_C2  = NOTE(C,  2);  // 36  —  65.41 Hz
static constexpr uint8_t MIDI_D2  = NOTE(D,  2);  // 38  —  73.42 Hz
static constexpr uint8_t MIDI_E2  = NOTE(E,  2);  // 40  —  82.41 Hz
static constexpr uint8_t MIDI_F2  = NOTE(F,  2);  // 41  —  87.31 Hz
static constexpr uint8_t MIDI_G2  = NOTE(G,  2);  // 43  —  98.00 Hz
static constexpr uint8_t MIDI_A2  = NOTE(A,  2);  // 45  — 110.00 Hz
static constexpr uint8_t MIDI_B2  = NOTE(B,  2);  // 47  — 123.47 Hz

// Oktáva 3
static constexpr uint8_t MIDI_C3  = NOTE(C,  3);  // 48  — 130.81 Hz
static constexpr uint8_t MIDI_Cs3 = NOTE(Cs, 3);  // 49  — 138.59 Hz
static constexpr uint8_t MIDI_D3  = NOTE(D,  3);  // 50  — 146.83 Hz
static constexpr uint8_t MIDI_Ds3 = NOTE(Ds, 3);  // 51  — 155.56 Hz
static constexpr uint8_t MIDI_E3  = NOTE(E,  3);  // 52  — 164.81 Hz
static constexpr uint8_t MIDI_F3  = NOTE(F,  3);  // 53  — 174.61 Hz
static constexpr uint8_t MIDI_Fs3 = NOTE(Fs, 3);  // 54  — 185.00 Hz
static constexpr uint8_t MIDI_G3  = NOTE(G,  3);  // 55  — 196.00 Hz
static constexpr uint8_t MIDI_Gs3 = NOTE(Gs, 3);  // 56  — 207.65 Hz
static constexpr uint8_t MIDI_A3  = NOTE(A,  3);  // 57  — 220.00 Hz
static constexpr uint8_t MIDI_As3 = NOTE(As, 3);  // 58  — 233.08 Hz
static constexpr uint8_t MIDI_B3  = NOTE(B,  3);  // 59  — 246.94 Hz

// Oktáva 4 (střední — nejpoužívanější)
static constexpr uint8_t MIDI_C4  = NOTE(C,  4);  // 60  — 261.63 Hz  ← střední C
static constexpr uint8_t MIDI_Cs4 = NOTE(Cs, 4);  // 61  — 277.18 Hz
static constexpr uint8_t MIDI_D4  = NOTE(D,  4);  // 62  — 293.66 Hz
static constexpr uint8_t MIDI_Ds4 = NOTE(Ds, 4);  // 63  — 311.13 Hz
static constexpr uint8_t MIDI_E4  = NOTE(E,  4);  // 64  — 329.63 Hz
static constexpr uint8_t MIDI_F4  = NOTE(F,  4);  // 65  — 349.23 Hz
static constexpr uint8_t MIDI_Fs4 = NOTE(Fs, 4);  // 66  — 369.99 Hz
static constexpr uint8_t MIDI_G4  = NOTE(G,  4);  // 67  — 392.00 Hz
static constexpr uint8_t MIDI_Gs4 = NOTE(Gs, 4);  // 68  — 415.30 Hz
static constexpr uint8_t MIDI_A4  = NOTE(A,  4);  // 69  — 440.00 Hz  ← ladičkový tón
static constexpr uint8_t MIDI_As4 = NOTE(As, 4);  // 70  — 466.16 Hz
static constexpr uint8_t MIDI_B4  = NOTE(B,  4);  // 71  — 493.88 Hz

// Oktáva 5
static constexpr uint8_t MIDI_C5  = NOTE(C,  5);  // 72  — 523.25 Hz
static constexpr uint8_t MIDI_D5  = NOTE(D,  5);  // 74  — 587.33 Hz
static constexpr uint8_t MIDI_E5  = NOTE(E,  5);  // 76  — 659.26 Hz
static constexpr uint8_t MIDI_G5  = NOTE(G,  5);  // 79  — 783.99 Hz
static constexpr uint8_t MIDI_A5  = NOTE(A,  5);  // 81  — 880.00 Hz

// ═══════════════════════════════════════════════════════════════════
//  4. VZORY STUPNIC
// ═══════════════════════════════════════════════════════════════════
//
//  ScalePattern obsahuje pole semitónových offsetů od kořene a délku.
//  Indexem [0] je vždy kořen (offset = 0).
//
//  Použití:
//    buildScale(NOTE(D, 4), SCALE_DORIAN, myFreqs);
//    → pole frekvencí D4 dórské stupnice

#define SCALES_MAX_LEN  12u   // max. délka stupnice (chromatická)

struct ScalePattern {
    uint8_t intervals[SCALES_MAX_LEN];  // semitónové offsety od kořene
    uint8_t len;                        // počet not ve stupnici
};

// ── Chromatická (všech 12 půltónů) ───────────────────────────────
static const ScalePattern SCALE_CHROMATIC = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }, 12
};

// ── Durové (ionická) ─────────────────────────────────────────────
// C D E F G A B  (W W H W W W H)
static const ScalePattern SCALE_MAJOR = {
    { 0, 2, 4, 5, 7, 9, 11 }, 7
};

// ── Přirozená mollová (aiolská) ──────────────────────────────────
// C D Eb F G Ab Bb  (W H W W H W W)
static const ScalePattern SCALE_NATURAL_MINOR = {
    { 0, 2, 3, 5, 7, 8, 10 }, 7
};

// ── Harmonická moll ──────────────────────────────────────────────
// C D Eb F G Ab B  (W H W W H A2 H) — zvýšený 7. stupeň
static const ScalePattern SCALE_HARMONIC_MINOR = {
    { 0, 2, 3, 5, 7, 8, 11 }, 7
};

// ── Pentatonická dur ─────────────────────────────────────────────
// C D E G A  — bez 4. a 7. stupně
static const ScalePattern SCALE_PENTATONIC_MAJOR = {
    { 0, 2, 4, 7, 9 }, 5
};

// ── Pentatonická moll ─────────────────────────────────────────────
// C Eb F G Bb — bez 2. a 6. stupně
static const ScalePattern SCALE_PENTATONIC_MINOR = {
    { 0, 3, 5, 7, 10 }, 5
};

// ── Bluesová stupnice ─────────────────────────────────────────────
// C Eb F F# G Bb — pentatonická moll + tritón (blue note)
static const ScalePattern SCALE_BLUES = {
    { 0, 3, 5, 6, 7, 10 }, 6
};

// ── Dórská (moll s zvýšeným 6.) ──────────────────────────────────
// C D Eb F G A Bb — typická pro jazz a funk
static const ScalePattern SCALE_DORIAN = {
    { 0, 2, 3, 5, 7, 9, 10 }, 7
};

// ── Frygická (moll se sníženým 2.) ──────────────────────────────
// C Db Eb F G Ab Bb — španělský / flamenco zvuk
static const ScalePattern SCALE_PHRYGIAN = {
    { 0, 1, 3, 5, 7, 8, 10 }, 7
};

// ── Lydická (dur se zvýšeným 4.) ─────────────────────────────────
// C D E F# G A B — magický, „nadpřirozený" zvuk
static const ScalePattern SCALE_LYDIAN = {
    { 0, 2, 4, 6, 7, 9, 11 }, 7
};

// ── Mixolydická (dur se sníženým 7.) ────────────────────────────
// C D E F G A Bb — rock, folk, dominant blues
static const ScalePattern SCALE_MIXOLYDIAN = {
    { 0, 2, 4, 5, 7, 9, 10 }, 7
};

// ── Celotónová ───────────────────────────────────────────────────
// C D E F# G# Bb — pouze celé tóny, debussyovský zvuk
static const ScalePattern SCALE_WHOLE_TONE = {
    { 0, 2, 4, 6, 8, 10 }, 6
};

// ── Zmenšená (oktatonická, střídání W-H) ─────────────────────────
// C D Eb F Gb Ab A B — symetrická, jazzová disonance
static const ScalePattern SCALE_DIMINISHED = {
    { 0, 2, 3, 5, 6, 8, 9, 11 }, 8
};

// ── Japonská insen ────────────────────────────────────────────────
// C Db F G Bb — japonský meditativní charakter
static const ScalePattern SCALE_INSEN = {
    { 0, 1, 5, 7, 10 }, 5
};

// ── Arabská/double harmonická ────────────────────────────────────
// C Db E F G Ab B — orientální, dramatický zvuk
static const ScalePattern SCALE_DOUBLE_HARMONIC = {
    { 0, 1, 4, 5, 7, 8, 11 }, 7
};

// ═══════════════════════════════════════════════════════════════════
//  5. buildScale() — naplní float pole frekvencí podle stupnice
// ═══════════════════════════════════════════════════════════════════
//
//  Parametry:
//    rootMidi   — MIDI číslo kořene (použij NOTE() nebo MIDI_xx konstanty)
//    pattern    — vzor stupnice (ScalePattern)
//    outFreqs   — výstupní pole (musí mít ≥ pattern.len prvků)
//    octaves    — kolik oktáv rozsahu generovat (1 = jen jedna, 2 = dvě …)
//
//  Vrátí celkový počet vygenerovaných not (pattern.len × octaves).
//
//  Příklady:
//    float f[7];  buildScale(NOTE(C,4), SCALE_MAJOR, f);
//    // → C4(261.6) D4(293.7) E4(329.6) F4(349.2) G4(392.0) A4(440.0) B4(493.9)
//
//    float f[10]; buildScale(NOTE(A,3), SCALE_PENTATONIC_MINOR, f, 2);
//    // → A3 C4 D4 E4 G4 | A4 C5 D5 E5 G5
//
//  POZOR: Nevolej opakovaně z loop() bez ochrany — powf() trvá ~1 µs.
//         Ideální místo: setup() nebo při změně stupnice.
//
static inline uint8_t buildScale(uint8_t rootMidi,
                                  const ScalePattern& pattern,
                                  float* outFreqs,
                                  uint8_t octaves = 1u) {
    uint8_t count = 0u;
    for (uint8_t oct = 0u; oct < octaves; ++oct) {
        for (uint8_t i = 0u; i < pattern.len; ++i) {
            const uint8_t midi = static_cast<uint8_t>(
                rootMidi + pattern.intervals[i] + oct * 12u);
            outFreqs[count++] = noteFreq(midi);
        }
    }
    return count;
}

// ═══════════════════════════════════════════════════════════════════
//  6. PŘEDDEFINOVANÉ FREKVENČNÍ TABULKY (inicializátory)
// ═══════════════════════════════════════════════════════════════════
//
//  Tyto tabulky lze použít přímo v loop() bez volání buildScale().
//  Jsou uloženy ve Flash (.rodata) jako const — neplýtvají RAM.
//
//  Pojmenování: SCALE_{kořen}{oktáva}_{typ}
//
//  Frekvence jsou zaokrouhleny na 2 desetinná místa pro čitelnost;
//  přesné hodnoty získáš přes noteFreq(NOTE(...)).

// C4 dur (iónská) — 7 not
static const float SCALE_C4_MAJOR[7] = {
    261.63f,  // C4
    293.66f,  // D4
    329.63f,  // E4
    349.23f,  // F4
    392.00f,  // G4
    440.00f,  // A4
    493.88f   // B4
};

// A3 přirozená moll — 7 not
static const float SCALE_A3_NATURAL_MINOR[7] = {
    220.00f,  // A3
    246.94f,  // B3
    261.63f,  // C4
    293.66f,  // D4
    329.63f,  // E4
    349.23f,  // F4
    392.00f   // G4
};

// C4 pentatonická dur — 5 not
static const float SCALE_C4_PENTATONIC_MAJOR[5] = {
    261.63f,  // C4
    293.66f,  // D4
    329.63f,  // E4
    392.00f,  // G4
    440.00f   // A4
};

// A3 pentatonická moll — 5 not (bluesy základ)
static const float SCALE_A3_PENTATONIC_MINOR[5] = {
    220.00f,  // A3
    261.63f,  // C4
    293.66f,  // D4
    329.63f,  // E4
    392.00f   // G4
};

// A3 blues — 6 not (pentatonická moll + blue note D#4/Eb4)
static const float SCALE_A3_BLUES[6] = {
    220.00f,  // A3
    261.63f,  // C4
    293.66f,  // D4
    311.13f,  // D#4/Eb4  ← blue note (tritón)
    329.63f,  // E4
    392.00f   // G4
};

// D4 dórská — 7 not
static const float SCALE_D4_DORIAN[7] = {
    293.66f,  // D4
    329.63f,  // E4
    349.23f,  // F4
    392.00f,  // G4
    440.00f,  // A4
    493.88f,  // B4
    523.25f   // C5
};

// E3 frygická — 7 not (flamenco / metal)
static const float SCALE_E3_PHRYGIAN[7] = {
    164.81f,  // E3
    174.61f,  // F3
    196.00f,  // G3
    220.00f,  // A3
    246.94f,  // B3
    261.63f,  // C4
    293.66f   // D4
};

// C4 celotónová — 6 not
static const float SCALE_C4_WHOLE_TONE[6] = {
    261.63f,  // C4
    293.66f,  // D4
    329.63f,  // E4
    369.99f,  // F#4
    415.30f,  // G#4
    466.16f   // A#4
};

// C3 chromatická — 12 not
static const float SCALE_C3_CHROMATIC[12] = {
    130.81f,  // C3
    138.59f,  // C#3
    146.83f,  // D3
    155.56f,  // D#3
    164.81f,  // E3
    174.61f,  // F3
    185.00f,  // F#3
    196.00f,  // G3
    207.65f,  // G#3
    220.00f,  // A3
    233.08f,  // A#3
    246.94f   // B3
};

// ═══════════════════════════════════════════════════════════════════
//  7. AKORDY — semitónové offsety od základního tónu
// ═══════════════════════════════════════════════════════════════════
//
//  Akordová makra vrátí inicializátor {root, third, fifth}.
//  Příklad:
//    uint8_t chord[3] = CHORD_MAJOR_MIDI(NOTE(G, 3));
//    engine.noteOn(0, noteFreq(chord[0]), 400, WaveType::SINE);
//    engine.noteOn(1, noteFreq(chord[1]), 400, WaveType::SINE);
//    engine.noteOn(2, noteFreq(chord[2]), 400, WaveType::SINE);

// Durový trojzvuk (1-3-5): offsety 0, +4, +7 semitónů
#define CHORD_MAJOR_MIDI(rootMidi)  \
    { (uint8_t)((rootMidi) + 0u),  \
      (uint8_t)((rootMidi) + 4u),  \
      (uint8_t)((rootMidi) + 7u) }

// Mollový trojzvuk (1-b3-5): offsety 0, +3, +7
#define CHORD_MINOR_MIDI(rootMidi)  \
    { (uint8_t)((rootMidi) + 0u),  \
      (uint8_t)((rootMidi) + 3u),  \
      (uint8_t)((rootMidi) + 7u) }

// Dominantní septakord (1-3-5-b7): offsety 0, +4, +7, +10
#define CHORD_DOM7_MIDI(rootMidi)   \
    { (uint8_t)((rootMidi) + 0u),  \
      (uint8_t)((rootMidi) + 4u),  \
      (uint8_t)((rootMidi) + 7u),  \
      (uint8_t)((rootMidi) + 10u) }

// Mollový septakord (1-b3-5-b7): offsety 0, +3, +7, +10
#define CHORD_MIN7_MIDI(rootMidi)   \
    { (uint8_t)((rootMidi) + 0u),  \
      (uint8_t)((rootMidi) + 3u),  \
      (uint8_t)((rootMidi) + 7u),  \
      (uint8_t)((rootMidi) + 10u) }

// Zmenšený trojzvuk (1-b3-b5): offsety 0, +3, +6
#define CHORD_DIM_MIDI(rootMidi)    \
    { (uint8_t)((rootMidi) + 0u),  \
      (uint8_t)((rootMidi) + 3u),  \
      (uint8_t)((rootMidi) + 6u) }

// ═══════════════════════════════════════════════════════════════════
//  8. HELPER — okamžitá frekvence akordu (3 hlasy)
// ═══════════════════════════════════════════════════════════════════
//
//  playChordMajor(engine, root, amplitude, wave)
//  Spustí durový trojzvuk na hlasech 0–2.
//  Pro zastavení: engine.noteOff(0); engine.noteOff(1); engine.noteOff(2);
//
//  Inline funkce — vhodné pro main.cpp, nevyžaduje Synthex.h v tomto souboru.
//  Aby fungovaly, musí být Synthex.h includován PŘED Scales.h.

#ifdef _SYNTHEX_H_INCLUDED_   // definuj tento guard v Synthex.h pokud chceš inlinery
static inline void playChordMajor(Synthex& eng, uint8_t rootMidi,
                                   uint16_t amp, WaveType wave) {
    eng.noteOn(0, noteFreq(rootMidi + 0u), amp, wave);
    eng.noteOn(1, noteFreq(rootMidi + 4u), amp, wave);
    eng.noteOn(2, noteFreq(rootMidi + 7u), amp, wave);
}

static inline void playChordMinor(Synthex& eng, uint8_t rootMidi,
                                   uint16_t amp, WaveType wave) {
    eng.noteOn(0, noteFreq(rootMidi + 0u), amp, wave);
    eng.noteOn(1, noteFreq(rootMidi + 3u), amp, wave);
    eng.noteOn(2, noteFreq(rootMidi + 7u), amp, wave);
}
#endif  // _SYNTHEX_H_INCLUDED_

// ═══════════════════════════════════════════════════════════════════
//  RYCHLÁ REFERENČNÍ TABULKA FREKVENCÍ (komentář)
// ═══════════════════════════════════════════════════════════════════
//
//  Oktáva:     0       1       2       3       4       5       6
//  ─────────────────────────────────────────────────────────────────
//  C        16.35   32.70   65.41  130.81  261.63  523.25  1046.50
//  C#/Db    17.32   34.65   69.30  138.59  277.18  554.37  1108.73
//  D        18.35   36.71   73.42  146.83  293.66  587.33  1174.66
//  D#/Eb    19.45   38.89   77.78  155.56  311.13  622.25  1244.51
//  E        20.60   41.20   82.41  164.81  329.63  659.26  1318.51
//  F        21.83   43.65   87.31  174.61  349.23  698.46  1396.91
//  F#/Gb    23.12   46.25   92.50  185.00  369.99  739.99  1479.98
//  G        24.50   49.00   98.00  196.00  392.00  783.99  1567.98
//  G#/Ab    25.96   51.91  103.83  207.65  415.30  830.61  1661.22
//  A        27.50   55.00  110.00  220.00  440.00  880.00  1760.00
//  A#/Bb    29.14   58.27  116.54  233.08  466.16  932.33  1864.66
//  B        30.87   61.74  123.47  246.94  493.88  987.77  1975.53
//  ─────────────────────────────────────────────────────────────────
//  Vzorec: A4=440 Hz, každá oktáva výš = ×2, každý semitón = ×2^(1/12)≈1.0595
