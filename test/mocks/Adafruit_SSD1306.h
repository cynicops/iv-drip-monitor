// Host mock: 128x64 SSD1306 OLED. Every print between clearDisplay() and
// display() is collected so tests can read what the screen would show.
#pragma once
#include "Arduino.h"
#include "Wire.h"
#define SSD1306_SWITCHCAPVCC 0x02
#define SSD1306_BLACK 0
#define SSD1306_WHITE 1
class Adafruit_SSD1306 {
 public:
  bool present = true;      // set false to simulate a missing / mis-wired OLED
  uint8_t address = 0;
  std::string buf, frame;   // frame = text of the last pushed frame
  bool bufInverted = false, frameInverted = false;
  uint32_t frames = 0;
  Adafruit_SSD1306(int, int, TwoWire*, int) {}
  bool begin(uint8_t, uint8_t addr) { address = addr; return present; }
  void clearDisplay() { buf.clear(); bufInverted = false; }
  void setTextColor(int) {}
  void setTextSize(int) {}
  void setCursor(int, int) { if (!buf.empty()) buf += '|'; }
  void fillRect(int, int, int, int, int) { bufInverted = true; }
  void print(const char* t) { buf += t; }
  void print(const String& t) { buf += t.s; }
  void print(int v) { buf += std::to_string(v); }
  void print(double v, int d = 2) { buf += String(v, d).s; }
  template <typename T> void println(const T& v) { print(v); buf += '\n'; }
  void println() { buf += '\n'; }
  void display() { frame = buf; frameInverted = bufInverted; frames++; }
};
