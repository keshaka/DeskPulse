/*
  DeskPulse ESP32 Client with Full-Width LVGL Dashboard (ESP32-2432S028)
  ----------------------------------------------------------------------
  Features:
  - Full-width UI for 320x240 landscape
  - Touch-enabled tabbed pages
  - Dashboard + dedicated detail pages
  - Shows all fields from Flask /stats API
  - WiFi + HTTP + JSON error handling

  Required libraries:
  - ArduinoJson
  - lvgl
  - TFT_eSPI

  Notes:
  - Ensure TFT_eSPI is configured for your ESP32-2432S028 display + touch.
  - SERVER_URL should point to your Flask /stats endpoint.
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

// -------------------- User Configuration --------------------
const char* WIFI_SSID = "Sathuta HQ 2.4G";
const char* WIFI_PASSWORD = "Qwer3552";
const char* SERVER_URL = "http://192.168.1.177:5000/stats";

const unsigned long POLL_INTERVAL_MS = 2000;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;

// -------------------- Display / LVGL --------------------
static const uint16_t SCREEN_WIDTH = 320;
static const uint16_t SCREEN_HEIGHT = 240;
static const uint16_t DRAW_BUF_LINES = 10;
static const uint32_t DRAW_BUF_PIXELS = SCREEN_WIDTH * DRAW_BUF_LINES;

TFT_eSPI tft = TFT_eSPI();
static lv_disp_draw_buf_t draw_buf;
static lv_color_t draw_buf_pixels[DRAW_BUF_PIXELS];
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

// -------------------- Touch (XPT2046) --------------------
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

// Raw calibration ranges for CYD-like panels. Adjust if your panel differs.
static const int TOUCH_MIN_X = 200;
static const int TOUCH_MAX_X = 3700;
static const int TOUCH_MIN_Y = 240;
static const int TOUCH_MAX_Y = 3800;

// Flip flags if touch appears mirrored on your hardware.
static const bool TOUCH_FLIP_X = false;
static const bool TOUCH_FLIP_Y = false;

XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);

// -------------------- Runtime State --------------------
unsigned long lastPollMs = 0;
bool refreshPending = false;
unsigned long prevNetSampleMs = 0;
uint64_t prevBytesSent = 0;
uint64_t prevBytesReceived = 0;
float gUploadBytesPerSec = 0.0f;
float gDownloadBytesPerSec = 0.0f;

struct StatsData {
  char apiStatus[16];
  char timestamp[40];

  float cpuPercent;
  bool cpuTempAvailable;
  float cpuTempC;
  float cpuTempF;

  float ramUsedPercent;
  float ramUsedGb;
  float ramAvailableGb;
  float ramTotalGb;

  bool gpuAvailable;
  float gpuUsagePercent;
  float gpuTempC;
  float gpuTempF;
  float gpuVramUsedMb;
  float gpuVramTotalMb;
  float gpuVramUsedPercent;

  float diskUsedPercent;
  float diskUsedGb;
  float diskFreeGb;
  float diskTotalGb;

  uint64_t bytesSent;
  uint64_t bytesReceived;
  uint64_t packetsSent;
  uint64_t packetsReceived;

  bool mediaAvailable;
  char mediaTitle[64];
  char mediaArtist[64];
  char mediaPlaybackState[16];
  char mediaPlayer[24];

  bool valid;
};

StatsData gStats = {};

// -------------------- UI Objects --------------------
lv_obj_t* tabview;
lv_obj_t* tabDashboard;
lv_obj_t* tabSystem;
lv_obj_t* tabGpuNet;
lv_obj_t* tabMedia;

// Header / status
lv_obj_t* labelHeaderTitle;
lv_obj_t* labelHeaderStatus;

// Dashboard widgets
lv_obj_t* barDashCpu;
lv_obj_t* barDashGpu;
lv_obj_t* barDashRam;
lv_obj_t* labelDashCpuValue;
lv_obj_t* labelDashGpuValue;
lv_obj_t* labelDashRamValue;
lv_obj_t* labelDashNetValue;

// System page
lv_obj_t* labelSystemCpu;
lv_obj_t* labelSystemCpuTemp;
lv_obj_t* labelSystemRam;
lv_obj_t* labelSystemGpu;
lv_obj_t* labelSystemGpuVram;
lv_obj_t* labelSystemDisk;
lv_obj_t* labelSystemNetBytes;
lv_obj_t* labelSystemNetPackets;
lv_obj_t* labelSystemApi;

// GPU / Network page
lv_obj_t* labelGpuInfo;
lv_obj_t* labelGpuVram;
lv_obj_t* labelNetBytes;
lv_obj_t* labelNetPackets;

// Media page
lv_obj_t* mediaHeroCard;
lv_obj_t* mediaControlsCard;
lv_obj_t* labelMediaHint;
lv_obj_t* labelMediaActionStatus;
lv_obj_t* labelMediaState;
lv_obj_t* labelMediaPlayer;
lv_obj_t* labelMediaTitle;
lv_obj_t* labelMediaArtist;

// -------------------- Utility --------------------
float clampFloat(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

void setSafeCopy(char* dst, size_t dstSize, const char* src, const char* fallback) {
  const char* finalSrc = src;
  if (finalSrc == nullptr || finalSrc[0] == '\0') {
    finalSrc = fallback;
  }
  strlcpy(dst, finalSrc, dstSize);
}

void formatSpeed(char* dst, size_t dstSize, float bytesPerSec) {
  if (bytesPerSec < 1024.0f) {
    snprintf(dst, dstSize, "%.0f B/s", bytesPerSec);
  } else if (bytesPerSec < (1024.0f * 1024.0f)) {
    snprintf(dst, dstSize, "%.1f KB/s", bytesPerSec / 1024.0f);
  } else {
    snprintf(dst, dstSize, "%.2f MB/s", bytesPerSec / (1024.0f * 1024.0f));
  }
}

void styleCard(lv_obj_t* obj);

void styleMediaCard(lv_obj_t* obj) {
  styleCard(obj);
  lv_obj_set_style_bg_color(obj, lv_color_hex(0x10141B), LV_PART_MAIN);
  lv_obj_set_style_border_color(obj, lv_color_hex(0x243043), LV_PART_MAIN);
}

void styleMediaButton(lv_obj_t* obj, lv_color_t bgColor) {
  lv_obj_set_style_bg_color(obj, bgColor, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(obj, 14, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
  lv_obj_set_style_text_color(obj, lv_color_hex(0xF5F7FA), LV_PART_MAIN);
  lv_obj_set_style_text_font(obj, &lv_font_montserrat_12, LV_PART_MAIN);
}

String getApiBaseUrl() {
  String baseUrl = SERVER_URL;
  int statsIndex = baseUrl.lastIndexOf("/stats");
  if (statsIndex >= 0) {
    baseUrl = baseUrl.substring(0, statsIndex);
  }
  return baseUrl;
}

void sendMediaCommand(const char* endpoint) {
  if (WiFi.status() != WL_CONNECTED || endpoint == nullptr || endpoint[0] == '\0') {
    if (labelMediaActionStatus != nullptr) {
      lv_label_set_text(labelMediaActionStatus, "Action unavailable");
    }
    return;
  }

  HTTPClient http;
  String url = getApiBaseUrl() + endpoint;
  if (!http.begin(url)) {
    if (labelMediaActionStatus != nullptr) {
      lv_label_set_text(labelMediaActionStatus, "Command failed");
    }
    return;
  }

  http.setConnectTimeout(3000);
  http.setTimeout(3000);
  int httpCode = http.POST("");
  http.end();

  if (labelMediaActionStatus != nullptr) {
    if (httpCode > 0 && httpCode < 400) {
      if (strcmp(endpoint, "/playpause") == 0) {
        lv_label_set_text(labelMediaActionStatus, "Play/Pause sent");
      } else if (strcmp(endpoint, "/previous") == 0) {
        lv_label_set_text(labelMediaActionStatus, "Previous sent");
      } else if (strcmp(endpoint, "/next") == 0) {
        lv_label_set_text(labelMediaActionStatus, "Next sent");
      } else if (strcmp(endpoint, "/volume_down") == 0) {
        lv_label_set_text(labelMediaActionStatus, "Volume down sent");
      } else if (strcmp(endpoint, "/volume_up") == 0) {
        lv_label_set_text(labelMediaActionStatus, "Volume up sent");
      } else {
        lv_label_set_text(labelMediaActionStatus, "Command sent");
      }
    } else {
      lv_label_set_text(labelMediaActionStatus, "Command failed");
    }
  }
}

void onMediaControlClicked(lv_event_t* e) {
  const char* endpoint = static_cast<const char*>(lv_event_get_user_data(e));
  sendMediaCommand(endpoint);
}

// -------------------- LVGL Display + Touch --------------------
void myDispFlush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
  uint32_t width = (area->x2 - area->x1 + 1);
  uint32_t height = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, width, height);
  tft.pushColors((uint16_t*)&color_p->full, width * height, true);
  tft.endWrite();

  lv_disp_flush_ready(disp);
}

void myTouchRead(lv_indev_drv_t* indev, lv_indev_data_t* data) {
  (void)indev;

  if (touchscreen.tirqTouched() && touchscreen.touched()) {
    TS_Point p = touchscreen.getPoint();

    int mappedX = map(p.x, TOUCH_MIN_X, TOUCH_MAX_X, 0, SCREEN_WIDTH - 1);
    int mappedY = map(p.y, TOUCH_MIN_Y, TOUCH_MAX_Y, 0, SCREEN_HEIGHT - 1);

    if (TOUCH_FLIP_X) {
      mappedX = (SCREEN_WIDTH - 1) - mappedX;
    }
    if (TOUCH_FLIP_Y) {
      mappedY = (SCREEN_HEIGHT - 1) - mappedY;
    }

    mappedX = clampInt(mappedX, 0, SCREEN_WIDTH - 1);
    mappedY = clampInt(mappedY, 0, SCREEN_HEIGHT - 1);

    data->state = LV_INDEV_STATE_PR;
    data->point.x = mappedX;
    data->point.y = mappedY;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

void initLvglDisplay() {
  lv_init();

  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  // Init XPT2046 touch controller on dedicated SPI pins.
  SPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin();
  touchscreen.setRotation(1);

  lv_disp_draw_buf_init(&draw_buf, draw_buf_pixels, NULL, DRAW_BUF_PIXELS);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_WIDTH;
  disp_drv.ver_res = SCREEN_HEIGHT;
  disp_drv.flush_cb = myDispFlush;
  disp_drv.draw_buf = &draw_buf;
  disp_drv.full_refresh = 0;
  lv_disp_drv_register(&disp_drv);

  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = myTouchRead;
  lv_indev_drv_register(&indev_drv);
}

// -------------------- UI Styling --------------------
void styleCard(lv_obj_t* obj) {
  lv_obj_set_style_bg_color(obj, lv_color_hex(0x141922), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(obj, lv_color_hex(0x283140), LV_PART_MAIN);
  lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(obj, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_all(obj, 8, LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

void styleBar(lv_obj_t* barObj, lv_color_t color) {
  lv_obj_set_height(barObj, 12);
  lv_obj_set_style_radius(barObj, 999, LV_PART_MAIN);
  lv_obj_set_style_bg_color(barObj, lv_color_hex(0x2A3140), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(barObj, LV_OPA_COVER, LV_PART_MAIN);

  lv_obj_set_style_radius(barObj, 999, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(barObj, color, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(barObj, LV_OPA_COVER, LV_PART_INDICATOR);
}

lv_obj_t* createSectionTitle(lv_obj_t* parent, const char* text, lv_coord_t y) {
  lv_obj_t* label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_hex(0x93A1B5), LV_PART_MAIN);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_pos(label, 8, y);
  return label;
}

// -------------------- UI Construction --------------------
void createDashboardPage() {
  lv_obj_set_style_pad_all(tabDashboard, 6, LV_PART_MAIN);

  lv_obj_t* cardCpu = lv_obj_create(tabDashboard);
  lv_obj_set_size(cardCpu, 148, 64);
  lv_obj_set_pos(cardCpu, 4, 6);
  styleCard(cardCpu);

  lv_obj_t* labelCpuTitle = lv_label_create(cardCpu);
  lv_label_set_text(labelCpuTitle, "CPU Usage + Temp");
  lv_obj_set_style_text_color(labelCpuTitle, lv_color_hex(0xD8E1EC), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelCpuTitle, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_pos(labelCpuTitle, 0, 0);

  labelDashCpuValue = lv_label_create(cardCpu);
  lv_label_set_text(labelDashCpuValue, "0.0% | N/A");
  lv_obj_set_width(labelDashCpuValue, 132);
  lv_label_set_long_mode(labelDashCpuValue, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(labelDashCpuValue, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelDashCpuValue, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_pos(labelDashCpuValue, 0, 16);

  barDashCpu = lv_bar_create(cardCpu);
  lv_obj_set_size(barDashCpu, 132, 8);
  lv_obj_set_pos(barDashCpu, 0, 38);
  lv_bar_set_range(barDashCpu, 0, 100);
  styleBar(barDashCpu, lv_color_hex(0x27C97B));

  lv_obj_t* cardGpu = lv_obj_create(tabDashboard);
  lv_obj_set_size(cardGpu, 148, 64);
  lv_obj_set_pos(cardGpu, 160, 6);
  styleCard(cardGpu);

  lv_obj_t* labelGpuTitle = lv_label_create(cardGpu);
  lv_label_set_text(labelGpuTitle, "GPU Usage + Temp");
  lv_obj_set_style_text_color(labelGpuTitle, lv_color_hex(0xD8E1EC), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelGpuTitle, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_pos(labelGpuTitle, 0, 0);

  labelDashGpuValue = lv_label_create(cardGpu);
  lv_label_set_text(labelDashGpuValue, "N/A");
  lv_obj_set_width(labelDashGpuValue, 132);
  lv_label_set_long_mode(labelDashGpuValue, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(labelDashGpuValue, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelDashGpuValue, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_pos(labelDashGpuValue, 0, 16);

  barDashGpu = lv_bar_create(cardGpu);
  lv_obj_set_size(barDashGpu, 132, 8);
  lv_obj_set_pos(barDashGpu, 0, 38);
  lv_bar_set_range(barDashGpu, 0, 100);
  styleBar(barDashGpu, lv_color_hex(0x6C8BFF));

  lv_obj_t* cardRam = lv_obj_create(tabDashboard);
  lv_obj_set_size(cardRam, 302, 52);
  lv_obj_set_pos(cardRam, 4, 75);
  styleCard(cardRam);

  lv_obj_t* labelRamTitle = lv_label_create(cardRam);
  lv_label_set_text(labelRamTitle, "RAM Usage");
  lv_obj_set_style_text_color(labelRamTitle, lv_color_hex(0xD8E1EC), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelRamTitle, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_pos(labelRamTitle, 0, 0);

  labelDashRamValue = lv_label_create(cardRam);
  lv_label_set_text(labelDashRamValue, "0.0% | 0.00/0.00 GB");
  lv_obj_set_style_text_color(labelDashRamValue, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelDashRamValue, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_align(labelDashRamValue, LV_ALIGN_TOP_RIGHT, 0, 0);

  barDashRam = lv_bar_create(cardRam);
  lv_obj_set_size(barDashRam, 286, 8);
  lv_obj_set_pos(barDashRam, 0, 25);
  lv_bar_set_range(barDashRam, 0, 100);
  styleBar(barDashRam, lv_color_hex(0x3A8DFF));

  lv_obj_t* cardNet = lv_obj_create(tabDashboard);
  lv_obj_set_size(cardNet, 302, 52);
  lv_obj_set_pos(cardNet, 4, 133);
  styleCard(cardNet);

  lv_obj_t* labelNetTitle = lv_label_create(cardNet);
  lv_label_set_text(labelNetTitle, "Network Speed (Download / Upload)");
  lv_obj_set_style_text_color(labelNetTitle, lv_color_hex(0xD8E1EC), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelNetTitle, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_pos(labelNetTitle, 0, 0);

  labelDashNetValue = lv_label_create(cardNet);
  lv_label_set_text(labelDashNetValue, "Down: 0 B/s | Up: 0 B/s");
  lv_obj_set_width(labelDashNetValue, 286);
  lv_label_set_long_mode(labelDashNetValue, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(labelDashNetValue, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelDashNetValue, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_pos(labelDashNetValue, 0, 20);

}

void createSystemPage() {
  lv_obj_set_style_pad_all(tabSystem, 8, LV_PART_MAIN);
  createSectionTitle(tabSystem, "SYSTEM DETAILS", 0);

  labelSystemCpu = lv_label_create(tabSystem);
  lv_label_set_text(labelSystemCpu, "CPU: 0.0%");
  lv_obj_set_style_text_color(labelSystemCpu, lv_color_hex(0xEEF3F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelSystemCpu, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_width(labelSystemCpu, 300);
  lv_label_set_long_mode(labelSystemCpu, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(labelSystemCpu, 8, 24);

  labelSystemCpuTemp = lv_label_create(tabSystem);
  lv_label_set_text(labelSystemCpuTemp, "CPU Temp: N/A");
  lv_obj_set_style_text_color(labelSystemCpuTemp, lv_color_hex(0xEEF3F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelSystemCpuTemp, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_width(labelSystemCpuTemp, 300);
  lv_label_set_long_mode(labelSystemCpuTemp, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(labelSystemCpuTemp, 8, 42);

  labelSystemRam = lv_label_create(tabSystem);
  lv_label_set_text(labelSystemRam, "RAM: 0.0% | U:0.00 A:0.00 T:0.00 GB");
  lv_obj_set_style_text_color(labelSystemRam, lv_color_hex(0xEEF3F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelSystemRam, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_width(labelSystemRam, 300);
  lv_label_set_long_mode(labelSystemRam, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(labelSystemRam, 8, 60);

  labelSystemGpu = lv_label_create(tabSystem);
  lv_label_set_text(labelSystemGpu, "GPU: unavailable");
  lv_obj_set_style_text_color(labelSystemGpu, lv_color_hex(0xEEF3F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelSystemGpu, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_width(labelSystemGpu, 300);
  lv_label_set_long_mode(labelSystemGpu, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(labelSystemGpu, 8, 78);

  labelSystemGpuVram = lv_label_create(tabSystem);
  lv_label_set_text(labelSystemGpuVram, "VRAM: 0.0/0.0 MB (0.0%)");
  lv_obj_set_style_text_color(labelSystemGpuVram, lv_color_hex(0xEEF3F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelSystemGpuVram, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_width(labelSystemGpuVram, 300);
  lv_label_set_long_mode(labelSystemGpuVram, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(labelSystemGpuVram, 8, 96);

  labelSystemDisk = lv_label_create(tabSystem);
  lv_label_set_text(labelSystemDisk, "Disk: 0.0% | U:0.00 F:0.00 T:0.00 GB");
  lv_obj_set_style_text_color(labelSystemDisk, lv_color_hex(0xEEF3F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelSystemDisk, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_width(labelSystemDisk, 300);
  lv_label_set_long_mode(labelSystemDisk, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(labelSystemDisk, 8, 114);

  labelSystemNetBytes = lv_label_create(tabSystem);
  lv_label_set_text(labelSystemNetBytes, "Net Bytes S/R: 0 / 0");
  lv_obj_set_style_text_color(labelSystemNetBytes, lv_color_hex(0xEEF3F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelSystemNetBytes, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_width(labelSystemNetBytes, 300);
  lv_label_set_long_mode(labelSystemNetBytes, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(labelSystemNetBytes, 8, 132);

  labelSystemNetPackets = lv_label_create(tabSystem);
  lv_label_set_text(labelSystemNetPackets, "Net Packets S/R: 0 / 0");
  lv_obj_set_style_text_color(labelSystemNetPackets, lv_color_hex(0xEEF3F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelSystemNetPackets, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_width(labelSystemNetPackets, 300);
  lv_label_set_long_mode(labelSystemNetPackets, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(labelSystemNetPackets, 8, 150);

  labelSystemApi = lv_label_create(tabSystem);
  lv_label_set_text(labelSystemApi, "API: unknown | ts: --");
  lv_obj_set_style_text_color(labelSystemApi, lv_color_hex(0x93A1B5), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelSystemApi, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_width(labelSystemApi, 300);
  lv_label_set_long_mode(labelSystemApi, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(labelSystemApi, 8, 168);
}

void createGpuNetPage() {
  lv_obj_set_style_pad_all(tabGpuNet, 8, LV_PART_MAIN);
  createSectionTitle(tabGpuNet, "GPU + NETWORK", 0);

  labelGpuInfo = lv_label_create(tabGpuNet);
  lv_label_set_text(labelGpuInfo, "GPU: unavailable");
  lv_obj_set_style_text_color(labelGpuInfo, lv_color_hex(0xEEF3F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelGpuInfo, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_pos(labelGpuInfo, 8, 24);

  labelGpuVram = lv_label_create(tabGpuNet);
  lv_label_set_text(labelGpuVram, "VRAM: 0/0 MB (0%)");
  lv_obj_set_style_text_color(labelGpuVram, lv_color_hex(0xEEF3F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelGpuVram, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_pos(labelGpuVram, 8, 46);

  labelNetBytes = lv_label_create(tabGpuNet);
  lv_label_set_text(labelNetBytes, "Bytes S/R: 0 / 0");
  lv_obj_set_style_text_color(labelNetBytes, lv_color_hex(0xEEF3F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelNetBytes, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_pos(labelNetBytes, 8, 78);

  labelNetPackets = lv_label_create(tabGpuNet);
  lv_label_set_text(labelNetPackets, "Packets S/R: 0 / 0");
  lv_obj_set_style_text_color(labelNetPackets, lv_color_hex(0xEEF3F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelNetPackets, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_pos(labelNetPackets, 8, 100);
}

void createMediaPage() {
  lv_obj_set_style_pad_all(tabMedia, 8, LV_PART_MAIN);

  mediaHeroCard = lv_obj_create(tabMedia);
  lv_obj_set_size(mediaHeroCard, 302, 84);
  lv_obj_set_pos(mediaHeroCard, 2, 2);
  styleMediaCard(mediaHeroCard);

  lv_obj_t* heroTitle = lv_label_create(mediaHeroCard);
  lv_label_set_text(heroTitle, "Now Playing");
  lv_obj_set_style_text_color(heroTitle, lv_color_hex(0x9BA7B8), LV_PART_MAIN);
  lv_obj_set_style_text_font(heroTitle, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_pos(heroTitle, 0, 0);

  labelMediaState = lv_label_create(mediaHeroCard);
  lv_label_set_text(labelMediaState, "State: idle");
  lv_obj_set_style_text_color(labelMediaState, lv_color_hex(0x62D394), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelMediaState, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_pos(labelMediaState, 170, 0);

  labelMediaPlayer = lv_label_create(mediaHeroCard);
  lv_label_set_text(labelMediaPlayer, "Player: --");
  lv_obj_set_style_text_color(labelMediaPlayer, lv_color_hex(0xB7C1D1), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelMediaPlayer, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_pos(labelMediaPlayer, 0, 18);

  labelMediaTitle = lv_label_create(mediaHeroCard);
  lv_label_set_text(labelMediaTitle, "Title: --");
  lv_obj_set_width(labelMediaTitle, 286);
  lv_label_set_long_mode(labelMediaTitle, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(labelMediaTitle, lv_color_hex(0xEEF3F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelMediaTitle, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_pos(labelMediaTitle, 0, 37);

  labelMediaArtist = lv_label_create(mediaHeroCard);
  lv_label_set_text(labelMediaArtist, "Artist: --");
  lv_obj_set_width(labelMediaArtist, 286);
  lv_label_set_long_mode(labelMediaArtist, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(labelMediaArtist, lv_color_hex(0xB7C1D1), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelMediaArtist, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_pos(labelMediaArtist, 0, 56);

  labelMediaActionStatus = lv_label_create(mediaHeroCard);
  lv_label_set_text(labelMediaActionStatus, "Ready");
  lv_obj_set_style_text_color(labelMediaActionStatus, lv_color_hex(0x7FA8FF), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelMediaActionStatus, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_pos(labelMediaActionStatus, 170, 18);

  mediaControlsCard = lv_obj_create(tabMedia);
  lv_obj_set_size(mediaControlsCard, 302, 86);
  lv_obj_set_pos(mediaControlsCard, 2, 93);
  styleMediaCard(mediaControlsCard);

  lv_obj_t* controlsTitle = lv_label_create(mediaControlsCard);
  lv_label_set_text(controlsTitle, "Controls");
  lv_obj_set_style_text_color(controlsTitle, lv_color_hex(0x9BA7B8), LV_PART_MAIN);
  lv_obj_set_style_text_font(controlsTitle, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_pos(controlsTitle, 0, 0);

  labelMediaHint = lv_label_create(mediaControlsCard);
  lv_label_set_text(labelMediaHint, "Tap a button to control playback");
  lv_obj_set_style_text_color(labelMediaHint, lv_color_hex(0x6E7B8D), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelMediaHint, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_obj_set_pos(labelMediaHint, 0, 14);

  const int buttonY = 34;
  const int buttonW = 54;
  const int buttonH = 28;
  const int buttonGap = 4;

  lv_obj_t* btnPrev = lv_btn_create(mediaControlsCard);
  lv_obj_set_size(btnPrev, buttonW, buttonH);
  lv_obj_set_pos(btnPrev, 0, buttonY);
  styleMediaButton(btnPrev, lv_color_hex(0x243043));
  lv_obj_add_event_cb(btnPrev, onMediaControlClicked, LV_EVENT_CLICKED, (void*)"/previous");
  lv_obj_t* btnPrevLabel = lv_label_create(btnPrev);
  lv_label_set_text(btnPrevLabel, "<<");
  lv_obj_center(btnPrevLabel);

  lv_obj_t* btnVolDown = lv_btn_create(mediaControlsCard);
  lv_obj_set_size(btnVolDown, buttonW, buttonH);
  lv_obj_set_pos(btnVolDown, buttonW + buttonGap, buttonY);
  styleMediaButton(btnVolDown, lv_color_hex(0x1E3C59));
  lv_obj_add_event_cb(btnVolDown, onMediaControlClicked, LV_EVENT_CLICKED, (void*)"/volume_down");
  lv_obj_t* btnVolDownLabel = lv_label_create(btnVolDown);
  lv_label_set_text(btnVolDownLabel, "-");
  lv_obj_center(btnVolDownLabel);

  lv_obj_t* btnPlay = lv_btn_create(mediaControlsCard);
  lv_obj_set_size(btnPlay, buttonW, buttonH);
  lv_obj_set_pos(btnPlay, (buttonW + buttonGap) * 2, buttonY);
  styleMediaButton(btnPlay, lv_color_hex(0x1B6A4A));
  lv_obj_add_event_cb(btnPlay, onMediaControlClicked, LV_EVENT_CLICKED, (void*)"/playpause");
  lv_obj_t* btnPlayLabel = lv_label_create(btnPlay);
  lv_label_set_text(btnPlayLabel, "Play");
  lv_obj_center(btnPlayLabel);

  lv_obj_t* btnVolUp = lv_btn_create(mediaControlsCard);
  lv_obj_set_size(btnVolUp, buttonW, buttonH);
  lv_obj_set_pos(btnVolUp, (buttonW + buttonGap) * 3, buttonY);
  styleMediaButton(btnVolUp, lv_color_hex(0x1E3C59));
  lv_obj_add_event_cb(btnVolUp, onMediaControlClicked, LV_EVENT_CLICKED, (void*)"/volume_up");
  lv_obj_t* btnVolUpLabel = lv_label_create(btnVolUp);
  lv_label_set_text(btnVolUpLabel, "+");
  lv_obj_center(btnVolUpLabel);

  lv_obj_t* btnNext = lv_btn_create(mediaControlsCard);
  lv_obj_set_size(btnNext, buttonW, buttonH);
  lv_obj_set_pos(btnNext, (buttonW + buttonGap) * 4, buttonY);
  styleMediaButton(btnNext, lv_color_hex(0x243043));
  lv_obj_add_event_cb(btnNext, onMediaControlClicked, LV_EVENT_CLICKED, (void*)"/next");
  lv_obj_t* btnNextLabel = lv_label_create(btnNext);
  lv_label_set_text(btnNextLabel, ">>");
  lv_obj_center(btnNextLabel);
}

void createUi() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

  tabview = lv_tabview_create(scr, LV_DIR_TOP, 34);
  lv_obj_set_size(tabview, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_pos(tabview, 0, 0);

  lv_obj_t* tabBtns = lv_tabview_get_tab_btns(tabview);
  lv_obj_set_style_bg_color(tabBtns, lv_color_hex(0x111827), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(tabBtns, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(tabBtns, 0, LV_PART_MAIN);
  lv_obj_set_style_text_font(tabBtns, &lv_font_montserrat_12, LV_PART_ITEMS);
  lv_obj_set_style_text_color(tabBtns, lv_color_hex(0xC9D4E3), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(tabBtns, lv_color_hex(0x2D3D58), LV_PART_ITEMS | LV_STATE_CHECKED);

  tabDashboard = lv_tabview_add_tab(tabview, "Dash");
  tabSystem = lv_tabview_add_tab(tabview, "System");
  tabGpuNet = lv_tabview_add_tab(tabview, "GPU/Net");
  tabMedia = lv_tabview_add_tab(tabview, "Media");

  lv_obj_set_style_bg_color(tabDashboard, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(tabDashboard, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(tabSystem, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(tabSystem, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(tabGpuNet, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(tabGpuNet, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(tabMedia, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(tabMedia, LV_OPA_COVER, LV_PART_MAIN);

  createDashboardPage();
  createSystemPage();
  createGpuNetPage();
  createMediaPage();

  labelHeaderTitle = lv_label_create(scr);
  lv_label_set_text(labelHeaderTitle, "");
  lv_obj_set_style_text_color(labelHeaderTitle, lv_color_hex(0xF2F5F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelHeaderTitle, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_align(labelHeaderTitle, LV_ALIGN_TOP_LEFT, 6, 8);

  labelHeaderStatus = lv_label_create(scr);
  lv_label_set_text(labelHeaderStatus, "");
  lv_obj_set_style_text_color(labelHeaderStatus, lv_color_hex(0x62D394), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelHeaderStatus, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_align(labelHeaderStatus, LV_ALIGN_TOP_RIGHT, -6, 8);
}

// -------------------- UI Refresh --------------------
void refreshUiFromStats() {
  char line[128];

  if (!gStats.valid) {
    lv_label_set_text(labelHeaderStatus, "");
    lv_bar_set_value(barDashCpu, 0, LV_ANIM_OFF);
    lv_bar_set_value(barDashGpu, 0, LV_ANIM_OFF);
    lv_bar_set_value(barDashRam, 0, LV_ANIM_OFF);
    lv_label_set_text(labelDashCpuValue, "0.0% | N/A");
    lv_label_set_text(labelDashGpuValue, "N/A");
    lv_label_set_text(labelDashRamValue, "0.0% | 0.00/0.00 GB");
    lv_label_set_text(labelDashNetValue, "Down: 0 B/s | Up: 0 B/s");
    return;
  }

  lv_label_set_text(labelHeaderStatus, "");

  lv_bar_set_value(barDashCpu, (int)clampFloat(gStats.cpuPercent, 0.0f, 100.0f), LV_ANIM_OFF);
  if (gStats.cpuTempAvailable) {
    snprintf(line, sizeof(line), "%.1f%% | %.1f C", gStats.cpuPercent, gStats.cpuTempC);
  } else {
    snprintf(line, sizeof(line), "%.1f%% | N/A", gStats.cpuPercent);
  }
  lv_label_set_text(labelDashCpuValue, line);

  if (gStats.gpuAvailable) {
    lv_bar_set_value(barDashGpu, (int)clampFloat(gStats.gpuUsagePercent, 0.0f, 100.0f), LV_ANIM_OFF);
    snprintf(line, sizeof(line), "%.1f%% | %.1f C", gStats.gpuUsagePercent, gStats.gpuTempC);
  } else {
    lv_bar_set_value(barDashGpu, 0, LV_ANIM_OFF);
    snprintf(line, sizeof(line), "N/A");
  }
  lv_label_set_text(labelDashGpuValue, line);

  lv_bar_set_value(barDashRam, (int)clampFloat(gStats.ramUsedPercent, 0.0f, 100.0f), LV_ANIM_OFF);
  snprintf(line, sizeof(line), "%.1f%% | %.2f/%.2f GB", gStats.ramUsedPercent, gStats.ramUsedGb, gStats.ramTotalGb);
  lv_label_set_text(labelDashRamValue, line);

  char downText[24];
  char upText[24];
  formatSpeed(downText, sizeof(downText), gDownloadBytesPerSec);
  formatSpeed(upText, sizeof(upText), gUploadBytesPerSec);
  snprintf(line, sizeof(line), "Down: %s | Up: %s", downText, upText);
  lv_label_set_text(labelDashNetValue, line);

  snprintf(line, sizeof(line), "CPU: %.1f%%", gStats.cpuPercent);
  lv_label_set_text(labelSystemCpu, line);

  if (gStats.cpuTempAvailable) {
    snprintf(line, sizeof(line), "CPU Temp: %.1f C / %.1f F", gStats.cpuTempC, gStats.cpuTempF);
  } else {
    snprintf(line, sizeof(line), "CPU Temp: unavailable");
  }
  lv_label_set_text(labelSystemCpuTemp, line);

  snprintf(line, sizeof(line), "RAM: %.1f%% | U:%.2f A:%.2f T:%.2f GB", gStats.ramUsedPercent, gStats.ramUsedGb, gStats.ramAvailableGb, gStats.ramTotalGb);
  lv_label_set_text(labelSystemRam, line);

  if (gStats.gpuAvailable) {
    snprintf(line, sizeof(line), "GPU: %.1f%% | %.1f C / %.1f F", gStats.gpuUsagePercent, gStats.gpuTempC, gStats.gpuTempF);
  } else {
    snprintf(line, sizeof(line), "GPU: unavailable");
  }
  lv_label_set_text(labelSystemGpu, line);

  snprintf(line, sizeof(line), "VRAM: %.1f/%.1f MB (%.1f%%)", gStats.gpuVramUsedMb, gStats.gpuVramTotalMb, gStats.gpuVramUsedPercent);
  lv_label_set_text(labelSystemGpuVram, line);

  snprintf(line, sizeof(line), "Disk: %.1f%% | U:%.2f F:%.2f T:%.2f GB", gStats.diskUsedPercent, gStats.diskUsedGb, gStats.diskFreeGb, gStats.diskTotalGb);
  lv_label_set_text(labelSystemDisk, line);

  snprintf(line, sizeof(line), "Net Bytes S/R: %llu / %llu", (unsigned long long)gStats.bytesSent, (unsigned long long)gStats.bytesReceived);
  lv_label_set_text(labelSystemNetBytes, line);

  snprintf(line, sizeof(line), "Net Packets S/R: %llu / %llu", (unsigned long long)gStats.packetsSent, (unsigned long long)gStats.packetsReceived);
  lv_label_set_text(labelSystemNetPackets, line);

  snprintf(line, sizeof(line), "API: %s | ts: %s", gStats.apiStatus, gStats.timestamp);
  lv_label_set_text(labelSystemApi, line);

  if (gStats.gpuAvailable) {
    snprintf(line, sizeof(line), "GPU: %.1f%% | %.1f C", gStats.gpuUsagePercent, gStats.gpuTempC);
  } else {
    snprintf(line, sizeof(line), "GPU: unavailable");
  }
  lv_label_set_text(labelGpuInfo, line);

  snprintf(line, sizeof(line), "VRAM: %.1f/%.1f MB (%.1f%%)", gStats.gpuVramUsedMb, gStats.gpuVramTotalMb, gStats.gpuVramUsedPercent);
  lv_label_set_text(labelGpuVram, line);

  snprintf(line, sizeof(line), "Bytes S/R: %llu / %llu", (unsigned long long)gStats.bytesSent, (unsigned long long)gStats.bytesReceived);
  lv_label_set_text(labelNetBytes, line);

  snprintf(line, sizeof(line), "Packets S/R: %llu / %llu", (unsigned long long)gStats.packetsSent, (unsigned long long)gStats.packetsReceived);
  lv_label_set_text(labelNetPackets, line);

  snprintf(line, sizeof(line), "State: %s", gStats.mediaPlaybackState);
  lv_label_set_text(labelMediaState, line);

  snprintf(line, sizeof(line), "Player: %s", gStats.mediaPlayer);
  lv_label_set_text(labelMediaPlayer, line);

  snprintf(line, sizeof(line), "Title: %s", gStats.mediaTitle);
  lv_label_set_text(labelMediaTitle, line);

  snprintf(line, sizeof(line), "Artist: %s", gStats.mediaArtist);
  lv_label_set_text(labelMediaArtist, line);
}

// -------------------- Connectivity --------------------
void connectToWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  lv_label_set_text(labelHeaderStatus, "");
  Serial.print("[WiFi] Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startMs = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(220);
    lv_timer_handler();
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Connected. IP: ");
    Serial.println(WiFi.localIP());
    lv_label_set_text(labelHeaderStatus, "");
  } else {
    Serial.println("[WiFi] Connection timeout.");
    lv_label_set_text(labelHeaderStatus, "");
  }
}

// -------------------- API --------------------
void fetchStats() {
  if (WiFi.status() != WL_CONNECTED) {
    lv_label_set_text(labelHeaderStatus, "");
    gUploadBytesPerSec = 0.0f;
    gDownloadBytesPerSec = 0.0f;
    prevNetSampleMs = 0;
    gStats.valid = false;
    refreshPending = true;
    return;
  }

  HTTPClient http;
  http.setConnectTimeout(4500);
  http.setTimeout(4500);

  if (!http.begin(SERVER_URL)) {
    lv_label_set_text(labelHeaderStatus, "");
    gStats.valid = false;
    refreshPending = true;
    http.end();
    return;
  }

  int httpCode = http.GET();
  if (httpCode <= 0) {
    Serial.print("[HTTP] Request failed: ");
    Serial.println(http.errorToString(httpCode));
    lv_label_set_text(labelHeaderStatus, "");
    gStats.valid = false;
    refreshPending = true;
    http.end();
    return;
  }

  if (httpCode != HTTP_CODE_OK) {
    Serial.print("[HTTP] Status: ");
    Serial.println(httpCode);
    lv_label_set_text(labelHeaderStatus, "");
    gStats.valid = false;
    refreshPending = true;
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
    lv_label_set_text(labelHeaderStatus, "");
    gStats.valid = false;
    refreshPending = true;
    return;
  }

  JsonObject data = doc["data"].as<JsonObject>();
  if (data.isNull()) {
    lv_label_set_text(labelHeaderStatus, "");
    gStats.valid = false;
    refreshPending = true;
    return;
  }

  setSafeCopy(gStats.apiStatus, sizeof(gStats.apiStatus), doc["status"] | "unknown", "unknown");
  setSafeCopy(gStats.timestamp, sizeof(gStats.timestamp), doc["timestamp"] | "--", "--");

  gStats.cpuPercent = data["cpu_percent"] | 0.0f;

  JsonObject cpuTemp = data["cpu_temp"].as<JsonObject>();
  gStats.cpuTempAvailable = cpuTemp["available"] | false;
  gStats.cpuTempC = cpuTemp["celsius"] | -1.0f;
  gStats.cpuTempF = cpuTemp["fahrenheit"] | -1.0f;

  JsonObject ram = data["ram"].as<JsonObject>();
  gStats.ramUsedPercent = ram["used_percent"] | 0.0f;
  gStats.ramUsedGb = ram["used_gb"] | 0.0f;
  gStats.ramAvailableGb = ram["available_gb"] | 0.0f;
  gStats.ramTotalGb = ram["total_gb"] | 0.0f;

  JsonObject gpu = data["gpu"].as<JsonObject>();
  gStats.gpuAvailable = gpu["available"] | false;
  gStats.gpuUsagePercent = gpu["gpu_usage_percent"] | 0.0f;
  gStats.gpuTempC = gpu["temperature_celsius"] | -1.0f;
  gStats.gpuTempF = gpu["temperature_fahrenheit"] | -1.0f;
  gStats.gpuVramUsedMb = gpu["vram_used_mb"] | 0.0f;
  gStats.gpuVramTotalMb = gpu["vram_total_mb"] | 0.0f;
  gStats.gpuVramUsedPercent = gpu["vram_used_percent"] | 0.0f;

  JsonObject disk = data["disk"].as<JsonObject>();
  gStats.diskUsedPercent = disk["used_percent"] | 0.0f;
  gStats.diskUsedGb = disk["used_gb"] | 0.0f;
  gStats.diskFreeGb = disk["free_gb"] | 0.0f;
  gStats.diskTotalGb = disk["total_gb"] | 0.0f;

  JsonObject network = data["network"].as<JsonObject>();
  gStats.bytesSent = network["bytes_sent"] | (uint64_t)0;
  gStats.bytesReceived = network["bytes_received"] | (uint64_t)0;
  gStats.packetsSent = network["packets_sent"] | (uint64_t)0;
  gStats.packetsReceived = network["packets_received"] | (uint64_t)0;

  unsigned long nowMs = millis();
  if (prevNetSampleMs > 0 && nowMs > prevNetSampleMs) {
    float dtSec = (float)(nowMs - prevNetSampleMs) / 1000.0f;
    if (dtSec > 0.0f) {
      uint64_t sentDelta = (gStats.bytesSent >= prevBytesSent) ? (gStats.bytesSent - prevBytesSent) : 0;
      uint64_t recvDelta = (gStats.bytesReceived >= prevBytesReceived) ? (gStats.bytesReceived - prevBytesReceived) : 0;
      gUploadBytesPerSec = (float)sentDelta / dtSec;
      gDownloadBytesPerSec = (float)recvDelta / dtSec;
    }
  }
  prevBytesSent = gStats.bytesSent;
  prevBytesReceived = gStats.bytesReceived;
  prevNetSampleMs = nowMs;

  JsonObject media = data["media"].as<JsonObject>();
  gStats.mediaAvailable = media["available"] | false;
  setSafeCopy(gStats.mediaTitle, sizeof(gStats.mediaTitle), media["title"] | "No media", "No media");
  setSafeCopy(gStats.mediaArtist, sizeof(gStats.mediaArtist), media["artist"] | "--", "--");
  setSafeCopy(gStats.mediaPlaybackState, sizeof(gStats.mediaPlaybackState), media["playback_state"] | "idle", "idle");
  setSafeCopy(gStats.mediaPlayer, sizeof(gStats.mediaPlayer), media["player"] | "--", "--");

  gStats.valid = true;
  refreshPending = true;

  Serial.println("------ DeskPulse ------");
  Serial.print("CPU: ");
  Serial.print(gStats.cpuPercent, 1);
  Serial.println(" %");
  Serial.print("RAM: ");
  Serial.print(gStats.ramUsedPercent, 1);
  Serial.println(" %");
  Serial.print("CPU Temp: ");
  if (gStats.cpuTempAvailable) {
    Serial.println(gStats.cpuTempC, 1);
  } else {
    Serial.println("N/A");
  }
  Serial.print("Media: ");
  Serial.print(gStats.mediaTitle);
  Serial.print(" - ");
  Serial.println(gStats.mediaArtist);
}

// -------------------- Arduino Entry --------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  initLvglDisplay();
  createUi();
  refreshUiFromStats();

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
  }

  if (refreshPending) {
    refreshPending = false;
    refreshUiFromStats();
  }
}
