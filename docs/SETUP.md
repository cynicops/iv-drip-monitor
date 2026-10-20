# Setup guide

From an empty laptop to a working monitor in about 30 minutes.

## 1. Install the software

1. Install **Arduino IDE 2.x** from <https://www.arduino.cc/en/software>.
2. **ESP32 boards:** go to *File → Preferences → Additional boards manager URLs* and add
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
   Then open *Tools → Board → Boards Manager*, search **esp32** and install **esp32 by Espressif Systems** (CI tests with **3.3.12**; any 3.x should work).
3. **Libraries:** open *Tools → Manage Libraries* and install:

   | Library | Tested version |
   |---|---|
   | Adafruit SSD1306 | 2.5.17 |
   | Adafruit GFX Library | 1.13.0 (installed with SSD1306) |
   | UniversalTelegramBot (Brian Lough) | 1.3.0 |
   | ArduinoJson (Benoit Blanchon) | 7.4.3 |

   ArduinoJson 7 prints some "deprecated" warnings from inside UniversalTelegramBot while compiling. They are harmless.
4. **USB driver:** if no COM port appears when you plug the ESP32 in, install the driver for its USB chip (CP210x or CH340, printed on the chip next to the USB socket).

With **arduino-cli** instead:

```bash
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.12
arduino-cli lib install "Adafruit SSD1306@2.5.17" "Adafruit GFX Library@1.13.0" "UniversalTelegramBot@1.3.0" "ArduinoJson@7.4.3"
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/IV_Drip_Monitor
arduino-cli upload  --fqbn esp32:esp32:esp32 -p COM5 firmware/IV_Drip_Monitor   # your port
```

## 2. Create the Telegram bot

1. In Telegram, open **@BotFather** and send `/newbot`. Choose a name and a username ending in `bot`.
2. BotFather replies with a **token** like `1234567890:AAH...`. That is your `BOT_TOKEN`. Keep it secret.
3. Open your new bot and send it any message (e.g. `hi`). A bot can't message you until you've messaged it first.
4. Find your **chat ID**. Either:
   - message **@userinfobot**, which replies with your numeric ID, or
   - open `https://api.telegram.org/bot<YOUR_TOKEN>/getUpdates` in a browser and read `"chat":{"id": ... }`.
5. **To alert the whole team:** create a group, add the bot, send a message in the group and read the group's chat ID from `getUpdates`. Group IDs are negative, e.g. `-1001234567890`. Use that as `CHAT_ID`.

## 3. Fill in your settings

Open `firmware/IV_Drip_Monitor/IV_Drip_Monitor.ino` and edit section **1. YOUR SETTINGS**:

```cpp
const char* WIFI_SSID = "YOUR_WIFI_NAME";     // 2.4 GHz network or phone hotspot
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
#define BOT_TOKEN "1234567890:REPLACE_WITH_YOUR_BOT_TOKEN"
#define CHAT_ID   "123456789"
```

- The ESP32 only connects to **2.4 GHz** WiFi. A phone hotspot works well for demos. On iPhones, turn on *Maximise Compatibility*.
- **Do not commit these real values to GitHub.** Make the edits in your local copy only. If you ever push a token by mistake, send `/revoke` to BotFather to get a new one.

Section **2. IV / ALARM SETTINGS** holds the defaults used after power-up. You can also change them at runtime with the buttons or Telegram.

| Setting | Default | Meaning |
|---|---|---|
| `DROP_FACTOR` | 20 | drops per mL of your IV set (printed on the pack) |
| `targetRate` | 100 | prescribed rate, mL/hr |
| `bagVolume` | 500 | bag size, mL |
| `TOLERANCE` | 0.20 | alarm when more than ±20 % off target… |
| `DEVIATION_ALARM_MS` | 30 000 | …for longer than 30 s |
| `STOP_MIN_MS` | 20 000 | no drop for 20 s (or 3 drop intervals, whichever is longer) = stopped |
| `REMIND_MS` | 120 000 | repeat Telegram alarm every 2 min |
| `MUTE_MS` | 120 000 | mute duration |
| `BAG_LOW_FRACTION` | 0.10 | bag-low warning at 10 % left |

## 4. Upload

1. *Tools → Board → esp32 → **ESP32 Dev Module***.
2. *Tools → Port* → the ESP32's COM port.
3. Click **Upload**. If the upload stalls at `Connecting.....`, hold the **BOOT** button on the ESP32 until it starts writing.
4. Open *Tools → Serial Monitor* at **115200 baud**. You should see `WiFi OK, IP ...` and receive *"IV monitor online"* on Telegram.

## 5. Calibrate the sensor

Do this once after mounting the sensor clip, and again whenever you change the IV set.

1. Set `const bool CALIBRATION_MODE = true;` and upload.
2. Close the Serial Monitor and open *Tools → **Serial Plotter*** (115200 baud). You will see four lines:
   - `signal`: the LED-on minus LED-off reading
   - `baseline`: what the firmware thinks "no drop" looks like
   - `low` / `high`: baseline ± `DETECT_THRESHOLD` (the detection band)
3. Start the drip. **Every drop must push `signal` clearly outside the low–high band.** Dips usually go down, but some setups give upward spikes. Both count.
4. Adjust:

   | What you see | Fix |
   |---|---|
   | dips are small, often stay inside the band | move LED/photodiode closer to the drip tip, align them better, or lower `DETECT_THRESHOLD` (e.g. 40) |
   | signal is noisy and wanders out of the band without drops | shield from room light, shorten/twist sensor wires, or raise `DETECT_THRESHOLD` (e.g. 80–100) |
   | signal stuck near 0 | LED not lighting (check polarity / GPIO25) or photodiode reversed |
   | signal stuck near 4095 | saturated: use a smaller load resistor (47 kΩ) or move the LED back |
   | one drop gives two dips | increase `REFRACTORY_MS` (default 120 ms) |

   A phone camera can see IR, which helps you check that the LED is on.
5. Set `CALIBRATION_MODE = false` and upload again.

## 6. Check the counts against a stopwatch

Count the drops yourself for 60 s and compare with the drops/min on the OLED (shown as `dpm`). They should agree to within one drop. The bench-test checklist in [TESTING.md](TESTING.md#bench-test-checklist-real-hardware) lists all the checks to run before a demo.

## Troubleshooting

| Problem | Likely cause |
|---|---|
| `OLED not found - check wiring / address 0x3C` | SDA/SCL swapped, or module is 0x3D (change `0x3C` in `setup()`) |
| `WiFi failed - running offline` | wrong SSID/password, or a 5 GHz-only network; the monitor still works locally |
| WiFi OK but no Telegram messages | wrong `BOT_TOKEN` / `CHAT_ID`, or you never messaged the bot. As a last resort set `USE_INSECURE_TLS = true` |
| Buzzer beeps all the time / never | module is low-trigger: set `BUZZER_ACTIVE_HIGH = false` |
| Rate reads double / random | false triggers: see the calibration table above |
| "TOO FAST/SLOW" right after setting the clamp | the alarm only fires after 30 s outside ±20 %; wait for the reading to settle (8 drops) |
