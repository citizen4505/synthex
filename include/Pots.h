/*
 * Pots.h — čtení potenciometrů pro Synthex
 *
 * Čtyři potenciometry mapované na parametry syntezátoru:
 *
 *   A0  VOLUME      → amplitude    0 – 4095  (lineární)
 *   A1  PORTAMENTO  → glide čas    0 – 500 ms (spodní ~6 % = vypnuto)
 *   A2  WAVETYPE    → typ vlny     SINE / SAW / SQUARE(noise) /
 *                                   TRIANGLE / BANDLIMITED_SAW / SAMPLE
 *   A3  TEMPO       → krok sekvenceru  500 – 80 ms  (CW = rychleji)
 *
 * Techniky šumové filtrace:
 *   EMA (Exponential Moving Average)
 *       smoothed = (smoothed * 15 + raw) >> 4
 *       Časová konstanta ≈ 15 vzorků × 20 ms = 300 ms — vyhlazuje ADC šum
 *       bez znatelného zpomalení odezvy.
 *
 *   Deadzone
 *       Parametr se přepočítá jen pokud se EMA hodnota změnila o více
 *       než POTS_DEADZONE. Zabraňuje neustálému přepisování engine parametrů
 *       kvůli jednobitovým fluktuacím na 12-bit ADC.
 *
 * Použití:
 *   // setup()
 *   Pots::getInstance().begin();
 *
 *   // loop()
 *   Pots& pots = Pots::getInstance();
 *   pots.update();                          // čtení ADC každých 20 ms
 *   engine.noteOn(0, freq, pots.amplitude(), pots.waveType());
 *   engine.setPortamento(pots.portamento());
 *   eventTimer.setInterval(pots.tempoMs());
 *
 * Hardware:
 *   Arduino Due, 12-bit ADC (0 – 4095)
 *   Potenciometry: 10 kΩ lin., napájení 3,3 V (Due není 5 V tolerantní!)
 */

#pragma once
#include <Arduino.h>
#include "wavetables.h"   // WaveType enum

// ─────────────────────────────────────────────
//  Konfigurace — uprav podle zapojení
// ─────────────────────────────────────────────

// Piny analogových vstupů (Arduino Due: A0–A11)
#define POTS_PIN_VOLUME      A0
#define POTS_PIN_PORTAMENTO  A1
#define POTS_PIN_WAVETYPE    A2
#define POTS_PIN_TEMPO       A3

// Počet potenciometrů
#define POTS_COUNT           4u

// ADC rozlišení (Arduino Due podporuje 12 bit)
#define POTS_ADC_BITS        12u
#define POTS_ADC_MAX         4095u    // 2^12 - 1

// EMA vyhlazování: nová hodnota má váhu 1/16, minulá 15/16
// Vyšší číslo = pomalejší, ale hladší odezva.
// Doporučený rozsah: 8 (rychlé) – 32 (velmi pomalé)
#define POTS_EMA_SHIFT       4u       // >> 4 znamená dělení 16

// Deadzone: minimální změna EMA hodnoty pro přepočet parametru
// Při 12-bit ADC a EMA 1/16: ~16 odpovídá ≈ 0,4 % rozsahu
#define POTS_DEADZONE        16u

// Interval pollingu ADC [ms]
#define POTS_POLL_MS         20u

// Portamento: pod tímto prahem (raw EMA) se nastaví 0 ms (vypnuto)
// ~6 % z 4095 ≈ 250 → pohodlné místo pro „úplné vypnutí"
#define POTS_PORTA_DEADBAND  250u

// Rozsah portamenta [ms]
#define POTS_PORTA_MAX_MS    500.0f

// Rozsah tempa sekvenceru [ms/krok]
#define POTS_TEMPO_MIN_MS    80u
#define POTS_TEMPO_MAX_MS    500u


// ─────────────────────────────────────────────
//  Enum pro indexování potenciometrů
// ─────────────────────────────────────────────
enum class PotId : uint8_t {
    VOLUME     = 0,
    PORTAMENTO = 1,
    WAVETYPE   = 2,
    TEMPO      = 3
};


// ─────────────────────────────────────────────
//  Třída Pots
// ─────────────────────────────────────────────
class Pots {
public:
    // Singleton — stejný vzor jako Synthex
    static Pots& getInstance();

    // Inicializace: nastaví 12-bit ADC a provede první čtení
    // (inicializuje EMA přímo na aktuální hodnotu → žádný studený start od 0)
    void begin();

    // Volej každou iterací loop() — interně polluje každých POTS_POLL_MS ms
    // Vrátí true, pokud se alespoň jeden parametr změnil (vhodné pro debug výpis)
    bool update();

    // ─────────────────────────────────────────────
    //  Namapované hodnoty — volej po update()
    // ─────────────────────────────────────────────

    // Amplituda pro noteOn(): 0 – 4095
    uint16_t amplitude()  const { return _amplitude; }

    // Čas portamenta pro setPortamento(): 0.0 – 500.0 ms
    // Vrátí 0.0 pokud je potenciometr v deadband oblasti
    float    portamento() const { return _portamento; }

    // Typ vlny pro noteOn()
    WaveType waveType()   const { return _waveType; }

    // Interval kroku sekvenceru [ms]: POTS_TEMPO_MAX_MS – POTS_TEMPO_MIN_MS
    // (CW = kratší interval = rychlejší tempo)
    uint32_t tempoMs()    const { return _tempoMs; }

    // Přístup k surové (EMA) hodnotě pro ladění / vlastní mapování
    // idx odpovídá PotId enumu
    uint16_t raw(PotId id) const {
        return _smoothed[static_cast<uint8_t>(id)];
    }

private:
    Pots();
    Pots(const Pots&)            = delete;
    Pots& operator=(const Pots&) = delete;

    // Pomocná funkce: přepočítá _smoothed[] → výstupní parametry
    // Vrátí bitmasku changed potenciometrů (bit 0 = VOLUME, ...)
    uint8_t _remap();

    // EMA krok pro jeden potenciometr
    // Využívá unsigned aritmetiku s bit-shiftem → vhodné pro ISR/fast loop
    static inline uint16_t _ema(uint16_t prev, uint16_t raw) {
        // prev*15/16 + raw/16  (bez přetečení uint16 pro 12-bit hodnoty)
        return static_cast<uint16_t>(
            (static_cast<uint32_t>(prev) * ((1u << POTS_EMA_SHIFT) - 1u)
             + raw) >> POTS_EMA_SHIFT
        );
    }

    // ── Stav ─────────────────────────────────
    uint16_t _smoothed[POTS_COUNT];   // EMA hodnoty (12-bit)
    uint16_t _last[POTS_COUNT];       // poslední hodnota při remapu

    // Namapované výstupy (cache — platné po update())
    uint16_t _amplitude;
    float    _portamento;
    WaveType _waveType;
    uint32_t _tempoMs;

    uint32_t _lastPollMs;
    bool     _initialized;

    // Pevné pole pinů — indexováno přes PotId
    static const uint8_t _PINS[POTS_COUNT];
};
