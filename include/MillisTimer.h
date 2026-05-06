#pragma once
#include <Arduino.h>

/*
 * MillisTimer — neblokovací časovač nad millis()
 *
 * Dva režimy:
 *   autoReset = false  →  expired() vrací true jen jednou; musíš volat reset()
 *   autoReset = true   →  expired() posune referenci o přesně _interval ms
 *                          → žádná akumulace driftu i při vynechání jednoho cyklu
 *
 * Příklad:
 *   MillisTimer ledBlink(500, true);   // bliká přesně každých 500 ms
 *   if (ledBlink.expired()) toggleLed();
 *
 *   MillisTimer oneShot(2000);          // spustí se jednou po 2 s
 *   if (oneShot.expired()) { doThing(); oneShot.reset(); }
 *
 * Bezpečné přes přetečení millis() (uint32 wrap ≈ po 49 dnech):
 *   (now - _last) je korektní i po přetečení díky unsigned aritmetice.
 */
class MillisTimer {
public:
    // interval    — perioda v milisekundách
    // autoReset   — automaticky posunout referenci po vypršení (bez driftu)
    explicit MillisTimer(uint32_t intervalMs, bool autoReset = false)
        : _interval(intervalMs), _last(millis()), _autoReset(autoReset) {}

    // Vrátí true, pokud uplynul interval.
    // V režimu autoReset: posune referenci o přesně _interval (ne na now),
    // takže celková perioda zůstane přesná i při opožděném poll.
    bool expired() {
        const uint32_t now = millis();
        if (now - _last >= _interval) {
            if (_autoReset) {
                _last += _interval;   // posun přesně o interval, ne na now → bez driftu
            }
            return true;
        }
        return false;
    }

    // Ruční reset — posune referenci na aktuální čas.
    // Používej v manuálním režimu (autoReset = false).
    void reset() { _last = millis(); }

    // Zbývající ms do příštího vypršení (0 pokud už vypršel)
    uint32_t remaining() const {
        const uint32_t elapsed = millis() - _last;
        return (elapsed >= _interval) ? 0u : (_interval - elapsed);
    }

    // Dynamická změna intervalu (platí od příštího cyklu)
    void setInterval(uint32_t ms) { _interval = ms; }

    // Přečíst aktuální interval
    uint32_t interval() const { return _interval; }

private:
    uint32_t _interval;
    uint32_t _last;
    bool     _autoReset;
};
