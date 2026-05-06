/*
 * Display.cpp
 * Fáze 5 — SCM1602-V3.1 (kontrolér SPLC780D, HD44780-kompatibilní)
 * Přímé paralelní připojení, 4-bit mód, bez I²C backpacku.
 *
 * ── SPLC780D vs HD44780 ───────────────────────────────────────────
 *   SPLC780D je přímý pin-kompatibilní klon HD44780.
 *   Příkazová sada je totožná → standardní Arduino LiquidCrystal
 *   knihovna funguje bez jakýchkoliv úprav.
 *   Časování E pulzu: t_pw(min) = 230 ns (HD44780: 450 ns).
 *   LiquidCrystal knihovna čeká > 1 µs → v pořádku pro oba.
 *
 * ── Inicializace 4-bit módu (interně v LiquidCrystal::begin()) ───
 *   HD44780 vyžaduje specifickou inicializační sekvenci po power-on:
 *     1. Počkat ≥ 15 ms po VCC ≥ 4.5 V (nebo ≥ 40 ms pro VCC ≥ 2.7 V)
 *     2. Odeslat 0x03 třikrát (s čekáním) pro synchronizaci
 *     3. Odeslat 0x02 pro přepnutí do 4-bit módu
 *     4. Function Set: 4-bit, 2 řádky, 5×8 font
 *     5. Display ON, cursor OFF, blink OFF
 *     6. Entry Mode: increment, no shift
 *   LiquidCrystal::begin(16, 2) toto provede automaticky.
 *   My jen zavoláme begin() po dostatečném zpoždění.
 *
 * ── CGRAM — vlastní znaky ─────────────────────────────────────────
 *   CGRAM adresa znaku N = 0x40 + N×8.
 *   Každý znak = 8 bytů, každý byte = 5 platných bitů (bity 4–0).
 *   Bit 4 = levý sloupec, bit 0 = pravý.
 *   Řádek 7 (cursor line) se obvykle nechává 0x00.
 *
 *   Postup:
 *     lcd.createChar(index, uint8_t data[8]);
 *   Po zápisu do CGRAM je nutné volat lcd.home() nebo lcd.setCursor()
 *   aby se adresový čítač přesunul zpět do DDRAM.
 *
 * ── Change-detection — proč a jak ────────────────────────────────
 *   Paralelní zápis jednoho 16-znakového řádku = 16× setCursor+print
 *   zabralo by ~600 µs (každý znak ≈ 37 µs zpracování + E puls).
 *   Zbytečný zápis stejných dat způsobuje viditelné "záblesky" LCD
 *   (HD44780 krátce přeruší výstup při příjmu dat).
 *
 *   Řešení:
 *     _new0/_new1 : právě vyrenderovaný obsah
 *     _old0/_old1 : co je skutečně na LCD
 *     if strcmp(_new0, _old0) != 0 → zapsat celý řádek najednou
 *     Zápis celého řádku: setCursor(0, N) + print(string)
 *                          = 1× setCursor + 16× send_byte ≈ 600 µs
 *     Pokud se řádek nezměnil: 0 µs.
 *
 * ── Řádek 0 — LIVE pohled ─────────────────────────────────────────
 *   Formát (16 znaků přesně):
 *     " BPM WAVE STATE"
 *     Pole:
 *       [0–2]  BPM (3 cifry, zarovnáno doprava, bez "BPM" textu kvůli místu)
 *       [3–5]  "BPM"
 *       [6]    ' '
 *       [7–9]  zkratka průběhu (3 znaky: SIN/SAW/SQR/TRI/BSW/SMP)
 *       [10–11] "  "
 *       [12]   vlastní znak ▶ nebo ■
 *       [13]   ' '
 *       [14-15] rezerva (mezery)
 *
 *   Příklad: "120BPM BSW  \x01  "
 *                              ↑ char(DISP_CGRAM_STOP)
 *
 * ── Řádek 1 — LIVE pohled ─────────────────────────────────────────
 *   16 znaků, každý = jeden krok sekvenceru (index 0–15).
 *   Priorita symbolů (od nejvyšší):
 *     1. playhead + aktivní → '*'
 *     2. playhead + neakt.  → char(DISP_CGRAM_HEAD)  ▸
 *     3. selected (edit)    → '#'   (jen pokud sekvencer zastaven)
 *     4. aktivní            → char(DISP_CGRAM_ACTIVE) ▮
 *     5. neaktivní          → '.'
 */

#include "Display.h"
#include <string.h>   // strcmp, memcpy, strlen

// ─────────────────────────────────────────────
//  Bitmapová data vlastních znaků (5×8 px)
//  Každý řádek = 1 byte, bit4 = levý pixel, bit0 = pravý pixel.
// ─────────────────────────────────────────────

// ▶ PLAY — plný trojúhelník doprava
static const uint8_t CGRAM_PLAY[8] = {
    0b10000,   //  █
    0b11000,   //  ██
    0b11100,   //  ███
    0b11110,   //  ████
    0b11100,   //  ███
    0b11000,   //  ██
    0b10000,   //  █
    0b00000,
};

// ■ STOP — plný čtverec
static const uint8_t CGRAM_STOP[8] = {
    0b00000,
    0b11111,   //  █████
    0b11111,   //  █████
    0b11111,   //  █████
    0b11111,   //  █████
    0b11111,   //  █████
    0b00000,
    0b00000,
};

// ▮ ACTIVE STEP — tlustý svislý pruh (aktivní krok v sekvenceru)
static const uint8_t CGRAM_ACTIVE[8] = {
    0b01110,   //  ███
    0b01110,   //  ███
    0b01110,   //  ███
    0b01110,   //  ███
    0b01110,   //  ███
    0b01110,   //  ███
    0b01110,   //  ███
    0b00000,
};

// ▸ PLAYHEAD — tenký trojúhelník (playhead na neaktivním kroku)
static const uint8_t CGRAM_HEAD[8] = {
    0b10000,   //  █
    0b11000,   //  ██
    0b11100,   //  ███
    0b11110,   //  ████
    0b11100,   //  ███
    0b11000,   //  ██
    0b10000,   //  █
    0b00000,
};

// ─────────────────────────────────────────────
//  Konstruktor
// ─────────────────────────────────────────────
Display::Display()
    : _lcd(DISP_RS_PIN, DISP_EN_PIN,
           DISP_D4_PIN, DISP_D5_PIN, DISP_D6_PIN, DISP_D7_PIN),
      _view(DisplayView::LIVE),
      _bpm(120), _wave(WaveType::SINE),
      _isPlaying(false), _stepMask(0),
      _playhead(0xFF), _selected(0),
      _editStep(0), _editAmp(512),
      _editWave(WaveType::SINE), _editBpm(120),
      _editActive(false), _editEnteredAt(0)
{
    // Naplnit buffery mezerami (prázdný displej)
    memset(_new0, ' ', DISP_COLS);  _new0[DISP_COLS] = '\0';
    memset(_new1, ' ', DISP_COLS);  _new1[DISP_COLS] = '\0';
    // Old buffery záměrně prázdné (strlen=0) → první flush vždy zapíše
    memset(_old0, 0, sizeof(_old0));
    memset(_old1, 0, sizeof(_old1));

    _editNote[0] = '-';
    _editNote[1] = '-';
    _editNote[2] = '\0';
}

// ─────────────────────────────────────────────
//  begin
//
//  Zpoždění 50 ms: zajistí, že LCD dokončil vlastní power-on reset
//  (SPLC780D vyžaduje ≥ 40 ms při VCC = 3.3 V).
//  LiquidCrystal::begin() pak provede inicializační sekvenci.
// ─────────────────────────────────────────────
void Display::begin() {
    delay(50);                        // LCD power-on reset čas
    _lcd.begin(DISP_COLS, DISP_ROWS); // 4-bit init, 2 řádky, 5×8 font
    _defineCgram();                   // zapsat vlastní znaky do CGRAM
    _lcd.clear();
    _lcd.noCursor();
    _lcd.noBlink();
}

// ─────────────────────────────────────────────
//  setLiveData / setEditData
// ─────────────────────────────────────────────
void Display::setLiveData(uint16_t bpm, WaveType wave,
                          bool isPlaying, uint16_t stepMask,
                          uint8_t playhead, uint8_t selected) {
    _bpm       = bpm;
    _wave      = wave;
    _isPlaying = isPlaying;
    _stepMask  = stepMask;
    _playhead  = playhead;
    _selected  = selected;
}

void Display::setEditData(uint8_t stepIdx, const char* noteName,
                          uint16_t amplitude, WaveType wave,
                          uint16_t bpm, bool stepActive) {
    _editStep   = stepIdx;
    _editAmp    = amplitude;
    _editWave   = wave;
    _editBpm    = bpm;
    _editActive = stepActive;
    strncpy(_editNote, noteName, 3);
    _editNote[3] = '\0';
}

// ─────────────────────────────────────────────
//  Přepínání pohledů
// ─────────────────────────────────────────────
void Display::showLive() {
    _view = DisplayView::LIVE;
}

void Display::showEdit() {
    _view          = DisplayView::EDIT;
    _editEnteredAt = millis();
}

// ─────────────────────────────────────────────
//  flush — hlavní metoda, volat z loop()
// ─────────────────────────────────────────────
void Display::flush() {
    // Automatický návrat z EDIT na LIVE po timeout
    if (_view == DisplayView::EDIT) {
        if (millis() - _editEnteredAt >= DISP_EDIT_TIMEOUT_MS) {
            _view = DisplayView::LIVE;
        }
    }

    if (_view == DisplayView::LIVE) _renderLive();
    else                            _renderEdit();

    _writeBuf();
}

// ─────────────────────────────────────────────
//  _renderLive
//
//  Řádek 0 — layout (16 znaků):
//    [0–2]   BPM číslo (3 cifry, %3u)
//    [3–5]   "BPM"
//    [6]     ' '
//    [7–9]   průběh (3 znaky)
//    [10–11] "  "
//    [12]    vlastní znak ▶/■
//    [13–15] "   "
//
//  Řádek 1 — 16 krokových symbolů (viz prioritní logika v hlavičce)
// ─────────────────────────────────────────────
void Display::_renderLive() {
    // ── Řádek 0 ──────────────────────────────────────────────────
    const char stateChar = _isPlaying
                           ? static_cast<char>(DISP_CGRAM_PLAY)
                           : static_cast<char>(DISP_CGRAM_STOP);

    snprintf(_new0, DISP_COLS + 1,
             "%3uBPM %s  %c   ",
             static_cast<unsigned>(_bpm),
             _waveShort(_wave),
             stateChar);
    _pad(_new0, DISP_COLS);

    // ── Řádek 1 — krokové symboly ────────────────────────────────
    for (uint8_t i = 0; i < DISP_COLS; ++i) {
        const bool active = (bool)(_stepMask & (1u << i));
        const bool isHead = (_isPlaying && (_playhead == i));
        const bool isSel  = (!_isPlaying && (_selected == i));

        char c;
        if      (isHead && active)  c = '*';
        else if (isHead && !active) c = static_cast<char>(DISP_CGRAM_HEAD);
        else if (isSel)             c = '#';
        else if (active)            c = static_cast<char>(DISP_CGRAM_ACTIVE);
        else                        c = '.';

        _new1[i] = c;
    }
    _new1[DISP_COLS] = '\0';
}

// ─────────────────────────────────────────────
//  _renderEdit
//
//  Řádek 0 — layout (16 znaků):
//    "S%02u  %-3s AMP:%4u"
//    Příklad: "S05  E4  AMP: 512"  → ořízne snprintf na 16 znaků
//
//  Řádek 1 — layout (16 znaků):
//    "WAVE:%-3s  %3uBPM"
//    Příklad: "WAVE:BSW  120BPM"
// ─────────────────────────────────────────────
void Display::_renderEdit() {
    const uint8_t displayStep = _editStep + 1u;   // zobraz 01–16

    snprintf(_new0, DISP_COLS + 1,
             "S%02u  %-3s AMP:%4u",
             static_cast<unsigned>(displayStep),
             _editNote,
             static_cast<unsigned>(_editAmp));
    _pad(_new0, DISP_COLS);

    snprintf(_new1, DISP_COLS + 1,
             "WAVE:%-3s  %3uBPM",
             _waveShort(_editWave),
             static_cast<unsigned>(_editBpm));
    _pad(_new1, DISP_COLS);
}

// ─────────────────────────────────────────────
//  _writeBuf — porovnat buffery a zapsat změny na LCD
//
//  Celý řádek se zapíše najednou (setCursor + print) —
//  LCD nepodporuje zápis od libovolné pozice bez setCursor,
//  proto je rychlejší přepsat celý řádek než psát znak po znaku
//  s setCursor před každým znakem.
//
//  Časování: zápis 16 znaků ≈ 16 × 37 µs + overhead ≈ 600–700 µs.
//  Pokud se řádek nezměnil: 0 µs.
// ─────────────────────────────────────────────
void Display::_writeBuf() {
    if (strcmp(_new0, _old0) != 0) {
        _lcd.setCursor(0, 0);
        _lcd.print(_new0);
        memcpy(_old0, _new0, DISP_COLS + 1);
    }
    if (strcmp(_new1, _old1) != 0) {
        _lcd.setCursor(0, 1);
        _lcd.print(_new1);
        memcpy(_old1, _new1, DISP_COLS + 1);
    }
}

// ─────────────────────────────────────────────
//  _defineCgram — zapsat 4 vlastní znaky do CGRAM
//
//  Voláno jednou z begin().
//  Po createChar() musí následovat setCursor() nebo home(),
//  protože createChar() nastavuje AC na CGRAM adresu,
//  nikoliv DDRAM — bez resetování by print() zapisoval do CGRAM.
// ─────────────────────────────────────────────
void Display::_defineCgram() {
    // createChar přijímá uint8_t* — cast je bezpečný (stejný typ)
    _lcd.createChar(DISP_CGRAM_PLAY,   const_cast<uint8_t*>(CGRAM_PLAY));
    _lcd.createChar(DISP_CGRAM_STOP,   const_cast<uint8_t*>(CGRAM_STOP));
    _lcd.createChar(DISP_CGRAM_ACTIVE, const_cast<uint8_t*>(CGRAM_ACTIVE));
    _lcd.createChar(DISP_CGRAM_HEAD,   const_cast<uint8_t*>(CGRAM_HEAD));

    // Reset adresového čítače zpět na DDRAM
    _lcd.setCursor(0, 0);
}

// ─────────────────────────────────────────────
//  _waveShort — 3-znakové zkratky průběhů
// ─────────────────────────────────────────────
const char* Display::_waveShort(WaveType w) {
    switch (w) {
        case WaveType::SINE:            return "SIN";
        case WaveType::SAW:             return "SAW";
        case WaveType::SQUARE:          return "SQR";
        case WaveType::TRIANGLE:        return "TRI";
        case WaveType::BANDLIMITED_SAW: return "BSW";
        case WaveType::SAMPLE:          return "SMP";
        default:                        return "???";
    }
}

// ─────────────────────────────────────────────
//  _pad — doplnit buffer mezerami na targetLen znaků
//
//  snprintf() vždy zapíše null-terminátor, ale nezaručuje délku
//  targetLen — může být kratší pokud jsou hodnoty malé.
//  _pad doplní zbytek mezerami aby řádek měl vždy přesně 16 znaků.
//  Tím se zamezí zobrazení "smetí" ze starého obsahu LCD,
//  protože LCD nepromazává nevyužité pozice.
// ─────────────────────────────────────────────
void Display::_pad(char* buf, uint8_t targetLen) {
    uint8_t len = static_cast<uint8_t>(strnlen(buf, targetLen));
    while (len < targetLen) {
        buf[len++] = ' ';
    }
    buf[targetLen] = '\0';
}
