#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#define USB_BAUD_RATE 115200
#define MAX_LINE_LENGTH 512
#define BUILT_IN_LED_PIN 15
#define TICK_INTERVAL 500
#define SUN_PACKET_SIZE 250

struct __attribute__((packed)) SunStatusHeader {
  uint8_t type;
  uint8_t node;
  uint8_t ttl;
  uint32_t globalTime;
};

static_assert(sizeof(SunStatusHeader) == 7, "Unexpected SUN status header size");

static uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static char usbLineBuffer[MAX_LINE_LENGTH];
static size_t usbLineLength = 0;
static QueueHandle_t sunPacketQueue = nullptr;
static volatile int32_t globalTimeOffset = 0;
static portMUX_TYPE globalTimeMux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t getGlobalTime() {
  return millis() + globalTimeOffset;
}

static void updateGlobalTime(uint32_t packetGlobalTime) {
  int32_t packetOffset = static_cast<int32_t>(packetGlobalTime - millis());
  portENTER_CRITICAL(&globalTimeMux);
  if (globalTimeOffset < packetOffset) {
    globalTimeOffset = packetOffset;
  }
  portEXIT_CRITICAL(&globalTimeMux);
}

static bool extractGlobalTime(const uint8_t *data, size_t len, uint32_t &globalTime) {
  static const char key[] = "globalTime";

  for (size_t i = 0; i < len; ++i) {
    if (data[i] != '"') {
      continue;
    }

    size_t keyStart = i + 1;
    size_t keyEnd = keyStart;
    bool escaped = false;
    while (keyEnd < len) {
      if (!escaped && data[keyEnd] == '"') {
        break;
      }
      if (!escaped && data[keyEnd] == '\\') {
        escaped = true;
      } else {
        escaped = false;
      }
      keyEnd++;
    }

    if (keyEnd >= len || keyEnd - keyStart != sizeof(key) - 1 ||
        memcmp(data + keyStart, key, sizeof(key) - 1) != 0) {
      i = keyEnd;
      continue;
    }

    size_t valueStart = keyEnd + 1;
    while (valueStart < len && (data[valueStart] == ' ' || data[valueStart] == '\t' ||
                                data[valueStart] == '\r' || data[valueStart] == '\n')) {
      valueStart++;
    }
    if (valueStart >= len || data[valueStart++] != ':') {
      i = keyEnd;
      continue;
    }

    while (valueStart < len && (data[valueStart] == ' ' || data[valueStart] == '\t' ||
                                data[valueStart] == '\r' || data[valueStart] == '\n')) {
      valueStart++;
    }
    if (valueStart >= len || data[valueStart] < '0' || data[valueStart] > '9') {
      i = keyEnd;
      continue;
    }

    uint32_t value = 0;
    while (valueStart < len && data[valueStart] >= '0' && data[valueStart] <= '9') {
      uint8_t digit = data[valueStart++] - '0';
      if (value > (UINT32_MAX - digit) / 10) {
        return false;
      }
      value = value * 10 + digit;
    }
    globalTime = value;
    return true;
  }

  return false;
}

static void addBroadcastPeer() {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastMac, sizeof(broadcastMac));
  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;

  if (!esp_now_is_peer_exist(broadcastMac)) {
    esp_now_add_peer(&peerInfo);
  }
}

static void sendToPeer(const uint8_t *peerMac, const uint8_t *payload, size_t len) {
  if (len == 0 || len > ESP_NOW_MAX_DATA_LEN || peerMac == nullptr) {
    return;
  }

  esp_now_send(peerMac, payload, len);
}

static void onEspNowReceive(const uint8_t *, const uint8_t *incomingData, int len) {
  if (incomingData == nullptr || len != SUN_PACKET_SIZE || sunPacketQueue == nullptr) {
    return;
  }

  SunStatusHeader sunHeader;
  memcpy(&sunHeader, incomingData, sizeof(sunHeader));
  updateGlobalTime(sunHeader.globalTime);
  xQueueSend(sunPacketQueue, incomingData, 0);
}

void setup() {
  Serial.begin(USB_BAUD_RATE);
  sunPacketQueue = xQueueCreate(8, SUN_PACKET_SIZE);
  if (sunPacketQueue == nullptr) {
    for (;;) {
      delay(1000);
    }
  }

  pinMode(BUILT_IN_LED_PIN, OUTPUT);
  analogWrite(BUILT_IN_LED_PIN, 0);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19dBm);

  esp_err_t channelStatus = esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (channelStatus != ESP_OK) {
    for (;;) {
      delay(1000);
    }
  }

  esp_err_t status = esp_now_init();
  if (status != ESP_OK) {
    for (;;) {
      delay(1000);
    }
  }

  esp_now_register_recv_cb(onEspNowReceive);
  addBroadcastPeer();
}

void loop() {
  static uint32_t lastTick = 0;
  uint32_t globalTime = getGlobalTime();
  if (globalTime - lastTick >= TICK_INTERVAL) {
    lastTick = globalTime;
    bool phase = (globalTime / TICK_INTERVAL) & 1;
    analogWrite(BUILT_IN_LED_PIN, phase ? 4 : 0);
  }

  while (Serial.available() > 0) {
    int ch = Serial.read();
    if (ch == '\r') {
      continue;
    }

    if (ch == '\n') {
      if (usbLineLength > 0) {
        uint32_t packetGlobalTime;
        if (extractGlobalTime(reinterpret_cast<const uint8_t *>(usbLineBuffer), usbLineLength, packetGlobalTime)) {
          updateGlobalTime(packetGlobalTime);
        }
        sendToPeer(broadcastMac, reinterpret_cast<const uint8_t *>(usbLineBuffer), usbLineLength);
        usbLineLength = 0;
      }
      continue;
    }

    if (usbLineLength < MAX_LINE_LENGTH - 1) {
      usbLineBuffer[usbLineLength++] = static_cast<char>(ch);
    }
  }

  uint8_t sunPacket[SUN_PACKET_SIZE];
  if (xQueueReceive(sunPacketQueue, sunPacket, 0) == pdTRUE) {
    Serial.write(sunPacket, SUN_PACKET_SIZE);
  }

  yield();
}