# Contributors

Six people designed, built and tested this instrument.

The work did not divide cleanly. Most of the firmware was written, read and
debugged collectively; the board was populated by several hands; and the
documentation was a shared effort. What follows records where each person led
and what they contributed — not an exclusive claim on any one part.

---

## How the work was shared

**Firmware** was worked on by Yaneth De Alwis, Uchitha Induranga, Vidumini
Sawandi, Yahani Jayawardena and Yohara Perera. That covered the application
layer — the main loop, the three-mode state machine, debounced button handling
and the cached LCD redraw — as well as both measurement paths and the wireless
link.

**Soldering and board population** — fitting the sense resistor, the INA226
module, the display and the panel controls — was done by Thamindu Nisal,
Yaneth De Alwis and Uchitha Induranga.

**Testing and calibration** of the finished instrument involved the whole team.

---

## Where each person led

### Uchitha Induranga
PCB design and single-sided layout, including placement and routing of the
analogue sense path; etching and drilling the board; and the move from the
earlier Vero-board prototype to the finished PCB. Built the enclosure and its
panel layout — cut-outs and mounting for the LCD, the three mode buttons and the
Bluetooth enable switch. Shared the soldering work, and contributed to the
firmware including the INA226 current-measurement path.

### Thamindu Nisal
Soldering and board population, the wiring harness and probe leads, and the full
assembly of the populated board into the enclosure. Led hardware bring-up on the
finished unit: continuity and isolation checks, first power-on, I²C address
verification, and fault-finding across the sense path and the display bus — the
work that turns a populated board into a working instrument.

### Yaneth De Alwis
Soldering and board population, and firmware work centred on the current
measurement path — the `INA226_STM32` driver, its device configuration
(`0x4527`: 16-sample hardware averaging, continuous mode) and the calibration
register value of 2048 that fixes the current LSB at 25 µA.

That figure follows from the hardware: a 2.5 µV shunt LSB across a 0.1 Ω sense
resistor gives 25 µA per raw bit, with full scale near 819 mA before the signed
16-bit register saturates. Also the mA and µA display scaling and the overload
threshold above 99 µA, and the resolution trade-off set out in
`docs/HARDWARE.md` — finer steps would need a larger shunt, at a direct cost in
burden voltage on the circuit being measured.

### Vidumini Sawandi
Firmware across the application layer, and — with Yahani Jayawardena — the whole
wireless side of the instrument. That meant bringing up USART1 at 9600 baud 8N1
for the HC-05, reading the `PC13` enable switch as a live level so streaming
starts and stops the moment the switch moves, and holding the interval timer in
reset while the switch is open so that switching it back on never releases a
stale reading.

Defined the ASCII telemetry format carried over the link — one CRLF-terminated
line per reading, distinct for voltage, milliamp and microamp modes, with
explicit overload and sensor-fault messages so a receiving device is never shown
a number the instrument cannot stand behind. Co-wrote the companion Android
application that pairs over SPP, parses those frames and displays live readings.
Project documentation.

### Yahani Jayawardena
Firmware across the application layer, and — with Vidumini Sawandi — the
wireless path: the HC-05 integration on USART1, the switch-gated streaming on
`PC13`, the five-second send cadence, and the per-mode frame formats described
above.

Co-wrote the companion Android application — SPP pairing, serial parsing and the
on-screen presentation of live readings — which is what makes the meter usable
away from a laptop and a terminal window, and which turned the project from a
bench instrument into something you can leave in a circuit and watch from your
phone. Project documentation.

### Yohara Perera
Firmware, centred on the voltage acquisition chain — the half of the instrument
that does not have a dedicated sensor doing the hard work for it, and therefore
the half where the accuracy has to be earned in software.

Implemented 16-sample oversampling of the ADC, averaging uncorrelated noise down
by √N and turning a last digit that flickers into one that sits still. Wrote the
power-on calibration routine that prompts `OPEN PROBES`, discards 32 warm-up
conversions and then averages 512 samples to characterise the ADC's zero-input
offset, which is subtracted from every subsequent reading. That result is held
in a `.noinit` section behind a `0xA55A5AA5` magic value so it survives a reset
without re-running — the meter only re-calibrates on a genuine cold start — and
guarded by a sanity ceiling that rejects an implausible offset rather than
letting it quietly corrupt every later measurement. Added the four-count
dead-band that makes an idle meter read exactly `0.0000 V` instead of drifting,
and the divider reconstruction maths that recovers the input voltage from the
measured node.

Also produced the drawings that document the system end to end: the block
diagram showing the two measurement front ends, the shared I²C bus carrying both
the sensor and the display, and the wireless path out to the phone, together
with the circuit diagram reproduced in `docs/HARDWARE.md`. Those drawings were
not decoration — they were the reference the team worked against when
reconciling firmware with the board as actually built, and mismatches between
code constants and physical hardware were caught against them. Project
documentation.

---

## Companion Android application

The Bluetooth link is paired with an Android application written alongside the
firmware, which receives the ASCII telemetry frames described in
`docs/HARDWARE.md`. That application is maintained separately from this
repository.
