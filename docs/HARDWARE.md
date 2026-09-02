# Hardware

Wiring, schematic walkthrough and module setup for the meter.

---

## Circuit

![Circuit diagram](assets/circuit_diagram.jpg)

The board is a single-sided design built around the STM32F401CEU6 "Black Pill"
module. Four functional groups hang off it: the voltage divider feeding the
on-chip ADC, the INA226 current monitor on I²C, the character LCD sharing that
same I²C bus, and the HC-05 Bluetooth module on USART1. Everything runs from a
single 3.3 V rail except the HC-05, which is powered at 5 V with a divided
receive line (see below).

---

## Pin map

| Signal | Pin | Peripheral | Configuration |
|---|---|---|---|
| Voltage input (divided) | `PB1` | ADC1 channel 9 | Analog, no pull |
| I²C clock | `PB10` | I2C2 SCL | Alternate function 4, open-drain |
| I²C data | `PB3` | I2C2 SDA | Alternate function 9, open-drain |
| Bluetooth TX (to HC-05 RX) | `PA9` | USART1 TX | Alternate function 7, push-pull |
| Bluetooth RX (from HC-05 TX) | `PA10` | USART1 RX | Alternate function 7, push-pull |
| Voltage mode button | `PB6` | GPIO input | Pull-down, active HIGH |
| Milliamp mode button | `PB7` | GPIO input | Pull-down, active HIGH |
| Microamp mode button | `PB8` | GPIO input | Pull-down, active HIGH |
| Bluetooth enable switch | `PC13` | GPIO input | Pull-up, active LOW (switch to GND) |

Both I²C devices — the INA226 at `0x44` and the LCD backpack at `0x27` — share
the single I2C2 bus at 100 kHz. The addresses do not collide, so no bus
multiplexing is needed. Standard 4.7 kΩ pull-ups to 3.3 V are required on SDA and
SCL; most PCF8574 LCD backpacks already carry their own, in which case a single
pair for the whole bus is sufficient.

---

## Voltage front end

The divider scales the input down into the ADC's range:

| Component | Value | Position |
|---|---|---|
| R1 | 9906 Ω | Low side (ADC node to GND) |
| R2 | 39010 Ω | High side (input to ADC node) |

```
        Vin ──[ R2 = 39010 Ω ]──┬──> PB1 (ADC1_IN9)
                                │
                          [ R1 = 9906 Ω ]
                                │
                               GND
```

The division ratio is `R1 / (R1 + R2) = 9906 / 48916 ≈ 0.2025`, so a 16 V input
presents about 3.24 V at the pin — just below the 3.313 V reference the firmware
assumes. That is deliberately tight: it uses nearly the full ADC span for
resolution, while leaving a little headroom before clipping.

The values above are the ones the firmware's reconstruction maths actually uses.
If you populate different resistors, measure them and update the constants in
`main.c` — the displayed voltage scales directly with them, and a 1 % error in
the divider is a 1 % error in every reading regardless of how many digits the
display shows.

**Input protection.** The divider itself limits current into the pin, but there
is no clamp diode or series protection on this board. Keep the input at or below
16 V; the meter is a bench instrument for low-voltage DC work, not a mains-rated
multimeter.

---

## Current front end

The INA226 sits high-side across a **0.1 Ω** sense resistor, with its address
pins strapped to give `0x44` (A1 to VS+, A0 to GND).

```
    Supply ──┬──[ 0.1 Ω shunt ]──┬── Load
             │                   │
            IN+                 IN−
             └──── INA226 ───────┘
                  │      │
                 SDA    SCL   ──> PB3 / PB10
```

The device measures the differential drop across the shunt with a fixed 2.5 µV
LSB, which gives 25 µA per bit through a 0.1 Ω resistor. Its calibration register
is programmed to 2048 so the on-chip current register carries that scale
directly. Configuration register `0x4527` selects a 16-sample hardware average
with 1.1 ms bus and shunt conversion times in continuous mode — averaging happens
inside the sensor, before the STM32 reads anything.

Points worth knowing when building or modifying this stage:

- **The shunt sets the resolution.** 0.1 Ω gives 25 µA steps. Going to 2.5 Ω
  would give 1 µA steps but multiplies the burden voltage by 25, which perturbs
  the circuit being measured. This design deliberately favours low burden.
- **Full scale is about 819 mA** (`32767 × 25 µA`), well inside the 81.92 mV
  differential limit of a 0.1 Ω shunt.
- **Sense resistor quality matters more than the ADC.** A 5 % shunt makes the
  µA display decorative. Use a low-tempco precision resistor and keep it away
  from heat sources.
- **Kelvin the connections.** Take the IN+/IN− taps at the resistor body, not
  from shared trace runs, or the trace resistance ends up in the measurement.

If the INA226 does not acknowledge on the bus at power-up, the firmware halts
with `INA226 ERROR! / Check wiring` on the LCD and sends the same message over
Bluetooth rather than reporting fabricated readings.

---

## Display

A standard HD44780 16×2 character LCD driven through a PCF8574 I²C backpack at
address `0x27`, in 4-bit mode. The backpack's contrast trimmer must be set
before anything legible appears — a blank-but-backlit panel almost always means
contrast, not wiring.

The firmware caches the digits it last drew and rewrites a field only when its
displayed value actually changes, so a stable reading generates no I²C traffic at
all. This matters because the LCD and the INA226 share the bus: a display that
redrew continuously would compete with sensor reads for bus time.

---

## Bluetooth

An **HC-05** module provides the wireless link, connected to USART1 at
**9600 baud, 8 data bits, no parity, 1 stop bit**.

### Wiring

| HC-05 pin | Connects to | Note |
|---|---|---|
| `VCC` | 5 V | The module's regulator expects 5 V |
| `GND` | GND | Common ground with the STM32 |
| `RXD` | `PA9` (STM32 TX) | Direct — see the level note below |
| `TXD` | `PA10` (STM32 RX) | Direct; the module idles at 3.3 V |

A word on logic levels, because this trips people up: the HC-05 board takes 5 V
on `VCC` for its regulator, but its `RXD` input is **3.3 V logic and is not 5 V
tolerant**. The STM32 already drives `PA9` at 3.3 V, so it can be wired straight
to `RXD` with no divider. Coming back the other way, the module's `TXD` also
idles at 3.3 V, which the STM32 reads correctly.

The divider you see in many HC-05 tutorials exists because those projects drive
the module from a 5 V board. If you ever swap this STM32 for a 5 V
microcontroller, add a divider (typically 1 kΩ / 2 kΩ) on the line feeding
`RXD`.

### Enable switch

`PC13` carries a simple slide switch to ground with the STM32's internal pull-up
enabled — no external resistor required:

- **Switch closed** → `PC13` reads LOW → telemetry enabled
- **Switch open** → `PC13` reads HIGH → telemetry stopped

The firmware samples this as a live level every loop rather than latching an
edge, so the stream responds immediately when the switch is thrown. While the
switch is open the five-second interval timer is held reset, which means turning
it on always produces a full five-second wait before the first frame instead of
firing a stale queued reading the instant it closes.

### Frame format

Frames are plain ASCII lines terminated with CRLF, so any serial terminal or
Bluetooth terminal app can read them without a decoder. The content follows the
currently selected display mode:

```
== V&I Meter Ready ==        sent once at startup
Vo: 4.9987 V                 voltage mode
I: 12.500 mA                 milliamp mode
I: 37.50 uA                  microamp mode
I: OVERLOAD >99uA            microamp mode, reading above range
INA226 INIT FAILED           sensor not detected at boot
```

### Pairing

The module presents itself as a standard SPP serial device. Pair with the
factory PIN (`1234` or `0000` on most HC-05 units), then open the resulting
serial port at 9600 baud. Changing the device name, PIN or baud rate is done
through the module's AT command mode — hold the module's `KEY`/`EN` pin high
while powering up, which brings it up at 38400 baud for configuration — and is
independent of this firmware.

---

## Power

The STM32 module, INA226 and LCD backpack all run from 3.3 V; the HC-05 takes
5 V. When running from USB, the Black Pill's on-board regulator supplies the
3.3 V rail and 5 V is available from the USB input pin.

Keep the measurement ground and the logic ground joined at a single point near
the shunt. Sharing a long ground return between the LCD backlight current and the
INA226 sense path is the most common source of unstable readings in a build like
this.
