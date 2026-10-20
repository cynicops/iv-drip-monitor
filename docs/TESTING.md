# Testing

Three kinds of automated testing run on every push to GitHub ([CI workflow](../.github/workflows/ci.yml)), plus a bench checklist for the real hardware.

| # | What | Tool | Result (21 Oct 2026) |
|---|---|---|---|
| 1 | **Firmware build** for the real board | `arduino-cli`, `esp32:esp32:esp32` core 3.3.12, `--warnings all` | ✅ builds, no warnings in the sketch |
| 2 | **Host test suite**: the sketch, unmodified, against a simulated drip chamber | `g++` + `make -C test` | ✅ **75 / 75 passed** (73 normal + 2 calibration-mode) |
| 3 | **Mutation check**: does the suite catch real bugs? | `test/mutation_check.sh` | ✅ **12 / 12 injected bugs caught** |
| 4 | **Bench test** on the real circuit | checklist below | to be filled in by the team |

## 1. Firmware build

```
Sketch uses 1067380 bytes (81%) of program storage space. Maximum is 1310720 bytes.
Global variables use 49672 bytes (15%) of dynamic memory, leaving 278008 bytes for local variables.
```

The only compiler warnings come from inside the UniversalTelegramBot library (ArduinoJson 7 deprecations). None come from our sketch.

## 2. Host test suite

### How it works

The firmware can't run on a PC as it is: it needs an ESP32, a photodiode, WiFi and Telegram. The test suite replaces only those outside pieces and runs **the real `IV_Drip_Monitor.ino` without changing a single line**:

```
test/
├── mocks/              fake Arduino.h, WiFi.h, UniversalTelegramBot.h, Adafruit_SSD1306.h …
├── harness.h           simulated drip chamber + scheduler
└── test_main.cpp       the tests, which #include the real sketch
```

- **Simulated time.** `millis()`, `delay()`, `delayMicroseconds()` and `vTaskDelay()` move a virtual clock and never sleep. Hours of dripping run in milliseconds, and every run gives the same result.
- **Simulated optics.** `analogRead(GPIO34)` returns *ambient light + mains flicker + IR from the LED (only while GPIO25 is HIGH) × (1 − fraction blocked by a drop) + Gaussian ADC noise*, clipped to 0–4095. Drops are scheduled at exact times with a chosen depth and duration.
- **Both tasks run for real.** The sketch's own `sensorTask()` runs on a thread. At every 1 ms `vTaskDelay()` the harness runs `loop()` (every 5 ms), so the two interleave as they do on the ESP32. Slow Telegram calls can be simulated by stalling `loop()` while the sensor keeps sampling.
- **Observed outputs.** Buzzer edges and on-time, the OLED's text and inverted bar, every Telegram message sent, Serial output, pin modes, and critical-section balance.
- **Isolation.** Each test runs in its own forked process, so every test starts from a fresh boot.

### Run it

```bash
make -C test            # build + run all tests
make -C test report     # + writes test/build/test-results.md (the table below)
./test/build/test_runner alarm      # only tests whose name contains "alarm"
test/mutation_check.sh
```

Needs `g++` (C++17) and `make`. It works on Linux, macOS and WSL on Windows.

### Results

Generated with `make -C test report`.

| Group | Test | What it checks | Result | Measured |
|---|---|---|---|---|
| boot | `boot_configures_hardware` | Pins, I2C, OLED address, sensor task, TLS and WiFi are set up as the wiring guide says | ✅ |  |
| boot | `boot_sends_online_message` | One 'IV monitor online' Telegram message with target and drop factor | ✅ |  |
| boot | `ir_led_is_pulsed` | IR LED is pulsed on/off for every sample (~770 samples/s in the model) | ✅ | IR LED pulses per second: 770 |
| boot | `oled_missing_keeps_running` | If the OLED is not found the monitor still measures and alarms | ✅ |  |
| sensor | `no_drops_no_false_counts` | Clean signal, no drops for 60 s -> zero counts, WAITING, silent | ✅ |  |
| sensor | `counts_every_drop` | 300 drops at 100 mL/h are counted exactly | ✅ |  |
| sensor | `bright_drops_detected` | Drops that focus light (signal goes UP) are counted too | ✅ |  |
| sensor | `threshold_sensitivity` | A drop changing the signal by 80 counts is seen; 40 counts is below DETECT_THRESHOLD (60) and ignored | ✅ |  |
| sensor | `splash_counted_once` | A drop that dips the beam twice within 30 ms (splash/wobble) counts once (refractory 120 ms) | ✅ |  |
| sensor | `long_drop_relearns_baseline` | A permanent shift (sensor bumped) counts one extra drop, then detection recovers with no alarm | ✅ |  |
| sensor | `room_light_rejected` | Bright room light + 100 Hz flicker (150 counts) + slow ramp cause no false drops | ✅ |  |
| sensor | `adc_noise_rejected` | ADC noise of 6 counts (1 sigma) for 10 min causes no false drops | ✅ |  |
| sensor | `led_drift_tracked` | IR level drifting 3 counts/s (temperature) is tracked by the baseline; drops still counted exactly | ✅ |  |
| sensor | `slow_telegram_does_not_lose_drops` | Even if every Telegram call blocks loop() for 800 ms, the sensor task counts every drop | ✅ |  |
| sensor | `critical_sections_balanced` | Shared drop data is always accessed inside balanced, non-nested critical sections | ✅ |  |
| flow | `needs_three_intervals` | Rate is only shown after 3 drop intervals (4 drops); before that WAITING | ✅ |  |
| flow | `single_stray_drop_no_alarm` | One stray trigger never establishes flow, so it never raises a STOPPED alarm | ✅ |  |
| flow | `rate_follows_change` | When the clamp is changed 100 -> 60 mL/h the reading settles within 8 drops | ✅ |  |
| flow | `infused_and_remaining_volume` | Volume infused = drops / drop factor; remaining = bag - infused | ✅ |  |
| flow | `max_rate_limit` | Known limit: drops closer than 120 ms (500 drops/min) are partly missed - info only | ✅ | 100 drops 120 ms apart -> 89 counted, rate shown 444 mL/h (real 500) |
| alarm | `normal_flow_is_silent` | On-target flow for 5 min: NORMAL, no buzzer, no alarm messages | ✅ |  |
| alarm | `flow_stopped` | No drop for 20 s -> STOPPED: buzzer 1 beep/s (200 ms), Telegram ALARM once, inverted status bar | ✅ |  |
| alarm | `reminder_every_two_minutes` | While an alarm continues, a REMINDER is sent every 2 minutes | ✅ |  |
| alarm | `flow_resumes_resolved` | After a stop, the first new drop sends 'Resolved.' and the rate restarts clean (gap not averaged in) | ✅ |  |
| alarm | `stop_timeout_scales_with_slow_drip` | At 20 mL/h (9 s between drops) STOPPED waits 3 intervals = 27 s, not 20 s | ✅ |  |
| alarm | `too_fast` | 130 mL/h vs target 100 (+30%) -> TOO FAST only after 30 s grace; clears when fixed | ✅ |  |
| alarm | `too_slow` | 70 mL/h vs target 100 (-30%) -> TOO SLOW after the 30 s grace period | ✅ |  |
| alarm | `within_tolerance_no_alarm` | 118 and 82 mL/h (inside +/-20%) never alarm | ✅ |  |
| alarm | `short_deviation_ignored` | A 12 s burst at 150 mL/h (someone bumps the clamp) does not alarm | ✅ |  |
| alarm | `bag_low_warning` | Telegram 'Bag low' once when <10% of the bag is left; remaining never goes negative | ✅ |  |
| buttons | `up_down_change_target` | UP/DOWN change the target by 5 mL/h, clamped to 5..500 | ✅ |  |
| buttons | `hold_is_one_step` | Holding UP for 3 s changes the target only once (no auto-repeat) | ✅ |  |
| buttons | `debounce` | A press with 20 ms of contact bounce on press and release counts once | ✅ |  |
| buttons | `mute_short_press` | Short MUTE press silences the buzzer for 2 min, screen shows (mute), then beeping resumes | ✅ |  |
| buttons | `mute_long_press_new_bag` | Holding MUTE 2 s resets the infused-volume counter (new bag) and mutes | ✅ |  |
| telegram | `status_command` | /status and /start reply with state, flow, target, drops/min and volume | ✅ |  |
| telegram | `status_shows_time_to_empty` | With flow running, /status includes 'Empty in about X h' | ✅ |  |
| telegram | `setrate_command` | /setrate accepts 5..500 and rejects anything else | ✅ |  |
| telegram | `setbag_command` | /setbag sets bag volume 50..3000 mL and resets the counter | ✅ |  |
| telegram | `dropfactor_command` | /dropfactor accepts only 10, 15, 20, 60 | ✅ |  |
| telegram | `mute_command` | /mute silences the buzzer for 2 minutes | ✅ |  |
| telegram | `unknown_command_help` | Anything else gets the list of commands | ✅ |  |
| telegram | `ignores_other_chats` | Messages from any chat other than CHAT_ID are ignored (no reply, no change) | ✅ |  |
| telegram | `several_messages_one_poll` | Three messages arriving together all get answered | ✅ |  |
| telegram | `poll_interval` | The bot is polled about every 3 s | ✅ |  |
| wifi | `offline_still_alarms` | No WiFi: boots after 15 s timeout, local buzzer alarm still works, nothing is sent | ✅ |  |
| wifi | `reconnects_and_resumes` | Offline: reconnect tried every 30 s; once back, alarms reach Telegram (via the 2-min reminder) | ✅ |  |
| display | `normal_screen_layout` | Normal screen: 'Set 100 mL/h', WiFi, big rate, drops/min, volume, 'Flow normal' | ✅ |  |
| display | `refresh_rate` | Screen refreshes about 5 times per second | ✅ |  |
| longrun | `millis_rollover` | Flow measurement and alarms keep working across the 49.7-day millis() wrap | ✅ |  |
| longrun | `buzzer_works_after_25_days` | After >24.8 days of uptime the STOPPED alarm must still beep (never muted) | ✅ |  |
| flow | `accuracy_20mlh_df20` | 20 mL/h with a 20 drops/mL set is measured within 1% | ✅ | set 20 mL/h @ 20 drops/mL -> measured 20.00 mL/h, 6.67 drops/min (expected 6.67) |
| flow | `accuracy_60mlh_df20` | 60 mL/h with a 20 drops/mL set is measured within 1% | ✅ | set 60 mL/h @ 20 drops/mL -> measured 60.02 mL/h, 20.01 drops/min (expected 20.00) |
| flow | `accuracy_100mlh_df20` | 100 mL/h with a 20 drops/mL set is measured within 1% | ✅ | set 100 mL/h @ 20 drops/mL -> measured 100.00 mL/h, 33.33 drops/min (expected 33.33) |
| flow | `accuracy_250mlh_df20` | 250 mL/h with a 20 drops/mL set is measured within 1% | ✅ | set 250 mL/h @ 20 drops/mL -> measured 250.35 mL/h, 83.45 drops/min (expected 83.33) |
| flow | `accuracy_500mlh_df20` | 500 mL/h with a 20 drops/mL set is measured within 1% | ✅ | set 500 mL/h @ 20 drops/mL -> measured 501.39 mL/h, 167.13 drops/min (expected 166.67) |
| flow | `accuracy_50mlh_df10` | 50 mL/h with a 10 drops/mL set is measured within 1% | ✅ | set 50 mL/h @ 10 drops/mL -> measured 50.01 mL/h, 8.33 drops/min (expected 8.33) |
| flow | `accuracy_100mlh_df10` | 100 mL/h with a 10 drops/mL set is measured within 1% | ✅ | set 100 mL/h @ 10 drops/mL -> measured 100.00 mL/h, 16.67 drops/min (expected 16.67) |
| flow | `accuracy_100mlh_df15` | 100 mL/h with a 15 drops/mL set is measured within 1% | ✅ | set 100 mL/h @ 15 drops/mL -> measured 100.04 mL/h, 25.01 drops/min (expected 25.00) |
| flow | `accuracy_10mlh_df60` | 10 mL/h with a 60 drops/mL set is measured within 1% | ✅ | set 10 mL/h @ 60 drops/mL -> measured 10.00 mL/h, 10.00 drops/min (expected 10.00) |
| flow | `accuracy_30mlh_df60` | 30 mL/h with a 60 drops/mL set is measured within 1% | ✅ | set 30 mL/h @ 60 drops/mL -> measured 30.02 mL/h, 30.02 drops/min (expected 30.00) |
| flow | `accuracy_60mlh_df60` | 60 mL/h with a 60 drops/mL set is measured within 1% | ✅ | set 60 mL/h @ 60 drops/mL -> measured 60.00 mL/h, 60.00 drops/min (expected 60.00) |
| flow | `accuracy_120mlh_df60` | 120 mL/h with a 60 drops/mL set is measured within 1% | ✅ | set 120 mL/h @ 60 drops/mL -> measured 120.00 mL/h, 120.00 drops/min (expected 120.00) |
| flow | `accuracy_250mlh_df60` | 250 mL/h with a 60 drops/mL set is measured within 1% | ✅ | set 250 mL/h @ 60 drops/mL -> measured 250.00 mL/h, 250.00 drops/min (expected 250.00) |
| flow | `accuracy_480mlh_df60` | 480 mL/h with a 60 drops/mL set is measured within 1% | ✅ | set 480 mL/h @ 60 drops/mL -> measured 483.87 mL/h, 483.87 drops/min (expected 480.00) |
| characterise | `char_flicker_150` | Ambient-light flicker tolerance (info only) | ✅ | 100 Hz flicker amplitude 150 counts -> 0 false drops/min |
| characterise | `char_flicker_300` | Ambient-light flicker tolerance (info only) | ✅ | 100 Hz flicker amplitude 300 counts -> 0 false drops/min |
| characterise | `char_flicker_450` | Ambient-light flicker tolerance (info only) | ✅ | 100 Hz flicker amplitude 450 counts -> 6 false drops/min |
| characterise | `char_flicker_600` | Ambient-light flicker tolerance (info only) | ✅ | 100 Hz flicker amplitude 600 counts -> 309 false drops/min |
| characterise | `char_noise_6` | ADC noise tolerance (info only) | ✅ | ADC noise sigma 6 counts -> 0 false drops/min |
| characterise | `char_noise_10` | ADC noise tolerance (info only) | ✅ | ADC noise sigma 10 counts -> 2 false drops/min |
| characterise | `char_noise_14` | ADC noise tolerance (info only) | ✅ | ADC noise sigma 14 counts -> 89 false drops/min |
| characterise | `char_noise_18` | ADC noise tolerance (info only) | ✅ | ADC noise sigma 18 counts -> 311 false drops/min |
| calibration | `no_wifi_in_calibration` | CALIBRATION_MODE: no WiFi or Telegram, loop() idles | ✅ |  |
| calibration | `serial_plotter_stream` | CALIBRATION_MODE streams 'signal,baseline,low,high' lines ~every 5 ms; drops visible as dips | ✅ | 769 plotter lines in 4 s, lowest signal 1120 (clear beam ~1500) |

### Characterisation: measured limits

These tests always pass. They measure where the detector starts to fail, using the default `DETECT_THRESHOLD = 60` and an LED signal of 1500 counts.

| Condition | Result |
|---|---|
| 100 Hz flicker, amplitude 150 / 300 counts | 0 false drops/min |
| 100 Hz flicker, amplitude 450 counts | 6 false drops/min |
| 100 Hz flicker, amplitude 600 counts | ~300 false drops/min |
| ADC noise σ = 6 counts | 0 false drops/min (10 min run: 0) |
| ADC noise σ = 10 counts | 2 false drops/min |
| ADC noise σ = 14 / 18 counts | 89 / ~300 false drops/min |
| drop changes signal by 40 counts | not detected (below threshold) |
| drop changes signal by 80 counts | 100 % detected |
| drops 120 ms apart (500 drops/min) | 88 of 100 counted, shown as 444 instead of 500 mL/hr |
| drops ≥ 125 ms apart (≤ 480 drops/min) | 100 % counted, rate within 1 % |

What this means in practice:

- Keep the photodiode in an **opaque housing**. Flicker only becomes a problem when light from a tube light falls straight onto it.
- If the Serial Plotter shows noise wider than about ±10 counts, shield and twist the sensor wires or raise `DETECT_THRESHOLD`. Each drop's dip has to stay well above the new threshold.
- Do not use a 60 drops/mL micro set above about 480 mL/hr. In practice micro sets run far slower.

### Bug found and fixed

`longrun/buzzer_works_after_25_days` **failed** on the original code. `muteUntil` started at 0, and the buzzer was silenced while `(int32_t)(millis() − muteUntil) < 0`. Once `millis()` passes 2³¹ ms (**about 24.8 days of continuous power**), that expression goes negative even if nobody ever pressed MUTE. From then on every alarm would be **silent**, and the screen would show `(mute)`. The fix adds a `muted` flag (`startMute()` / `isMuted()`), so only a real mute can silence the buzzer. The test now passes, and `longrun/millis_rollover` confirms everything else survives the 49.7-day wrap-around.

One minor behaviour was also found and documented rather than changed: drops in the first 120 ms after power-up are ignored, because the refractory timer starts at 0. This has no practical effect.

## 3. Mutation check

A test suite is only useful if it fails when the code is wrong. `test/mutation_check.sh` makes 12 small, realistic mistakes in a copy of the sketch and confirms that at least one test fails for each:

| Injected bug | Caught by |
|---|---|
| Telegram accepts commands from any chat | ✅ |
| stop timeout shortened to 10 s | ✅ |
| refractory period removed | ✅ |
| average divides by the wrong count | ✅ |
| average not reset after a stop | ✅ |
| beep length changed | ✅ |
| alarm tolerance widened to 40 % | ✅ |
| no 30 s grace period for fast/slow | ✅ |
| only darkening drops detected | ✅ |
| mute ignored by the buzzer | ✅ |
| bag-low message repeats | ✅ |
| LED never switched off (no ambient cancelling) | ✅ |

## What the automated tests cannot prove

The simulation checks the **logic** of the firmware: what it does with a given sensor signal. It cannot check the physical parts: whether your photodiode actually produces an 80-count dip for your drip chamber, how noisy your ESP32's ADC is, whether the OLED is wired right, or whether your WiFi reaches Telegram. That is what the bench checklist is for.

## Bench test checklist (real hardware)

Run these on the assembled circuit before the demo and fill in the results. Use a real IV set and bag of water (no patient).

| # | Test | How | Expected | Result |
|---|---|---|---|---|
| B1 | Power-up | plug in USB | OLED shows "IV Drip Monitor", then the main screen; Telegram "IV monitor online" | |
| B2 | IR LED working | look at the LED through a phone camera | faint purple glow | |
| B3 | Calibration | `CALIBRATION_MODE = true`, Serial Plotter | each drop clearly crosses the low/high band, no crossings between drops | |
| B4 | Room light | in calibration mode, switch the room lights on/off, shine a phone torch nearby | signal returns to baseline, no false drops | |
| B5 | Drop count | count drops for 60 s by eye vs `dpm` on the OLED | within ±1 drop | |
| B6 | Flow accuracy | set the clamp to ~100 mL/hr (33 drops/min on a 20 set), compare | OLED within ±5 % of the hand count | |
| B7 | Stop alarm | close the roller clamp | after ~20 s: "FLOW STOPPED", beeping, Telegram ALARM | |
| B8 | Resume | open the clamp | "Resolved." on Telegram, beeping stops, rate returns | |
| B9 | Too fast | target 100, open the clamp to ~140 | after 30 s: "TOO FAST" + Telegram | |
| B10 | Too slow | target 100, close the clamp to ~60 | after 30 s: "TOO SLOW" + Telegram | |
| B11 | MUTE | press MUTE during an alarm | silent for 2 min, "(mute)" shown | |
| B12 | New bag | hold MUTE 2 s | volume counter returns to 0 | |
| B13 | UP / DOWN | press each | target changes by 5 | |
| B14 | Telegram commands | `/status`, `/setrate 120`, `/setbag 1000`, `/dropfactor 20`, `/mute` | correct replies | |
| B15 | WiFi loss | switch the hotspot off during an alarm | buzzer still works, OLED shows `----`; hotspot back on → reconnects within ~30 s | |
| B16 | Bump | tap the sensor clip | at most one extra count, no alarm | |
| B17 | Long run | leave dripping for 30 min | stable reading, no false alarms | |
