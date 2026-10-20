/*
  IV Drip Flow-Rate Monitor  (ESP32)
  --------------------------------------------------------------
  An IR LED and photodiode across the drip chamber count falling
  drops. The ESP32 turns drop timing into a flow rate (mL/hr),
  shows it on an OLED, and raises a buzzer + Telegram alarm if the
  flow stops or runs too fast / too slow.

  Wiring (see build guide):
    IR LED      : GPIO25 -> 100R -> LED anode, cathode -> GND
    Photodiode  : cathode -> 3V3, anode -> GPIO34, 100k GPIO34 -> GND
    OLED SSD1306: SDA -> GPIO21, SCL -> GPIO22, VCC 3V3, GND
    Buzzer      : I/O -> GPIO26
    Buttons     : UP GPIO32, DOWN GPIO33, MUTE GPIO27 (other leg to GND)

  Libraries: Adafruit SSD1306, Adafruit GFX, UniversalTelegramBot,
             ArduinoJson.  Board: "ESP32 Dev Module".
  Educational prototype only - not a medical device.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// =================== 1. YOUR SETTINGS ===================
const char* WIFI_SSID = "YOUR_WIFI_NAME";     // 2.4 GHz network or phone hotspot
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
#define BOT_TOKEN "1234567890:REPLACE_WITH_YOUR_BOT_TOKEN"
#define CHAT_ID   "123456789"                  // your (or the group's) chat ID

const bool CALIBRATION_MODE   = false; // true = stream sensor data to Serial Plotter
const bool BUZZER_ACTIVE_HIGH = true;  // false if your buzzer beeps when pin is LOW
const bool USE_INSECURE_TLS   = false; // set true only if Telegram messages never arrive

// =================== 2. IV / ALARM SETTINGS ===================
float DROP_FACTOR = 20.0;     // drops per mL: 20 = adult macro set, 60 = micro set
float targetRate  = 100.0;    // starting target flow rate, mL/hr
float bagVolume   = 500.0;    // bag/bottle volume, mL

const float RATE_STEP = 5.0;          // UP/DOWN step, mL/hr
const float RATE_MIN  = 5.0;
const float RATE_MAX  = 500.0;
const float TOLERANCE = 0.20;         // alarm if more than 20% off target...
const uint32_t DEVIATION_ALARM_MS = 30000;  // ...for 30 seconds
const uint32_t STOP_MIN_MS        = 20000;  // no drop for 20 s (or 3 intervals) = stopped
const uint32_t REMIND_MS          = 120000; // repeat Telegram alarm every 2 min
const uint32_t MUTE_MS            = 120000; // MUTE silences buzzer for 2 min
const float    BAG_LOW_FRACTION   = 0.10;   // warn when 10% of bag is left

// =================== 3. SENSOR TUNING ===================
const int      DETECT_THRESHOLD = 60;   // ADC counts away from baseline = a drop
const uint32_t REFRACTORY_MS    = 120;  // ignore re-triggers within this time
const float    BASELINE_ALPHA   = 0.002;// how fast the baseline follows slow drift
const int      AVG_DROPS        = 8;    // rate = average of last N drop intervals

// =================== 4. PINS ===================
const int PIN_IR_LED = 25;
const int PIN_PD     = 34;   // ADC1 pin (works with WiFi on)
const int PIN_BUZZER = 26;
const int PIN_UP     = 32;
const int PIN_DOWN   = 33;
const int PIN_MUTE   = 27;

// =================== GLOBALS ===================
Adafruit_SSD1306 display(128, 64, &Wire, -1);
WiFiClientSecure secureClient;
UniversalTelegramBot bot(BOT_TOKEN, secureClient);

// Shared between the sensor task and loop()
portMUX_TYPE dropMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t dropCount = 0;          // total drops since reset
volatile uint32_t lastDropMs = 0;         // time of the latest drop
volatile uint32_t intervals[AVG_DROPS];   // recent drop-to-drop times
volatile int intervalIdx = 0;
volatile int intervalsFilled = 0;
volatile bool resetIntervals = false;     // set by loop() after a stop

// Buttons (defined up here so Arduino's auto-prototypes can see the type)
struct Button {
  int pin;
  bool lastState;
  uint32_t lastChange;
  uint32_t pressedAt;
  bool longFired;
};
Button btnUp   = {PIN_UP,   HIGH, 0, 0, false};
Button btnDown = {PIN_DOWN, HIGH, 0, 0, false};
Button btnMute = {PIN_MUTE, HIGH, 0, 0, false};

enum State { WAITING, NORMAL, STOPPED, TOO_FAST, TOO_SLOW };
State state = WAITING;
State lastNotifiedState = WAITING;

float flowRate = 0;        // mL/hr
float dropsPerMin = 0;
float infusedMl = 0;
float remainingMl = 0;
uint32_t avgIntervalMs = 0;
uint32_t lastGoodIntervalMs = 3000;
uint32_t deviationSince = 0;
uint32_t muteUntil = 0;
bool muted = false;        // true while MUTE is active (see isMuted)
uint32_t lastAlarmMsgMs = 0;
uint32_t lastBotCheck = 0;
uint32_t lastScreen = 0;
uint32_t lastWifiTry = 0;
bool bagLowSent = false;
bool flowEstablished = false;  // true once a steady drip has been measured
uint32_t dropOffset = 0;   // drop count at the last "new bag" reset

// =================== SENSOR TASK ===================
// Runs ~1000 times a second on its own FreeRTOS task, so drops are
// never missed while the main loop is busy talking to Telegram.
void sensorTask(void* param) {
  float baseline = 0;
  bool haveBaseline = false;
  bool inDrop = false;
  uint32_t dropStartMs = 0;
  uint32_t lastTrigMs = 0;
  // calibration plot: report the biggest deviation in each 5 ms window
  float peakDev = 0;
  int peakSig = 0;
  uint32_t lastPlotMs = 0;

  for (;;) {
    // Pulse the LED: reading(on) - reading(off) cancels room light
    digitalWrite(PIN_IR_LED, HIGH);
    delayMicroseconds(150);
    int onVal = analogRead(PIN_PD);
    digitalWrite(PIN_IR_LED, LOW);
    delayMicroseconds(150);
    int offVal = analogRead(PIN_PD);
    int sig = onVal - offVal;

    if (!haveBaseline) { baseline = sig; haveBaseline = true; }
    float dev = sig - baseline;
    uint32_t now = millis();

    if (!inDrop) {
      if (fabsf(dev) > DETECT_THRESHOLD && (now - lastTrigMs) > REFRACTORY_MS) {
        // ---- a drop has entered the beam ----
        inDrop = true;
        dropStartMs = now;
        lastTrigMs = now;
        portENTER_CRITICAL(&dropMux);
        if (resetIntervals) {             // first drop after a stop: start fresh
          intervalsFilled = 0;
          intervalIdx = 0;
          resetIntervals = false;
        } else if (lastDropMs != 0) {
          intervals[intervalIdx] = now - lastDropMs;
          intervalIdx = (intervalIdx + 1) % AVG_DROPS;
          if (intervalsFilled < AVG_DROPS) intervalsFilled = intervalsFilled + 1;
        }
        lastDropMs = now;
        dropCount = dropCount + 1;
        portEXIT_CRITICAL(&dropMux);
      } else {
        baseline += BASELINE_ALPHA * dev;  // follow slow drift only
      }
    } else {
      // drop has left the beam once the signal is back near baseline
      if (fabsf(dev) < DETECT_THRESHOLD / 2) inDrop = false;
      // safety: if the signal shifted permanently (clamp moved), re-learn
      if (now - dropStartMs > 300) { baseline = sig; inDrop = false; }
    }

    if (CALIBRATION_MODE) {
      if (fabsf(dev) >= fabsf(peakDev)) { peakDev = dev; peakSig = sig; }
      if (now - lastPlotMs >= 5) {
        lastPlotMs = now;
        Serial.printf("signal:%d,baseline:%d,low:%d,high:%d\n", peakSig, (int)baseline,
                      (int)(baseline - DETECT_THRESHOLD), (int)(baseline + DETECT_THRESHOLD));
        peakDev = 0;
        peakSig = sig;
      }
    }

    vTaskDelay(1);  // ~1 ms between samples, lets other tasks run
  }
}

// =================== HELPERS ===================
void buzzer(bool on) {
  digitalWrite(PIN_BUZZER, (on == BUZZER_ACTIVE_HIGH) ? HIGH : LOW);
}

const char* stateText(State s) {
  switch (s) {
    case WAITING:  return "Waiting for drops";
    case NORMAL:   return "Flow normal";
    case STOPPED:  return "FLOW STOPPED";
    case TOO_FAST: return "TOO FAST";
    case TOO_SLOW: return "TOO SLOW";
  }
  return "";
}

bool isAlarm(State s) { return s == STOPPED || s == TOO_FAST || s == TOO_SLOW; }

String statusMessage() {
  String m = "IV monitor: ";
  m += stateText(state);
  m += "\nFlow: " + String(flowRate, 1) + " mL/hr (target " + String(targetRate, 0) + ")";
  m += "\nDrops/min: " + String(dropsPerMin, 1);
  m += "\nInfused: " + String(infusedMl, 0) + " of " + String(bagVolume, 0) + " mL";
  if (flowRate > 0.5 && remainingMl > 0) {
    m += "\nEmpty in about " + String(remainingMl / flowRate, 1) + " h";
  }
  return m;
}

void sendTelegram(const String& text) {
  if (WiFi.status() != WL_CONNECTED) return;
  bot.sendMessage(CHAT_ID, text, "");
}

void resetBag() {
  portENTER_CRITICAL(&dropMux);
  dropOffset = dropCount;
  portEXIT_CRITICAL(&dropMux);
  bagLowSent = false;
  flowEstablished = false;
}

// Mute is tracked with a flag as well as a time, so that an unused
// muteUntil = 0 can never look like "still muted" after millis() passes
// 2^31 (about 24.8 days of uptime).
void startMute() {
  muted = true;
  muteUntil = millis() + MUTE_MS;
}

bool isMuted() {
  if (muted && (int32_t)(millis() - muteUntil) >= 0) muted = false;  // mute expired
  return muted;
}

// =================== WIFI ===================
void connectWiFi(uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(250);
}

// =================== BUTTONS ===================

// returns 1 = short press (on release), 2 = long press (held 2 s), 0 = nothing
int readButton(Button& b) {
  bool s = digitalRead(b.pin);
  uint32_t now = millis();
  int event = 0;
  if (s != b.lastState && now - b.lastChange > 30) {   // debounce
    b.lastChange = now;
    b.lastState = s;
    if (s == LOW) { b.pressedAt = now; b.longFired = false; }
    else if (!b.longFired) event = 1;
  }
  if (s == LOW && !b.longFired && now - b.pressedAt > 2000) {
    b.longFired = true;
    event = 2;
  }
  return event;
}

void handleButtons() {
  if (readButton(btnUp) != 0)   targetRate = min(RATE_MAX, targetRate + RATE_STEP);
  if (readButton(btnDown) != 0) targetRate = max(RATE_MIN, targetRate - RATE_STEP);
  int e = readButton(btnMute);
  if (e == 1) startMute();                   // short press: mute 2 min
  if (e == 2) { resetBag(); startMute(); }   // hold 2 s: new bag
}

// =================== TELEGRAM COMMANDS ===================
void handleTelegram() {
  if (WiFi.status() != WL_CONNECTED) return;
  int n = bot.getUpdates(bot.last_message_received + 1);
  for (int i = 0; i < n; i++) {
    String chat = bot.messages[i].chat_id;
    String text = bot.messages[i].text;
    if (chat != CHAT_ID) continue;   // ignore strangers
    text.trim();
    if (text == "/status" || text == "/start") {
      sendTelegram(statusMessage());
    } else if (text == "/mute") {
      startMute();
      sendTelegram("Buzzer muted for 2 minutes.");
    } else if (text.startsWith("/setrate")) {
      float r = text.substring(8).toFloat();
      if (r >= RATE_MIN && r <= RATE_MAX) {
        targetRate = r;
        sendTelegram("Target set to " + String(targetRate, 0) + " mL/hr.");
      } else {
        sendTelegram("Use: /setrate 100  (5 to 500 mL/hr)");
      }
    } else if (text.startsWith("/setbag")) {
      float v = text.substring(7).toFloat();
      if (v >= 50 && v <= 3000) {
        bagVolume = v;
        resetBag();
        sendTelegram("New bag: " + String(bagVolume, 0) + " mL. Volume counter reset.");
      } else {
        sendTelegram("Use: /setbag 500  (50 to 3000 mL)");
      }
    } else if (text.startsWith("/dropfactor")) {
      float f = text.substring(11).toFloat();
      if (f == 10 || f == 15 || f == 20 || f == 60) {
        DROP_FACTOR = f;
        sendTelegram("Drop factor set to " + String(DROP_FACTOR, 0) + " drops/mL.");
      } else {
        sendTelegram("Use: /dropfactor 20  (10, 15, 20 or 60)");
      }
    } else {
      sendTelegram("Commands:\n/status\n/mute\n/setrate 100\n/setbag 500\n/dropfactor 20");
    }
  }
}

// =================== FLOW CALCULATION ===================
void updateFlow() {
  uint32_t count, lastDrop, sum = 0;
  int filled;
  portENTER_CRITICAL(&dropMux);
  count = dropCount;
  lastDrop = lastDropMs;
  filled = intervalsFilled;
  for (int i = 0; i < filled; i++) sum += intervals[i];
  portEXIT_CRITICAL(&dropMux);

  uint32_t now = millis();
  infusedMl = (count - dropOffset) / DROP_FACTOR;
  remainingMl = max(0.0f, bagVolume - infusedMl);

  if (filled >= 3) {
    avgIntervalMs = sum / filled;
    lastGoodIntervalMs = avgIntervalMs;
    dropsPerMin = 60000.0 / avgIntervalMs;
    flowRate = dropsPerMin * 60.0 / DROP_FACTOR;
    flowEstablished = true;
  }

  // ---- decide the state ----
  uint32_t stopTimeout = max(STOP_MIN_MS, 3 * lastGoodIntervalMs);
  if (lastDrop == 0 || (!flowEstablished && now - lastDrop > stopTimeout)) {
    // nothing yet, or only a stray trigger: keep waiting, no alarm
    state = WAITING;
    flowRate = 0; dropsPerMin = 0;
  } else if (now - lastDrop > stopTimeout) {
    if (state != STOPPED) resetIntervals = true;  // next drop starts a fresh average
    state = STOPPED;
    flowRate = 0; dropsPerMin = 0;
    deviationSince = 0;
  } else if (filled < 3) {
    state = WAITING;   // drops seen, still measuring
  } else {
    bool fast = flowRate > targetRate * (1 + TOLERANCE);
    bool slow = flowRate < targetRate * (1 - TOLERANCE);
    if (fast || slow) {
      if (deviationSince == 0) deviationSince = now;
      if (now - deviationSince > DEVIATION_ALARM_MS) state = fast ? TOO_FAST : TOO_SLOW;
      else if (!isAlarm(state)) state = NORMAL;   // within grace period
    } else {
      deviationSince = 0;
      state = NORMAL;
    }
  }
}

// =================== ALARMS ===================
void updateAlarms() {
  uint32_t now = millis();
  bool alarm = isAlarm(state);

  // buzzer: 200 ms beep every second while alarming (unless muted)
  bool beep = alarm && !isMuted() && (now % 1000) < 200;
  buzzer(beep);

  // Telegram: on every change into/out of alarm, then reminders
  if (state != lastNotifiedState && (alarm || isAlarm(lastNotifiedState))) {
    if (alarm) sendTelegram(String("ALARM: ") + stateText(state) + "\n" + statusMessage());
    else       sendTelegram(String("Resolved. ") + statusMessage());
    lastNotifiedState = state;
    lastAlarmMsgMs = now;
  } else if (state != lastNotifiedState) {
    lastNotifiedState = state;   // e.g. WAITING -> NORMAL, no message needed
  } else if (alarm && now - lastAlarmMsgMs > REMIND_MS) {
    sendTelegram(String("REMINDER: ") + stateText(state) + "\n" + statusMessage());
    lastAlarmMsgMs = now;
  }

  if (!bagLowSent && infusedMl > 0 && remainingMl < bagVolume * BAG_LOW_FRACTION) {
    sendTelegram("Bag low: about " + String(remainingMl, 0) + " mL left. Prepare the next bag.");
    bagLowSent = true;
  }
}

// =================== DISPLAY ===================
void drawScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // top line: target and WiFi
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("Set ");
  display.print(targetRate, 0);
  display.print(" mL/h");
  display.setCursor(104, 0);
  display.print(WiFi.status() == WL_CONNECTED ? "WiFi" : "----");

  // big flow rate
  display.setTextSize(3);
  display.setCursor(0, 12);
  if (state == WAITING || state == STOPPED) display.print("---");
  else display.print(flowRate, 0);
  display.setTextSize(1);
  display.setCursor(92, 20);
  display.print("mL/h");

  // details
  display.setCursor(0, 38);
  display.print(dropsPerMin, 1);
  display.print(" dpm  ");
  display.print(infusedMl, 0);
  display.print("/");
  display.print(bagVolume, 0);
  display.print("mL");

  // status bar (inverted when alarming)
  if (isAlarm(state)) {
    display.fillRect(0, 50, 128, 14, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  }
  display.setCursor(2, 53);
  display.print(stateText(state));
  if (isAlarm(state) && isMuted()) display.print(" (mute)");
  display.display();
}

// =================== SETUP & LOOP ===================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_IR_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  buzzer(false);
  pinMode(PIN_UP, INPUT_PULLUP);
  pinMode(PIN_DOWN, INPUT_PULLUP);
  pinMode(PIN_MUTE, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_PD, ADC_11db);   // full 0-3.3 V range

  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found - check wiring / address 0x3C");
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("IV Drip Monitor");
  display.println();
  display.println(CALIBRATION_MODE ? "CALIBRATION MODE" : "Connecting WiFi...");
  display.display();

  // start the sensor on core 1 at higher priority than loop()
  xTaskCreatePinnedToCore(sensorTask, "sensor", 4096, NULL, 2, NULL, 1);

  if (CALIBRATION_MODE) return;   // no WiFi needed while calibrating

  if (USE_INSECURE_TLS) secureClient.setInsecure();
  else secureClient.setCACert(TELEGRAM_CERTIFICATE_ROOT);

  connectWiFi(15000);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi OK, IP ");
    Serial.println(WiFi.localIP());
    sendTelegram("IV monitor online. Target " + String(targetRate, 0) +
                 " mL/hr, drop factor " + String(DROP_FACTOR, 0) +
                 ". Send /status any time.");
  } else {
    Serial.println("WiFi failed - running offline (local alarms still work)");
  }
}

void loop() {
  if (CALIBRATION_MODE) { delay(50); return; }

  uint32_t now = millis();
  handleButtons();
  updateFlow();
  updateAlarms();

  if (now - lastScreen > 200) { lastScreen = now; drawScreen(); }

  if (now - lastBotCheck > 3000) { lastBotCheck = now; handleTelegram(); }

  if (WiFi.status() != WL_CONNECTED && now - lastWifiTry > 30000) {
    lastWifiTry = now;
    WiFi.reconnect();
  }
  delay(5);
}
