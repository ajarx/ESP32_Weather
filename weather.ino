// weather.ino - ESP32-C3 + QWeather (gzip) -> LittleFS -> ESP32-targz 解压 -> OLED 显示

// 使用 LittleFS 作为目标文件系统（ESP32-targz 要求）
#define DEST_FS_USES_LITTLEFS
#include <ESP32-targz.h>

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <LittleFS.h>

// OLED via SW I2C on SDA=6, SCL=7
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, /* clock=*/7, /* data=*/6, /* reset=*/ U8X8_PIN_NONE);

// WiFi credentials (you provided)
const char* ssid     = "wifi_ssid";
const char* password = "wifi_password";

// QWeather API Key (you provided)
const char* apiKey = "your_api_key";
const char* apiHost = "your_api_host";

// Shanghai coords
const float lat = 31.2304;
const float lon = 121.4737;

// Update interval (10 minutes)
const unsigned long updateInterval = 10UL * 60UL * 1000UL;
unsigned long lastUpdate = 0;

// helper: save HTTPS response stream to file (binary)
bool fetchAndSaveGzipHTTPS(const String &url, const char *dstPath) {
  Serial.printf("Fetching: %s\n", url.c_str());

  WiFiClientSecure client;
  client.setInsecure(); // dev only; in production provide root certs

  HTTPClient https;
  if (!https.begin(client, url)) {
    Serial.println("HTTPS begin failed");
    return false;
  }

  // ask for gzip (server may return gzip)
  https.addHeader("Accept-Encoding", "gzip");

  int code = https.GET();
  if (code <= 0) {
    Serial.printf("HTTPS GET failed, code=%d\n", code);
    https.end();
    return false;
  }

  Serial.printf("HTTP status: %d\n", code);

  // open file for write
  File f = LittleFS.open(dstPath, "w");
  if (!f) {
    Serial.println("Failed to open file for writing");
    https.end();
    return false;
  }

  // read raw stream and write to file
  WiFiClient *stream = https.getStreamPtr();
  const size_t bufSize = 256;
  uint8_t buf[bufSize];
  unsigned long lastRead = millis();
  // read until connection closed and no more data
  while (https.connected() || stream->available()) {
    while (stream->available()) {
      size_t r = stream->readBytes(buf, bufSize);
      if (r > 0) {
        f.write(buf, r);
        lastRead = millis();
      }
    }
    // safety timeout for stalled connections
    if (millis() - lastRead > 8000) break;
    delay(1);
  }

  f.close();
  https.end();

  // sanity: file size
  File check = LittleFS.open(dstPath, "r");
  if (!check) {
    Serial.println("Saved gzip file open error");
    return false;
  }
  size_t sz = check.size();
  check.close();
  Serial.printf("Saved %s (%u bytes)\n", dstPath, (unsigned int)sz);
  return (sz > 0);
}

// decompress gz file -> json file using ESP32-targz GzUnpacker
String decompressGzipToString(const char* gzPath, const char* outJsonPath) {
  // ensure gz exists
  File fg = LittleFS.open(gzPath, "r");
  if (!fg) {
    Serial.printf("gzip file %s not found\n", gzPath);
    return String();
  }
  fg.close();

  // remove old output if exists
  if (LittleFS.exists(outJsonPath)) LittleFS.remove(outJsonPath);

  GzUnpacker *GZ = new GzUnpacker();
  GZ->haltOnError(true);

  Serial.printf("Ungzipping %s -> %s\n", gzPath, outJsonPath);
  bool ok = GZ->gzExpander(LittleFS, gzPath, LittleFS, outJsonPath);
  int err = GZ->tarGzGetError();
  delete GZ;

  if (!ok) {
    Serial.printf("gzExpander failed, code=%d\n", err);
    return String();
  }

  // read decompressed file
  File fj = LittleFS.open(outJsonPath, "r");
  if (!fj) {
    Serial.println("Failed opening decompressed json");
    return String();
  }
  String json = fj.readString();
  fj.close();
  return json;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // init LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    while (1) delay(1000);
  }

  // init display
  u8g2.begin();
  //u8g2.setFont(u8g2_font_wqy12_t_gb2312);

  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.clearBuffer();
  u8g2.drawStr(0, 12, "Connecting WiFi...");
  u8g2.sendBuffer();

  // connect WiFi
  WiFi.begin(ssid, password);
  Serial.printf("Connecting to %s\n", ssid);
  int waitCount = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++waitCount > 120) { // timeout ~60s
      Serial.println("\nWiFi connect timeout");
      break;
    }
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("WiFi not connected");
  }

  u8g2.clearBuffer();
  if (WiFi.status() == WL_CONNECTED) u8g2.drawStr(0, 12, "WiFi Connected!");
  else u8g2.drawStr(0, 12, "WiFi Failed");
  u8g2.sendBuffer();
  delay(800);
}

void loop() {
  if (millis() - lastUpdate > updateInterval || lastUpdate == 0) {
    fetchAndDisplay();
    lastUpdate = millis();
  }
  delay(200);
}

String convertCondition(const String &cn) {
  if (cn == "晴") return "Sunny";
  if (cn == "多云") return "Cloudy";
  if (cn == "小雨") return "LightRain";
  if (cn == "霾") return "Haze";
  if (cn == "阴") return "Overcast";
  return cn;
}

String convertAQICategory(const String &cn) {
  if (cn == "优") return "Good";
  if (cn == "良") return "Fair";
  if (cn == "轻度污染") return "MildPoll";
  if (cn == "中度污染") return "ModPoll";
  if (cn == "重度污染") return "HeavyPoll";
  return cn;
}

void fetchAndDisplay() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("No WiFi");
    return;
  }

  // prepare urls (QWeather v7 endpoints)
  String weatherUrl = "https://" + String(apiHost) + "/v7/weather/now?location="  + String(lon) + "," + String(lat) + "&key=" + String(apiKey);
  String aqiUrl = "https://" + String(apiHost) + "/airquality/v1/current/" + String(lat) + "/" + String(lon) + "?key=" + String(apiKey);
  // TEMP files
  const char* gzWeather = "/weather.gz";
  const char* gzAQI     = "/aqi.gz";
  const char* jsonWeather = "/weather.json";
  const char* jsonAQI     = "/aqi.json";

  // remove old files
  if (LittleFS.exists(gzWeather)) LittleFS.remove(gzWeather);
  if (LittleFS.exists(gzAQI)) LittleFS.remove(gzAQI);
  if (LittleFS.exists(jsonWeather)) LittleFS.remove(jsonWeather);
  if (LittleFS.exists(jsonAQI)) LittleFS.remove(jsonAQI);

  u8g2.clearBuffer();
  u8g2.drawStr(0, 12, "Request Weather...");
  u8g2.sendBuffer();
  // fetch and save gzip streams
  bool okW = fetchAndSaveGzipHTTPS(weatherUrl, gzWeather);
  u8g2.clearBuffer();
  u8g2.drawStr(0, 12, "Request AQI...");
  u8g2.sendBuffer();
  bool okA = fetchAndSaveGzipHTTPS(aqiUrl, gzAQI);

  if (!okW) {
    Serial.println("Failed to fetch weather gzip");
    u8g2.clearBuffer();
    u8g2.drawStr(0,12,"Weather fetch failed");
    u8g2.sendBuffer();
    return;
  }
  if (!okA) {
    Serial.println("Failed to fetch aqi gzip");
    u8g2.clearBuffer();
    u8g2.drawStr(0,12,"AQI fetch failed");
    u8g2.sendBuffer();
    return;
  }

  // decompress gzip -> json
  u8g2.clearBuffer();
  u8g2.drawStr(0, 12, "Decompress Response...");
  u8g2.sendBuffer();
  String weatherJSON = decompressGzipToString(gzWeather, jsonWeather);
  String aqiJSON = decompressGzipToString(gzAQI, jsonAQI);

  Serial.println("=== Weather JSON ===");
  Serial.println(weatherJSON);
  Serial.println("=== AQI JSON ===");
  Serial.println(aqiJSON);

  // parse JSON
  float temp = 0.0;
  int humidity = 0;
  String cond = "";
  int aqi = -1;
  String aqiCategory = "";

  if (weatherJSON.length() > 10) {
    StaticJsonDocument<1024> docW;
    auto err = deserializeJson(docW, weatherJSON);
    if (!err) {
      // QWeather v7/weather/now -> "now"
      if (docW.containsKey("now")) {
        temp = docW["now"]["temp"].as<float>();
        humidity = docW["now"]["humidity"].as<int>();
        cond = docW["now"]["text"].as<const char*>();
      }
    } else {
      Serial.print("Weather JSON parse error: ");
      Serial.println(err.c_str());
    }
  }

  if (aqiJSON.length() > 10) {
    // AQI JSON can be large — use dynamic allocation
    const size_t cap = 16 * 1024; // 16 KB
    DynamicJsonDocument docA(cap);
    auto err = deserializeJson(docA, aqiJSON);
    if (!err) {
      // Try v7 style: now.aqi
      if (docA.containsKey("now") && docA["now"].containsKey("aqi")) {
        // some responses keep aqi as number, some as string
        if (docA["now"]["aqi"].is<const char*>()) {
          aqi = atoi(docA["now"]["aqi"].as<const char*>());
        } else {
          aqi = docA["now"]["aqi"].as<int>();
        }
        if (docA["now"].containsKey("category")) aqiCategory = String((const char*)docA["now"]["category"].as<const char*>());
      }
      // Try airquality v1 style: indexes[0].aqi
      else if (docA.containsKey("indexes") && docA["indexes"].is<JsonArray>() && docA["indexes"].size() > 0) {
        JsonObject idx0 = docA["indexes"][0];
        if (idx0.containsKey("aqi")) {
          aqi = idx0["aqi"].as<int>();
        }
        if (idx0.containsKey("category")) {
          // category might be Chinese; convert to String
          aqiCategory = String((const char*)idx0["category"].as<const char*>());
        } else if (idx0.containsKey("name")) {
          aqiCategory = String((const char*)idx0["name"].as<const char*>());
        }
      } else if (docA.containsKey("data") && docA["data"].containsKey("aqi")) { // some variants
        aqi = docA["data"]["aqi"].as<int>();
      } else {
        Serial.println("AQI JSON: unknown structure");
      }
    } else {
      Serial.print("AQI JSON parse error: ");
      Serial.println(err.c_str());
    }
  } else {
    Serial.println("AQI JSON empty");
  }
  // Debug prints
  Serial.printf("Parsed: temp=%.1f humidity=%d cond='%s' aqi=%d aqiCategory='%s'\n",
                temp, humidity, cond.c_str(), aqi, aqiCategory.c_str());
  cond = convertCondition(cond);
  aqiCategory = convertAQICategory(aqiCategory);

  // display on OLED
  u8g2.clearBuffer();
  u8g2.drawStr(0, 10, "Shanghai Weather");
  u8g2.drawStr(0, 22, (String("Temp: ") + String(temp, 1) + " C").c_str());
  u8g2.drawStr(0, 32, (String("Humidity: ") + String(humidity) + " %").c_str());
  if (aqi >= 0) {
    u8g2.drawStr(0, 44, (String("AQI: ") + String(aqi) + " " + aqiCategory).c_str());
  } else {
    u8g2.drawStr(0, 44, "AQI: N/A");
  }
  u8g2.drawStr(0, 54, (String("Cond: ") + cond).c_str());
  u8g2.sendBuffer();

}
