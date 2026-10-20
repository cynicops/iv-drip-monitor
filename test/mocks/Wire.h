// Host mock: I2C bus (records the pins the sketch starts it on).
#pragma once
#include "Arduino.h"
class TwoWire {
 public:
  int sda = -1, scl = -1;
  bool begin(int sdaPin = -1, int sclPin = -1, uint32_t = 0) { sda = sdaPin; scl = sclPin; return true; }
};
inline TwoWire Wire;
