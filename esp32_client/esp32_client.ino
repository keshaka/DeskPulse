/*
  DeskPulse ESP32 Client with LVGL UI (ESP32-2432S028)
  -----------------------------------------------------
  Features:
  - Connects to WiFi
  - Fetches /stats from Flask server
  - Parses JSON with ArduinoJson
  - Displays CPU, RAM, Temperature using LVGL labels and bars
  - Prints values to Serial Monitor
  - Handles connection and parsing errors gracefully

  Required libraries:
  - ArduinoJson
  - lvgl
  - TFT_eSPI

  Required TFT_eSPI setup:
  - Configure User_Setup for ESP32-2432S028 (ILI9341, 320x240)
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <TFT_eSPI.h>

// -------------------- User Configuration --------------------
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* SERVER_URL = "http://192.168.1.100:5000/stats";

const unsigned long POLL_INTERVAL_MS = 2000;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;

// -------------------- Display / LVGL --------------------
static const uint16_t SCREEN_WIDTH = 320;
static const uint16_t SCREEN_HEIGHT = 240;
static const uint32_t DRAW_BUF_PIXELS = SCREEN_WIDTH * 20;

TFT_eSPI tft = TFT_eSPI();
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[DRAW_BUF_PIXELS];
static lv_disp_drv_t disp_drv;

// -------------------- Runtime State --------------------
unsigned long lastPollMs = 0;
float cpuPercent = 0.0f;
float ramPercent = 0.0f;
float tempCelsius = -1.0f;
bool statsValid = false;

// -------------------- UI Objects --------------------
lv_obj_t* labelStatus;

lv_obj_t* labelCpuTitle;
lv_obj_t* barCpu;
lv_obj_t* labelCpuValue;

lv_obj_t* labelRamTitle;
lv_obj_t* barRam;
lv_obj_t* labelRamValue;

lv_obj_t* labelTempTitle;
lv_obj_t* barTemp;
lv_obj_t* labelTempValue;

// -------------------- LVGL Helpers --------------------
void myDispFlush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
  uint32_t width = (area->x2 - area->x1 + 1);
  uint32_t height = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, width, height);
  tft.pushColors((uint16_t*)&color_p->full, width * height, true);
  tft.endWrite();

  lv_disp_flush_ready(disp);
}

void initLvglDisplay() {
  lv_init();

  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  lv_disp_draw_buf_init(&draw_buf, buf, NULL, DRAW_BUF_PIXELS);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_WIDTH;
  disp_drv.ver_res = SCREEN_HEIGHT;
  disp_drv.flush_cb = myDispFlush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);
}

void styleBar(lv_obj_t* barObj, lv_color_t color) {
  lv_obj_set_height(barObj, 14);
  lv_obj_set_style_bg_color(barObj, lv_color_hex(0x2C2F33), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(barObj, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(barObj, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_color(barObj, color, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(barObj, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(barObj, 8, LV_PART_INDICATOR);
}

void createUi() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101214), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 10, LV_PART_MAIN);

  labelStatus = lv_label_create(scr);
  lv_label_set_text(labelStatus, "DeskPulse | Connecting...");
  lv_obj_set_style_text_color(labelStatus, lv_color_hex(0xE6EDF3), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelStatus, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(labelStatus, LV_ALIGN_TOP_LEFT, 0, 0);

  // CPU row
  labelCpuTitle = lv_label_create(scr);
  lv_label_set_text(labelCpuTitle, "CPU");
  lv_obj_set_style_text_color(labelCpuTitle, lv_color_hex(0x8B949E), LV_PART_MAIN);
  lv_obj_align(labelCpuTitle, LV_ALIGN_TOP_LEFT, 0, 34);

  labelCpuValue = lv_label_create(scr);
  lv_label_set_text(labelCpuValue, "0.0%");
  lv_obj_set_style_text_color(labelCpuValue, lv_color_hex(0xE6EDF3), LV_PART_MAIN);
  lv_obj_align(labelCpuValue, LV_ALIGN_TOP_RIGHT, 0, 34);

  barCpu = lv_bar_create(scr);
  lv_obj_set_width(barCpu, 300);
  lv_obj_align(barCpu, LV_ALIGN_TOP_LEFT, 0, 56);
  lv_bar_set_range(barCpu, 0, 100);
  styleBar(barCpu, lv_color_hex(0x2EA043));

  // RAM row
  labelRamTitle = lv_label_create(scr);
  lv_label_set_text(labelRamTitle, "RAM");
  lv_obj_set_style_text_color(labelRamTitle, lv_color_hex(0x8B949E), LV_PART_MAIN);
  lv_obj_align(labelRamTitle, LV_ALIGN_TOP_LEFT, 0, 84);

  labelRamValue = lv_label_create(scr);
  lv_label_set_text(labelRamValue, "0.0%");
  lv_obj_set_style_text_color(labelRamValue, lv_color_hex(0xE6EDF3), LV_PART_MAIN);
  lv_obj_align(labelRamValue, LV_ALIGN_TOP_RIGHT, 0, 84);

  barRam = lv_bar_create(scr);
  lv_obj_set_width(barRam, 300);
  lv_obj_align(barRam, LV_ALIGN_TOP_LEFT, 0, 106);
  lv_bar_set_range(barRam, 0, 100);
  styleBar(barRam, lv_color_hex(0x1F6FEB));

  // Temperature row
  labelTempTitle = lv_label_create(scr);
  lv_label_set_text(labelTempTitle, "TEMP");
  lv_obj_set_style_text_color(labelTempTitle, lv_color_hex(0x8B949E), LV_PART_MAIN);
  lv_obj_align(labelTempTitle, LV_ALIGN_TOP_LEFT, 0, 134);

  labelTempValue = lv_label_create(scr);
  lv_label_set_text(labelTempValue, "--.- C");
  lv_obj_set_style_text_color(labelTempValue, lv_color_hex(0xE6EDF3), LV_PART_MAIN);
  lv_obj_align(labelTempValue, LV_ALIGN_TOP_RIGHT, 0, 134);

  barTemp = lv_bar_create(scr);
  lv_obj_set_width(barTemp, 300);
  lv_obj_align(barTemp, LV_ALIGN_TOP_LEFT, 0, 156);
  lv_bar_set_range(barTemp, 0, 100);
  styleBar(barTemp, lv_color_hex(0xDB6D28));
}

void refreshUi() {
  char text[32];

  if (!statsValid) {
    lv_label_set_text(labelStatus, "DeskPulse | Waiting for data...");
    return;
  }

  lv_label_set_text(labelStatus, "DeskPulse | Live");

  lv_bar_set_value(barCpu, (int)cpuPercent, LV_ANIM_ON);
  snprintf(text, sizeof(text), "%.1f%%", cpuPercent);
  lv_label_set_text(labelCpuValue, text);

  lv_bar_set_value(barRam, (int)ramPercent, LV_ANIM_ON);
  snprintf(text, sizeof(text), "%.1f%%", ramPercent);
  lv_label_set_text(labelRamValue, text);

  if (tempCelsius >= 0.0f) {
    int tempBar = (int)tempCelsius;
    if (tempBar > 100) {
      tempBar = 100;
    }
    lv_bar_set_value(barTemp, tempBar, LV_ANIM_ON);
    snprintf(text, sizeof(text), "%.1f C", tempCelsius);
    lv_label_set_text(labelTempValue, text);
  } else {
    lv_bar_set_value(barTemp, 0, LV_ANIM_ON);
    lv_label_set_text(labelTempValue, "N/A");
  }
}

// -------------------- Connectivity --------------------
void connectToWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  lv_label_set_text(labelStatus, "DeskPulse | Connecting WiFi...");
  Serial.print("[WiFi] Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startMs = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(300);
    lv_timer_handler();
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Connected. IP: ");
    Serial.println(WiFi.localIP());
    lv_label_set_text(labelStatus, "DeskPulse | WiFi connected");
  } else {
    Serial.println("[WiFi] Connection failed. Retrying later.");
    lv_label_set_text(labelStatus, "DeskPulse | WiFi offline");
  }
}

// -------------------- API --------------------
void fetchStats() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] Skipped: WiFi not connected.");
    lv_label_set_text(labelStatus, "DeskPulse | WiFi offline");
    statsValid = false;
    return;
  }

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);

  if (!http.begin(SERVER_URL)) {
    Serial.println("[HTTP] Request init failed.");
    lv_label_set_text(labelStatus, "DeskPulse | HTTP init error");
    statsValid = false;
    http.end();
    return;
  }

  int httpCode = http.GET();
  if (httpCode <= 0) {
    Serial.print("[HTTP] Request failed: ");
    Serial.println(http.errorToString(httpCode));
    lv_label_set_text(labelStatus, "DeskPulse | Server unreachable");
    statsValid = false;
    http.end();
    return;
  }

  if (httpCode != HTTP_CODE_OK) {
    Serial.print("[HTTP] Unexpected status: ");
    Serial.println(httpCode);
    lv_label_set_text(labelStatus, "DeskPulse | Server error");
    statsValid = false;
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("[JSON] Parse failed: ");
    Serial.println(err.c_str());
    lv_label_set_text(labelStatus, "DeskPulse | JSON parse error");
    statsValid = false;
    return;
  }

  JsonObject data = doc["data"].as<JsonObject>();
  if (data.isNull()) {
    Serial.println("[JSON] Missing data object.");
    lv_label_set_text(labelStatus, "DeskPulse | Invalid payload");
    statsValid = false;
    return;
  }

  cpuPercent = data["cpu_percent"] | 0.0f;

  JsonObject ramObj = data["ram"].as<JsonObject>();
  ramPercent = ramObj["used_percent"] | 0.0f;

  // Temperature preference: CPU temp first, fallback to GPU temp.
  tempCelsius = -1.0f;
  JsonObject cpuTempObj = data["cpu_temp"].as<JsonObject>();
  if (!cpuTempObj.isNull()) {
    bool cpuTempAvailable = cpuTempObj["available"] | false;
    if (cpuTempAvailable) {
      tempCelsius = cpuTempObj["celsius"] | -1.0f;
    }
  }

  if (tempCelsius < 0.0f) {
    JsonObject gpuObj = data["gpu"].as<JsonObject>();
    if (!gpuObj.isNull()) {
      tempCelsius = gpuObj["temperature_celsius"] | -1.0f;
    }
  }

  statsValid = true;

  Serial.println("------ DeskPulse ------");
  Serial.print("CPU: ");
  Serial.print(cpuPercent, 1);
  Serial.println(" %");
  Serial.print("RAM: ");
  Serial.print(ramPercent, 1);
  Serial.println(" %");
  Serial.print("Temp: ");
  if (tempCelsius >= 0.0f) {
    Serial.print(tempCelsius, 1);
    Serial.println(" C");
  } else {
    Serial.println("N/A");
  }
}

// -------------------- Arduino Entry --------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  initLvglDisplay();
  createUi();
  refreshUi();

  connectToWiFi();
}

void loop() {
  lv_timer_handler();
  delay(5);

  if (WiFi.status() != WL_CONNECTED) {
    connectToWiFi();
  }

  unsigned long nowMs = millis();
  if (nowMs - lastPollMs >= POLL_INTERVAL_MS) {
    lastPollMs = nowMs;
    fetchStats();
    refreshUi();
  }
}
