/*
 * main.cpp — Phase 7: MIDI vstup
 *
 * Hardware zapojení (UART MIDI — DIN-5):
 * ┌────────────────────────────────────────────────────────┐
 * │  DIN-5 pin 4 ─── 220Ω ─── anoda  │                    │
 * │  DIN-5 pin 5 ─── GND              │  6N138 / H11L1     │
 * │  optočlen výstup ─────────────────── Serial1 RX (pin 19)│
 * │  optočlen Vcc ─── 3.3 V (Due)     │                    │
 * └────────────────────────────────────────────────────────┘
 *
 * Serial1 = piny 18 (TX, nevyužito) a 19 (RX) na Arduino Due.
 * Rychlost: 31250 baud (standard MIDI; Arduino Due tuto rychlost podporuje).
 *
 * ── MIDI over USB (volitelně) ────────────────────────────────────────────
 * Arduino Due má dva USB porty:
 *   • "Programming USB" (blíže k reset tlačítku) → Serial, upload
 *   • "Native USB"      (blíže k napájení)       → SerialUSB, USB MIDI device
 *
 * Pro USB MIDI přidej do platformio.ini:
 *   lib_deps = lathoub/USB-MIDI@^1.1.0
 * A odkomentuj sekci #ifdef USE_USB_MIDI níže.
 *
 * ── Co program dělá ──────────────────────────────────────────────────────
 * Čte MIDI ze Serial1, parsuje bajty přes MidiParser a předává MidiRouteru.
 * Router alokuje hlasy enginu, aplikuje pitch bend a CC.
 * Každých 5 s se vypíše tabulka aktivních hlasů (debug).
 */

#include "Synthex.h"
#include "MillisTimer.h"
#include "MidiParser.h"
#include "MidiRouter.h"

// ── Volitelně: USB MIDI (odkomentuj a nainstaluj knihovnu) ───────────────
#define USE_USB_MIDI
#ifdef USE_USB_MIDI
#include <USB-MIDI.h>
USBMIDI_CREATE_DEFAULT_INSTANCE();
#endif

// ── Engine + MIDI objekty ────────────────────────────────────────────────

Synthex&   engine = Synthex::getInstance();
MidiParser parser;
MidiRouter router(engine, WaveType::BANDLIMITED_SAW);

// ── Diagnostický timer ───────────────────────────────────────────────────

MillisTimer diagTimer(5000, true);   // výpis slotů každých 5 s

// ─────────────────────────────────────────────────────────────────────────
//  setup
// ─────────────────────────────────────────────────────────────────────────

void setup() {
    // Debug konzole (Programming USB, 115200 baud)
    Serial.begin(115200);
    while (!Serial && millis() < 3000) {}
    delay(100);

    // UART MIDI (31250 baud — standard MIDI, DIN-5 konektor)
    // Arduino Due: Serial1 = piny 18/19 s optočlenem (6N138 nebo H11L1)
    Serial1.begin(31250);

    // Synthex engine
    engine.begin();

    Serial.println("╔══════════════════════════════════════╗");
    Serial.println("║   Synthex — Fáze 7 · MIDI vstup      ║");
    Serial.println("╚══════════════════════════════════════╝");
    Serial.print("Hlasy:        "); Serial.println(SYNTHEX_VOICES);
    Serial.print("Sample rate:  "); Serial.print(SYNTHEX_SAMPLE_RATE);
    Serial.println(" Hz");
    Serial.print("Pitch bend:   ±"); Serial.print(PITCH_BEND_RANGE);
    Serial.println(" půltóny");
    Serial.println();
    Serial.println("Čekám na MIDI zprávy (Serial1, 31250 baud)…");
    Serial.println("CC 64 = Sustain  |  CC 123 = All Notes Off");
    Serial.println("────────────────────────────────────────");

#ifdef USE_USB_MIDI
    MIDI.begin(MIDI_CHANNEL_OMNI);
    Serial.println("USB MIDI: aktivní (Native USB port)");
#endif
}

// ─────────────────────────────────────────────────────────────────────────
//  loop
// ─────────────────────────────────────────────────────────────────────────

void loop() {
    MidiMsg msg;

    // ── UART MIDI (Serial1) ──────────────────────────────────────────────
    // Vyčerp celý buffer najednou — MIDI přichází v burstách (akordy).
    // Každý bajt prochází parserem; pokud je zpráva kompletní, jde do routeru.
    while (Serial1.available()) {
        if (parser.parse(Serial1.read(), msg)) {
            router.handleMsg(msg);
        }
    }

    // ── USB MIDI (volitelně) ─────────────────────────────────────────────
#ifdef USE_USB_MIDI
    // Knihovna USB-MIDI používá callbacky; alternativně polluj:
    // MIDI.read();
    // if (MIDI.getType() == midi::NoteOn) { ... }
    // Pro integraci s MidiRouter viz docs: lathoub/Arduino-USBMIDI
#endif

    // ── Periodický debug výpis ───────────────────────────────────────────
    if (diagTimer.expired()) {
        router.printSlots();
    }
}