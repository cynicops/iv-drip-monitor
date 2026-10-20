# Hardware guide

## Bill of materials

| # | Part | Qty | Approx. cost (INR) | Notes |
|---|---|---|---|---|
| 1 | ESP32 DevKit V1 (ESP32-WROOM-32, 30 or 38 pin) | 1 | 350–450 | any board that shows up as "ESP32 Dev Module" |
| 2 | IR LED, 940 nm, 5 mm | 1 | 5 | transmitter |
| 3 | IR photodiode, 940 nm, 5 mm (black/dark lens) | 1 | 10 | receiver; a dark lens filters visible light |
| 4 | 0.96" OLED, SSD1306, I2C, 128×64 | 1 | 200–250 | 4-pin (VCC, GND, SCL, SDA), address 0x3C |
| 5 | Active buzzer module, 3-pin (VCC, I/O, GND) | 1 | 30 | beeps when I/O is HIGH (set `BUZZER_ACTIVE_HIGH = false` for low-trigger modules) |
| 6 | Tactile push button, 6×6 mm | 3 | 10 | UP, DOWN, MUTE |
| 7 | Resistor 100 Ω | 1 | 1 | IR LED current limit |
| 8 | Resistor 100 kΩ | 1 | 1 | photodiode load |
| 9 | Breadboard, 830 point | 2 | 120 | joined side by side |
| 10 | Jumper wires (M-M, M-F) | ~25 | 60 | |
| 11 | Opaque sensor clip (3D-printed, or black card / insulation tape) | 1 | — | holds LED and photodiode either side of the drip chamber |
| 12 | Micro-USB cable / 5 V power bank | 1 | — | power and programming |

Total: roughly ₹800–1000.

## Pin map

| ESP32 pin | Connected to | Direction | Why this pin |
|---|---|---|---|
| GPIO25 | IR LED (through 100 Ω) | output | any output pin works |
| GPIO34 | photodiode anode + 100 kΩ to GND | analog input | **ADC1** channel: ADC2 pins stop working when WiFi is on; GPIO34 is input-only, which is fine |
| GPIO21 | OLED SDA | I2C | ESP32 default I2C pins |
| GPIO22 | OLED SCL | I2C | |
| GPIO26 | buzzer I/O | output | |
| GPIO32 | UP button | input, internal pull-up | |
| GPIO33 | DOWN button | input, internal pull-up | |
| GPIO27 | MUTE button | input, internal pull-up | |
| 3V3 | photodiode cathode, OLED VCC, buzzer VCC | power | |
| GND | LED cathode, 100 kΩ, OLED, buzzer, buttons | ground | |

## Schematic

```
                3V3
                 │
                 ┴  photodiode (reverse biased)
                 ▲  cathode (short leg) up, anode (long leg) down
                 │
  GPIO34 ────────┤
                 │
                ┌┴┐
                │ │ 100 kΩ
                └┬┘
                 │
                GND

  GPIO25 ──[100 Ω]──►|── GND        IR LED: anode (long leg) to resistor
                                    ~20 mA pulses, ~12 % duty

  GPIO21 ── SDA ┐
  GPIO22 ── SCL ├─ OLED SSD1306 (VCC 3V3, GND)
                ┘
  GPIO26 ── I/O ── buzzer module (VCC 3V3, GND)

  GPIO32 ──o/ o── GND   UP
  GPIO33 ──o/ o── GND   DOWN
  GPIO27 ──o/ o── GND   MUTE
```

### Why it is wired like this

- **Photodiode, reverse biased.** In reverse bias the photodiode passes a current proportional to the light falling on it. That current flows through the 100 kΩ load and gives a voltage on GPIO34 of *V = I<sub>photo</sub> × 100 kΩ*. More light means a higher voltage. A drop crossing the beam scatters part of the IR, the voltage dips for a few milliseconds, and the firmware sees that dip. A larger resistor gives a bigger signal but saturates sooner in bright rooms. 100 kΩ is a good starting point. Try 47 kΩ if the signal sits near the top of the range, or 220 kΩ if the dips are too small.
- **IR LED at 100 Ω.** With (3.3 V − 1.2 V) / 100 Ω ≈ 21 mA, the LED runs within the ESP32's recommended pin current. The LED is only on for 150 µs out of every ~1.3 ms, so it stays cool.
- **ADC range.** The firmware sets 11 dB attenuation, so GPIO34 reads 0 to about 3.1 V as 0–4095.

## Building it on two breadboards

1. Put the ESP32 across the centre gap of breadboard 1. It is wide, so one row of pins may hang over the edge. That is fine.
2. Run 3V3 and GND from the ESP32 to the power rails of **both** breadboards (join the rails with jumpers).
3. Breadboard 2 holds the OLED, the buzzer and the three buttons. Keep the buttons in a row so the order is UP, DOWN, MUTE.
4. Take the IR LED and photodiode off the board on ~20 cm of twisted wire to the sensor clip.

## Mounting on the drip chamber

```
            ┌──────── drip chamber ────────┐
            │            ▼ drop            │
  IR LED ►  │      ─────  ●  ─────►        │  ◄ photodiode
            │                              │
            └──────────────────────────────┘
               beam crosses just below the drip orifice
```

- Place the LED and photodiode **directly facing each other** across the chamber, about 1 cm below the tip the drops fall from. Drops are still well formed there.
- Keep the beam **above the fluid level** in the chamber.
- Cover the clip with black tape or put it in an opaque housing. Most false counts come from room light that reaches the photodiode directly.
- Hang the bag so the chamber is **vertical**. If it is tilted, drops run down the wall and miss the beam.
- After mounting, use **calibration mode** ([SETUP.md](SETUP.md#5-calibrate-the-sensor)) to check that every drop gives a clear dip.

## Power

The board runs from USB (laptop or a 5 V power bank). Average current is about 100–150 mA with WiFi on, so a 10 000 mAh power bank lasts well over a day.

## Safety notes

- Nothing in the circuit touches the fluid. It only looks through the plastic chamber.
- Do not modify, pierce or heat the IV set.
- This is a teaching prototype. Do not rely on it for a real patient.
