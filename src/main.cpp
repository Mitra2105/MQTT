#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <FS.h>

// =====================================================
// WIFI RECEIVER
// =====================================================
const char* WIFI_SSID = "powerten-server";
const char* WIFI_PASSWORD = "12345678";

// Central ESP32-S3 receiver AP/TCP server
IPAddress RECEIVER_IP(192, 168, 4, 1);
const uint16_t RECEIVER_PORT = 5000;
const char* DEVICE_NAME = "BMS";

// =====================================================
// RS485 / BMS
// =====================================================
#define RS485_TX 15
#define RS485_RX 16
#define RS485_DE 21
#define BMS_BAUD 9600

HardwareSerial BMS(1);
WiFiClient tcpClient;

const char BMS_REQUEST[] = "~201246610000FDAA\r";

const unsigned long POLL_INTERVAL = 5000;
const unsigned long WIFI_RETRY_INTERVAL = 10000;
const unsigned long SEND_RETRY_INTERVAL = 1000;
const unsigned long ACK_TIMEOUT_MS = 3000;

const char* QUEUE_FILE = "/queue.txt";
const char* TEMP_FILE = "/queue.tmp";

unsigned long lastPoll = 0;
unsigned long lastWiFiAttempt = 0;
unsigned long lastSendAttempt = 0;
unsigned long nextMessageId = 1;

String responseFrame = "";

bool wifiStarted = false;
bool flashReady = false;
volatile unsigned long acknowledgedMessageId = 0;

// =====================================================
// PERSISTENT MESSAGE ID
// =====================================================
unsigned long extractMessageId(const String& json) {
  int pos = json.indexOf("\"message_id\":");
  if (pos < 0) return 0;

  pos += strlen("\"message_id\":");
  while (pos < json.length() && json[pos] == ' ') pos++;

  return strtoul(json.c_str() + pos, nullptr, 10);
}

void recoverMessageId() {
  unsigned long highestId = 0;

  File file = LittleFS.open(QUEUE_FILE, "r");
  if (file) {
    while (file.available()) {
      String line = file.readStringUntil('\n');
      unsigned long id = extractMessageId(line);
      if (id > highestId) highestId = id;
    }
    file.close();
  }

  nextMessageId = highestId + 1;
  if (nextMessageId == 0) nextMessageId = 1;

  Serial.printf("Next message ID: %lu\n", nextMessageId);
}

// =====================================================
// FLASH QUEUE
// =====================================================
bool enqueueJSON(const char* json) {
  if (!flashReady) {
    Serial.println("FLASH ERROR: queue unavailable");
    return false;
  }

  File file = LittleFS.open(QUEUE_FILE, "a");
  if (!file) {
    Serial.println("FLASH ERROR: cannot open queue");
    return false;
  }

  size_t written = file.println(json);
  file.flush();
  file.close();

  if (written == 0) {
    Serial.println("FLASH ERROR: queue write failed");
    return false;
  }

  Serial.println("SAVED TO FLASH QUEUE");
  Serial.println(json);
  return true;
}

bool queueHasData() {
  File file = LittleFS.open(QUEUE_FILE, "r");
  if (!file) return false;

  bool hasData = file.available();
  file.close();
  return hasData;
}

String peekQueue() {
  File file = LittleFS.open(QUEUE_FILE, "r");
  if (!file) return "";

  String line = file.readStringUntil('\n');
  file.close();
  line.trim();

  return line;
}

// Remove the oldest record only after its matching ACK.
bool removeFirstQueueRecord() {
  File source = LittleFS.open(QUEUE_FILE, "r");
  if (!source) {
    Serial.println("FLASH ERROR: cannot read queue");
    return false;
  }

  if (LittleFS.exists(TEMP_FILE)) {
    LittleFS.remove(TEMP_FILE);
  }

  File temp = LittleFS.open(TEMP_FILE, "w");
  if (!temp) {
    source.close();
    Serial.println("FLASH ERROR: cannot create temp queue");
    return false;
  }

  // Skip the first queued line.
  if (source.available()) {
    source.readStringUntil('\n');
  }

  // Copy the remaining records.
  while (source.available()) {
    String line = source.readStringUntil('\n');
    if (line.length() > 0) {
      temp.println(line);
    }
  }

  temp.flush();
  temp.close();
  source.close();

  LittleFS.remove(QUEUE_FILE);

  if (!LittleFS.rename(TEMP_FILE, QUEUE_FILE)) {
    Serial.println("FLASH ERROR: queue replacement failed");
    return false;
  }

  return true;
}

void printQueueCount() {
  File file = LittleFS.open(QUEUE_FILE, "r");
  unsigned long count = 0;

  if (file) {
    while (file.available()) {
      file.readStringUntil('\n');
      count++;
    }
    file.close();
  }

  Serial.printf("Messages waiting in flash: %lu\n", count);
}

// =====================================================
// WIFI MANAGEMENT
// =====================================================
void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_NAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  wifiStarted = true;
  lastWiFiAttempt = millis();

  Serial.printf("Attempting Wi-Fi: %s\n", WIFI_SSID);
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  tcpClient.stop();

  if (!wifiStarted ||
      millis() - lastWiFiAttempt >= WIFI_RETRY_INTERVAL) {
    startWiFi();
  }
}

// =====================================================
// TCP RECEIVER + JSON ACK
// Receiver accepts one JSON object per line and responds with
// {"type":"ACK","device":"BMS","message_id":N,...}

// =====================================================
bool connectReceiver() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (tcpClient.connected()) return true;

  tcpClient.stop();
  Serial.printf("Connecting TCP receiver %s:%u\n",
                RECEIVER_IP.toString().c_str(), RECEIVER_PORT);
  if (!tcpClient.connect(RECEIVER_IP, RECEIVER_PORT)) {
    Serial.println("TCP connection failed; queued data retained");
    return false;
  }
  tcpClient.setNoDelay(true);
  tcpClient.setTimeout(1000);
  Serial.println("TCP receiver connected");

  // Receiver sends a CONNECTED greeting. Consume it so it is not
  // mistaken for the ACK to the next telemetry message.
  unsigned long started = millis();
  while (millis() - started < 250) {
    while (tcpClient.available()) {
      String greeting = tcpClient.readStringUntil('\n');
      greeting.trim();
      if (greeting.length()) Serial.printf("Receiver: %s\n", greeting.c_str());
    }
    delay(2);
  }
  return true;
}

bool ackMatches(const String& line, unsigned long expectedId) {
  if (line.indexOf("\"type\":\"ACK\"") < 0 &&
      line.indexOf("\"type\": \"ACK\"") < 0) return false;
  if (line.indexOf("\"device\":\"BMS\"") < 0 &&
      line.indexOf("\"device\": \"BMS\"") < 0) return false;

  int key = line.indexOf("\"message_id\"");
  if (key < 0) return false;
  int colon = line.indexOf(':', key);
  if (colon < 0) return false;
  int pos = colon + 1;
  while (pos < (int)line.length() && line[pos] == ' ') pos++;
  unsigned long got = strtoul(line.c_str() + pos, nullptr, 10);
  return got == expectedId;
}

void sendQueuedMessage() {
  if (!flashReady || !queueHasData()) return;
  if (millis() - lastSendAttempt < SEND_RETRY_INTERVAL) return;
  lastSendAttempt = millis();

  if (!connectReceiver()) return;

  String json = peekQueue();
  if (!json.length()) return;
  unsigned long id = extractMessageId(json);
  if (id == 0) {
    Serial.println("QUEUE ERROR: message_id missing; keeping record");
    Serial.println(json);
    return;
  }

  // Drain any old lines before sending this message.
  while (tcpClient.available()) tcpClient.read();

  Serial.printf("Sending DATA message_id=%lu\n", id);
  size_t sent = tcpClient.println(json);
  if (sent == 0) {
    Serial.println("TCP write failed; queue record retained");
    tcpClient.stop();
    return;
  }

  String line;
  unsigned long started = millis();
  while (millis() - started < ACK_TIMEOUT_MS) {
    while (tcpClient.available()) {
      char c = (char)tcpClient.read();
      if (c == '\r') continue;
      if (c == '\n') {
        line.trim();
        if (line.length()) {
          Serial.printf("Receiver reply: %s\n", line.c_str());
          if (ackMatches(line, id)) {
            Serial.printf("ACK confirmed for message_id=%lu\n", id);
            if (removeFirstQueueRecord()) printQueueCount();
            return;
          }
          if (line.indexOf("\"type\":\"ERROR\"") >= 0 ||
              line.indexOf("\"type\": \"ERROR\"") >= 0) {
            Serial.println("Receiver rejected message; keeping queue record");
          }
        }
        line = "";
      } else if (line.length() < 500) {
        line += c;
      }
    }
    if (!tcpClient.connected() && !tcpClient.available()) break;
    delay(5);
  }
  Serial.printf("No matching ACK for ID %lu; record retained for retry\n", id);
  tcpClient.stop();
}

// =====================================================
// BMS DECODE HELPERS
// Assumes the previously tested Pylontech-style layout.
// =====================================================
uint32_t hexValue(const String& value) {
  return (uint32_t)strtoul(value.c_str(), nullptr, 16);
}

uint16_t readU16(const String& data, int& pos) {
  uint16_t value =
      (uint16_t)hexValue(data.substring(pos, pos + 4));

  pos += 4;
  return value;
}

float decodeTemperature(uint16_t raw) {
  return ((float)raw - 2731.0f) / 10.0f;
}

bool isHexString(const String& data) {
  for (int i = 0; i < data.length(); i++) {
    if (!isxdigit((unsigned char)data[i])) return false;
  }

  return true;
}

// =====================================================
// DECODE TELEMETRY, THEN SAVE TO FLASH
// =====================================================
void decodeAndQueue(String frame) {
  frame.trim();

  if (frame.length() < 20 || frame[0] != '~') {
    Serial.println("Invalid BMS frame");
    return;
  }

  if (frame.substring(1, 3) != "20" ||
      frame.substring(5, 7) != "46") {
    Serial.println("Unexpected BMS response header");
    Serial.println(frame);
    return;
  }

  int infoLength = (int)hexValue(frame.substring(10, 13));

  if (infoLength != 98 ||
      frame.length() < 13 + infoLength + 4) {
    Serial.println("Unexpected telemetry length");
    Serial.println(frame);
    return;
  }

  String data = frame.substring(13, 13 + infoLength);

  if (data.length() != 98 || !isHexString(data)) {
    Serial.println("Invalid telemetry data");
    return;
  }

  int p = 0;

  float voltage_V = readU16(data, p) / 1000.0f;

  int16_t currentRaw = (int16_t)readU16(data, p);
  float current_A = currentRaw / 1000.0f;

  int soc = (int)hexValue(data.substring(p, p + 2));
  p += 2;

  uint16_t averageCycles = readU16(data, p);
  uint16_t maximumCycles = readU16(data, p);

  int averageSOH = (int)hexValue(data.substring(p, p + 2));
  p += 2;

  int minimumSOH = (int)hexValue(data.substring(p, p + 2));
  p += 2;

  float highestCellV = readU16(data, p) / 1000.0f;
  uint16_t highestCellLocation = readU16(data, p);

  float lowestCellV = readU16(data, p) / 1000.0f;
  uint16_t lowestCellLocation = readU16(data, p);

  float averageCellTemp = decodeTemperature(readU16(data, p));
  float maximumCellTemp = decodeTemperature(readU16(data, p));
  uint16_t maximumCellTempLocation = readU16(data, p);

  float minimumCellTemp = decodeTemperature(readU16(data, p));
  uint16_t minimumCellTempLocation = readU16(data, p);

  float mosfetAverageTemp = decodeTemperature(readU16(data, p));
  float mosfetMaximumTemp = decodeTemperature(readU16(data, p));
  uint16_t mosfetMaximumLocation = readU16(data, p);

  float mosfetMinimumTemp = decodeTemperature(readU16(data, p));
  uint16_t mosfetMinimumLocation = readU16(data, p);

  float bmsAverageTemp = decodeTemperature(readU16(data, p));
  float bmsMaximumTemp = decodeTemperature(readU16(data, p));
  uint16_t bmsMaximumLocation = readU16(data, p);

  float bmsMinimumTemp = decodeTemperature(readU16(data, p));
  uint16_t bmsMinimumLocation = readU16(data, p);

  Serial.println("\n========== BMS TELEMETRY ==========");
  Serial.printf("Pack voltage: %.3f V\n", voltage_V);
  Serial.printf("Current: %.3f A\n", current_A);
  Serial.printf("SOC: %d %%\n", soc);
  Serial.printf("Average cycles: %u\n", averageCycles);
  Serial.printf("Maximum cycles: %u\n", maximumCycles);
  Serial.printf("Average SOH: %d %%\n", averageSOH);
  Serial.printf("Minimum SOH: %d %%\n", minimumSOH);

  Serial.printf("Highest cell: %.3f V (location %u)\n",
                highestCellV, highestCellLocation);

  Serial.printf("Lowest cell: %.3f V (location %u)\n",
                lowestCellV, lowestCellLocation);

  Serial.printf("Average cell temperature: %.1f C\n", averageCellTemp);

  Serial.printf("Maximum cell temperature: %.1f C (location %u)\n",
                maximumCellTemp, maximumCellTempLocation);

  Serial.printf("Minimum cell temperature: %.1f C (location %u)\n",
                minimumCellTemp, minimumCellTempLocation);

  Serial.printf("MOSFET average temperature: %.1f C\n", mosfetAverageTemp);

  Serial.printf("MOSFET maximum temperature: %.1f C (location %u)\n",
                mosfetMaximumTemp, mosfetMaximumLocation);

  Serial.printf("MOSFET minimum temperature: %.1f C (location %u)\n",
                mosfetMinimumTemp, mosfetMinimumLocation);

  Serial.printf("BMS average temperature: %.1f C\n", bmsAverageTemp);

  Serial.printf("BMS maximum temperature: %.1f C (location %u)\n",
                bmsMaximumTemp, bmsMaximumLocation);

  Serial.printf("BMS minimum temperature: %.1f C (location %u)\n",
                bmsMinimumTemp, bmsMinimumLocation);

  Serial.println("===================================");

  unsigned long id = nextMessageId++;

  char json[1500];

  snprintf(
    json, sizeof(json),
    "{\"type\":\"DATA\",\"device\":\"BMS\","
    "\"message_id\":%lu,"
    "\"voltage_V\":%.3f,"
    "\"current_A\":%.3f,"
    "\"soc_percent\":%d,"
    "\"average_cycles\":%u,"
    "\"maximum_cycles\":%u,"
    "\"average_soh_percent\":%d,"
    "\"minimum_soh_percent\":%d,"
    "\"highest_cell_voltage_V\":%.3f,"
    "\"highest_cell_location\":%u,"
    "\"lowest_cell_voltage_V\":%.3f,"
    "\"lowest_cell_location\":%u,"
    "\"average_cell_temperature_C\":%.1f,"
    "\"maximum_cell_temperature_C\":%.1f,"
    "\"maximum_cell_temperature_location\":%u,"
    "\"minimum_cell_temperature_C\":%.1f,"
    "\"minimum_cell_temperature_location\":%u,"
    "\"mosfet_average_temperature_C\":%.1f,"
    "\"mosfet_maximum_temperature_C\":%.1f,"
    "\"mosfet_maximum_temperature_location\":%u,"
    "\"mosfet_minimum_temperature_C\":%.1f,"
    "\"mosfet_minimum_temperature_location\":%u,"
    "\"bms_average_temperature_C\":%.1f,"
    "\"bms_maximum_temperature_C\":%.1f,"
    "\"bms_maximum_temperature_location\":%u,"
    "\"bms_minimum_temperature_C\":%.1f,"
    "\"bms_minimum_temperature_location\":%u}",

    id,
    voltage_V, current_A, soc,
    averageCycles, maximumCycles, averageSOH, minimumSOH,
    highestCellV, highestCellLocation,
    lowestCellV, lowestCellLocation,
    averageCellTemp, maximumCellTemp, maximumCellTempLocation,
    minimumCellTemp, minimumCellTempLocation,
    mosfetAverageTemp, mosfetMaximumTemp, mosfetMaximumLocation,
    mosfetMinimumTemp, mosfetMinimumLocation,
    bmsAverageTemp, bmsMaximumTemp, bmsMaximumLocation,
    bmsMinimumTemp, bmsMinimumLocation
  );

  // Persist FIRST. Network transmission happens later.
  if (enqueueJSON(json)) {
    printQueueCount();
  }
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(RS485_DE, OUTPUT);
  digitalWrite(RS485_DE, LOW);

  BMS.begin(BMS_BAUD, SERIAL_8N1, RS485_RX, RS485_TX);

  Serial.println("\n==================================");
  Serial.println("BMS OFFLINE-CAPABLE TRANSMITTER");
  Serial.println("Polling interval: 5 seconds");
  Serial.println("Persistent queue: LittleFS");
  Serial.println("Receiver: 192.168.4.1:5000 (TCP JSON lines)");
  Serial.println("==================================");

  // Format only if LittleFS has never been initialized.
  // Do not format on normal startup: that erases queued data.
  flashReady = LittleFS.begin(false);

  if (!flashReady) {
    Serial.println("LittleFS mount failed; trying first-time format");

    if (LittleFS.begin(true)) {
      flashReady = true;
      Serial.println("LittleFS initialized");
    } else {
      Serial.println("FATAL: flash queue unavailable");
    }
  }

  if (flashReady) {
    recoverMessageId();
    printQueueCount();
  }

  startWiFi();

  lastPoll = millis() - POLL_INTERVAL;
}

// =====================================================
// MAIN LOOP
// =====================================================
void loop() {
  maintainWiFi();

  // Poll BMS every 5 seconds independently of Wi-Fi.
  if (millis() - lastPoll >= POLL_INTERVAL) {
    lastPoll = millis();

    while (BMS.available()) {
      BMS.read();
    }

    Serial.println("\nRequesting BMS telemetry...");

    digitalWrite(RS485_DE, HIGH);
    delay(2);

    BMS.print(BMS_REQUEST);
    BMS.flush();

    delay(2);
    digitalWrite(RS485_DE, LOW);
  }

  // Collect the BMS response frame.
  while (BMS.available()) {
    char c = (char)BMS.read();

    if (c == '~') {
      responseFrame = "~";
    } else if (responseFrame.length() > 0) {
      if (c == '\r' || c == '\n') {
        if (responseFrame.length() > 1) {
          Serial.print("Raw BMS frame: ");
          Serial.println(responseFrame);

          decodeAndQueue(responseFrame);
        }

        responseFrame = "";
      } else if (responseFrame.length() < 300) {
        responseFrame += c;
      } else {
        Serial.println("BMS frame too long; discarded");
        responseFrame = "";
      }
    }
  }

  // Send one queued message at a time.
  // If offline or unacknowledged, retain it in flash.
  sendQueuedMessage();
}