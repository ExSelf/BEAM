#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <mbedtls/md.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef SUN_PACKET_HMAC_KEY
#error "Define SUN_PACKET_HMAC_KEY in secrets.h to match SUN"
#endif

#ifndef DEFAULT_WIFI_CHANNEL
#ifdef WIFI_CHANNEL
#define DEFAULT_WIFI_CHANNEL WIFI_CHANNEL
#else
#error "Define DEFAULT_WIFI_CHANNEL in secrets.h to match SUN"
#endif
#endif

#define USB_BAUD_RATE 115200
#define BUILT_IN_LED_PIN 15
#define TICK_INTERVAL 500
#define COMMAND_LED_PULSE_DURATION_MS 10

static constexpr size_t SUN_PACKET_SIZE = 250;
static constexpr size_t SUN_AUTHENTICATED_SIZE = 242;
static constexpr size_t SUN_AUTH_TAG_SIZE = 8;
static constexpr uint8_t SUN_COMMAND_PACKET_TYPE = 1;
static constexpr uint8_t SUN_STATUS_PACKET_TYPE = 2;
static constexpr uint8_t SUN_MIN_TTL = 1;
static constexpr uint8_t SUN_MAX_TTL = 12;

static_assert(sizeof(SUN_PACKET_HMAC_KEY) - 1 >= 32,
              "SUN_PACKET_HMAC_KEY must be at least 32 characters");

static uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t usbPacketBuffer[SUN_PACKET_SIZE];
static size_t usbPacketLength = 0;
static QueueHandle_t sunPacketQueue = nullptr;
static volatile int32_t globalTimeOffset = 0;
static portMUX_TYPE globalTimeMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t commandLedPulseStartedAt = 0;
static bool commandLedPulseActive = false;
static uint8_t ledBlinkBrightness = 0;

static uint32_t readUint32Le(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

static bool calculatePacketAuthTag(const uint8_t *data, uint8_t *authTag) {
  static const char key[] = SUN_PACKET_HMAC_KEY;
  const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  uint8_t digest[32];
  if (sha256 == nullptr ||
      mbedtls_md_hmac(sha256,
                      reinterpret_cast<const unsigned char *>(key),
                      sizeof(key) - 1,
                      data,
                      SUN_AUTHENTICATED_SIZE,
                      digest) != 0) {
    return false;
  }

  memcpy(authTag, digest, SUN_AUTH_TAG_SIZE);
  return true;
}

static bool hasValidPacketAuthTag(const uint8_t *packet) {
  uint8_t expected[SUN_AUTH_TAG_SIZE];
  if (!calculatePacketAuthTag(packet, expected)) {
    return false;
  }

  uint8_t difference = 0;
  for (size_t i = 0; i < SUN_AUTH_TAG_SIZE; ++i) {
    difference |= expected[i] ^ packet[SUN_AUTHENTICATED_SIZE + i];
  }
  return difference == 0;
}

static bool hasValidSpacePacket(const uint8_t *packet, uint8_t packetType) {
  return packet[0] == packetType && packet[2] >= SUN_MIN_TTL &&
         packet[2] <= SUN_MAX_TTL && hasValidPacketAuthTag(packet);
}

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

static bool addBroadcastPeer() {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastMac, sizeof(broadcastMac));
  peerInfo.channel = DEFAULT_WIFI_CHANNEL;
  peerInfo.ifidx = WIFI_IF_STA;
  peerInfo.encrypt = false;

  if (esp_now_is_peer_exist(broadcastMac)) {
    return true;
  }

  esp_err_t result = esp_now_add_peer(&peerInfo);
  if (result != ESP_OK) {
    Serial.printf("ESP-NOW broadcast peer add failed: %d\n", result);
    return false;
  }
  return true;
}

static bool sendBroadcast(const uint8_t *payload, size_t len) {
  if (payload == nullptr || len != SUN_PACKET_SIZE || len > ESP_NOW_MAX_DATA_LEN) {
    Serial.println("ESP-NOW broadcast skipped: invalid packet size");
    return false;
  }

  esp_err_t result = esp_now_send(broadcastMac, payload, len);
  if (result != ESP_OK) {
    Serial.printf("ESP-NOW broadcast failed: %d\n", result);
    return false;
  }
  return true;
}

static void onEspNowReceive(const uint8_t *, const uint8_t *incomingData, int len) {
  if (incomingData == nullptr || len != static_cast<int>(SUN_PACKET_SIZE) ||
      incomingData[0] != SUN_STATUS_PACKET_TYPE ||
      incomingData[2] < SUN_MIN_TTL || incomingData[2] > SUN_MAX_TTL ||
      !hasValidPacketAuthTag(incomingData) || sunPacketQueue == nullptr) {
    return;
  }

  updateGlobalTime(readUint32Le(incomingData + 3));

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

  esp_err_t channelStatus = esp_wifi_set_channel(DEFAULT_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (channelStatus != ESP_OK) {
    Serial.printf("Wi-Fi channel setup failed: %d\n", channelStatus);
    for (;;) {
      delay(1000);
    }
  }

  esp_err_t status = esp_now_init();
  if (status != ESP_OK) {
    Serial.printf("ESP-NOW initialization failed: %d\n", status);
    for (;;) {
      delay(1000);
    }
  }

  status = esp_now_register_recv_cb(onEspNowReceive);
  if (status != ESP_OK) {
    Serial.printf("ESP-NOW receive callback registration failed: %d\n", status);
    for (;;) {
      delay(1000);
    }
  }
  if (!addBroadcastPeer()) {
    for (;;) {
      delay(1000);
    }
  }
}

void loop() {
  uint32_t globalTime = getGlobalTime();
  uint8_t tickBrightness = ((globalTime / TICK_INTERVAL) & 1) ? 4 : 0;
  if (tickBrightness != ledBlinkBrightness) {
    ledBlinkBrightness = tickBrightness;
    if (!commandLedPulseActive) {
      analogWrite(BUILT_IN_LED_PIN, ledBlinkBrightness);
    }
  }

  if (commandLedPulseActive &&
      millis() - commandLedPulseStartedAt >= COMMAND_LED_PULSE_DURATION_MS) {
    commandLedPulseActive = false;
    analogWrite(BUILT_IN_LED_PIN, ledBlinkBrightness);
  }

  while (Serial.available() > 0) {
    usbPacketBuffer[usbPacketLength++] = static_cast<uint8_t>(Serial.read());
    if (usbPacketLength == SUN_PACKET_SIZE) {
      if (hasValidSpacePacket(usbPacketBuffer, SUN_COMMAND_PACKET_TYPE)) {
        updateGlobalTime(readUint32Le(usbPacketBuffer + 3));
        commandLedPulseStartedAt = millis();
        commandLedPulseActive = true;
        analogWrite(BUILT_IN_LED_PIN, 255);
        sendBroadcast(usbPacketBuffer, SUN_PACKET_SIZE);
        usbPacketLength = 0;
      } else {
        memmove(usbPacketBuffer, usbPacketBuffer + 1, SUN_PACKET_SIZE - 1);
        --usbPacketLength;
      }
    }
  }

  uint8_t sunPacket[SUN_PACKET_SIZE];
  if (xQueueReceive(sunPacketQueue, sunPacket, 0) == pdTRUE) {
    if (Serial.write(sunPacket, SUN_PACKET_SIZE) != SUN_PACKET_SIZE) {
      Serial.println("USB status packet write failed");
    }
  }

  yield();
}
