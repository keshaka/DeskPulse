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
lv_obj_t* barCpu;
lv_obj_t* barRam;
lv_obj_t* barTemp;
lv_obj_t* labelCpuValue;
lv_obj_t* labelRamValue;
lv_obj_t* labelTempValue;
lv_obj_t* labelDashNowPlaying;
lv_obj_t* labelDashNowState;

// System page
lv_obj_t* labelSystemCpu;
lv_obj_t* labelSystemCpuTemp;
lv_obj_t* labelSystemRam;
lv_obj_t* labelSystemDisk;
lv_obj_t* labelSystemApi;

// GPU / Network page
lv_obj_t* labelGpuInfo;
lv_obj_t* labelGpuVram;
lv_obj_t* labelNetBytes;
lv_obj_t* labelNetPackets;

// Media page
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
  lv_obj_set_pos(cardCpu, 6, 8);
  styleCard(cardCpu);

  lv_obj_t* labelCpuTitle = lv_label_create(cardCpu);
  lv_label_set_text(labelCpuTitle, "CPU");
  lv_obj_set_style_text_color(labelCpuTitle, lv_color_hex(0xD8E1EC), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelCpuTitle, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_pos(labelCpuTitle, 0, 0);

  labelCpuValue = lv_label_create(cardCpu);
  lv_label_set_text(labelCpuValue, "0.0%");
  lv_obj_set_style_text_color(labelCpuValue, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelCpuValue, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(labelCpuValue, LV_ALIGN_TOP_RIGHT, 0, 0);

  barCpu = lv_bar_create(cardCpu);
  lv_obj_set_size(barCpu, 132, 12);
  lv_obj_set_pos(barCpu, 0, 36);
  lv_bar_set_range(barCpu, 0, 100);
  styleBar(barCpu, lv_color_hex(0x27C97B));

  lv_obj_t* cardRam = lv_obj_create(tabDashboard);
  lv_obj_set_size(cardRam, 148, 64);
  lv_obj_set_pos(cardRam, 160, 8);
  styleCard(cardRam);

  lv_obj_t* labelRamTitle = lv_label_create(cardRam);
  lv_label_set_text(labelRamTitle, "RAM");
  lv_obj_set_style_text_color(labelRamTitle, lv_color_hex(0xD8E1EC), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelRamTitle, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_pos(labelRamTitle, 0, 0);

  labelRamValue = lv_label_create(cardRam);
  lv_label_set_text(labelRamValue, "0.0%");
  lv_obj_set_style_text_color(labelRamValue, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelRamValue, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(labelRamValue, LV_ALIGN_TOP_RIGHT, 0, 0);

  barRam = lv_bar_create(cardRam);
  lv_obj_set_size(barRam, 132, 12);
  lv_obj_set_pos(barRam, 0, 36);
  lv_bar_set_range(barRam, 0, 100);
  styleBar(barRam, lv_color_hex(0x3A8DFF));

  lv_obj_t* cardTemp = lv_obj_create(tabDashboard);
  lv_obj_set_size(cardTemp, 302, 64);
  lv_obj_set_pos(cardTemp, 6, 78);
  styleCard(cardTemp);

  lv_obj_t* labelTempTitle = lv_label_create(cardTemp);
  lv_label_set_text(labelTempTitle, "Temperature");
  lv_obj_set_style_text_color(labelTempTitle, lv_color_hex(0xD8E1EC), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelTempTitle, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_pos(labelTempTitle, 0, 0);

  labelTempValue = lv_label_create(cardTemp);
  lv_label_set_text(labelTempValue, "N/A");
  lv_obj_set_style_text_color(labelTempValue, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelTempValue, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(labelTempValue, LV_ALIGN_TOP_RIGHT, 0, 0);

  barTemp = lv_bar_create(cardTemp);
  lv_obj_set_size(barTemp, 286, 12);
  lv_obj_set_pos(barTemp, 0, 36);
  lv_bar_set_range(barTemp, 0, 100);
  styleBar(barTemp, lv_color_hex(0xFF8A3D));

  lv_obj_t* cardMedia = lv_obj_create(tabDashboard);
  lv_obj_set_size(cardMedia, 302, 74);
  lv_obj_set_pos(cardMedia, 6, 148);
  styleCard(cardMedia);

  lv_obj_t* labelNow = lv_label_create(cardMedia);
  lv_label_set_text(labelNow, "Now Playing");
  lv_obj_set_style_text_color(labelNow, lv_color_hex(0x93A1B5), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelNow, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_pos(labelNow, 0, 0);

  labelDashNowState = lv_label_create(cardMedia);
  lv_label_set_text(labelDashNowState, "idle");
  lv_obj_set_style_text_color(labelDashNowState, lv_color_hex(0x62D394), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelDashNowState, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_align(labelDashNowState, LV_ALIGN_TOP_RIGHT, 0, 0);

  labelDashNowPlaying = lv_label_create(cardMedia);
  lv_label_set_text(labelDashNowPlaying, "No media playing");
  lv_obj_set_style_text_color(labelDashNowPlaying, lv_color_hex(0xEEF3F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelDashNowPlaying, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_width(labelDashNowPlaying, 286);
  lv_label_set_long_mode(labelDashNowPlaying, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(labelDashNowPlaying, 0, 22);
}

void createSystemPage() {
  lv_obj_set_style_pad_all(tabSystem, 8, LV_PART_MAIN);
  createSectionTitle(tabSystem, "SYSTEM DETAILS", 0);

  labelSystemCpu = lv_label_create(tabSystem);
  lv_label_set_text(labelSystemCpu, "CPU: 0.0%");
  lv_obj_set_style_text_color(labelSystemCpu, lv_color_hex(0xEEF3F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelSystemCpu, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_pos(labelSystemCpu, 8, 24);

  labelSystemCpuTemp = lv_label_create(tabSystem);
  lv_label_set_text(labelSystemCpuTemp, "CPU Temp: N/A");
  lv_obj_set_style_text_color(labelSystemCpuTemp, lv_color_hex(0xEEF3F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelSystemCpuTemp, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_pos(labelSystemCpuTemp, 8, 46);

  labelSystemRam = lv_label_create(tabSystem);
  lv_label_set_text(labelSystemRam, "RAM: 0.0% | 0.00/0.00 GB");
  lv_obj_set_style_text_color(labelSystemRam, lv_color_hex(0xEEF3F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelSystemRam, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_pos(labelSystemRam, 8, 68);

  labelSystemDisk = lv_label_create(tabSystem);
  lv_label_set_text(labelSystemDisk, "Disk: 0.0% | 0.00/0.00 GB");
  lv_obj_set_style_text_color(labelSystemDisk, lv_color_hex(0xEEF3F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelSystemDisk, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_pos(labelSystemDisk, 8, 90);

  labelSystemApi = lv_label_create(tabSystem);
  lv_label_set_text(labelSystemApi, "API: unknown | ts: --");
  lv_obj_set_style_text_color(labelSystemApi, lv_color_hex(0x93A1B5), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelSystemApi, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_pos(labelSystemApi, 8, 116);
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
  createSectionTitle(tabMedia, "MEDIA", 0);

  labelMediaState = lv_label_create(tabMedia);
  lv_label_set_text(labelMediaState, "State: idle");
  lv_obj_set_style_text_color(labelMediaState, lv_color_hex(0x62D394), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelMediaState, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_pos(labelMediaState, 8, 24);

  labelMediaPlayer = lv_label_create(tabMedia);
  lv_label_set_text(labelMediaPlayer, "Player: --");
  lv_obj_set_style_text_color(labelMediaPlayer, lv_color_hex(0xEEF3F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelMediaPlayer, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_pos(labelMediaPlayer, 8, 46);

  labelMediaTitle = lv_label_create(tabMedia);
  lv_label_set_text(labelMediaTitle, "Title: --");
  lv_obj_set_width(labelMediaTitle, 300);
  lv_label_set_long_mode(labelMediaTitle, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(labelMediaTitle, lv_color_hex(0xEEF3F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelMediaTitle, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_pos(labelMediaTitle, 8, 74);

  labelMediaArtist = lv_label_create(tabMedia);
  lv_label_set_text(labelMediaArtist, "Artist: --");
  lv_obj_set_width(labelMediaArtist, 300);
  lv_label_set_long_mode(labelMediaArtist, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(labelMediaArtist, lv_color_hex(0xEEF3F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelMediaArtist, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_pos(labelMediaArtist, 8, 96);
}

void createUi() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B1018), LV_PART_MAIN);
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

  createDashboardPage();
  createSystemPage();
  createGpuNetPage();
  createMediaPage();

  labelHeaderTitle = lv_label_create(scr);
  lv_label_set_text(labelHeaderTitle, "DeskPulse");
  lv_obj_set_style_text_color(labelHeaderTitle, lv_color_hex(0xF2F5F9), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelHeaderTitle, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_align(labelHeaderTitle, LV_ALIGN_TOP_LEFT, 6, 8);

  labelHeaderStatus = lv_label_create(scr);
  lv_label_set_text(labelHeaderStatus, "Starting...");
  lv_obj_set_style_text_color(labelHeaderStatus, lv_color_hex(0x62D394), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelHeaderStatus, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_align(labelHeaderStatus, LV_ALIGN_TOP_RIGHT, -6, 8);
}

// -------------------- UI Refresh --------------------
void refreshUiFromStats() {
  char line[128];

  if (!gStats.valid) {
    lv_label_set_text(labelHeaderStatus, "Offline / no data");
    lv_label_set_text(labelDashNowState, "idle");
    lv_label_set_text(labelDashNowPlaying, "No media playing");
    return;
  }

  lv_label_set_text(labelHeaderStatus, "Live");

  lv_bar_set_value(barCpu, (int)clampFloat(gStats.cpuPercent, 0.0f, 100.0f), LV_ANIM_OFF);
  snprintf(line, sizeof(line), "%.1f%%", gStats.cpuPercent);
  lv_label_set_text(labelCpuValue, line);

  lv_bar_set_value(barRam, (int)clampFloat(gStats.ramUsedPercent, 0.0f, 100.0f), LV_ANIM_OFF);
  snprintf(line, sizeof(line), "%.1f%%", gStats.ramUsedPercent);
  lv_label_set_text(labelRamValue, line);

  float displayTemp = gStats.cpuTempAvailable ? gStats.cpuTempC : gStats.gpuTempC;
  if (displayTemp >= 0.0f) {
    lv_bar_set_value(barTemp, (int)clampFloat(displayTemp, 0.0f, 100.0f), LV_ANIM_OFF);
    snprintf(line, sizeof(line), "%.1f C", displayTemp);
    lv_label_set_text(labelTempValue, line);
  } else {
    lv_bar_set_value(barTemp, 0, LV_ANIM_OFF);
    lv_label_set_text(labelTempValue, "N/A");
  }

  lv_label_set_text(labelDashNowState, gStats.mediaPlaybackState);
  if (gStats.mediaAvailable) {
    snprintf(line, sizeof(line), "%s - %s", gStats.mediaTitle, gStats.mediaArtist);
    lv_label_set_text(labelDashNowPlaying, line);
  } else {
    lv_label_set_text(labelDashNowPlaying, "No media playing");
  }

  snprintf(line, sizeof(line), "CPU: %.1f%%", gStats.cpuPercent);
  lv_label_set_text(labelSystemCpu, line);

  if (gStats.cpuTempAvailable) {
    snprintf(line, sizeof(line), "CPU Temp: %.1f C / %.1f F", gStats.cpuTempC, gStats.cpuTempF);
  } else {
    snprintf(line, sizeof(line), "CPU Temp: unavailable");
  }
  lv_label_set_text(labelSystemCpuTemp, line);

  snprintf(line, sizeof(line), "RAM: %.1f%% | %.2f/%.2f GB", gStats.ramUsedPercent, gStats.ramUsedGb, gStats.ramTotalGb);
  lv_label_set_text(labelSystemRam, line);

  snprintf(line, sizeof(line), "Disk: %.1f%% | %.2f/%.2f GB", gStats.diskUsedPercent, gStats.diskUsedGb, gStats.diskTotalGb);
  lv_label_set_text(labelSystemDisk, line);

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

  lv_label_set_text(labelHeaderStatus, "WiFi connecting...");
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
    lv_label_set_text(labelHeaderStatus, "WiFi connected");
  } else {
    Serial.println("[WiFi] Connection timeout.");
    lv_label_set_text(labelHeaderStatus, "WiFi offline");
  }
}

// -------------------- API --------------------
void fetchStats() {
  if (WiFi.status() != WL_CONNECTED) {
    lv_label_set_text(labelHeaderStatus, "WiFi offline");
    gStats.valid = false;
    refreshPending = true;
    return;
  }

  HTTPClient http;
  http.setConnectTimeout(4500);
  http.setTimeout(4500);

  if (!http.begin(SERVER_URL)) {
    lv_label_set_text(labelHeaderStatus, "HTTP init error");
    gStats.valid = false;
    refreshPending = true;
    http.end();
    return;
  }

  int httpCode = http.GET();
  if (httpCode <= 0) {
    Serial.print("[HTTP] Request failed: ");
    Serial.println(http.errorToString(httpCode));
    lv_label_set_text(labelHeaderStatus, "Server unreachable");
    gStats.valid = false;
    refreshPending = true;
    http.end();
    return;
  }

  if (httpCode != HTTP_CODE_OK) {
    Serial.print("[HTTP] Status: ");
    Serial.println(httpCode);
    lv_label_set_text(labelHeaderStatus, "Server error");
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
    lv_label_set_text(labelHeaderStatus, "JSON parse error");
    gStats.valid = false;
    refreshPending = true;
    return;
  }

  JsonObject data = doc["data"].as<JsonObject>();
  if (data.isNull()) {
    lv_label_set_text(labelHeaderStatus, "Invalid payload");
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
