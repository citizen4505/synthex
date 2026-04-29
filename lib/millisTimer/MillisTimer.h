/*
 * MillisTimer.h
 * Pomocné časovací nástroje založené na millis() odečtu.
 *
 * Proč NE delay()?
 *   delay() blokuje CPU — žádná ISR, žádný Serial, žádný event handling.
 *   millis()-odečet umožňuje "čekání" bez blokování smyčky.
 *
 * Proč uint32_t přetečení NEVADÍ?
 *   millis() přeteče po ~49.7 dnech (0xFFFFFFFF / 1000 / 60 / 60 / 24).
 *   Unsigned odečet wraps-around správně:
 *     start = 0xFFFFFF00, now = 0x000000FF
 *     now - start = 0x1FF = 511 ms  ✓
 *
 * Hardware: Arduino Due (SAM3X8E, 84 MHz)
 */

#pragma once
#include <Arduino.h>
#include <stdint.h>

// ─────────────────────────────────────────────────────────
//  1) Blokující delay — používej POUZE mimo ISR (init, debug)
//
//  Ekvivalent delay(), ale explicitně ukazuje mechanismus.
//  Vhodné pro: čekání na stabilizaci hardware po resetu.
// ─────────────────────────────────────────────────────────
inline void blocking_delay_ms(uint32_t ms) {
    const uint32_t start = millis();
    // millis() - start je vždy >= 0 díky unsigned wrapping
    while ((millis() - start) < ms) {
        // CPU čeká — ISR stále běží (TC3_Handler ano, ale loop ne)
        // yield() by šlo zavolat pro RTOS kompatibilitu
    }
}

// ─────────────────────────────────────────────────────────
//  2) Neblokující timer — základ event-driven architektu
//
//  Použití v loop():
//    static MillisTimer ledTimer(500);
//    if (ledTimer.expired()) {
//        toggleLed();
//        ledTimer.reset();  // nebo auto-reset přes autoReset=true
//    }
// ─────────────────────────────────────────────────────────
class MillisTimer {
public:
    // Konstruktor — interval v ms, volitelný auto-reset
    explicit MillisTimer(uint32_t intervalMs, bool autoReset = false)
        : _interval(intervalMs)
        , _startMs(millis())       // start od okamžiku vytvoření
        , _autoReset(autoReset)
    {}

    // Vrátí true, pokud uplynul interval od posledního resetu
    bool expired() {
        if ((millis() - _startMs) >= _interval) {
            if (_autoReset) {
                // Posuneme startovní bod o přesný interval
                // (ne millis()) — zabraňuje drift akumulaci
                _startMs += _interval;
            }
            return true;
        }
        return false;
    }

    // Ruční reset časovače (bez auto-reset)
    void reset() {
        _startMs = millis();
    }

    // Reset s novým intervalem
    void reset(uint32_t newIntervalMs) {
        _interval  = newIntervalMs;
        _startMs   = millis();
    }

    // Kolik ms zbývá do expirace (0 pokud již expiroval)
    uint32_t remaining() const {
        const uint32_t elapsed = millis() - _startMs;
        return (elapsed < _interval) ? (_interval - elapsed) : 0u;
    }

    // Kolik ms uplynulo od posledního resetu
    uint32_t elapsed() const {
        return millis() - _startMs;
    }

private:
    uint32_t _interval;
    uint32_t _startMs;
    bool     _autoReset;
};

// ─────────────────────────────────────────────────────────
//  3) Jednorázový Stopwatch — měření doby trvání úseku kódu
//
//  Použití:
//    Stopwatch sw;
//    sw.start();
//    doSomething();
//    Serial.println(sw.elapsedMs());
// ─────────────────────────────────────────────────────────
class Stopwatch {
public:
    Stopwatch() : _startMs(0), _running(false) {}

    void start() {
        _startMs = millis();
        _running = true;
    }

    // Vrátí ms od start() — nebo celkový čas pokud byl zastaven
    uint32_t elapsedMs() const {
        return _running ? (millis() - _startMs) : _snapshot;
    }

    // Zastaví měření, uloží snapshot
    uint32_t stop() {
        _snapshot = millis() - _startMs;
        _running  = false;
        return _snapshot;
    }

    bool isRunning() const { return _running; }

private:
    uint32_t _startMs;
    uint32_t _snapshot = 0;
    bool     _running;
};
