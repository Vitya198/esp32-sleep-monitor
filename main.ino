#include <Wire.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <math.h>
#include <driver/i2s.h>
#include <BH1750.h>
#include "Adafruit_TCS34725.h"
#include <Adafruit_AHTX0.h>
#include <DFRobot_ENS160.h>
#include <ArduinoJson.h>
#include <vector>
#include <functional>
#include <Arduino.h>

/* ----------------------------------------------------------------------------------------------------------- */
/** GLOBAL CONFIGURATION **/
/* ----------------------------------------------------------------------------------------------------------- */
const char* ssid = "hotspotName";
const char* password = "hotspotPassword";
const char* serverUrl = "http://192.168.119.122:5000/data";

bool ecoMode = false;
unsigned long lastEcoCheck = 0;
#define ECO_CHECK_INTERVAL 300000

#define LOG_INTERVAL_NORMAL 300000
#define LOG_INTERVAL_BOOST 1000
unsigned long lastLogTime = 0;

#define ENS160_WARMUP_TIME 30000
#define FALLBACK_TIME 300000

unsigned long tempInterval = 1800000;
unsigned long gasInterval = 60000;
unsigned long lightInterval = 60000;
unsigned long colorInterval = 60000;

int luxThreshold = 50;
int colorThreshold = 10;
int eco2Threshold = 100;
int tvocThreshold = 100;

#define I2C_SDA 21
#define I2C_SCL 22
#define TCS_LED_PIN 4
#define I2S_WS 25
#define I2S_SCK 26
#define I2S_DATA 33
#define I2S_SAMPLE_RATE 16000
const int bufferLength = 1024;
int16_t i2sBuffer[bufferLength];

BH1750 lightSensor(0x23);
Adafruit_TCS34725 colorSensor = Adafruit_TCS34725(TCS34725_INTEGRATIONTIME_50MS, TCS34725_GAIN_4X);
Adafruit_AHTX0 ahtSensor;
DFRobot_ENS160_I2C ENS160(&Wire, 0x53);

float tempC = NAN, humidityRH = NAN;
float luxLevel = NAN;
uint16_t r = 0, g = 0, b = 0, c = 0;
uint16_t eco2 = 0, tvoc = 0;
uint8_t ens160Status = 255;

#define NOISE_FAST_INTERVAL 500
#define NOISE_BOOST_DURATION 30000
#define NOISE_HYSTERESIS 5
#define NOISE_BASE_LEVEL 10
#define NOISE_MIN_DELTA 15
#define BASELINE_UPDATE_INTERVAL 600000
#define MIC_BASELINE_INIT_TIME 60000
#define NOISE_SEND_INTERVAL 5000
float noiseLevel = NAN, micBaseline = 0.0, lastNoise = 0.0;
bool noiseEvent = false;
unsigned long lastNoiseRead = 0, noiseSampleInterval = 10000, noiseBoostUntil = 0, baselineUpdateTime = 0;

unsigned long lastSendTime = 0;
uint16_t lastCO2 = 0, lastTVOC = 0;
float lastLux = -1;
uint16_t lastR = 0, lastG = 0, lastB = 0;


struct SensorTask {
  const char* name;
  unsigned long interval;
  unsigned long lastRun;
  std::function<void(unsigned long)> callback;
};

std::string createMsgPackPayload() {
  StaticJsonDocument<384> doc; // kicsit nagyobb, de még mindig hatékony
  doc["temp"] = tempC;
  doc["humidity"] = humidityRH;
  doc["lux"] = luxLevel;
  doc["r"] = r;
  doc["g"] = g;
  doc["b"] = b;
  doc["eco2"] = eco2;
  doc["tvoc"] = tvoc;
  doc["ens160Status"] = ens160Status;
  doc["noise"] = noiseLevel;

  std::string output;
  if (serializeMsgPack(doc, output) == 0) {
    Serial.println("MsgPack serialization failed!");
  }
  return output;
}

void sendDataUnified(bool reconnect) {
  if (reconnect) connectWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ Nincs WiFi – nem tudunk adatot küldeni.");
    return;
  }

  HTTPClient http;
  if (!http.begin(serverUrl)) {
    Serial.println("❌ HTTP begin hiba!");
    return;
  }

  http.addHeader("Content-Type", "application/msgpack");
  http.addHeader("Authorization", "Bearer alvashozTitkosToken123");

  // Itt hozod létre a payload-ot!
  std::string msgpackPayload = createMsgPackPayload();

  // Másolat készítése írható vektorként
  std::vector<uint8_t> buffer(msgpackPayload.begin(), msgpackPayload.end());
  int responseCode = http.sendRequest("POST", buffer.data(), buffer.size());

  if (responseCode > 0) {
    Serial.println("✅ Adat elküldve, válaszkód: " + String(responseCode));
  } else {
    Serial.println("❌ HTTP POST hiba: " + String(http.errorToString(responseCode)));
  }

  http.end();
  if (reconnect) disableWiFi();
}


// A többi rész változatlan marad...


void connectWiFi() {
  WiFi.begin(ssid, password);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(200);
  }
}

void disableWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  btStop();
}

void fetchConfigFromServer() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin("http://192.168.119.122:5000/config");
    int code = http.GET();
    if (code == 200) {
      String payload = http.getString();
      DynamicJsonDocument doc(1024);
      deserializeJson(doc, payload);
      if (doc.containsKey("temp_interval")) tempInterval = doc["temp_interval"];
      if (doc.containsKey("gas_interval")) gasInterval = doc["gas_interval"];
      if (doc.containsKey("light_interval")) lightInterval = doc["light_interval"];
      if (doc.containsKey("color_interval")) colorInterval = doc["color_interval"];
      if (doc.containsKey("lux_threshold")) luxThreshold = doc["lux_threshold"];
      if (doc.containsKey("color_threshold")) colorThreshold = doc["color_threshold"];
      if (doc.containsKey("eco2_threshold")) eco2Threshold = doc["eco2_threshold"];
      if (doc.containsKey("tvoc_threshold")) tvocThreshold = doc["tvoc_threshold"];
    }
    http.end();
  }
  applyEcoIntervals();
}

unsigned long lastEcoFetch = 0;
#define ECO_FETCH_INTERVAL 60000  // 60 másodpercenként
void maybeFetchEcoStatus(unsigned long now) {
  if (now - lastEcoFetch < ECO_FETCH_INTERVAL) return;

  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin("http://192.168.119.122:5000/eco_status");
    int code = http.GET();

    if (code == 200) {
      String payload = http.getString();
      DynamicJsonDocument doc(256);
      DeserializationError err = deserializeJson(doc, payload);
      if (!err && doc.containsKey("eco_mode")) {
        bool newEcoMode = doc["eco_mode"];
        if (newEcoMode != ecoMode) {
          ecoMode = newEcoMode;
          Serial.println("🔄 Eco mód frissítve szerverről: " + String(ecoMode ? "BE" : "KI"));
          applyEcoIntervals();
        }
      }
    } else {
      Serial.println("⚠️ eco_status lekérdezés sikertelen: HTTP " + String(code));
    }

    http.end();
    if (ecoMode) disableWiFi();
  }

  lastEcoFetch = now;
}


unsigned long lastConfigFetch = 0;
#define CONFIG_FETCH_INTERVAL 600000
void maybeFetchConfig(unsigned long now) {
  if (now - lastConfigFetch < CONFIG_FETCH_INTERVAL) return;

  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin("http://192.168.119.122:5000/config");
    int code = http.GET();

    if (code == 200) {
      String payload = http.getString();
      DynamicJsonDocument doc(1024);
      DeserializationError err = deserializeJson(doc, payload);

      if (err) {
        Serial.println("⚠️ JSON parsing hiba: " + String(err.c_str()));
      } else {
        if (doc.containsKey("temp_interval")) tempInterval = doc["temp_interval"];
        if (doc.containsKey("gas_interval")) gasInterval = doc["gas_interval"];
        if (doc.containsKey("light_interval")) lightInterval = doc["light_interval"];
        if (doc.containsKey("color_interval")) colorInterval = doc["color_interval"];
        if (doc.containsKey("lux_threshold")) luxThreshold = doc["lux_threshold"];
        if (doc.containsKey("color_threshold")) colorThreshold = doc["color_threshold"];
        if (doc.containsKey("eco2_threshold")) eco2Threshold = doc["eco2_threshold"];
        if (doc.containsKey("tvoc_threshold")) tvocThreshold = doc["tvoc_threshold"];

        Serial.println("🟢 Konfiguráció frissítve a szerverről.");
        lastConfigFetch = now;  // csak akkor frissítjük, ha sikerült
      }
    } else {
      Serial.println("⚠️ Konfiguráció lekérés sikertelen: HTTP " + String(code));
    }

    http.end();
    if (ecoMode) disableWiFi();
  } else {
    Serial.println("⚠️ Nincs WiFi – nem lehet konfigurációt lekérni.");
  }
   applyEcoIntervals();
}
void applyEcoIntervals() {
  if (ecoMode) {
    tempInterval = 3600000;     // 1 óra
    gasInterval = 300000;       // 5 perc
    lightInterval = 300000;
    colorInterval = 300000;
  } else {
    tempInterval = 1800000;     // 30 perc
    gasInterval = 60000;
    lightInterval = 60000;
    colorInterval = 60000;
  }
}

void checkAndSend() {
  // Érvénytelen adatok szűrése
  if (isnan(tempC) || isnan(humidityRH) || isnan(noiseLevel) || isnan(luxLevel)) {
    Serial.println("‼️ Hibás szenzoradat – nem küldünk");
    return;
  }

  bool send = false;
  unsigned long now = millis();

  // Gázszenzor: CO2 és TVOC
  if (abs((int)eco2 - (int)lastCO2) > eco2Threshold) {
    if (eco2 > 5000 || eco2 < 400) {
      Serial.println("⚠️ Szélsőséges CO2 érték detektálva: " + String(eco2));
    }
    lastCO2 = eco2;
    send = true;
  }

  if (abs((int)tvoc - (int)lastTVOC) > tvocThreshold) {
    lastTVOC = tvoc;
    send = true;
  }

  // Fény
  if (abs(luxLevel - lastLux) > luxThreshold) {
    lastLux = luxLevel;
    send = true;
  }

  // Szín
  if (abs((int)r - (int)lastR) > colorThreshold ||
      abs((int)g - (int)lastG) > colorThreshold ||
      abs((int)b - (int)lastB) > colorThreshold) {
    lastR = r;
    lastG = g;
    lastB = b;
    send = true;
  }

  // Zajesemény vagy túl rég nem küldtünk
  if (noiseEvent) {
    Serial.println("🔊 Zajesemény – azonnali adatküldés");
    send = true;
  }

  if ((now - lastSendTime) > FALLBACK_TIME) {
    Serial.println("⌛ Biztonsági újraküldés");
    send = true;
  }

// Normál módban mindig küldünk
if (!ecoMode) {
  sendDataUnified(false);  // reconnect nem kell
  lastSendTime = now;
  return;
}

// Eco módban: csak akkor, ha indokolt
if (send) {
  sendDataUnified(true);  // eco módban reconnect kellhet
  lastSendTime = now;
}

}
void updateNoiseSensor(unsigned long now) {
  if (now - lastNoiseRead < noiseSampleInterval) return;
  lastNoiseRead = now;

  size_t bytesRead = 0;
  esp_err_t result = i2s_read(I2S_NUM_0, &i2sBuffer, sizeof(i2sBuffer), &bytesRead, portMAX_DELAY);

  if (result != ESP_OK || bytesRead == 0) {
    Serial.println("⚠️ I2S olvasási hiba vagy 0 bájt");
    return;
  }

  int samples = bytesRead / 2;
  float sum = 0;
  for (int i = 0; i < samples; i++) {
    float sample = (float)i2sBuffer[i];
    sum += sample * sample;
  }

  float rms = sqrt(sum / samples);
  if (rms < 1e-3) rms = 1e-3; // védelem log10(0) ellen
  float db = 20.0 * log10(rms);
  if (!isfinite(db)) db = 0;

  noiseLevel = db;

  // Baseline frissítés
  if (isnan(micBaseline) && now > MIC_BASELINE_INIT_TIME) {
    micBaseline = noiseLevel;
    baselineUpdateTime = now;
    Serial.println("🔧 Mikrofonszint alapérték inicializálva");
  }

  bool aboveBase = noiseLevel > micBaseline + NOISE_BASE_LEVEL;
  bool belowBase = noiseLevel < micBaseline - NOISE_BASE_LEVEL;
  bool significant = abs(noiseLevel - lastNoise) > NOISE_MIN_DELTA;
  noiseEvent = (aboveBase || belowBase) && significant;


  if (noiseEvent) {
    noiseSampleInterval = NOISE_FAST_INTERVAL;
    noiseBoostUntil = now + NOISE_BOOST_DURATION;
  } else if (now > noiseBoostUntil) {
    if (micBaseline - noiseLevel > NOISE_HYSTERESIS) {
      noiseSampleInterval = 10000;
    }
  }

  // Lassú baseline update
  if (!noiseEvent && now - baselineUpdateTime >= BASELINE_UPDATE_INTERVAL) {
    float delta = abs(noiseLevel - micBaseline);
    if (delta < 1.0) {
      micBaseline = 0.98 * micBaseline + 0.02 * noiseLevel;
      baselineUpdateTime = now;
    }
  }

  // Külön küldés esemény esetén
  if (noiseEvent && now - lastLogTime > NOISE_SEND_INTERVAL) {
    sendDataUnified(ecoMode);
    lastLogTime = now;
  }

  lastNoise = noiseLevel;
}

void runTempSensor(unsigned long now) {
  sensors_event_t tempEvent, humidityEvent;
  ahtSensor.getEvent(&humidityEvent, &tempEvent);

  if (!isnan(tempEvent.temperature)) tempC = tempEvent.temperature;
  if (!isnan(humidityEvent.relative_humidity)) humidityRH = humidityEvent.relative_humidity;
}


void runLightSensor(unsigned long now) {
  float lux = lightSensor.readLightLevel();
  if (lux >= 0) luxLevel = lux;
}

void runColorSensor(unsigned long now) {
  static unsigned long lastEnableTime = 0;
  static bool isEnabled = false;

  if (!isEnabled) {
    colorSensor.enable();
    lastEnableTime = now;
    isEnabled = true;
    return;  // engedélyezés után visszatér, majd legközelebb olvas
  }

  if (now - lastEnableTime >= 100) {
    colorSensor.getRawData(&r, &g, &b, &c);
    colorSensor.disable();
    isEnabled = false;
  }
}

void runGasSensor(unsigned long now) {
  if (ecoMode) {
   ENS160.setPWRMode(ENS160_SLEEP_MODE);
    return;  // eco módban nem mérünk, csak alvásba tesszük
  }

  ENS160.setPWRMode(ENS160_STANDARD_MODE);  // visszakapcsolás mérés előtt
  eco2 = ENS160.getECO2();
  tvoc = ENS160.getTVOC();
  ens160Status = ENS160.getENS160Status();
}

void maybeDisableWiFi() {
  if (ecoMode && WiFi.status() == WL_CONNECTED) {
    Serial.println("🌙 Eco mód: Wi-Fi kikapcsolása");
    disableWiFi();
  }
}

std::vector<SensorTask> sensorTasks = {
  {"temperature", tempInterval + 1, 0, runTempSensor},
  {"light", lightInterval + 1, 0, runLightSensor},
  {"gas", gasInterval + 1, 0, runGasSensor},
  {"color", colorInterval + 1, 0, runColorSensor}
};


void setup() {
  Serial.begin(115200);
  Serial.flush();

  pinMode(TCS_LED_PIN, OUTPUT);
  digitalWrite(TCS_LED_PIN, LOW);  // Csak akkor kapcsoljuk fel, ha mérünk

  Wire.begin(I2C_SDA, I2C_SCL);

  if (!lightSensor.begin()) Serial.println("⚠️ BH1750 nem válaszol.");
  else lightSensor.configure(BH1750::CONTINUOUS_HIGH_RES_MODE);

  if (!colorSensor.begin()) Serial.println("⚠️ TCS34725 nem válaszol.");

  if (!ahtSensor.begin()) Serial.println("⚠️ AHT10/AHT20 nem válaszol.");

  if (!ENS160.begin()) Serial.println("⚠️ ENS160 nem inicializálható.");
  else ENS160.setPWRMode(ENS160_STANDARD_MODE);

  connectWiFi();
  fetchConfigFromServer();

  // I2S beállítás
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = I2S_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 64  // megnövelve a 32-ről, stabilabb lehet
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_DATA
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);

  unsigned long now = millis();
  updateNoiseSensor(now);

  for (auto& task : sensorTasks) {
    task.callback(now);  // kezdeti lekérés
    task.lastRun = now;
  }

  sendDataUnified(ecoMode);
  lastLogTime = now;
  if (ecoMode) {
  setCpuFrequencyMhz(80);  // normál: 240 MHz → 80 MHz-re csökkentés
  Serial.println("⚡ CPU frekvencia eco módban 80 MHz-re állítva");
}

}


void loop() {
  unsigned long now = millis();

  maybeFetchConfig(now);
  maybeFetchEcoStatus(now);

  for (auto& task : sensorTasks) {
    unsigned long currentInterval = 0;
    if (strcmp(task.name, "temperature") == 0) currentInterval = tempInterval;
    else if (strcmp(task.name, "light") == 0) currentInterval = lightInterval;
    else if (strcmp(task.name, "gas") == 0) currentInterval = gasInterval;
    else if (strcmp(task.name, "color") == 0) currentInterval = colorInterval;

    if (now - task.lastRun >= currentInterval) {
      task.callback(now);
      task.lastRun = now;
    }
  }

  updateNoiseSensor(now);

  unsigned long logInterval = (now < noiseBoostUntil) ? LOG_INTERVAL_BOOST : LOG_INTERVAL_NORMAL;
  if (now - lastLogTime >= logInterval) {
    sendDataUnified(ecoMode);
    lastLogTime = now;
  }
  checkAndSend();
  maybeDisableWiFi();

  delay(10);
}