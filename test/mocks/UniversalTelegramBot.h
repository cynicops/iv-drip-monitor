// Host mock: Telegram bot. Outgoing messages are recorded; tests queue
// incoming messages in `inbox` and they are delivered by getUpdates().
#pragma once
#include "Arduino.h"
#include "WiFiClientSecure.h"
#define TELEGRAM_CERTIFICATE_ROOT "-----MOCK TELEGRAM ROOT CA-----"
struct telegramMessage {
  String text, chat_id, chat_title, from_id, from_name, date, type;
  int message_id = 0;
};
struct SentMessage { std::string chat, text; uint32_t atMs; };
class UniversalTelegramBot {
 public:
  telegramMessage messages[10];
  long last_message_received = 0;
  std::string token;
  std::vector<SentMessage> sent;
  std::vector<telegramMessage> inbox;
  int getUpdatesCalls = 0;
  UniversalTelegramBot(const String& t, WiFiClientSecure&) : token(t.s) {}
  bool sendMessage(const String& chat, const String& text, const String& = "") {
    sent.push_back({chat.s, text.s, millis()});
    return true;
  }
  int getUpdates(long offset) {
    getUpdatesCalls++;
    (void)offset;
    int n = (int)std::min<size_t>(inbox.size(), 10);
    for (int i = 0; i < n; i++) messages[i] = inbox[i];
    inbox.erase(inbox.begin(), inbox.begin() + n);
    last_message_received += n;
    return n;
  }
};
