/*
 * Display.cpp — implementace HD44780 LCD 1602 driveru pro Synthex sekvencer
 *
 * Fyzika 4-bitového módu (pro pochopení, co LiquidCrystal dělá uvnitř):
 *
 *   Každý byte se přenese ve dvou nibblech přes piny D4–D7:
 *     1. Horní nibble (bit 7–4) → D7–D4, pulz EN
 *     2. Dolní nibble (bit 3–0) → D7–D4, pulz EN
 *   RS=0: instrukce (setCursor, clear…)
 *   RS=1: data (write)
 *
 *   Inicializační sekvence (3× zápis 0x30 v 8-bit módu, pak přepnutí
 *   na 4-bit) je automaticky v LiquidCrystal::begin().
 *
 * CGRAM adresování:
 *   HD44780: CGRAM adresy 0x00–0x07 odpovídají custom znakům 0–7.
 *   createChar(slot, data[8]) uloží 8 bytů vzoru do CGRAM[slot].
 *   Každý byte = 1 pixel řádek (pouze bity 0–4, bit 5–7 ignorovány).
 *   Bit 4 = levý sloupec, bit 0 = pravý sloupec (0b00000 = blank řádek).
 *   8. byte (data[7]) je řádek kurzoru → ponecháme 0x00.
 */

#include "Display.h"
#include <string.h>   // snprintf

// ─────────────────────────────────────────────
//  Jméno not: indexováno semitónem (0=C … 11=B)
// ─────────────────────────────────────────────
static const char* const NOTE_NAMES[] = {
    "C  ", "C# ", "D  ", "D# ", "E  ", "F  ",
    "F# ", "G  ", "G# ", "A  ", "A# ", "B  "
};

// ─────────────────────────────────────────────
//  CGRAM pixel vzory (5 sloupců × 8 řádků)
//
//  Každý byte: bity 4–0 reprezentují sloupce (bit4=vlevo, bit0=vpravo).
//  Řádek 7 (data[7]) = kurzorový řádek → vždy 0x00.
//  Pixelová mřížka HD44780 je 5×8, horních 7 řádků je viditelných.
// ─────────────────────────────────────────────

// Slot 0: CUR_NOTE — rámeček s tečkou uprostřed
// ┌───┐    11111
// │   │    10001
// │ · │    10101  ← tečka = bit 2
// │   │    10001
// └───┘    11111
//          00000
//          00000
//          00000
static const uint8_t CGRAM_CUR_NOTE[8] = {
    0x1F,   // 11111
    0x11,   // 10001
    0x15,   // 10101
    0x11,   // 10001
    0x1F,   // 11111
    0x00,
    0x00,
    0x00
};

// Slot 1: CUR_REST — prázdný rámeček (aktuální krok, žádná nota)
// ┌───┐    11111
// │   │    10001
// │   │    10001
// │   │    10001
// └───┘    11111
//          00000
//          00000
//          00000
static const uint8_t CGRAM_CUR_REST[8] = {
    0x1F,   // 11111
    0x11,   // 10001
    0x11,   // 10001
    0x11,   // 10001
    0x1F,   // 11111
    0x00,
    0x00,
    0x00
};

// Slot 2: NOTE — plný blok (horní 4 řádky = nota)
// █████    11111
// █████    11111
// █████    11111
// █████    11111
//          00000
//          00000
//          00000
//          00000
static const uint8_t CGRAM_NOTE[8] = {
    0x1F,   // 11111
    0x1F,   // 11111
    0x1F,   // 11111
    0x1F,   // 11111
    0x00,
    0x00,
    0x00,
    0x00
};

// Slot 3: REST — spodní čára (pauza / rest krok)
// (prázdné)
// (prázdné)
// (prázdné)
// (prázdné)
// (prázdné)
// _____    11111
//          00000
//          00000
static const uint8_t CGRAM_REST[8] = {
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x1F,   // 11111
    0x00,
    0x00
};

// ─────────────────────────────────────────────
//  Singleton
// ─────────────────────────────────────────────
Display& Display::getInstance() {
    static Display instance;
    return instance;
}

// ─────────────────────────────────────────────
//  Konstruktor
//  LiquidCrystal vyžaduje pinout v konstruktoru.
//  4-bitový mód: předáváme pouze RS, EN, D4–D7 (bez D0–D3).
// ─────────────────────────────────────────────
Display::Display()
    : _lcd(LCD_RS, LCD_EN, LCD_D4, LCD_D5, LCD_D6, LCD_D7),
      _initialized(false)
{}

// ─────────────────────────────────────────────
//  begin()
//
//  Volej jednou v setup() po engine.begin() a pots.begin().
//  Pořadí: _loadCustomChars() → lcd.begin() → lcd.noCursor().
//
//  POZOR: createChar() musí být voláno PŘED lcd.begin() nebo
//  ihned po něm, ale vždy PŘED prvním setCursor/print.
//  LiquidCrystal interně resetuje CGRAM při begin() → voláme
//  _loadCustomChars() IHNED po begin().
// ─────────────────────────────────────────────
bool Display::begin() {
    _lcd.begin(LCD_COLS, LCD_ROWS);

    // Nahrání CGRAM vzorů — musí být po begin(), která inicializuje HW
    _loadCustomChars();

    _lcd.noCursor();    // skrytí HW kurzoru (podtrhávání)
    _lcd.noBlink();     // vypnutí blinkání kurzoru
    _lcd.clear();       // jednorázové čištění při startu (OK — jen tady)
    _lcd.home();

    _initialized = true;
    return true;
}

// ─────────────────────────────────────────────
//  _loadCustomChars()
//
//  Uloží 4 CGRAM vzory do HD44780 CGRAM.
//  createChar(slot, data) volá RS=0 + příkaz nastavení CGRAM adresy,
//  pak 8× write(byte). Po skončení obnoví DDRAM adr. na 0 (home).
//
//  Tato metoda způsobí dočasné smetí na displeji (~few ms) —
//  proto ji voláme pouze jednou v begin(), ne v refresh().
// ─────────────────────────────────────────────
void Display::_loadCustomChars() {
    // createChar() přebírá uint8_t* ale API je deklarováno jako byte*
    // → v Arduino prostředí uint8_t == byte → cast je bezpečný
    _lcd.createChar(LCD_CHAR_CUR_NOTE,
                    const_cast<uint8_t*>(CGRAM_CUR_NOTE));
    _lcd.createChar(LCD_CHAR_CUR_REST,
                    const_cast<uint8_t*>(CGRAM_CUR_REST));
    _lcd.createChar(LCD_CHAR_NOTE,
                    const_cast<uint8_t*>(CGRAM_NOTE));
    _lcd.createChar(LCD_CHAR_REST,
                    const_cast<uint8_t*>(CGRAM_REST));

    // Po createChar() se LCD přepne do CGRAM zápisu.
    // Vracíme se do DDRAM zápisem home(), aby setCursor() fungoval.
    _lcd.home();
}

// ─────────────────────────────────────────────
//  refresh()
//
//  Překresli celý displej — oba řádky, každý znak na místě.
//  Bez lcd.clear() → bez viditelného blikání, bez zbytečného delay().
//
//  Pořadí:
//    1. setCursor(0, 0) + zápis 16 symbolů step gridu
//    2. setCursor(0, 1) + zápis 16 znaků info řádku
//
//  Celková doba: ~1,5 ms (viz hlavičkový soubor).
// ─────────────────────────────────────────────
void Display::refresh(const Sequencer& seq, const Pots& pots) {
    if (!_initialized) return;

    _drawStepRow(seq);
    _drawInfoRow(seq, pots);
}

// ─────────────────────────────────────────────
//  _drawStepRow()  — řádek 0
//
//  Iteruje přes 16 kroků aktuálního patternu.
//  Pro každý krok vybere správný symbol (CGRAM nebo ASCII).
//
//  Logika výběru symbolu:
//
//    isCurrent (== currentStep):
//      stepHasNote → CGRAM 0 (CUR_NOTE, rámeček s tečkou)
//      jinak       → CGRAM 1 (CUR_REST, prázdný rámeček)
//
//    stepHasNote (active + freq > 0):
//      → CGRAM 2 (NOTE, plný blok)
//
//    stepIsActive (active=true, freq=0, tj. pauza):
//      → CGRAM 3 (REST, spodní čára)
//
//    jinak (inactive):
//      → ASCII '.' (nic nestahovat z CGRAM)
//
//  Trik: setCursor() voláme jednou pro celý řádek a pak píšeme
//  16 znaků za sebou — LCD automaticky posune DDRAM adr. o +1.
//  To šetří 15 zbytečných setCursor() volání.
// ─────────────────────────────────────────────
void Display::_drawStepRow(const Sequencer& seq) {
    const uint8_t cur = seq.currentStep();

    _lcd.setCursor(0, 0);

    for (uint8_t i = 0u; i < SEQ_STEPS; ++i) {
        const bool isCurrent = (i == cur);
        const bool hasNote   = seq.stepHasNote(i);
        const bool isActive  = seq.stepIsActive(i);

        if (isCurrent) {
            // Aktuální krok: ukaž, zda nota zní nebo je ticho
            _lcd.write(static_cast<uint8_t>(
                hasNote ? LCD_CHAR_CUR_NOTE : LCD_CHAR_CUR_REST));

        } else if (hasNote) {
            // Krok s notou (active=true, freq>0)
            _lcd.write(static_cast<uint8_t>(LCD_CHAR_NOTE));

        } else if (isActive) {
            // Aktivní krok bez noty = pauza (rest)
            _lcd.write(static_cast<uint8_t>(LCD_CHAR_REST));

        } else {
            // Neaktivní krok
            _lcd.write('.');
        }
    }
}

// ─────────────────────────────────────────────
//  _drawInfoRow()  — řádek 1
//
//  Pevný formát, přesně 16 znaků: ">120 A#3 BSAW P2"
//
//  Segmenty (viz hlavička pro detaily):
//    [0]    stav transportu
//    [1-3]  BPM (max 999, snprintf zarovná vlevo; nad 999 = "999")
//    [4]    mezera
//    [5-7]  jméno noty aktuálního kroku (3 znaky, zarovnáno vlevo)
//    [8]    mezera
//    [9-12] zkrácený název vlny (přesně 4 znaky)
//    [13]   mezera
//    [14]   'P'
//    [15]   číslo patternu '1'–'4'
//
//  Pro note: čteme krok ze seq.step(currentPattern, currentStep).
//  Pokud krok nemá notu (freq=0 nebo inactive) → zobrazíme "---".
//
//  BPM clampujeme na 999 — při extrémně nízkém tempoMs
//  (< 20 ms) by BPM = 15000/20 = 750 → vejde se do 3 číslic.
//  Pokud by byl výpočet ≥ 1000, zobrazt "999".
// ─────────────────────────────────────────────
void Display::_drawInfoRow(const Sequencer& seq, const Pots& pots) {
    // ── Sesbírej data ────────────────────────
    const uint16_t bpm  = seq.bpm();
    const uint8_t  cp   = seq.currentPattern();
    const uint8_t  cs   = seq.currentStep();
    const SeqStep& s    = seq.step(cp, cs);

    // Stav transportu
    const char stCh = _stateChar(seq.state());

    // BPM — clamp na 999 pro 3 znaky
    const uint16_t bpmDisp = (bpm > 999u) ? 999u : bpm;

    // Jméno noty aktuálního kroku
    char noteBuf[5];
    if (s.active && s.freq > 1.0f) {
        _midiToName(s.midiNote, noteBuf, sizeof(noteBuf));
    } else {
        // Pauza nebo neaktivní krok
        noteBuf[0] = '-'; noteBuf[1] = '-'; noteBuf[2] = '-'; noteBuf[3] = '\0';
    }

    // Název vlny (globální přepis má přednost)
    const WaveType wt = seq.globalWaveEn() ? seq.globalWave()
                                           : s.waveType;

    // Číslo patternu (1-based pro display)
    const char patChar = static_cast<char>('1' + cp);

    // ── Sestav řetězec — přesně 16 znaků ────
    // Formát: "%c%3u %3s %4s P%c"
    // Délka:   1 + 3 + 1 + 3 + 1 + 4 + 1 + 1 + 1 = 16 ✓
    char buf[17];
    snprintf(buf, sizeof(buf), "%c%3u %3s %4s P%c",
             stCh,
             static_cast<unsigned>(bpmDisp),
             noteBuf,
             _waveName(wt),
             patChar);

    // ── Zápis na LCD ─────────────────────────
    // Jistíme délku: pokud by snprintf zkrátil řetězec (nemělo by se stát),
    // doplníme mezerami aby DDRAM neobsahovala staré znaky.
    _lcd.setCursor(0, 1);
    uint8_t written = 0u;
    for (uint8_t i = 0u; buf[i] != '\0' && written < LCD_COLS; ++i) {
        _lcd.write(static_cast<uint8_t>(buf[i]));
        written++;
    }
    // Doplň mezerami pokud byl buf kratší než LCD_COLS
    while (written < LCD_COLS) {
        _lcd.write(' ');
        written++;
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Statické pomocné funkce
// ═══════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────
//  _midiToName()
//
//  Převod MIDI čísla na jméno noty s oktávou.
//  Výsledek je vždy 3 znaky (zarovnáno mezerou zprava) + '\0'.
//
//  MIDI čísla:
//    0   = rest (→ "---")
//    12  = C0, 24 = C1, 36 = C2, 48 = C3, 60 = C4, 69 = A4 (440 Hz)
//    oktáva = midiNote / 12 - 1   (MIDI standard)
//    semitón = midiNote % 12
//
//  Výstup "C4 " (3 znaky) zarovnává info řádek bez podmínek.
//  Křižkové noty jsou 2 znaky + číslice = 3 znaky ("C#4").
//  Základní noty jsou 1 znak + číslice + mezera = 3 znaky ("C4 ").
//
//  Záporná oktáva (MIDI 1–11 → oktáva -1):
//  Na Due int8_t → snprintf %d → zobrazí "-1" → note "C-1"
//  ale MIDI 1–11 jsou pod slysitelný rozsah (< 8 Hz), v projektu
//  se nepoužívají, takže to není praktický problém.
// ─────────────────────────────────────────────
void Display::_midiToName(uint8_t midiNote, char* buf, uint8_t bufLen) {
    if (midiNote == 0u || bufLen < 4u) {
        if (bufLen >= 4u) { buf[0]='-'; buf[1]='-'; buf[2]='-'; buf[3]='\0'; }
        return;
    }

    const uint8_t semitone = midiNote % 12u;
    const int8_t  octave   = static_cast<int8_t>(midiNote / 12u) - 1;

    // NOTE_NAMES[semitone] je "X  " nebo "X# " — vždy 3 znaky + '\0'
    // Ale my chceme "X#4" (3 znaky s číslem oktávy).
    // Rozlišíme: pokud 2. znak == '#' → křižek (2 znaky) + číslo
    //            jinak → základní nota (1 znak) + číslo + mezera

    const char* name = NOTE_NAMES[semitone];

    if (name[1] == '#') {
        // Křižková nota: "C#" + "4" → "C#4"
        snprintf(buf, bufLen, "%c%c%d", name[0], name[1],
                 static_cast<int>(octave));
    } else {
        // Základní nota: "C" + "4" + " " → "C4 "
        snprintf(buf, bufLen, "%c%d ", name[0],
                 static_cast<int>(octave));
    }
}

// ─────────────────────────────────────────────
//  _waveName()
//
//  Vrátí přesně 4 znaky + '\0' pro daný WaveType.
//  Kratší názvy jsou zarovnány mezerou zprava.
// ─────────────────────────────────────────────
const char* Display::_waveName(WaveType wt) {
    switch (wt) {
        case WaveType::SINE:            return "SINE";
        case WaveType::SAW:             return "SAW ";
        case WaveType::SQUARE:          return "SQRE";
        case WaveType::TRIANGLE:        return "TRI ";
        case WaveType::BANDLIMITED_SAW: return "BSAW";
        case WaveType::SAMPLE:          return "SMPL";
        default:                        return "??? ";
    }
}

// ─────────────────────────────────────────────
//  _stateChar()
// ─────────────────────────────────────────────
char Display::_stateChar(SeqState st) {
    switch (st) {
        case SeqState::PLAYING: return '>';
        case SeqState::PAUSED:  return '|';
        default:                return '.';   // STOPPED
    }
}
