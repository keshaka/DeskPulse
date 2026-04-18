/*
  DeskPulse ESP32 Client with LVGL UI (ESP32-2432S028)
  -----------------------------------------------------
  Dashboard features:
  - Modern header with connection status
  - Styled system stats cards with icon chips and bars
  - Media info section for title, artist, and playback state
  - Lightweight polling and low-overhead screen updates

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
const char* WIFI_SSID = "Sathuta HQ 2.4G";
const char* WIFI_PASSWORD = "Qwer3552";
const char* SERVER_URL = "http://192.168.1.177:5000/stats";

const unsigned long POLL_INTERVAL_MS = 2000;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;

// -------------------- Display / LVGL --------------------
static const uint16_t SCREEN_WIDTH = 240;
static const uint16_t SCREEN_HEIGHT = 320;
static const uint32_t DRAW_BUF_LINES = 16;
static const uint32_t DRAW_BUF_PIXELS = SCREEN_WIDTH * DRAW_BUF_LINES;

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
bool refreshPending = false;

char mediaTitle[48] = "No media";
char mediaArtist[48] = "--";
char mediaState[16] = "idle";
bool mediaValid = false;

// -------------------- UI Objects --------------------
lv_obj_t* panelHeader;
lv_obj_t* labelTitle;
lv_obj_t* labelSubtitle;
lv_obj_t* labelStatus;

lv_obj_t* panelSystem;
lv_obj_t* labelSystemTitle;
lv_obj_t* chipCpuIcon;
lv_obj_t* labelCpuTitle;
lv_obj_t* barCpu;
lv_obj_t* labelCpuValue;
lv_obj_t* chipRamIcon;
lv_obj_t* labelRamTitle;
lv_obj_t* barRam;
lv_obj_t* labelRamValue;
lv_obj_t* chipTempIcon;
lv_obj_t* labelTempTitle;
lv_obj_t* barTemp;
lv_obj_t* labelTempValue;

lv_obj_t* panelMedia;
lv_obj_t* labelMediaTitle;
lv_obj_t* labelMediaState;
lv_obj_t* labelMediaSong;
lv_obj_t* labelMediaArtist;

lv_obj_t* labelFooter;

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
  disp_drv.full_refresh = 0;
  lv_disp_drv_register(&disp_drv);
}

void styleCard(lv_obj_t* obj) {
  lv_obj_set_style_bg_color(obj, lv_color_hex(0x15181D), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(obj, lv_color_hex(0x232830), LV_PART_MAIN);
  lv_obj_set_style_border_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(obj, 18, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(obj, 10, LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

void styleBar(lv_obj_t* barObj, lv_color_t color) {
  lv_obj_set_height(barObj, 12);
  lv_obj_set_style_bg_color(barObj, lv_color_hex(0x232830), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(barObj, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(barObj, 999, LV_PART_MAIN);
  lv_obj_set_style_bg_color(barObj, color, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(barObj, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(barObj, 999, LV_PART_INDICATOR);
}

void styleIconChip(lv_obj_t* obj, lv_color_t color) {
  lv_obj_set_size(obj, 26, 26);
  lv_obj_set_style_radius(obj, 999, LV_PART_MAIN);
  lv_obj_set_style_bg_color(obj, color, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

void updateMediaSection() {
  if (mediaValid) {
    lv_label_set_text(labelMediaState, mediaState);
    lv_label_set_text(labelMediaSong, mediaTitle);
    lv_label_set_text(labelMediaArtist, mediaArtist);
  } else {
    lv_label_set_text(labelMediaState, "idle");
    lv_label_set_text(labelMediaSong, "No media playing");
    lv_label_set_text(labelMediaArtist, "Waiting for player...");
  }
}

void createUi() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B0F14), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(scr, 8, LV_PART_MAIN);

  panelHeader = lv_obj_create(scr);
  lv_obj_set_size(panelHeader, 224, 54);
  lv_obj_align(panelHeader, LV_ALIGN_TOP_MID, 0, 0);
  styleCard(panelHeader);

  labelTitle = lv_label_create(panelHeader);
  lv_label_set_text(labelTitle, "DESKPULSE");
  lv_obj_set_style_text_color(labelTitle, lv_color_hex(0xF2F5F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelTitle, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_align(labelTitle, LV_ALIGN_TOP_LEFT, 0, -2);

  labelSubtitle = lv_label_create(panelHeader);
  lv_label_set_text(labelSubtitle, "Live system dashboard");
  lv_obj_set_style_text_color(labelSubtitle, lv_color_hex(0x8B949E), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelSubtitle, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_align(labelSubtitle, LV_ALIGN_BOTTOM_LEFT, 0, 2);

  labelStatus = lv_label_create(panelHeader);
  lv_label_set_text(labelStatus, "WiFi connecting...");
  lv_obj_set_style_text_color(labelStatus, lv_color_hex(0x7EE787), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelStatus, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_align(labelStatus, LV_ALIGN_TOP_RIGHT, 0, -1);

  panelSystem = lv_obj_create(scr);
  lv_obj_set_size(panelSystem, 224, 172);
  lv_obj_align(panelSystem, LV_ALIGN_TOP_MID, 0, 62);
  styleCard(panelSystem);

  labelSystemTitle = lv_label_create(panelSystem);
  lv_label_set_text(labelSystemTitle, "SYSTEM STATS");
  lv_obj_set_style_text_color(labelSystemTitle, lv_color_hex(0x8B949E), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelSystemTitle, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_align(labelSystemTitle, LV_ALIGN_TOP_LEFT, 0, -2);

  chipCpuIcon = lv_obj_create(panelSystem);
  styleIconChip(chipCpuIcon, lv_color_hex(0x2EA043));
  lv_obj_align(chipCpuIcon, LV_ALIGN_TOP_LEFT, 0, 18);

  lv_obj_t* cpuIconLabel = lv_label_create(chipCpuIcon);
  lv_label_set_text(cpuIconLabel, "C");
  lv_obj_set_style_text_color(cpuIconLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(cpuIconLabel, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_center(cpuIconLabel);

  labelCpuTitle = lv_label_create(panelSystem);
  lv_label_set_text(labelCpuTitle, "CPU");
  lv_obj_set_style_text_color(labelCpuTitle, lv_color_hex(0xDCE3EA), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelCpuTitle, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(labelCpuTitle, LV_ALIGN_TOP_LEFT, 36, 14);

  labelCpuValue = lv_label_create(panelSystem);
  lv_label_set_text(labelCpuValue, "0.0%");
  lv_obj_set_style_text_color(labelCpuValue, lv_color_hex(0xE6EDF3), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelCpuValue, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(labelCpuValue, LV_ALIGN_TOP_RIGHT, -2, 14);

  barCpu = lv_bar_create(panelSystem);
  lv_obj_set_width(barCpu, 180);
  lv_obj_align(barCpu, LV_ALIGN_TOP_LEFT, 0, 40);
  lv_bar_set_range(barCpu, 0, 100);
  styleBar(barCpu, lv_color_hex(0x2EA043));

  chipRamIcon = lv_obj_create(panelSystem);
  styleIconChip(chipRamIcon, lv_color_hex(0x1F6FEB));
  lv_obj_align(chipRamIcon, LV_ALIGN_TOP_LEFT, 0, 64);

  lv_obj_t* ramIconLabel = lv_label_create(chipRamIcon);
  lv_label_set_text(ramIconLabel, "R");
  lv_obj_set_style_text_color(ramIconLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(ramIconLabel, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_center(ramIconLabel);

  labelRamTitle = lv_label_create(panelSystem);
  lv_label_set_text(labelRamTitle, "RAM");
  lv_obj_set_style_text_color(labelRamTitle, lv_color_hex(0xDCE3EA), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelRamTitle, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(labelRamTitle, LV_ALIGN_TOP_LEFT, 36, 60);

  labelRamValue = lv_label_create(panelSystem);
  lv_label_set_text(labelRamValue, "0.0%");
  lv_obj_set_style_text_color(labelRamValue, lv_color_hex(0xE6EDF3), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelRamValue, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(labelRamValue, LV_ALIGN_TOP_RIGHT, -2, 60);

  barRam = lv_bar_create(panelSystem);
  lv_obj_set_width(barRam, 180);
  lv_obj_align(barRam, LV_ALIGN_TOP_LEFT, 0, 86);
  lv_bar_set_range(barRam, 0, 100);
  styleBar(barRam, lv_color_hex(0x1F6FEB));

  chipTempIcon = lv_obj_create(panelSystem);
  styleIconChip(chipTempIcon, lv_color_hex(0xDB6D28));
  lv_obj_align(chipTempIcon, LV_ALIGN_TOP_LEFT, 0, 110);

  lv_obj_t* tempIconLabel = lv_label_create(chipTempIcon);
  lv_label_set_text(tempIconLabel, "T");
  lv_obj_set_style_text_color(tempIconLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(tempIconLabel, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_center(tempIconLabel);

  labelTempTitle = lv_label_create(panelSystem);
  lv_label_set_text(labelTempTitle, "TEMP");
  lv_obj_set_style_text_color(labelTempTitle, lv_color_hex(0xDCE3EA), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelTempTitle, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(labelTempTitle, LV_ALIGN_TOP_LEFT, 36, 106);

  labelTempValue = lv_label_create(panelSystem);
  lv_label_set_text(labelTempValue, "--.- C");
  lv_obj_set_style_text_color(labelTempValue, lv_color_hex(0xE6EDF3), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelTempValue, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(labelTempValue, LV_ALIGN_TOP_RIGHT, -2, 106);

  barTemp = lv_bar_create(panelSystem);
  lv_obj_set_width(barTemp, 180);
  lv_obj_align(barTemp, LV_ALIGN_TOP_LEFT, 0, 132);
  lv_bar_set_range(barTemp, 0, 100);
  styleBar(barTemp, lv_color_hex(0xDB6D28));

  panelMedia = lv_obj_create(scr);
  lv_obj_set_size(panelMedia, 224, 76);
  lv_obj_align(panelMedia, LV_ALIGN_TOP_MID, 0, 242);
  styleCard(panelMedia);

  labelMediaTitle = lv_label_create(panelMedia);
  lv_label_set_text(labelMediaTitle, "MEDIA INFO");
  lv_obj_set_style_text_color(labelMediaTitle, lv_color_hex(0x8B949E), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelMediaTitle, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_align(labelMediaTitle, LV_ALIGN_TOP_LEFT, 0, -2);

  labelMediaState = lv_label_create(panelMedia);
  lv_label_set_text(labelMediaState, "idle");
  lv_obj_set_style_text_color(labelMediaState, lv_color_hex(0x7EE787), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelMediaState, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_align(labelMediaState, LV_ALIGN_TOP_RIGHT, 0, -2);

  labelMediaSong = lv_label_create(panelMedia);
  lv_label_set_text(labelMediaSong, "No media playing");
  lv_obj_set_style_text_color(labelMediaSong, lv_color_hex(0xF2F5F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelMediaSong, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(labelMediaSong, LV_ALIGN_TOP_LEFT, 0, 18);

  labelMediaArtist = lv_label_create(panelMedia);
  lv_label_set_text(labelMediaArtist, "Waiting for player...");
  lv_obj_set_style_text_color(labelMediaArtist, lv_color_hex(0x8B949E), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelMediaArtist, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_align(labelMediaArtist, LV_ALIGN_TOP_LEFT, 0, 40);

  labelFooter = lv_label_create(scr);
  lv_label_set_text(labelFooter, "Polling Flask server every 2s");
  lv_obj_set_style_text_color(labelFooter, lv_color_hex(0x6E7681), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelFooter, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_obj_align(labelFooter, LV_ALIGN_BOTTOM_MID, 0, 0);
}

void refreshUi() {
  char text[32];

  if (!statsValid) {
    lv_label_set_text(labelStatus, "Offline / waiting...");
    updateMediaSection();
    return;
  }

  lv_label_set_text(labelStatus, "Live");

  lv_bar_set_value(barCpu, (int)cpuPercent, LV_ANIM_OFF);
  snprintf(text, sizeof(text), "%.1f%%", cpuPercent);
  lv_label_set_text(labelCpuValue, text);

  lv_bar_set_value(barRam, (int)ramPercent, LV_ANIM_OFF);
  snprintf(text, sizeof(text), "%.1f%%", ramPercent);
  lv_label_set_text(labelRamValue, text);

  if (tempCelsius >= 0.0f) {
    int tempBar = (int)tempCelsius;
    if (tempBar > 100) {
      tempBar = 100;
    }
    lv_bar_set_value(barTemp, tempBar, LV_ANIM_OFF);
    snprintf(text, sizeof(text), "%.1f C", tempCelsius);
    lv_label_set_text(labelTempValue, text);
  } else {
    lv_bar_set_value(barTemp, 0, LV_ANIM_OFF);
    lv_label_set_text(labelTempValue, "N/A");
  }

  updateMediaSection();
}

// -------------------- Connectivity --------------------
void connectToWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  lv_label_set_text(labelStatus, "WiFi connecting...");
  Serial.print("[WiFi] Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startMs = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(220);
    lv_timer_handler();
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Connected. IP: ");
    Serial.println(WiFi.localIP());
    lv_label_set_text(labelStatus, "WiFi connected");
  } else {
    Serial.println("[WiFi] Connection failed. Retrying later.");
    lv_label_set_text(labelStatus, "WiFi offline");
  }
}

// -------------------- API --------------------
void fetchStats() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] Skipped: WiFi not connected.");
    lv_label_set_text(labelStatus, "WiFi offline");
    statsValid = false;
    return;
  }

  HTTPClient http;
  http.setConnectTimeout(4500);
  http.setTimeout(4500);

  if (!http.begin(SERVER_URL)) {
    Serial.println("[HTTP] Request init failed.");
    lv_label_set_text(labelStatus, "HTTP init error");
    statsValid = false;
    http.end();
    return;
  }

  int httpCode = http.GET();
  if (httpCode <= 0) {
    Serial.print("[HTTP] Request failed: ");
    Serial.println(http.errorToString(httpCode));
    lv_label_set_text(labelStatus, "Server unreachable");
    statsValid = false;
    http.end();
    return;
  }

  if (httpCode != HTTP_CODE_OK) {
    Serial.print("[HTTP] Unexpected status: ");
    Serial.println(httpCode);
    lv_label_set_text(labelStatus, "Server error");
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
    lv_label_set_text(labelStatus, "JSON parse error");
    statsValid = false;
    return;
  }

  JsonObject data = doc["data"].as<JsonObject>();
  if (data.isNull()) {
    Serial.println("[JSON] Missing data object.");
    lv_label_set_text(labelStatus, "Invalid payload");
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

  JsonObject mediaObj = data["media"].as<JsonObject>();
  if (!mediaObj.isNull()) {
    const char* title = mediaObj["title"] | "No media";
    const char* artist = mediaObj["artist"] | "--";
    const char* state = mediaObj["playback_state"] | "idle";

    strlcpy(mediaTitle, title, sizeof(mediaTitle));
    strlcpy(mediaArtist, artist, sizeof(mediaArtist));
    strlcpy(mediaState, state, sizeof(mediaState));
    mediaValid = mediaObj["available"] | false;
  } else {
    strlcpy(mediaTitle, "No media", sizeof(mediaTitle));
    strlcpy(mediaArtist, "--", sizeof(mediaArtist));
    strlcpy(mediaState, "idle", sizeof(mediaState));
    mediaValid = false;
  }

  statsValid = true;
  refreshPending = true;

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
  updateMediaSection();

  connectToWiFi();
  refreshUi();
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
  }

  if (refreshPending) {
    refreshPending = false;
    refreshUi();
  }
}
