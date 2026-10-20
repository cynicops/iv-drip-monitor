// Minimal host-side stand-in for the Arduino-ESP32 core.
// Only what IV_Drip_Monitor.ino uses is implemented. Time is simulated:
// nothing here ever sleeps, so hours of drip can be tested in seconds.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using std::max;
using std::min;

#define HIGH 1
#define LOW 0
#define INPUT 0x01
#define OUTPUT 0x03
#define INPUT_PULLUP 0x05
typedef uint8_t byte;
typedef bool boolean;

// ------------------------------------------------------------ String
class String {
 public:
  std::string s;
  String() {}
  String(const char* c) : s(c ? c : "") {}
  String(const std::string& x) : s(x) {}
  String(char c) : s(1, c) {}
  String(int v) : s(std::to_string(v)) {}
  String(unsigned int v) : s(std::to_string(v)) {}
  String(long v) : s(std::to_string(v)) {}
  String(unsigned long v) : s(std::to_string(v)) {}
  String(float v, unsigned int dec = 2) { fmt((double)v, dec); }
  String(double v, unsigned int dec = 2) { fmt(v, dec); }

  const char* c_str() const { return s.c_str(); }
  unsigned int length() const { return (unsigned int)s.size(); }
  String& operator+=(const String& o) { s += o.s; return *this; }
  String& operator+=(const char* o) { s += o; return *this; }
  friend String operator+(const String& a, const String& b) { return String(a.s + b.s); }
  friend String operator+(const String& a, const char* b) { return String(a.s + b); }
  friend String operator+(const char* a, const String& b) { return String(std::string(a) + b.s); }
  bool operator==(const String& o) const { return s == o.s; }
  bool operator==(const char* o) const { return s == o; }
  bool operator!=(const String& o) const { return s != o.s; }
  bool operator!=(const char* o) const { return s != o; }
  bool startsWith(const String& p) const { return s.compare(0, p.s.size(), p.s) == 0; }
  int indexOf(const String& p) const {
    size_t i = s.find(p.s);
    return i == std::string::npos ? -1 : (int)i;
  }
  String substring(unsigned int from) const { return from >= s.size() ? String() : String(s.substr(from)); }
  String substring(unsigned int from, unsigned int to) const {
    if (from >= s.size() || to <= from) return String();
    return String(s.substr(from, to - from));
  }
  float toFloat() const { return (float)atof(s.c_str()); }
  long toInt() const { return atol(s.c_str()); }
  void trim() {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
  }

 private:
  void fmt(double v, unsigned int dec) {
    char b[64];
    snprintf(b, sizeof b, "%.*f", (int)dec, v);
    s = b;
  }
};

// ------------------------------------------------------------ simulation core
namespace sim {
inline uint64_t us = 0;          // simulated time since boot, microseconds
inline uint32_t msOffset = 0;    // added to millis() (used for the rollover test)
inline bool inHook = false;      // true while loop() is being run by the scheduler
inline void (*tickHook)() = nullptr;          // called by vTaskDelay()
inline int (*analogHook)(int pin) = nullptr;  // photodiode model
inline int pinLevel[64];         // what digitalRead() returns (buttons)
inline int pinOut[64];           // last value written with digitalWrite()
inline int pinModes[64];
inline uint32_t risingEdges[64]; // count of LOW->HIGH writes per pin
inline uint64_t highSinceUs[64];
inline uint64_t highTotalUs[64]; // total time each output pin spent HIGH
inline int criticalDepth = 0;    // portENTER/EXIT_CRITICAL balance check
inline int maxCriticalDepth = 0;
inline void resetPins() {
  for (int i = 0; i < 64; i++) {
    pinLevel[i] = HIGH;
    pinOut[i] = LOW;
    pinModes[i] = -1;
    risingEdges[i] = 0;
    highSinceUs[i] = 0;
    highTotalUs[i] = 0;
  }
}
}  // namespace sim

inline uint32_t millis() { return (uint32_t)(sim::us / 1000ULL + sim::msOffset); }
inline uint32_t micros() { return (uint32_t)sim::us; }
inline void delayMicroseconds(uint32_t u) { sim::us += u; }
inline void delay(uint32_t ms) {
  // Inside the scheduler the loop cadence is fixed by the harness, so
  // loop()'s own delay(5) must not move time. Outside it (setup), it does.
  if (!sim::inHook) sim::us += (uint64_t)ms * 1000ULL;
}

inline void pinMode(int pin, int mode) { sim::pinModes[pin] = mode; }
inline int digitalRead(int pin) { return sim::pinLevel[pin]; }
inline void digitalWrite(int pin, int v) {
  v = v ? HIGH : LOW;
  if (v == HIGH && sim::pinOut[pin] == LOW) {
    sim::risingEdges[pin]++;
    sim::highSinceUs[pin] = sim::us;
  }
  if (v == LOW && sim::pinOut[pin] == HIGH) sim::highTotalUs[pin] += sim::us - sim::highSinceUs[pin];
  sim::pinOut[pin] = v;
}
inline int analogRead(int pin) { return sim::analogHook ? sim::analogHook(pin) : 0; }
inline void analogReadResolution(int) {}
enum adc_attenuation_t { ADC_0db, ADC_2_5db, ADC_6db, ADC_11db };
inline void analogSetPinAttenuation(int, adc_attenuation_t) {}

// ------------------------------------------------------------ FreeRTOS bits
typedef struct { int owner; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
inline void portENTER_CRITICAL(portMUX_TYPE*) {
  sim::criticalDepth++;
  sim::maxCriticalDepth = std::max(sim::maxCriticalDepth, sim::criticalDepth);
}
inline void portEXIT_CRITICAL(portMUX_TYPE*) { sim::criticalDepth--; }
typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);
namespace sim {
inline TaskFunction_t taskFn = nullptr;
inline int taskCore = -1, taskPriority = -1;
}
inline int xTaskCreatePinnedToCore(TaskFunction_t fn, const char*, uint32_t, void*, int prio,
                                   TaskHandle_t*, int core) {
  sim::taskFn = fn;
  sim::taskPriority = prio;
  sim::taskCore = core;
  return 1;
}
inline void vTaskDelay(uint32_t ticks) {  // 1 tick = 1 ms on the ESP32 Arduino core
  sim::us += (uint64_t)ticks * 1000ULL;
  if (sim::tickHook) sim::tickHook();
}

// ------------------------------------------------------------ Serial
struct IPAddress {
  String toString() const { return String("192.168.4.20"); }
};
class HardwareSerialMock {
 public:
  std::string out;
  void begin(unsigned long) {}
  void print(const char* t) { out += t; }
  void print(const String& t) { out += t.s; }
  void print(int v) { out += std::to_string(v); }
  void print(unsigned int v) { out += std::to_string(v); }
  void print(long v) { out += std::to_string(v); }
  void print(unsigned long v) { out += std::to_string(v); }
  void print(double v, int d = 2) { out += String(v, d).s; }
  void print(const IPAddress& ip) { out += ip.toString().s; }
  template <typename T>
  void println(const T& v) { print(v); out += "\r\n"; }
  void println(double v, int d) { print(v, d); out += "\r\n"; }
  void println() { out += "\r\n"; }
  int printf(const char* f, ...) {
    char b[256];
    va_list a;
    va_start(a, f);
    int n = vsnprintf(b, sizeof b, f, a);
    va_end(a);
    out += b;
    return n;
  }
};
inline HardwareSerialMock Serial;
