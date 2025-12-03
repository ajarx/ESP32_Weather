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
// --------------------- 16x16 icons (monochrome XBM-like arrays) ---------------------
// Each icon is 16x16 = 32 bytes (LSB first per byte). These are simple pixel patterns.
// You can replace them with better bitmaps if you like.

static const unsigned char icon_sunny_bits[] U8X8_PROGMEM = {
  0x00,0x00, 0x10,0x04, 0x10,0x04, 0x38,0x0E,
  0x7C,0x3E, 0xFE,0x7F, 0xFE,0x7F, 0x7C,0x3E,
  0x38,0x0E, 0x10,0x04, 0x10,0x04, 0x00,0x00,
  0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00
};

static const unsigned char icon_partly_bits[] U8X8_PROGMEM = {
  0x00,0x00, 0x1C,0x07, 0x36,0x0C, 0x63,0x18,
  0xE3,0x38, 0xC3,0x30, 0xC3,0x30, 0x63,0x18,
  0x36,0x0C, 0x1C,0x07, 0x00,0x00, 0x00,0x00,
  0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00
};

static const unsigned char icon_cloudy_bits[] U8X8_PROGMEM = {
  0x00,0x00, 0x00,0x00, 0x3C,0x0F, 0x7E,0x1F,
  0xFF,0x3F, 0xFF,0x3F, 0x7E,0x1F, 0x1C,0x07,
  0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00,
  0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00
};

static const unsigned char icon_rain_bits[] U8X8_PROGMEM = {
  0x00,0x00, 0x38,0x0E, 0x7C,0x3E, 0xFE,0x7F,
  0xFF,0x3F, 0x7E,0x1F, 0x38,0x0E, 0x00,0x00,
  0x10,0x04, 0x28,0x0A, 0x10,0x04, 0x28,0x0A,
  0x10,0x04, 0x00,0x00, 0x00,0x00, 0x00,0x00
};

static const unsigned char icon_thunder_bits[] U8X8_PROGMEM = {
  0x00,0x00, 0x10,0x04, 0x38,0x0E, 0x7C,0x3E,
  0xFE,0x7F, 0x7C,0x3E, 0x38,0x0E, 0x10,0x04,
  0x08,0x02, 0x18,0x06, 0x30,0x0C, 0x18,0x06,
  0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00
};

static const unsigned char icon_snow_bits[] U8X8_PROGMEM = {
  0x00,0x00, 0x24,0x09, 0x12,0x09, 0x7F,0x3E,
  0x7F,0x3E, 0x12,0x09, 0x24,0x09, 0x00,0x00,
  0x00,0x00, 0x24,0x09, 0x12,0x09, 0x00,0x00,
  0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00
};

static const unsigned char icon_fog_bits[] U8X8_PROGMEM = {
  0x00,0x00, 0x00,0x00, 0x7E,0x1F, 0x00,0x00,
  0x7E,0x1F, 0x00,0x00, 0x7E,0x1F, 0x00,0x00,
  0x00,0x00, 0x7E,0x1F, 0x00,0x00, 0x00,0x00,
  0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00
};

static const unsigned char icon_haze_bits[] U8X8_PROGMEM = {
  0x00,0x00, 0x3C,0x0F, 0x66,0x19, 0xC3,0x30,
  0xFF,0x3F, 0xC3,0x30, 0x66,0x19, 0x3C,0x0F,
  0x00,0x00, 0x00,0x00, 0x18,0x06, 0x18,0x06,
  0x3C,0x0F, 0x00,0x00, 0x00,0x00, 0x00,0x00
};

// --------------------- helper mappings ---------------------
String convertCondition(const String &cn) {
  if (cn == "晴") return "Sunny";
  if (cn == "多云") return "Cloudy";
  if (cn == "少云") return "Cloudy";
  if (cn == "阴") return "Cloudy";
  if (cn == "小雨") return "Rain!";
  if (cn == "中雨") return "Rain!!";
  if (cn == "大雨") return "Rain!!!";
  if (cn == "阵雨") return "Shower";
  if (cn == "雷阵雨") return "Thunder";
  if (cn == "雷暴") return "Thunder";
  if (cn == "雪" ) return "Snow";
  if (cn == "小雪") return "Snow*";
  if (cn == "中雪") return "Snow**";
  if (cn == "大雪") return "Snow***";
  if (cn == "雾" || cn == "轻雾") return "Fog";
  if (cn == "霾") return "Haze";
  if (cn == "扬沙" || cn == "浮尘" || cn == "沙尘暴") return "Dust";
  return cn;
}

String convertAQICategory(const String &cn) {
  if (cn == "优") return "Good";
  if (cn == "良") return "Fair";
  if (cn == "轻度污染") return "Mild";
  if (cn == "中度污染") return "Moderate";
  if (cn == "重度污染") return "Heavy";
  if (cn == "严重污染") return "Severe";
  return cn;
}

// choose icon pointer based on original Chinese condition (cn)
const unsigned char* chooseIcon(const String &cn) {
  // check substrings to be robust
  if (cn.indexOf("晴") >= 0) return icon_sunny_bits;
  if (cn.indexOf("多云") >= 0) return icon_cloudy_bits;
  if (cn.indexOf("少云") >= 0 || cn.indexOf("局部") >= 0) return icon_partly_bits;
  if (cn.indexOf("雨") >= 0 && cn.indexOf("雷") < 0) return icon_rain_bits;
  if (cn.indexOf("雷") >= 0) return icon_thunder_bits;
  if (cn.indexOf("雪") >= 0) return icon_snow_bits;
  if (cn.indexOf("雾") >= 0) return icon_fog_bits;
  if (cn.indexOf("霾") >= 0 || cn.indexOf("沙") >= 0 || cn.indexOf("尘") >= 0) return icon_haze_bits;
  return icon_cloudy_bits;
}

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
int i = 0;
/*
void loop()
{
  testDisplay(i);
  i = (i + 1) % 17;
  delay(2000);
}
*/
void testDisplay(int index)
{
  String conds[] = {"晴", "多云", "少云", "阴", "小雨", "中雨", "大雨", "阵雨", "雷阵雨", "雷暴", "雪", "小雪", "中雪", "大雪", "雾", "霾", "扬沙"};
  String cond = conds[index];
  cond = convertCondition(cond);
  float temp = 20;
  float tempMin = -6;
  float tempMax = 30;
  int humidity = 70;
  int aqi = 200;
  printToDisplay(cond, temp, tempMin, tempMax, humidity, aqi);
}

void printToDisplay(String cond, float temp, float tempMin, float tempMax, int humidity, int aqi)
{
  u8g2.clearBuffer();
  //
  // 第 1 行：城市名（上海）
  //
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(0, 10, "Shanghai");

  //
  // 第 2 行：图标 + 当前温度
  //
  u8g2.setFont(u8g2_font_10x20_tr); // big font
  u8g2.drawStr(0, 26, cond.c_str());
  String tempStr = String(temp, 0) + "C";
  u8g2.drawStr(90, 26, tempStr.c_str());

  //
  // 第 3 行：最低温 - 最高温°C
  //
  u8g2.setFont(u8g2_font_8x13_tf);
  String rangeStr = String(tempMin, 0) + " - " + String(tempMax, 0) + " C";
  u8g2.drawStr(0, 42, rangeStr.c_str());

  //
  // 第 4 行：湿度 + AQI
  //
  u8g2.setFont(u8g2_font_7x13_tf);
  String humStr = "Hum:" + String(humidity) + "%";
  u8g2.drawStr(0, 58, humStr.c_str());

  String aqiStr = "AQI:" + String(aqi);
  u8g2.drawStr(70, 58, aqiStr.c_str());

  u8g2.sendBuffer();
}
void fetchAndDisplay() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("No WiFi");
    return;
  }

  // prepare urls (QWeather v7 endpoints)
  String weatherUrl = "https://" + String(apiHost) + "/v7/weather/now?location="  + String(lon) + "," + String(lat) + "&key=" + String(apiKey);
  String aqiUrl = "https://" + String(apiHost) + "/airquality/v1/current/" + String(lat) + "/" + String(lon) + "?key=" + String(apiKey);
  String url3Day = "https://" + String(apiHost) + "/v7/weather/3d?location=" +  String(lon) + "," + String(lat) + "&key=" + apiKey;

  // TEMP files
  const char* gzWeather = "/weather.gz";
  const char* gzAQI     = "/aqi.gz";
  const char* gz3Day = "/forecast.gz";
  const char* jsonWeather = "/weather.json";
  const char* jsonAQI     = "/aqi.json";
  const char* js3Day = "/forecast.json";

  // remove old files
  if (LittleFS.exists(gzWeather)) LittleFS.remove(gzWeather);
  if (LittleFS.exists(gzAQI)) LittleFS.remove(gzAQI);
  if (LittleFS.exists(gz3Day)) LittleFS.remove(gz3Day);
  if (LittleFS.exists(jsonWeather)) LittleFS.remove(jsonWeather);
  if (LittleFS.exists(jsonAQI)) LittleFS.remove(jsonAQI);
  if (LittleFS.exists(js3Day)) LittleFS.remove(js3Day);

  u8g2.clearBuffer();
  u8g2.drawStr(0, 12, "Request Weather...");
  u8g2.sendBuffer();
  // fetch and save gzip streams
  bool okW = fetchAndSaveGzipHTTPS(weatherUrl, gzWeather);
  u8g2.clearBuffer();
  u8g2.drawStr(0, 12, "Request AQI...");
  u8g2.sendBuffer();
  bool okA = fetchAndSaveGzipHTTPS(aqiUrl, gzAQI);
  u8g2.clearBuffer();
  u8g2.drawStr(0, 12, "Request 3 day forcast...");
  u8g2.sendBuffer();
  bool ok3day = fetchAndSaveGzipHTTPS(url3Day, gz3Day);

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
  if (!ok3day) {
    Serial.println("Failed to fetch 3 day forcast gzip");
    u8g2.clearBuffer();
    u8g2.drawStr(0,12,"3 day forcast fetch failed");
    u8g2.sendBuffer();
    return;
  }
  // decompress gzip -> json
  u8g2.clearBuffer();
  u8g2.drawStr(0, 12, "Decompress Response...");
  u8g2.sendBuffer();
  String weatherJSON = decompressGzipToString(gzWeather, jsonWeather);
  String aqiJSON = decompressGzipToString(gzAQI, jsonAQI);
  String f3dJSON  = decompressGzipToString(gz3Day, js3Day);

  Serial.println("=== Weather JSON ===");
  Serial.println(weatherJSON);
  Serial.println("=== AQI JSON ===");
  Serial.println(aqiJSON);
  Serial.println("=== 3 DAY ===");
  Serial.println(f3dJSON);

  // parse JSON
  float temp = 0.0;
  int humidity = 0;
  String cond = "";
  int aqi = -1;
  String aqiCategory = "";
  float tempMin = 0;
  float tempMax = 0;

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
  // FORECAST (3 days)
  {
    StaticJsonDocument<4096> doc;
    if (deserializeJson(doc, f3dJSON) == DeserializationError::Ok) {
      tempMin = doc["daily"][0]["tempMin"].as<float>();
      tempMax = doc["daily"][0]["tempMax"].as<float>();
    }
  }
  // Debug prints
  Serial.printf("Parsed: temp=%.1f ~ %.1f(%.1f) humidity=%d cond='%s' aqi=%d aqiCategory='%s'\n",
                tempMin, tempMax, temp, humidity, cond.c_str(), aqi, aqiCategory.c_str());
  cond = convertCondition(cond);
  aqiCategory = convertAQICategory(aqiCategory);
/*
  // display on OLED
  u8g2.clearBuffer();
  u8g2.drawStr(0, 10, "Shanghai Weather");
  u8g2.drawStr(0, 22, (String("Temp: ") + String(tempMin, 0) + " - " + String(tempMax, 0) + " (" + String(temp, 0) + ")" + " C").c_str());
  u8g2.drawStr(0, 32, (String("Humidity: ") + String(humidity) + " %").c_str());
  if (aqi >= 0) {
    u8g2.drawStr(0, 44, (String("AQI: ") + String(aqi) + " " + aqiCategory).c_str());
  } else {
    u8g2.drawStr(0, 44, "AQI: N/A");
  }
  u8g2.drawStr(0, 54, (String("Cond: ") + cond).c_str());
  u8g2.sendBuffer();
*/
  printToDisplay(cond, temp, tempMin, tempMax, humidity, aqi);
  // cleanup (optional)
  // LittleFS.remove(gzWeather); LittleFS.remove(gzAQI); // keep for debugging if needed
}
