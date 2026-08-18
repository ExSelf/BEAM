#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>

#define USB_BAUD_RATE 115200
#define MAX_LINE_LENGTH 512
#define MAX_PEERS 255

static uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t knownPeers[MAX_PEERS][6];
static uint8_t knownPeerCount = 0;
static char usbLineBuffer[MAX_LINE_LENGTH];
static size_t usbLineLength = 0;

static void printMac(const uint8_t *mac, char *out) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool parseMacString(const String &input, uint8_t out[6]) {
  String value = input;
  value.trim();
  value.replace(":", "");
  value.replace("-", "");
  value.replace(" ", "");

  if (value.length() != 12) {
    return false;
  }

  for (int i = 0; i < 6; ++i) {
    String hex = value.substring(i * 2, i * 2 + 2);
    char *end = nullptr;
    long v = strtol(hex.c_str(), &end, 16);
    if (end == hex.c_str() || *end != '\0') {
      return false;
    }
    out[i] = static_cast<uint8_t>(v);
  }

  return true;
}

static bool parseMacString(const char *input, uint8_t out[6]) {
  return parseMacString(String(input), out);
}

static void addKnownPeer(const uint8_t *mac) {
  for (uint8_t i = 0; i < knownPeerCount; ++i) {
    if (memcmp(knownPeers[i], mac, 6) == 0) {
      return;
    }
  }

  if (knownPeerCount >= MAX_PEERS) {
    Serial.println("[ESP_NOW] peer table full");
    return;
  }

  memcpy(knownPeers[knownPeerCount], mac, 6);
  knownPeerCount++;
}

static bool addEspNowPeer(const uint8_t *mac) {
  if (memcmp(mac, broadcastMac, 6) == 0) {
    return true;
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 1;
  peerInfo.encrypt = false;

  if (esp_now_is_peer_exist(mac)) {
    addKnownPeer(mac);
    return true;
  }

  esp_err_t err = esp_now_add_peer(&peerInfo);
  if (err != ESP_OK) {
    Serial.printf("[ESP_NOW] add peer failed: %d\n", err);
    return false;
  }

  addKnownPeer(mac);
  return true;
}

static void sendToPeer(const uint8_t *peerMac, const uint8_t *payload, size_t len) {
  if (len == 0 || peerMac == nullptr) {
    return;
  }

  esp_err_t err = esp_now_send(peerMac, payload, len);
  if (err != ESP_OK) {
    Serial.printf("[ESP_NOW] send failed: %d\n", err);
  }
}

static void sendToKnownPeers(const char *payload) {
  if (payload == nullptr || *payload == '\0') {
    return;
  }

  if (knownPeerCount == 0) {
    return;
  }

  size_t len = strlen(payload);
  for (uint8_t i = 0; i < knownPeerCount; ++i) {
    sendToPeer(knownPeers[i], reinterpret_cast<const uint8_t *>(payload), len);
  }
}

static String trimString(const String &input) {
  int start = 0;
  while (start < input.length() && (input.charAt(start) == ' ' || input.charAt(start) == '\t' || input.charAt(start) == '\r' || input.charAt(start) == '\n')) {
    start++;
  }

  int end = input.length() - 1;
  while (end >= start && (input.charAt(end) == ' ' || input.charAt(end) == '\t' || input.charAt(end) == '\r' || input.charAt(end) == '\n')) {
    end--;
  }

  if (start > end) {
    return "";
  }

  return input.substring(start, end + 1);
}

static String extractJsonValue(const String &json, const String &key) {
  String pattern = "\"" + key + "\"";
  int pos = json.indexOf(pattern);
  if (pos < 0) {
    return "";
  }

  int valueStart = json.indexOf(':', pos + pattern.length());
  if (valueStart < 0) {
    return "";
  }

  valueStart++;
  while (valueStart < json.length() && json.charAt(valueStart) == ' ') {
    valueStart++;
  }

  if (valueStart >= json.length()) {
    return "";
  }

  char quote = json.charAt(valueStart);
  if (quote == '\"') {
    int valueEnd = json.indexOf('"', valueStart + 1);
    if (valueEnd < 0) {
      return "";
    }
    return json.substring(valueStart + 1, valueEnd);
  }

  int valueEnd = valueStart;
  while (valueEnd < json.length() && json.charAt(valueEnd) != ',' && json.charAt(valueEnd) != '}') {
    valueEnd++;
  }
  return trimString(json.substring(valueStart, valueEnd));
}

static void sendJsonPacketToUsb(const uint8_t *srcMac, const uint8_t *dstMac, const String &payload, uint8_t ttl = 0) {
  char src[18];
  char dst[18];
  printMac(srcMac, src);
  printMac(dstMac, dst);

  Serial.print("{\"type\":\"espnow\",\"from\":\"");
  Serial.print(src);
  Serial.print("\",\"to\":\"");
  Serial.print(dst);
  Serial.print("\",\"ttl\":");
  Serial.print(String(ttl));
  Serial.print(",\"payload\":\"");
  for (size_t i = 0; i < payload.length(); ++i) {
    char c = payload[i];
    if (c == '\\' || c == '"') {
      Serial.print('\\');
    }
    Serial.print(c);
  }
  Serial.println("}");
}

static void handleJsonUsbPacket(const String &line) {
  String dstValue = extractJsonValue(line, "to");
  if (dstValue.length() == 0) {
    dstValue = extractJsonValue(line, "dst");
  }
  if (dstValue.length() == 0) {
    dstValue = extractJsonValue(line, "mac");
  }

  String payloadValue = extractJsonValue(line, "payload");
  if (payloadValue.length() == 0) {
    payloadValue = extractJsonValue(line, "data");
  }
  if (payloadValue.length() == 0) {
    payloadValue = extractJsonValue(line, "cmd");
  }
  if (payloadValue.length() == 0) {
    payloadValue = extractJsonValue(line, "message");
  }

  String ttlValue = extractJsonValue(line, "ttl");
  uint8_t ttl = ttlValue.length() > 0 ? static_cast<uint8_t>(ttlValue.toInt()) : 10;

  if (payloadValue.length() == 0) {
    Serial.println("[USB] JSON packet missing payload");
    return;
  }

  String packet = line;
  if (dstValue.length() == 0) {
    packet = String("{\"from\":\"") + WiFi.macAddress().c_str() + "\",\"ttl\":" + String(ttl) + ",\"payload\":\"" + payloadValue + "\"}";
  }

  bool isNodeNumber = true;
  for (size_t i = 0; i < dstValue.length(); ++i) {
    char c = dstValue.charAt(i);
    if (c < '0' || c > '9') {
      isNodeNumber = false;
      break;
    }
  }

  if (isNodeNumber && dstValue.length() > 0) {
    // Node numbers are logical ids, not MACs. In this BEAM layer we simply
    // broadcast the packet so the mesh can decide whether it is for this node.
    sendToPeer(broadcastMac, reinterpret_cast<const uint8_t *>(packet.c_str()), packet.length());
    return;
  }

  uint8_t targetMac[6];
  if (dstValue.length() == 0 || !parseMacString(dstValue, targetMac)) {
    Serial.println("[USB] JSON packet missing valid target MAC or node number");
    return;
  }

  sendToPeer(targetMac, reinterpret_cast<const uint8_t *>(packet.c_str()), packet.length());
}

static void handleUsbMessage(const String &line) {
  String trimmed = line;
  trimmed.trim();

  if (trimmed.length() == 0) {
    return;
  }

  if (trimmed.equalsIgnoreCase("PING")) {
    Serial.println("PONG");
    return;
  }

  if (trimmed.equalsIgnoreCase("STATUS")) {
    char mac[18];
    WiFi.macAddress((uint8_t *)mac);
    Serial.printf("STATUS MAC=%s CHANNEL=%d PEERS=%u\n", mac, WiFi.channel(), knownPeerCount);
    return;
  }

  if (trimmed.startsWith("ADD_PEER ")) {
    String macValue = trimmed.substring(strlen("ADD_PEER "));
    uint8_t peerMac[6];
    if (!parseMacString(macValue, peerMac)) {
      Serial.println("[USB] invalid peer MAC, expected AA:BB:CC:DD:EE:FF");
      return;
    }

    if (addEspNowPeer(peerMac)) {
      char mac[18];
      printMac(peerMac, mac);
      Serial.printf("[USB] peer added: %s\n", mac);
    }
    return;
  }

  if (trimmed.startsWith("BROADCAST ")) {
    String payload = trimmed.substring(strlen("BROADCAST "));
    payload.trim();
    if (payload.length() == 0) {
      return;
    }

    sendToPeer(broadcastMac, reinterpret_cast<const uint8_t *>(payload.c_str()), payload.length());
    return;
  }

  if (trimmed.startsWith("SEND ")) {
    String remainder = trimmed.substring(strlen("SEND "));
    int firstSpace = remainder.indexOf(' ');
    if (firstSpace <= 0) {
      Serial.println("[USB] SEND format: SEND <MAC> <payload>");
      return;
    }

    String macValue = remainder.substring(0, firstSpace);
    String payload = remainder.substring(firstSpace + 1);
    payload.trim();
    if (payload.length() == 0) {
      return;
    }

    uint8_t peerMac[6];
    if (!parseMacString(macValue, peerMac)) {
      Serial.println("[USB] invalid target MAC");
      return;
    }

    sendToPeer(peerMac, reinterpret_cast<const uint8_t *>(payload.c_str()), payload.length());
    return;
  }

  if (trimmed.startsWith("{") && trimmed.endsWith("}")) {
    handleJsonUsbPacket(trimmed);
    return;
  }

  sendToKnownPeers(trimmed.c_str());
}

static void onEspNowSend(const uint8_t *mac, esp_now_send_status_t status) {
  char addr[18];
  printMac(mac, addr);
  Serial.printf("ESP_NOW_TX %s %s\n", addr, status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

static void onEspNowReceive(const uint8_t *mac, const uint8_t *incomingData, int len) {
  if (len <= 0 || incomingData == nullptr) {
    return;
  }

  String packetText = String((const char *)incomingData).substring(0, len);

  char sender[18];
  printMac(mac, sender);

  if (packetText.startsWith("{")) {
    Serial.print("ESP_NOW_JSON_FROM ");
    Serial.print(sender);
    Serial.print(" ");
    Serial.println(packetText);
  } else {
    Serial.print("ESP_NOW_FROM ");
    Serial.print(sender);
    Serial.print(" ");
    Serial.write(incomingData, len);
    Serial.println();
  }

  addKnownPeer(mac);
}

void setup() {
  Serial.begin(USB_BAUD_RATE);
  delay(1000);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19dBm);

  esp_err_t status = esp_now_init();
  if (status != ESP_OK) {
    Serial.printf("[ESP_NOW] init failed: %d\n", status);
    for (;;) {
      delay(1000);
    }
  }

  esp_now_register_send_cb(onEspNowSend);
  esp_now_register_recv_cb(onEspNowReceive);
  addEspNowPeer(broadcastMac);

  Serial.println("ESP32-S2 USB tunnel ready");
  Serial.println("Bridge mode: USB <-> ESP-NOW only");
  Serial.println("Commands: PING, STATUS, ADD_PEER <AA:BB:CC:DD:EE:FF>, BROADCAST <payload>, SEND <MAC> <payload>");
}

void loop() {
  while (Serial.available() > 0) {
    int ch = Serial.read();
    if (ch == '\r') {
      continue;
    }

    if (ch == '\n') {
      if (usbLineLength > 0) {
        usbLineBuffer[usbLineLength] = '\0';
        handleUsbMessage(String(usbLineBuffer));
        usbLineLength = 0;
      }
      continue;
    }

    if (usbLineLength < MAX_LINE_LENGTH - 1) {
      usbLineBuffer[usbLineLength++] = static_cast<char>(ch);
    }
  }

  delay(10);
}