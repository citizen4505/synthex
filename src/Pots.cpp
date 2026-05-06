/*
 * Pots.cpp — implementace čtení potenciometrů
 *
 * Schéma zapojení (Arduino Due, 3,3 V logika!):
 *
 *   3,3 V ──┬── [10 kΩ pot] ──┬── GND
 *           │                 │
 *          (CW konec)    (jezdec) ──── A0..A3
 *
 *   POZOR: Due má analog piny tolerantní pouze do 3,3 V.
 *          Nikdy nepřipojuj přes 5 V (zničíš ADC).
 *
 * EMA vzorec (integer, bez float):
 *
 *   smoothed = (prev * (N-1) + raw) / N     kde N = 2^POTS_EMA_SHIFT = 16
 *
 *   Bit-shift varianta (bez dělení):
 *   smoothed = (prev * 15 + raw) >> 4
 *
 *   Ekvivalentní alpha = 1/16 = 0,0625 → pomalu sleduje,
 *   silně tlumí výkyvy. Vhodné pro levné kartonové potenciometry.
 *
 * Deadzone logika:
 *   Přepočet výstupního parametru nastane jen pokud:
 *       |smoothed[i] - last[i]| > POTS_DEADZONE
 *   Tím se zamezí „chvění" hodnot u potenciometrů s mechanickým šumem
 *   na konci otočení nebo při pomalých teplotních driftech.
 *
 * Wave type mapování (6 typů, 12-bit ADC → 682 ADC jednotek/typ):
 *
 *   ┌──────────┬────────────────────┬──────────────────────────────┐
 *   │ Raw ADC  │ WaveType           │ Zvukový charakter            │
 *   ├──────────┼────────────────────┼──────────────────────────────┤
 *   │    0–682 │ SINE               │ čistý tón, bez aliasingu     │
 *   │  683–1365│ SAW                │ ostrý, bohatý na harmoniky   │
 *   │ 1366–2047│ SQUARE (= NOISE)   │ bílý šum (LFSR v tomto enginu)│
 *   │ 2048–2730│ TRIANGLE           │ měkký, lichá harmonika       │
 *   │ 2731–3413│ BANDLIMITED_SAW    │ pilovitý bez Gibbsova efektu │
 *   │ 3414–4095│ SAMPLE             │ ukázkový perkusní sample     │
 *   └──────────┴────────────────────┴──────────────────────────────┘
 */

#include "Pots.h"

// ─────────────────────────────────────────────
//  Definice statického pole pinů
//  Pořadí musí odpovídat PotId enumu!
// ─────────────────────────────────────────────
const uint8_t Pots::_PINS[POTS_COUNT] = {
    POTS_PIN_VOLUME,
    POTS_PIN_PORTAMENTO,
    POTS_PIN_WAVETYPE,
    POTS_PIN_TEMPO
};

// ─────────────────────────────────────────────
//  Singleton
// ─────────────────────────────────────────────
Pots& Pots::getInstance() {
    static Pots instance;
    return instance;
}

Pots::Pots()
    : _smoothed{},
      _last{},
      _amplitude(POTS_ADC_MAX / 2u),
      _portamento(0.0f),
      _waveType(WaveType::SINE),
      _tempoMs(250u),
      _lastPollMs(0u),
      _initialized(false)
{}

// ─────────────────────────────────────────────
//  begin()
//
//  Nastaví 12-bit ADC a inicializuje EMA přímo na první reálné čtení.
//  Bez toho by EMA potřebovala desítky vzorků, než dojede z 0 na skutečnou
//  hodnotu → zvuk by při startu „přeběhl" přes celý rozsah.
// ─────────────────────────────────────────────
void Pots::begin() {
    // Arduino Due nativně podporuje 12-bit ADC
    analogReadResolution(POTS_ADC_BITS);

    // Inicializace EMA z reálných hodnot (ne z nuly)
    for (uint8_t i = 0u; i < POTS_COUNT; ++i) {
        const uint16_t firstRead = static_cast<uint16_t>(analogRead(_PINS[i]));
        _smoothed[i] = firstRead;
        _last[i]     = firstRead;
    }

    // Přepočítej výstupní parametry (nastaví _amplitude, _portamento atd.)
    _remap();

    _lastPollMs  = millis();
    _initialized = true;
}

// ─────────────────────────────────────────────
//  update()
//
//  Volej každou iterací loop().
//  Interní timer zajistí, že ADC se čte pouze každých POTS_POLL_MS ms.
//
//  Proč ne v každém loopu?
//    analogRead() na Due trvá ~40 µs. Čtení 4 kanálů každých 160 µs
//    v těsné smyčce zbytečně zatěžuje CPU. 20 ms interval je pro uši
//    naprosto dostatečný (> 50 Hz odezva).
//
//  Vrátí true, pokud se alespoň jeden parametr změnil (vhodné pro debug).
// ─────────────────────────────────────────────
bool Pots::update() {
    if (!_initialized) return false;

    const uint32_t now = millis();
    if (now - _lastPollMs < POTS_POLL_MS) return false;
    _lastPollMs = now;

    // EMA update pro každý potenciometr
    for (uint8_t i = 0u; i < POTS_COUNT; ++i) {
        const uint16_t raw = static_cast<uint16_t>(analogRead(_PINS[i]));
        _smoothed[i] = _ema(_smoothed[i], raw);
    }

    // Přepočítej parametry a vrať, zda se něco změnilo
    return (_remap() != 0u);
}

// ─────────────────────────────────────────────
//  _remap() — přepočet EMA hodnot na výstupní parametry
//
//  Pro každý potenciometr:
//    1. Zkontroluj deadzone — pokud se hodnota nezměnila dost, přeskoč
//    2. Namapuj na výstupní rozsah
//    3. Ulož novou referenci do _last[]
//
//  Vrátí bitmasku changed potů (bit i = PotId i se změnil).
// ─────────────────────────────────────────────
uint8_t Pots::_remap() {
    uint8_t changed = 0u;

    // ── VOLUME (A0) ───────────────────────────────────────────
    //  Lineární mapování: 0 → 0, 4095 → 4095
    //  Deadzone zabraňuje zbytečné aktualizaci při šumu blízko krajních poloh.
    {
        const uint16_t v = _smoothed[static_cast<uint8_t>(PotId::VOLUME)];
        uint16_t&      l = _last[static_cast<uint8_t>(PotId::VOLUME)];

        const int32_t diff = static_cast<int32_t>(v) - static_cast<int32_t>(l);
        if (diff > POTS_DEADZONE || diff < -static_cast<int32_t>(POTS_DEADZONE)) {
            _amplitude = v;   // přímo 0–4095
            l = v;
            changed |= (1u << static_cast<uint8_t>(PotId::VOLUME));
        }
    }

    // ── PORTAMENTO (A1) ───────────────────────────────────────
    //  Lineární mapování: [DEADBAND..4095] → [0..POTS_PORTA_MAX_MS]
    //  Spodní deadband (~6 %) zaručuje, že lze portamento zcela vypnout
    //  i přesto, že levná potenciometry nikdy nedosáhnou přesně nuly.
    {
        const uint16_t v = _smoothed[static_cast<uint8_t>(PotId::PORTAMENTO)];
        uint16_t&      l = _last[static_cast<uint8_t>(PotId::PORTAMENTO)];

        const int32_t diff = static_cast<int32_t>(v) - static_cast<int32_t>(l);
        if (diff > POTS_DEADZONE || diff < -static_cast<int32_t>(POTS_DEADZONE)) {
            if (v < POTS_PORTA_DEADBAND) {
                _portamento = 0.0f;
            } else {
                // Lineární přepočet z [DEADBAND, 4095] → [0, PORTA_MAX_MS]
                const float range = static_cast<float>(POTS_ADC_MAX - POTS_PORTA_DEADBAND);
                _portamento = static_cast<float>(v - POTS_PORTA_DEADBAND)
                              / range * POTS_PORTA_MAX_MS;
            }
            l = v;
            changed |= (1u << static_cast<uint8_t>(PotId::PORTAMENTO));
        }
    }

    // ── WAVETYPE (A2) ─────────────────────────────────────────
    //  Celý rozsah 0–4095 je rovnoměrně rozdělen na COUNT zón.
    //  Hystereze: přepnutí nastane jen pokud se raw EMA změní o >DEADZONE.
    //
    //  Výpočet: index = raw * COUNT / (ADC_MAX + 1)
    //  Dělení (ADC_MAX+1) = 4096 = 2^12 → lze nahradit shiftem >> 12:
    //       index = (raw * 6) >> 12
    //  Pro COUNT = 6: zóny po 682 ADC krocích (682 ≈ 4096/6).
    {
        const uint16_t v = _smoothed[static_cast<uint8_t>(PotId::WAVETYPE)];
        uint16_t&      l = _last[static_cast<uint8_t>(PotId::WAVETYPE)];

        const int32_t diff = static_cast<int32_t>(v) - static_cast<int32_t>(l);
        if (diff > POTS_DEADZONE || diff < -static_cast<int32_t>(POTS_DEADZONE)) {
            // uint32_t aby násobení nepřeteklo: 4095 * 6 = 24570 < 65535 (OK i u16)
            const uint8_t zone = static_cast<uint8_t>(
                (static_cast<uint32_t>(v) * static_cast<uint32_t>(WaveType::COUNT))
                >> POTS_ADC_BITS   // >> 12 = / 4096
            );
            // Clamp — pro případ raw == 4095: 4095*6>>12 = 5, ale ověřme
            _waveType = static_cast<WaveType>(
                (zone < static_cast<uint8_t>(WaveType::COUNT)) ? zone
                    : static_cast<uint8_t>(WaveType::COUNT) - 1u
            );
            l = v;
            changed |= (1u << static_cast<uint8_t>(PotId::WAVETYPE));
        }
    }

    // ── TEMPO (A3) ────────────────────────────────────────────
    //  Invertované mapování: pot CW (4095) = rychlé (TEMPO_MIN_MS)
    //                        pot CCW (0)   = pomalé (TEMPO_MAX_MS)
    //
    //  tempoMs = MAX - raw * (MAX - MIN) / 4095
    //          = MAX - raw * RANGE / 4095
    //
    //  Integer: tempoMs = MAX - (raw * RANGE) / POTS_ADC_MAX
    {
        const uint16_t v = _smoothed[static_cast<uint8_t>(PotId::TEMPO)];
        uint16_t&      l = _last[static_cast<uint8_t>(PotId::TEMPO)];

        const int32_t diff = static_cast<int32_t>(v) - static_cast<int32_t>(l);
        if (diff > POTS_DEADZONE || diff < -static_cast<int32_t>(POTS_DEADZONE)) {
            constexpr uint32_t RANGE = POTS_TEMPO_MAX_MS - POTS_TEMPO_MIN_MS;
            _tempoMs = POTS_TEMPO_MAX_MS
                       - static_cast<uint32_t>(v) * RANGE / POTS_ADC_MAX;
            l = v;
            changed |= (1u << static_cast<uint8_t>(PotId::TEMPO));
        }
    }

    return changed;
}
