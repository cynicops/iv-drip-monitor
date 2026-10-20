// Host mock: WiFi station. Tests decide whether the network is reachable.
#pragma once
#include "Arduino.h"
#define WL_IDLE_STATUS 0
#define WL_CONNECTED 3
#define WL_CONNECT_FAILED 4
#define WL_DISCONNECTED 6
#define WIFI_STA 1
class WiFiClass {
 public:
  bool networkUp = true;   // set by tests
  int st = WL_DISCONNECTED;
  int beginCalls = 0, reconnectCalls = 0, modeSet = -1;
  bool autoReconnect = false;
  std::string ssid, pass;
  void mode(int m) { modeSet = m; }
  void setAutoReconnect(bool b) { autoReconnect = b; }
  void begin(const char* s, const char* p) {
    ssid = s; pass = p; beginCalls++;
    st = networkUp ? WL_CONNECTED : WL_DISCONNECTED;
  }
  int status() { return st; }
  bool reconnect() { reconnectCalls++; st = networkUp ? WL_CONNECTED : WL_DISCONNECTED; return networkUp; }
  void dropConnection() { st = WL_DISCONNECTED; }
  IPAddress localIP() { return IPAddress(); }
};
inline WiFiClass WiFi;
