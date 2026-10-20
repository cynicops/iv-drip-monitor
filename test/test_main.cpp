// Host-side tests for firmware/IV_Drip_Monitor/IV_Drip_Monitor.ino
//
// The real sketch is compiled unmodified against small mocks of the
// Arduino-ESP32 core, WiFi, Telegram and the OLED (test/mocks). A simulated
// drip chamber feeds the photodiode input, and the sketch's own sensorTask()
// and loop() run against simulated time. Each test runs in a fresh child
// process, so every test starts from a clean boot.
//
// Build & run:  make -C test        (or: make -C test run)
#include "Arduino.h"

#ifndef SKETCH
#define SKETCH "../firmware/IV_Drip_Monitor/IV_Drip_Monitor.ino"
#endif
#include SKETCH

#include "harness.h"

#include <chrono>
#include <fcntl.h>
#include <regex>
#include <sys/wait.h>
#include <unistd.h>

using h::advance;
using h::boot;
using optics::addDrops;
using optics::intervalFor;

// ------------------------------------------------------------ registry
struct TestCase {
  std::string group, name, purpose;
  std::function<void()> fn;
};
static std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}
struct Reg {
  Reg(const char* g, const char* n, const char* p, void (*f)()) { registry().push_back({g, n, p, f}); }
};
#define TEST(group, name, purpose)                                    \
  static void test_##name();                                          \
  static Reg reg_##name(#group, #name, purpose, test_##name);         \
  static void test_##name()

static bool everAlarm = false;
static void watchForAlarms() {
  everAlarm = false;
  h::observer = [] { if (isAlarm(state)) everAlarm = true; };
}
static std::string fmt(const char* f, double a, double b = 0, double c = 0) {
  char buf[200];
  snprintf(buf, sizeof buf, f, a, b, c);
  return buf;
}

#ifndef CALIBRATION_BUILD
// ======================================================================
// 1. Boot & configuration
// ======================================================================
TEST(boot, boot_configures_hardware, "Pins, I2C, OLED address, sensor task, TLS and WiFi are set up as the wiring guide says") {
  boot();
  CHECK_EQ(sim::pinModes[PIN_IR_LED], OUTPUT);
  CHECK_EQ(sim::pinModes[PIN_BUZZER], OUTPUT);
  CHECK_EQ(sim::pinModes[PIN_UP], INPUT_PULLUP);
  CHECK_EQ(sim::pinModes[PIN_DOWN], INPUT_PULLUP);
  CHECK_EQ(sim::pinModes[PIN_MUTE], INPUT_PULLUP);
  CHECK_EQ(sim::pinOut[PIN_BUZZER], LOW);
  CHECK_EQ(Wire.sda, 21);
  CHECK_EQ(Wire.scl, 22);
  CHECK_EQ((int)display.address, 0x3C);
  CHECK(sim::taskFn == sensorTask);
  CHECK_EQ(sim::taskCore, 1);
  CHECK_EQ(sim::taskPriority, 2);
  CHECK(!secureClient.insecure);
  CHECK(secureClient.caCert && strcmp(secureClient.caCert, TELEGRAM_CERTIFICATE_ROOT) == 0);
  CHECK_EQ(WiFi.modeSet, WIFI_STA);
  CHECK(WiFi.autoReconnect);
  CHECK(Serial.out.find("WiFi OK, IP 192.168.4.20") != std::string::npos);
}

TEST(boot, boot_sends_online_message, "One 'IV monitor online' Telegram message with target and drop factor") {
  boot();
  CHECK_EQ(bot.sent.size(), (size_t)1);
  CHECK(bot.sent[0].chat == CHAT_ID);
  CHECK(bot.sent[0].text.find("IV monitor online") != std::string::npos);
  CHECK(bot.sent[0].text.find("Target 100 mL/hr") != std::string::npos);
  CHECK(bot.sent[0].text.find("drop factor 20") != std::string::npos);
}

TEST(boot, ir_led_is_pulsed, "IR LED is pulsed on/off for every sample (~770 samples/s in the model)") {
  boot();
  uint32_t e0 = sim::risingEdges[PIN_IR_LED];
  advance(1000);
  uint32_t perSec = sim::risingEdges[PIN_IR_LED] - e0;
  t::info(fmt("IR LED pulses per second: %.0f", perSec));
  CHECK_NEAR(perSec, 1000.0 / 1.3, 15);
}

TEST(boot, oled_missing_keeps_running, "If the OLED is not found the monitor still measures and alarms") {
  display.present = false;
  boot();
  CHECK(Serial.out.find("OLED not found") != std::string::npos);
  int n = h::drip(100, 30000);
  advance(31000);
  CHECK_EQ(h::drops(), (uint32_t)n);
  CHECK_STATE(NORMAL);
}

// ======================================================================
// 2. Drop detection (sensor task)
// ======================================================================
TEST(sensor, no_drops_no_false_counts, "Clean signal, no drops for 60 s -> zero counts, WAITING, silent") {
  boot();
  advance(60000);
  CHECK_EQ(h::drops(), 0u);
  CHECK_STATE(WAITING);
  CHECK_EQ(flowRate, 0.0f);
  CHECK_EQ(h::beeps(), 0u);
  CHECK_EQ(bot.sent.size(), (size_t)1);
  CHECK(h::screenHas("---"));
  CHECK(h::screenHas("Waiting for drops"));
  CHECK(!display.frameInverted);
}

TEST(sensor, counts_every_drop, "300 drops at 100 mL/h are counted exactly") {
  boot();
  addDrops(50, 1800, 300);
  advance(50 + 1800 * 300);
  CHECK_EQ(h::drops(), 300u);
}

TEST(sensor, bright_drops_detected, "Drops that focus light (signal goes UP) are counted too") {
  boot();
  addDrops(50, 1800, 40, -0.20);
  advance(50 + 1800 * 40);
  CHECK_EQ(h::drops(), 40u);
}

TEST(sensor, threshold_sensitivity, "A drop changing the signal by 80 counts is seen; 40 counts is below DETECT_THRESHOLD (60) and ignored") {
  boot();
  addDrops(50, 1800, 20, 40.0 / optics::ledSignal);
  advance(50 + 1800 * 20);
  CHECK_EQ(h::drops(), 0u);
  addDrops(50, 1800, 20, 80.0 / optics::ledSignal);
  advance(50 + 1800 * 20);
  CHECK_EQ(h::drops(), 20u);
}

TEST(sensor, splash_counted_once, "A drop that dips the beam twice within 30 ms (splash/wobble) counts once (refractory 120 ms)") {
  boot();
  addDrops(50, 1800, 50, 0.25, 6);
  addDrops(80, 1800, 50, 0.15, 5);  // second dip 30 ms after each drop
  advance(50 + 1800 * 50);
  CHECK_EQ(h::drops(), 50u);
}

TEST(sensor, long_drop_relearns_baseline, "A permanent shift (sensor bumped) counts one extra drop, then detection recovers with no alarm") {
  boot();
  watchForAlarms();
  addDrops(50, 1800, 60);
  optics::stepAtMs = optics::nowMs() + 30950;  // half-way between two drops
  optics::stepCounts = -300;
  advance(50 + 1800 * 60);
  CHECK_EQ(h::drops(), 61u);
  CHECK_STATE(NORMAL);
  CHECK_NEAR(flowRate, 100, 1.5);
  CHECK(!everAlarm);
}

TEST(sensor, room_light_rejected, "Bright room light + 100 Hz flicker (150 counts) + slow ramp cause no false drops") {
  optics::ambient = 400;
  optics::ambientRampPerSec = 8;
  optics::flickerAmp = 150;
  boot();
  advance(120000);
  CHECK_EQ(h::drops(), 0u);
  addDrops(50, 1800, 33);
  advance(50 + 1800 * 33);
  CHECK_EQ(h::drops(), 33u);
  CHECK_NEAR(flowRate, 100, 1.5);
}

TEST(sensor, adc_noise_rejected, "ADC noise of 6 counts (1 sigma) for 10 min causes no false drops") {
  optics::noiseSigma = 6;
  boot();
  advance(600000);
  CHECK_EQ(h::drops(), 0u);
  addDrops(50, 1800, 50);
  advance(50 + 1800 * 50);
  CHECK_EQ(h::drops(), 50u);
}

TEST(sensor, led_drift_tracked, "IR level drifting 3 counts/s (temperature) is tracked by the baseline; drops still counted exactly") {
  optics::ledDriftPerSec = -3;
  boot();
  addDrops(50, 1800, 160);
  advance(50 + 1800 * 160);
  CHECK_EQ(h::drops(), 160u);
}

TEST(sensor, slow_telegram_does_not_lose_drops, "Even if every Telegram call blocks loop() for 800 ms, the sensor task counts every drop") {
  h::telegramLatencyMs = 800;
  boot();
  addDrops(50, 1800, 150);
  for (int i = 0; i < 10; i++) {
    h::telegram("/status");
    advance(1800 * 15);
  }
  advance(100);
  CHECK_EQ(h::drops(), 150u);
  CHECK_NEAR(flowRate, 100, 1.5);
  CHECK(h::sentContaining("IV monitor: Flow normal") >= 9);
}

TEST(sensor, critical_sections_balanced, "Shared drop data is always accessed inside balanced, non-nested critical sections") {
  boot();
  h::drip(100, 120000);
  h::telegram("/status");
  advance(121000);
  CHECK_EQ(sim::criticalDepth, 0);
  CHECK_EQ(sim::maxCriticalDepth, 1);
}

// ======================================================================
// 3. Flow-rate maths
// ======================================================================
static void flowAccuracy(double rate, double df) {
  targetRate = rate;
  DROP_FACTOR = df;
  boot();
  double iv = intervalFor(rate, df);
  int n = 14;
  addDrops(100, iv, n);
  advance(100 + iv * (n - 1) + iv / 2);
  t::info(fmt("set %.0f mL/h @ %.0f drops/mL -> measured %.2f mL/h", rate, df, flowRate) +
          fmt(", %.2f drops/min (expected %.2f)", dropsPerMin, rate * df / 60));
  CHECK_STATE(NORMAL);
  CHECK_NEAR(flowRate, rate, rate * 0.01);
  CHECK_NEAR(dropsPerMin, rate * df / 60.0, rate * df / 60.0 * 0.01);
  CHECK_EQ(h::drops(), (uint32_t)n);
  CHECK_NEAR(infusedMl, n / df, 1e-3);
}
static void registerAccuracyTests() {
  const double cases[][2] = {{20, 20}, {60, 20}, {100, 20}, {250, 20}, {500, 20}, {50, 10}, {100, 10},
                             {100, 15}, {10, 60}, {30, 60}, {60, 60}, {120, 60}, {250, 60}, {480, 60}};
  for (auto& c : cases) {
    double r = c[0], d = c[1];
    char name[64], purpose[128];
    snprintf(name, sizeof name, "accuracy_%.0fmlh_df%.0f", r, d);
    snprintf(purpose, sizeof purpose, "%.0f mL/h with a %.0f drops/mL set is measured within 1%%", r, d);
    registry().push_back({"flow", name, purpose, [r, d] { flowAccuracy(r, d); }});
  }
}

TEST(flow, needs_three_intervals, "Rate is only shown after 3 drop intervals (4 drops); before that WAITING") {
  boot();
  addDrops(50, 1800, 3);
  advance(50 + 1800 * 2 + 100);
  CHECK_STATE(WAITING);
  CHECK_EQ(flowRate, 0.0f);
  addDrops(1700, 1800, 1);
  advance(1800);
  CHECK_STATE(NORMAL);
  CHECK_NEAR(flowRate, 100, 1);
}

TEST(flow, single_stray_drop_no_alarm, "One stray trigger never establishes flow, so it never raises a STOPPED alarm") {
  boot();
  addDrops(50, 1000, 1);
  advance(120000);
  CHECK_STATE(WAITING);
  CHECK_EQ(h::beeps(), 0u);
  CHECK_EQ(bot.sent.size(), (size_t)1);
}

TEST(flow, rate_follows_change, "When the clamp is changed 100 -> 60 mL/h the reading settles within 8 drops") {
  targetRate = 60;
  boot();
  addDrops(50, 1800, 20);
  advance(50 + 1800 * 19 + 100);
  CHECK_NEAR(flowRate, 100, 1);
  double iv = intervalFor(60, 20);
  addDrops(iv - 100, iv, 12);
  advance(iv * 9);
  CHECK_NEAR(flowRate, 60, 0.6);
}

TEST(flow, infused_and_remaining_volume, "Volume infused = drops / drop factor; remaining = bag - infused") {
  targetRate = 500;
  boot();
  addDrops(50, 360, 200);
  advance(50 + 360 * 200);
  CHECK_EQ(h::drops(), 200u);
  CHECK_NEAR(infusedMl, 10.0, 1e-4);
  CHECK_NEAR(remainingMl, 490.0, 1e-4);
}

TEST(flow, max_rate_limit, "Known limit: drops closer than 120 ms (500 drops/min) are partly missed - info only") {
  targetRate = 500;
  DROP_FACTOR = 60;  // 500 mL/h with a micro set = 500 drops/min = 120 ms apart
  boot();
  addDrops(100, 120, 100);
  advance(100 + 120 * 100);
  t::info(fmt("100 drops 120 ms apart -> %.0f counted, rate shown %.0f mL/h (real 500)", h::drops(), flowRate));
  CHECK(h::drops() < 100u);  // documents the limit; keep micro-set rates below ~480 mL/h
}

// ======================================================================
// 4. Alarms
// ======================================================================
TEST(alarm, normal_flow_is_silent, "On-target flow for 5 min: NORMAL, no buzzer, no alarm messages") {
  boot();
  watchForAlarms();
  h::drip(100, 300000);
  advance(300000);
  CHECK_STATE(NORMAL);
  CHECK(!everAlarm);
  CHECK_EQ(h::beeps(), 0u);
  CHECK_EQ(bot.sent.size(), (size_t)1);
}

TEST(alarm, flow_stopped, "No drop for 20 s -> STOPPED: buzzer 1 beep/s (200 ms), Telegram ALARM once, inverted status bar") {
  boot();
  addDrops(50, 1800, 33);                // last drop at 57.65 s
  advance(57750);
  CHECK_STATE(NORMAL);
  advance(19000);                         // 19.1 s since the last drop
  CHECK_STATE(NORMAL);
  CHECK_EQ(h::beeps(), 0u);
  advance(2000);                          // 21.1 s
  CHECK_STATE(STOPPED);
  CHECK_EQ(h::sentContaining("ALARM: FLOW STOPPED"), 1);
  CHECK(h::screenHas("FLOW STOPPED"));
  CHECK(display.frameInverted);
  CHECK(h::screenHas("---"));
  uint32_t b0 = h::beeps();
  uint64_t on0 = sim::highTotalUs[PIN_BUZZER];
  advance(10000);
  CHECK_NEAR(h::beeps() - b0, 10, 1);
  CHECK_NEAR((sim::highTotalUs[PIN_BUZZER] - on0) / 1000.0, 2000, 120);
  CHECK_EQ(h::sentContaining("ALARM: FLOW STOPPED"), 1);  // not repeated every loop
}

TEST(alarm, reminder_every_two_minutes, "While an alarm continues, a REMINDER is sent every 2 minutes") {
  boot();
  addDrops(50, 1800, 10);
  advance(50 + 1800 * 9 + 21000);
  CHECK_STATE(STOPPED);
  advance(115000);
  CHECK_EQ(h::sentContaining("REMINDER: FLOW STOPPED"), 0);
  advance(10000);
  CHECK_EQ(h::sentContaining("REMINDER: FLOW STOPPED"), 1);
  advance(120000);
  CHECK_EQ(h::sentContaining("REMINDER: FLOW STOPPED"), 2);
}

TEST(alarm, flow_resumes_resolved, "After a stop, the first new drop sends 'Resolved.' and the rate restarts clean (gap not averaged in)") {
  boot();
  addDrops(50, 1800, 10);
  advance(50 + 1800 * 9 + 25000);
  CHECK_STATE(STOPPED);
  addDrops(50, 1800, 30);
  advance(200);
  CHECK_EQ(h::sentContaining("Resolved."), 1);
  CHECK_STATE(WAITING);
  advance(1800 * 3 + 100);
  CHECK_STATE(NORMAL);
  CHECK_NEAR(flowRate, 100, 1);
  uint32_t b = h::beeps();
  advance(5000);
  CHECK_EQ(h::beeps(), b);
}

TEST(alarm, stop_timeout_scales_with_slow_drip, "At 20 mL/h (9 s between drops) STOPPED waits 3 intervals = 27 s, not 20 s") {
  targetRate = 20;
  boot();
  addDrops(50, 9000, 14);
  advance(50 + 9000 * 13 + 100);
  CHECK_STATE(NORMAL);
  advance(26000);
  CHECK_STATE(NORMAL);
  advance(2000);
  CHECK_STATE(STOPPED);
}

TEST(alarm, too_fast, "130 mL/h vs target 100 (+30%) -> TOO FAST only after 30 s grace; clears when fixed") {
  boot();
  h::drip(130, 300000);
  advance(25000);
  CHECK_STATE(NORMAL);
  advance(15000);
  CHECK_STATE(TOO_FAST);
  CHECK_EQ(h::sentContaining("ALARM: TOO FAST"), 1);
  CHECK(h::beeps() > 0);
  optics::clearFutureDrops();
  h::drip(100, 60000, 20, 1800);
  advance(1800 * 10);
  CHECK_STATE(NORMAL);
  CHECK_EQ(h::sentContaining("Resolved."), 1);
}

TEST(alarm, too_slow, "70 mL/h vs target 100 (-30%) -> TOO SLOW after the 30 s grace period") {
  boot();
  h::drip(70, 300000);
  advance(30000);
  CHECK_STATE(NORMAL);
  advance(15000);
  CHECK_STATE(TOO_SLOW);
  CHECK_EQ(h::sentContaining("ALARM: TOO SLOW"), 1);
}

TEST(alarm, within_tolerance_no_alarm, "118 and 82 mL/h (inside +/-20%) never alarm") {
  boot();
  watchForAlarms();
  h::drip(118, 180000);
  advance(180000);
  CHECK_STATE(NORMAL);
  optics::clearFutureDrops();
  h::drip(82, 180000, 20, optics::intervalFor(82, 20));
  advance(180000);
  CHECK_STATE(NORMAL);
  CHECK(!everAlarm);
  CHECK_EQ(bot.sent.size(), (size_t)1);
}

TEST(alarm, short_deviation_ignored, "A 12 s burst at 150 mL/h (someone bumps the clamp) does not alarm") {
  boot();
  watchForAlarms();
  addDrops(50, 1800, 17);                         // 100 mL/h to 28.85 s
  addDrops(50 + 1800 * 16 + 1200, 1200, 10);      // 150 mL/h for 12 s
  addDrops(50 + 1800 * 16 + 1200 * 10 + 1800, 1800, 60);  // back to 100
  advance(140000);
  CHECK(!everAlarm);
  CHECK_STATE(NORMAL);
}

TEST(alarm, bag_low_warning, "Telegram 'Bag low' once when <10% of the bag is left; remaining never goes negative") {
  targetRate = 500;
  boot();
  h::telegram("/setbag 50");
  advance(3500);
  CHECK_NEAR(bagVolume, 50, 1e-6);
  addDrops(50, 360, 1150);              // 50 mL bag = 1000 drops; 10% left after drop 901
  advance(50 + 360 * 880);
  CHECK_EQ(h::sentContaining("Bag low"), 0);
  advance(360 * 40);
  CHECK_EQ(h::sentContaining("Bag low"), 1);
  CHECK_EQ(h::sentContaining("Bag low: about 5 mL left"), 1);
  advance(360 * 240);
  CHECK_EQ(h::sentContaining("Bag low"), 1);
  CHECK_EQ(remainingMl, 0.0f);
  h::telegram("/setbag 500");
  advance(3500);
  CHECK(!bagLowSent);
  CHECK_NEAR(remainingMl, 500, 1);
}

// ======================================================================
// 5. Buttons
// ======================================================================
TEST(buttons, up_down_change_target, "UP/DOWN change the target by 5 mL/h, clamped to 5..500") {
  boot();
  for (int i = 0; i < 3; i++) h::press(PIN_UP);
  CHECK_NEAR(targetRate, 115, 1e-6);
  h::press(PIN_DOWN);
  CHECK_NEAR(targetRate, 110, 1e-6);
  for (int i = 0; i < 30; i++) h::press(PIN_DOWN);
  CHECK_NEAR(targetRate, 5, 1e-6);
  for (int i = 0; i < 110; i++) h::press(PIN_UP);
  CHECK_NEAR(targetRate, 500, 1e-6);
  advance(300);
  CHECK(h::screenHas("Set 500 mL/h"));
}

TEST(buttons, hold_is_one_step, "Holding UP for 3 s changes the target only once (no auto-repeat)") {
  boot();
  h::press(PIN_UP, 3000);
  CHECK_NEAR(targetRate, 105, 1e-6);
}

TEST(buttons, debounce, "A press with 20 ms of contact bounce on press and release counts once") {
  boot();
  auto bounce = [](int finalLevel) {
    for (int i = 0; i < 5; i++) {
      sim::pinLevel[PIN_UP] = finalLevel;
      advance(2);
      sim::pinLevel[PIN_UP] = !finalLevel;
      advance(2);
    }
    sim::pinLevel[PIN_UP] = finalLevel;
  };
  bounce(LOW);
  advance(150);
  bounce(HIGH);
  advance(200);
  CHECK_NEAR(targetRate, 105, 1e-6);
}

static void startStoppedAlarm() {
  boot();
  addDrops(50, 1800, 6);
  advance(50 + 1800 * 5 + 21000);
  CHECK_STATE(STOPPED);
  CHECK(h::beeps() > 0);
}

TEST(buttons, mute_short_press, "Short MUTE press silences the buzzer for 2 min, screen shows (mute), then beeping resumes") {
  startStoppedAlarm();
  h::press(PIN_MUTE, 100);
  uint32_t b = h::beeps();
  advance(110000);
  CHECK_EQ(h::beeps(), b);
  CHECK(h::screenHas("(mute)"));
  CHECK_STATE(STOPPED);
  advance(15000);
  CHECK(h::beeps() > b);
  CHECK(!h::screenHas("(mute)"));
}

TEST(buttons, mute_long_press_new_bag, "Holding MUTE 2 s resets the infused-volume counter (new bag) and mutes") {
  targetRate = 500;
  boot();
  h::drip(500, 60000);
  advance(30000);
  uint32_t before = h::drops();
  CHECK(infusedMl > 4.0f);
  h::press(PIN_MUTE, 2500);
  CHECK(dropOffset >= before && dropOffset <= h::drops());
  CHECK(infusedMl < 0.3f);
  CHECK((int32_t)(muteUntil - millis()) > 100000);
  advance(10000);
  CHECK_NEAR(infusedMl, (h::drops() - dropOffset) / 20.0, 1e-3);
}

// ======================================================================
// 6. Telegram bot
// ======================================================================
TEST(telegram, status_command, "/status and /start reply with state, flow, target, drops/min and volume") {
  boot();
  h::telegram("/status");
  advance(3500);
  CHECK_EQ(h::sentContaining("IV monitor: Waiting for drops"), 1);
  CHECK(h::lastSent().find("Flow: 0.0 mL/hr (target 100)") != std::string::npos);
  CHECK(h::lastSent().find("Infused: 0 of 500 mL") != std::string::npos);
  h::telegram("/start");
  advance(3100);
  CHECK_EQ(h::sentContaining("IV monitor: Waiting for drops"), 2);
  h::telegram("   /status  ");
  advance(3100);
  CHECK_EQ(h::sentContaining("IV monitor: Waiting for drops"), 3);
}

TEST(telegram, status_shows_time_to_empty, "With flow running, /status includes 'Empty in about X h'") {
  boot();
  h::drip(100, 120000);
  advance(60000);
  h::telegram("/status");
  advance(3100);
  std::string s = h::lastSent();
  CHECK(s.find("IV monitor: Flow normal") != std::string::npos);
  size_t p = s.find("Empty in about ");
  CHECK(p != std::string::npos);
  if (p != std::string::npos) {
    double hrs = atof(s.c_str() + p + 15);
    CHECK_NEAR(hrs, remainingMl / flowRate, 0.051);
    CHECK_NEAR(hrs, 4.97, 0.06);
  }
}

TEST(telegram, setrate_command, "/setrate accepts 5..500 and rejects anything else") {
  boot();
  h::telegram("/setrate 120");
  advance(3500);
  CHECK_NEAR(targetRate, 120, 1e-6);
  CHECK(h::lastSent() == "Target set to 120 mL/hr.");
  for (const char* bad : {"/setrate 4", "/setrate 501", "/setrate", "/setrate abc"}) {
    h::telegram(bad);
    advance(3100);
    CHECK_NEAR(targetRate, 120, 1e-6);
    CHECK(h::lastSent().find("Use: /setrate") == 0);
  }
}

TEST(telegram, setbag_command, "/setbag sets bag volume 50..3000 mL and resets the counter") {
  targetRate = 500;
  boot();
  h::drip(500, 20000);
  advance(20000);
  CHECK(infusedMl > 2);
  h::telegram("/setbag 1000");
  advance(3500);
  CHECK_NEAR(bagVolume, 1000, 1e-6);
  CHECK(infusedMl < 0.01f);
  CHECK(h::lastSent() == "New bag: 1000 mL. Volume counter reset.");
  h::telegram("/setbag 10");
  advance(3100);
  CHECK_NEAR(bagVolume, 1000, 1e-6);
  CHECK(h::lastSent().find("Use: /setbag") == 0);
}

TEST(telegram, dropfactor_command, "/dropfactor accepts only 10, 15, 20, 60") {
  boot();
  for (int f : {10, 15, 60, 20}) {
    char cmd[32];
    snprintf(cmd, sizeof cmd, "/dropfactor %d", f);
    h::telegram(cmd);
    advance(3100);
    CHECK_NEAR(DROP_FACTOR, f, 1e-6);
  }
  h::telegram("/dropfactor 33");
  advance(3100);
  CHECK_NEAR(DROP_FACTOR, 20, 1e-6);
  CHECK(h::lastSent().find("Use: /dropfactor") == 0);
}

TEST(telegram, mute_command, "/mute silences the buzzer for 2 minutes") {
  startStoppedAlarm();
  h::telegram("/mute");
  advance(3100);
  CHECK(h::lastSent() == "Buzzer muted for 2 minutes.");
  uint32_t b = h::beeps();
  advance(100000);
  CHECK_EQ(h::beeps(), b);
}

TEST(telegram, unknown_command_help, "Anything else gets the list of commands") {
  boot();
  h::telegram("hello");
  advance(3500);
  CHECK(h::lastSent().find("Commands:\n/status\n/mute\n/setrate 100\n/setbag 500\n/dropfactor 20") == 0);
}

TEST(telegram, ignores_other_chats, "Messages from any chat other than CHAT_ID are ignored (no reply, no change)") {
  boot();
  size_t before = bot.sent.size();
  h::telegram("/setrate 200", "987654321");
  h::telegram("/mute", "987654321");
  advance(3500);
  CHECK_NEAR(targetRate, 100, 1e-6);
  CHECK_EQ(muteUntil, 0u);
  CHECK_EQ(bot.sent.size(), before);
}

TEST(telegram, several_messages_one_poll, "Three messages arriving together all get answered") {
  boot();
  h::telegram("/setrate 110");
  h::telegram("/dropfactor 60");
  h::telegram("/status");
  advance(3500);
  CHECK_NEAR(targetRate, 110, 1e-6);
  CHECK_NEAR(DROP_FACTOR, 60, 1e-6);
  CHECK_EQ(bot.sent.size(), (size_t)4);
}

TEST(telegram, poll_interval, "The bot is polled about every 3 s") {
  boot();
  advance(60000);
  CHECK_NEAR(bot.getUpdatesCalls, 20, 1);
}

// ======================================================================
// 7. WiFi
// ======================================================================
TEST(wifi, offline_still_alarms, "No WiFi: boots after 15 s timeout, local buzzer alarm still works, nothing is sent") {
  boot(false);
  CHECK(sim::us >= 15000000ULL);
  CHECK(Serial.out.find("WiFi failed - running offline") != std::string::npos);
  CHECK_EQ(bot.sent.size(), (size_t)0);
  addDrops(50, 1800, 10);
  advance(50 + 1800 * 9 + 21000);
  CHECK_STATE(STOPPED);
  CHECK(h::beeps() > 0);
  CHECK(h::screenHas("----"));
  CHECK_EQ(bot.sent.size(), (size_t)0);
  CHECK_EQ(bot.getUpdatesCalls, 0);
}

TEST(wifi, reconnects_and_resumes, "Offline: reconnect tried every 30 s; once back, alarms reach Telegram (via the 2-min reminder)") {
  boot(false);
  addDrops(50, 1800, 10);
  advance(50 + 1800 * 9 + 21000);
  CHECK_STATE(STOPPED);
  int r0 = WiFi.reconnectCalls;
  advance(90000);
  CHECK_NEAR(WiFi.reconnectCalls - r0, 3, 1);
  WiFi.networkUp = true;
  advance(31000);
  CHECK_EQ(WiFi.status(), WL_CONNECTED);
  advance(300);
  CHECK(h::screenHas("WiFi"));
  advance(120000);
  CHECK(h::sentContaining("REMINDER: FLOW STOPPED") >= 1);
  h::telegram("/status");
  advance(3100);
  CHECK(h::lastSent().find("IV monitor: FLOW STOPPED") == 0);
}

// ======================================================================
// 8. Display
// ======================================================================
TEST(display, normal_screen_layout, "Normal screen: 'Set 100 mL/h', WiFi, big rate, drops/min, volume, 'Flow normal'") {
  boot();
  addDrops(50, 1800, 20);
  advance(50 + 1800 * 19 + 300);
  CHECK(h::screenHas("Set 100 mL/h"));
  CHECK(h::screenHas("|WiFi|"));
  CHECK(h::screenHas("|100|mL/h|"));
  CHECK(h::screenHas("33.3 dpm"));
  CHECK(h::screenHas("1/500mL"));
  CHECK(h::screenHas("Flow normal"));
  CHECK(!display.frameInverted);
}

TEST(display, refresh_rate, "Screen refreshes about 5 times per second") {
  boot();
  uint32_t f0 = display.frames;
  advance(10000);
  CHECK_NEAR(display.frames - f0, 49, 3);
}

// ======================================================================
// 9. Long-running behaviour (millis() wrap-around)
// ======================================================================
TEST(longrun, millis_rollover, "Flow measurement and alarms keep working across the 49.7-day millis() wrap") {
  sim::msOffset = 0xFFFFFFFFu - 40000u + 1u;  // wraps 40 s after boot
  boot();
  watchForAlarms();
  addDrops(50, 1800, 100);
  advance(50 + 1800 * 99 + 100);
  CHECK(millis() < 1000000u);  // we are past the wrap
  CHECK_STATE(NORMAL);
  CHECK_NEAR(flowRate, 100, 1);
  CHECK(!everAlarm);
  h::observer = nullptr;
  advance(21000);
  CHECK_STATE(STOPPED);
  CHECK(h::beeps() > 0);
}

TEST(longrun, buzzer_works_after_25_days, "After >24.8 days of uptime the STOPPED alarm must still beep (never muted)") {
  sim::msOffset = 0x80000000u + 5000u;  // 24.86 days of uptime, mute never pressed
  boot();
  addDrops(50, 1800, 10);
  advance(50 + 1800 * 9 + 21000);
  CHECK_STATE(STOPPED);
  CHECK(h::beeps() > 0);
  CHECK(!h::screenHas("(mute)"));
}

// ======================================================================
// 10. Characterisation (always pass; numbers go into docs/TESTING.md)
// ======================================================================
static void flickerSweep(double amp) {
  optics::flickerAmp = amp;
  optics::ambient = 2000 - amp;  // keep the ADC out of saturation
  optics::ledSignal = 1500;
  boot();
  advance(60000);
  t::info(fmt("100 Hz flicker amplitude %.0f counts -> %.0f false drops/min", amp, h::drops()));
}
static void noiseSweep(double sigma) {
  optics::noiseSigma = sigma;
  boot();
  advance(60000);
  t::info(fmt("ADC noise sigma %.0f counts -> %.0f false drops/min", sigma, h::drops()));
}
static void registerCharacterisation() {
  for (double a : {150.0, 300.0, 450.0, 600.0}) {
    char n[64];
    snprintf(n, sizeof n, "char_flicker_%.0f", a);
    registry().push_back({"characterise", n, "Ambient-light flicker tolerance (info only)", [a] { flickerSweep(a); }});
  }
  for (double s : {6.0, 10.0, 14.0, 18.0}) {
    char n[64];
    snprintf(n, sizeof n, "char_noise_%.0f", s);
    registry().push_back({"characterise", n, "ADC noise tolerance (info only)", [s] { noiseSweep(s); }});
  }
}

#else  // ===================== CALIBRATION_MODE = true build =====================
TEST(calibration, no_wifi_in_calibration, "CALIBRATION_MODE: no WiFi or Telegram, loop() idles") {
  boot();
  CHECK_EQ(WiFi.beginCalls, 0);
  CHECK_EQ(bot.sent.size(), (size_t)0);
  CHECK(sim::taskFn == sensorTask);
  advance(5000);
  CHECK_EQ(bot.getUpdatesCalls, 0);
}

TEST(calibration, serial_plotter_stream, "CALIBRATION_MODE streams 'signal,baseline,low,high' lines ~every 5 ms; drops visible as dips") {
  boot();
  Serial.out.clear();
  addDrops(300, 1800, 3);  // drops at +0.3, +2.1, +3.9 s
  advance(4000);
  std::regex line("signal:(-?\\d+),baseline:(-?\\d+),low:(-?\\d+),high:(-?\\d+)");
  int lines = 0, bad = 0, minSig = 99999;
  size_t pos = 0;
  while (pos < Serial.out.size()) {
    size_t e = Serial.out.find('\n', pos);
    if (e == std::string::npos) break;
    std::string l = Serial.out.substr(pos, e - pos);
    pos = e + 1;
    std::smatch m;
    if (!std::regex_match(l, m, line)) { bad++; continue; }
    lines++;
    int sig = std::stoi(m[1]), base = std::stoi(m[2]), lo = std::stoi(m[3]), hi = std::stoi(m[4]);
    if (hi - lo != 2 * DETECT_THRESHOLD || base - lo != DETECT_THRESHOLD) bad++;
    minSig = std::min(minSig, sig);
  }
  t::info(fmt("%.0f plotter lines in 4 s, lowest signal %.0f (clear beam ~%.0f)", lines, minSig, optics::ledSignal));
  CHECK_EQ(bad, 0);
  CHECK_NEAR(lines, 4000 / 5.2, 40);
  CHECK(minSig < optics::ledSignal - DETECT_THRESHOLD);  // the drop dips are visible
  CHECK_EQ(h::drops(), 3u);
}
static void registerAccuracyTests() {}
static void registerCharacterisation() {}
#endif

// ------------------------------------------------------------ runner
int main(int argc, char** argv) {
  registerAccuracyTests();
  registerCharacterisation();
  std::string filter = argc > 1 ? argv[1] : "";
  const char* reportPath = getenv("TEST_REPORT");
  FILE* report = reportPath ? fopen(reportPath, "a") : nullptr;
  int pass = 0, failed = 0;
  for (auto& tc : registry()) {
    std::string full = tc.group + "/" + tc.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) continue;
    int pipefd[2];
    if (pipe(pipefd) != 0) return 2;
    auto t0 = std::chrono::steady_clock::now();
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
      close(pipefd[0]);
      dup2(pipefd[1], 1);
      dup2(pipefd[1], 2);
      alarm(120);  // a hung test is a failed test
      t::failures = 0;
      tc.fn();
      fflush(stdout);
      fflush(stderr);
      _exit(t::failures ? 1 : 0);
    }
    close(pipefd[1]);
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof buf)) > 0) out.append(buf, n);
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (WIFSIGNALED(status)) out += "    FAIL crashed with signal " + std::to_string(WTERMSIG(status)) + "\n";
    printf("%s  %-48s %7.0f ms\n", ok ? "PASS" : "FAIL", full.c_str(), ms);
    fputs(out.c_str(), stdout);
    ok ? pass++ : failed++;
    if (report) {
      std::string notes;
      size_t p = 0;
      while ((p = out.find("INFO ", p)) != std::string::npos) {
        size_t e = out.find('\n', p);
        notes += (notes.empty() ? "" : "<br>") + out.substr(p + 5, e - p - 5);
        p = e;
      }
      fprintf(report, "| %s | `%s` | %s | %s | %s |\n", tc.group.c_str(), tc.name.c_str(), tc.purpose.c_str(),
              ok ? "PASS" : "**FAIL**", notes.c_str());
    }
  }
  printf("\n%d passed, %d failed\n", pass, failed);
  if (report) fclose(report);
  return failed ? 1 : 0;
}
