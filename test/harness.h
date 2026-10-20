// Test harness: simulated drip chamber optics + a cooperative scheduler
// that runs the sketch's real sensorTask() and loop() against simulated time.
//
// Included by test_main.cpp *after* the sketch, so it can see the sketch's
// globals (state, flowRate, bot, display, ...) and functions (setup, loop).
#pragma once
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

// ------------------------------------------------------------ tiny test framework
namespace t {
inline int failures = 0;
inline void fail(const char* file, int line, const std::string& what) {
  failures++;
  fprintf(stderr, "    FAIL %s:%d  %s\n", file, line, what.c_str());
}
inline void info(const std::string& s) { fprintf(stdout, "    INFO %s\n", s.c_str()); }
}  // namespace t

#define CHECK(cond)                                         \
  do {                                                      \
    if (!(cond)) t::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)
#define CHECK_EQ(a, b)                                                                 \
  do {                                                                                 \
    auto _a = (a);                                                                     \
    auto _b = (b);                                                                     \
    if (!(_a == _b))                                                                   \
      t::fail(__FILE__, __LINE__,                                                      \
              std::string("CHECK_EQ(" #a ", " #b ")  got ") + std::to_string(_a) +     \
                  " vs " + std::to_string(_b));                                        \
  } while (0)
#define CHECK_NEAR(a, b, tol)                                                          \
  do {                                                                                 \
    double _a = (a), _b = (b), _t = (tol);                                             \
    if (std::fabs(_a - _b) > _t)                                                       \
      t::fail(__FILE__, __LINE__,                                                      \
              std::string("CHECK_NEAR(" #a ", " #b ")  got ") + std::to_string(_a) +   \
                  " expected " + std::to_string(_b) + " +/- " + std::to_string(_t));   \
  } while (0)
#define CHECK_STATE(s) \
  do { if (state != (s)) t::fail(__FILE__, __LINE__, std::string("state is ") + stateText(state) + ", expected " + stateText(s)); } while (0)

// ------------------------------------------------------------ drip chamber optics
// The photodiode reading is:
//   ambient light (+ optional mains flicker)
// + IR LED light that reaches the photodiode (only while the LED is on),
//   reduced while a drop is in the beam
// + random ADC noise.
namespace optics {
struct Drop {
  double t;      // ms since boot when the drop enters the beam
  double dur;    // ms it stays in the beam
  double depth;  // fraction of IR blocked (negative = drop focuses light, brighter)
};
inline double ledSignal = 1500;      // ADC counts from the IR LED, beam clear
inline double ambient = 200;         // ADC counts from room light
inline double ambientRampPerSec = 0; // slow room-light change (sunlight, lights switched)
inline double flickerAmp = 0;        // mains flicker amplitude, counts
inline double flickerHz = 100;       // 2 x 50 Hz mains in India
inline double ledDriftPerSec = 0;    // LED/photodiode drift (temperature), counts/s
inline double stepAtMs = -1;         // permanent offset in IR level (sensor bumped)
inline double stepCounts = 0;
inline double noiseSigma = 2;        // ADC noise, counts (1 sigma)
inline double dropDurMs = 8;
inline double dropDepth = 0.25;
inline std::vector<Drop> drops;
inline size_t firstActive = 0;
inline std::mt19937 rng(12345);

inline double nowMs() { return (double)sim::us / 1000.0; }

inline double blockedFraction(double ms) {
  while (firstActive < drops.size() && drops[firstActive].t + drops[firstActive].dur < ms) firstActive++;
  double f = 0;
  for (size_t i = firstActive; i < drops.size() && drops[i].t <= ms; i++)
    if (ms < drops[i].t + drops[i].dur) f += drops[i].depth;
  return f;
}

inline int read(int pin) {
  if (pin != PIN_PD) return 0;
  double ms = nowMs(), s = ms / 1000.0;
  double v = ambient + ambientRampPerSec * s;
  if (flickerAmp > 0) v += flickerAmp * std::sin(2 * M_PI * flickerHz * s);
  if (sim::pinOut[PIN_IR_LED] == HIGH) {
    double led = ledSignal + ledDriftPerSec * s;
    if (stepAtMs >= 0 && ms >= stepAtMs) led += stepCounts;
    v += led * (1.0 - blockedFraction(ms));
  }
  if (noiseSigma > 0) v += std::normal_distribution<double>(0, noiseSigma)(rng);
  if (v < 0) v = 0;
  if (v > 4095) v = 4095;  // 12-bit ADC
  return (int)std::lround(v);
}

// Schedule `count` drops, first one `startInMs` from now, `intervalMs` apart.
inline void addDrops(double startInMs, double intervalMs, int count, double depth = -999, double dur = -1) {
  double t0 = nowMs() + startInMs;
  for (int i = 0; i < count; i++)
    drops.push_back({t0 + i * intervalMs, dur > 0 ? dur : dropDurMs, depth == -999 ? dropDepth : depth});
  // keep drops in time order (only future drops are ever added)
  std::stable_sort(drops.begin() + firstActive, drops.end(),
                   [](const Drop& a, const Drop& b) { return a.t < b.t; });
}
// Stop the drip: forget every drop that has not started yet.
inline void clearFutureDrops() {
  double ms = nowMs();
  drops.erase(std::remove_if(drops.begin() + firstActive, drops.end(),
                             [ms](const Drop& d) { return d.t > ms; }),
              drops.end());
}
inline double intervalFor(double mlPerHr, double dropFactor) { return 3600000.0 / (mlPerHr * dropFactor); }
}  // namespace optics

// ------------------------------------------------------------ scheduler
// sensorTask() runs on its own std::thread (it never returns). Each 1 ms
// vTaskDelay() hands control to tick(), which runs loop() every 5 ms of
// simulated time and pauses the simulation when the test's time budget is
// used up. Only one thread ever runs at a time, so results are deterministic.
namespace h {
inline std::mutex m;
inline std::condition_variable cv;
inline bool sensorTurn = false, started = false;
inline uint64_t endUs = 0, nextLoopUs = 0;
inline uint32_t loopPeriodUs = 5000;
inline uint64_t loopStallUs = 0;          // simulated blocking network call inside loop()
inline std::function<void()> observer;    // called every tick (for "never happened" checks)
inline uint32_t telegramLatencyMs = 0;    // how long each Telegram HTTPS call blocks loop()
inline size_t lastSentSeen = 0;
inline int lastGetUpdates = 0;

inline void tick() {
  if (sim::us >= nextLoopUs) {
    sim::inHook = true;
    loop();
    sim::inHook = false;
    // A real Telegram call blocks loop() for hundreds of ms while the
    // sensor task keeps sampling. Model that by delaying the next loop().
    uint64_t stall = 0;
    if (telegramLatencyMs) {
      size_t newSent = bot.sent.size() - lastSentSeen;
      int newPolls = bot.getUpdatesCalls - lastGetUpdates;
      stall = (uint64_t)(newSent + newPolls) * telegramLatencyMs * 1000ULL;
    }
    lastSentSeen = bot.sent.size();
    lastGetUpdates = bot.getUpdatesCalls;
    nextLoopUs = sim::us + loopPeriodUs + stall;
  }
  if (observer) observer();
  if (sim::us >= endUs) {
    std::unique_lock<std::mutex> lk(m);
    sensorTurn = false;
    cv.notify_all();
    cv.wait(lk, [] { return sensorTurn; });
  }
}

// Run the whole system for `ms` of simulated time.
inline void advance(double ms) {
  endUs = sim::us + (uint64_t)(ms * 1000.0);
  std::unique_lock<std::mutex> lk(m);
  sensorTurn = true;
  if (!started) {
    started = true;
    lastSentSeen = bot.sent.size();
    lastGetUpdates = bot.getUpdatesCalls;
    std::thread([] { sim::taskFn(nullptr); }).detach();
  } else {
    cv.notify_all();
  }
  cv.wait(lk, [] { return !sensorTurn; });
}

inline void boot(bool wifiUp = true) {
  sim::resetPins();
  sim::tickHook = tick;
  sim::analogHook = optics::read;
  WiFi.networkUp = wifiUp;
  setup();
  nextLoopUs = sim::us;
  // The sketch ignores triggers in its first 120 ms (lastTrigMs starts at 0
  // and REFRACTORY_MS is 120), so let it run briefly before any drops.
  advance(300);
}

// ---- helpers used by the tests
inline void press(int pin, double holdMs = 100) {
  sim::pinLevel[pin] = LOW;
  advance(holdMs);
  sim::pinLevel[pin] = HIGH;
  advance(60);
}
inline void telegram(const char* text, const char* chat = CHAT_ID) {
  telegramMessage msg;
  msg.text = text;
  msg.chat_id = chat;
  bot.inbox.push_back(msg);
}
inline int sentContaining(const std::string& needle) {
  int n = 0;
  for (auto& s : bot.sent) if (s.text.find(needle) != std::string::npos) n++;
  return n;
}
inline std::string lastSent() { return bot.sent.empty() ? "" : bot.sent.back().text; }
inline uint32_t beeps() { return sim::risingEdges[PIN_BUZZER]; }
inline bool screenHas(const std::string& s) { return display.frame.find(s) != std::string::npos; }
inline uint32_t drops() {
  portENTER_CRITICAL(&dropMux);
  uint32_t c = dropCount;
  portEXIT_CRITICAL(&dropMux);
  return c;
}
// drip at a given rate for a while, starting shortly after "now"
inline int drip(double mlPerHr, double forMs, double df = 20, double startInMs = 50) {
  double iv = optics::intervalFor(mlPerHr, df);
  int n = (int)(forMs / iv);
  if (n < 1) n = 1;
  optics::addDrops(startInMs, iv, n);
  return n;
}
}  // namespace h
