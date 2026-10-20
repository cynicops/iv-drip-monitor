// Host mock: TLS client (records which certificate mode was chosen).
#pragma once
#include "Arduino.h"
class WiFiClientSecure {
 public:
  bool insecure = false;
  const char* caCert = nullptr;
  void setInsecure() { insecure = true; }
  void setCACert(const char* c) { caCert = c; }
};
