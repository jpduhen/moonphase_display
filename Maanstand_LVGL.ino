/*
 * Maanstand op ronde GC9A01 TFT (240x240) met LVGL
 * Tekst rond de maan via lv_arclabel.
 * ESP32-S3 SuperMini – WiFi + NTP.
 */

#include <lvgl.h>
#if LV_USE_TFT_ESPI
#include <TFT_eSPI.h>
#endif

#include <WiFi.h>
#include <FS.h>           // Vereist voor WebServer.h (via WiFiManager) – FS in scope
using namespace fs;       // Zorgt dat FS in scope is voor WebServer.h
#include <WiFiManager.h>
#include <time.h>
#include <math.h>
#include "moon_image.h"

#define WIFI_AP_NAAM "Maanstand_WiFi"   // AP-naam bij eerste keer / geen opgeslagen netwerk
#define NTP_SERVER "pool.ntp.org"
// NL: wintertijd UTC+1, zomertijd UTC+2; expliciete offset voor CEST (sommige ESP32-cores)
#define TZ_NEDERLAND "CET-1CEST-2,M3.5.0,M10.5.0/3"

#define NEW_MOON_REF_EPOCH 947182440UL
#define LUNAR_CYCLE_DAYS 29.530588853f

#define TFT_HOR_RES   240
#define TFT_VER_RES   240
#define TFT_ROTATION  LV_DISPLAY_ROTATION_180

#define CX 120
#define CY 120
#define MOON_R 88
#define MOON_SZ (MOON_R * 2)

#define DRAW_BUF_SIZE (TFT_HOR_RES * TFT_VER_RES / 10 * (LV_COLOR_DEPTH / 8))
static uint32_t draw_buf[DRAW_BUF_SIZE / 4];

static uint32_t my_tick(void) { return millis(); }

const char* faseNamen[] = {
  "Nieuwe maan",
  "Wassende sikkel",
  "Eerste kwartier",
  "Wassende maan",
  "Volle maan",
  "Krimpende maan",
  "Laatste kwartier",
  "Afnemende sikkel"
};

// Wicca / Keltisch jaarwiel (volgorde: na Yule)
static const char* seizoenNamen[] = {
  "Yule",         // Winterzonnewende ~21 dec
  "Imbolc",       // 1–2 feb
  "Ostara",       // Lente-equinox ~20 mrt
  "Beltane",      // 30 apr – 1 mei
  "Litha",        // Zomerzonnewende ~21 jun
  "Lughnasadh",   // 1–2 aug (Lammas)
  "Mabon",        // Herfst-equinox ~22 sep
  "Samhain"       // 31 okt – 1 nov
};

static int seizoenIndex(int mon, int mday) {
  int d = mon * 32 + mday;
  if (d >= 12*32+21 || d <  2*32+1)  return 0;   // Yule (21 dec t/m 31 jan)
  if (d <  3*32+20) return 1;   // Imbolc (1 feb t/m 19 mrt)
  if (d <  5*32+1)  return 2;   // Ostara (20 mrt t/m 30 apr)
  if (d <  6*32+21) return 3;   // Beltane (1 mei t/m 20 jun)
  if (d <  8*32+1)  return 4;   // Litha (21 jun t/m 31 jul)
  if (d <  9*32+22) return 5;   // Lughnasadh (1 aug t/m 21 sep)
  if (d < 11*32+1)  return 6;   // Mabon (22 sep t/m 31 okt)
  return 7;   // Samhain (1 nov t/m 20 dec)
}

// UI-handles (na eerste build)
static lv_obj_t * labelStatus = NULL;
static lv_obj_t * moonObj = NULL;
static lv_obj_t * shadowObj = NULL;   // container met donkere maan-foto
static lv_obj_t * shadowImg = NULL;   // kind: maanbeeld met recolor
static lv_obj_t * arclabelFase = NULL;   // fase-naam bovenaan scherm, gebogen
static lv_obj_t * arclabelOnder = NULL; // "Xe dag - dd-mm-yyyy hh:mm:ss" onderaan, gebogen, 1 regel

static float maanfase() {
  time_t now;
  time(&now);
  if (now < NEW_MOON_REF_EPOCH) return 0.0f;
  float dagen = (float)(now - NEW_MOON_REF_EPOCH) / 86400.0f;
  return fmodf(dagen, LUNAR_CYCLE_DAYS) / LUNAR_CYCLE_DAYS;
}

static int maanDag() {
  return (int)(maanfase() * LUNAR_CYCLE_DAYS) % 30;
}

/** Maanleeftijd in dagen sinds nieuwe maan (bijv. 2.74). */
static float maanLeeftijdDagen(float fase) {
  return fase * LUNAR_CYCLE_DAYS;
}

/** Illuminatie in procent (0..100), formule (1-cos(2π·fase))/2. */
static float illuminatiePct(float fase) {
  return (1.0f - cosf(2.0f * 3.14159265f * fase)) * 50.0f;
}

static int faseIndex(float fase) {
  return (int)(fase * 8) % 8;
}

static void maanstandUI(lv_obj_t * screen) {
  float fase = maanfase();
  int fi = faseIndex(fase);
  // Schaduw: wassend = rechterkant verlicht (schaduw links), afnemend = linkerkant (schaduw rechts).
  // Cosinus-mapping zodat sikkelgrootte beter overeenkomt met illuminatie (dunne sikkel bij lage %).
  float phaseT = (fase < 0.5f) ? (2.0f * fase) : (2.0f * (1.0f - fase));
  float offset = (MOON_R * 0.5f) * (1.0f - cosf(3.14159265f * phaseT));
  int sx = (fase < 0.5f) ? (32 - (int)offset) : (32 + (int)offset);

  lv_color_t skyColor = lv_color_make(32, 8, 48);
  lv_color_t moonColor = lv_color_make(230, 230, 200);
  lv_color_t textColor = lv_color_white();
  lv_color_t textColor2 = lv_color_make(192, 192, 192);

  lv_obj_set_style_bg_color(screen, skyColor, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(screen, 0, LV_PART_MAIN);

  // Maan: realistische foto (of placeholder uit moon_image.h)
  moonObj = lv_image_create(screen);
  lv_obj_set_size(moonObj, MOON_SZ, MOON_SZ);
  lv_obj_set_pos(moonObj, 32, 32);
  lv_image_set_src(moonObj, &moon_img_dsc);
  lv_image_set_inner_align(moonObj, LV_IMAGE_ALIGN_STRETCH);
  lv_obj_set_style_radius(moonObj, MOON_R, LV_PART_MAIN);
  lv_obj_set_style_clip_corner(moonObj, true, LV_PART_MAIN);
  lv_obj_set_style_border_width(moonObj, 0, LV_PART_MAIN);
  lv_obj_clear_flag(moonObj, LV_OBJ_FLAG_SCROLLABLE);

  // Schaduw = donkere versie van maanfoto: donkere ondergrond + maanbeeld op ~40% opa (natuurlijker dan recolor)
  lv_color_t shadowBg = lv_color_hex(0x0a0810);   // donker paarsgrijs, past bij sky
  shadowObj = lv_obj_create(screen);
  lv_obj_set_size(shadowObj, MOON_SZ, MOON_SZ);
  lv_obj_set_pos(shadowObj, sx, 32);
  lv_obj_set_style_pad_all(shadowObj, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(shadowObj, MOON_R, LV_PART_MAIN);
  lv_obj_set_style_bg_color(shadowObj, shadowBg, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(shadowObj, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(shadowObj, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(shadowObj, 0, LV_PART_MAIN);
  lv_obj_set_style_clip_corner(shadowObj, true, LV_PART_MAIN);
  lv_obj_clear_flag(shadowObj, LV_OBJ_FLAG_SCROLLABLE);

  shadowImg = lv_image_create(shadowObj);
  lv_obj_set_size(shadowImg, MOON_SZ, MOON_SZ);
  lv_obj_set_pos(shadowImg, 32 - sx, 0);
  lv_obj_set_style_pad_all(shadowImg, 0, LV_PART_MAIN);
  lv_image_set_src(shadowImg, &moon_img_dsc);
  lv_image_set_inner_align(shadowImg, LV_IMAGE_ALIGN_STRETCH);
  lv_obj_set_style_image_opa(shadowImg, LV_OPA_20, LV_PART_MAIN);  // 20% maantextuur + 80% shadowBg = nog donkerdere schaduw
  lv_obj_set_style_border_width(shadowImg, 0, LV_PART_MAIN);
  lv_obj_clear_flag(shadowImg, LV_OBJ_FLAG_SCROLLABLE);

  // Bovenste regel: "Seizoen - X% - Maanfase" (Wicca + illuminatie + fase), gebogen BOVENAAN.
  static char bovenBuf[56];
  struct tm t;
  int si = 0;
  if (getLocalTime(&t)) si = seizoenIndex(t.tm_mon + 1, t.tm_mday);
  float illum = illuminatiePct(fase);
  snprintf(bovenBuf, sizeof(bovenBuf), "%s   %.0f%%   %s", seizoenNamen[si], illum, faseNamen[fi]);

  arclabelFase = lv_arclabel_create(screen);
  lv_obj_set_size(arclabelFase, 240, 240);
  lv_obj_set_style_bg_opa(arclabelFase, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(arclabelFase, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(arclabelFase, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(arclabelFase, 0, LV_PART_MAIN);
  lv_obj_set_style_text_color(arclabelFase, textColor, LV_PART_MAIN);
  lv_obj_set_style_text_font(arclabelFase, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_arclabel_set_text(arclabelFase, bovenBuf);
  lv_arclabel_set_angle_start(arclabelFase, 180);   // bovenboog, midden op 270°
  lv_arclabel_set_angle_size(arclabelFase, 180);
  lv_arclabel_set_radius(arclabelFase, 95);         // iets kleiner dan 105 i.v.m. grotere font
  lv_arclabel_set_offset(arclabelFase, 0);          // geen offset = tekst gecentreerd, letters rechtop
  lv_arclabel_set_dir(arclabelFase, LV_ARCLABEL_DIR_CLOCKWISE);  // letters rechtop aan bovenkant
  lv_arclabel_set_text_vertical_align(arclabelFase, LV_ARCLABEL_TEXT_ALIGN_TRAILING);
  lv_arclabel_set_text_horizontal_align(arclabelFase, LV_ARCLABEL_TEXT_ALIGN_CENTER);
  lv_obj_center(arclabelFase);

  // Onder: "dag X.XX - dd-mm-yyyy hh:mm:ss" gebogen ONDERAAN, letters rechtop, gecentreerd.
  static char onderBuf[56];
  float leeftijd = maanLeeftijdDagen(fase);
  if (getLocalTime(&t)) {
    char dt[24];
    strftime(dt, sizeof(dt), "%d-%m-%Y   %H:%M:%S", &t);
    snprintf(onderBuf, sizeof(onderBuf), "Dag %.2f   %s", leeftijd, dt);
  } else {
    snprintf(onderBuf, sizeof(onderBuf), "Dag %.2f   --", leeftijd);
  }
  arclabelOnder = lv_arclabel_create(screen);
  lv_obj_set_size(arclabelOnder, 240, 240);
  lv_obj_set_style_bg_opa(arclabelOnder, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(arclabelOnder, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(arclabelOnder, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(arclabelOnder, 0, LV_PART_MAIN);
  lv_obj_set_style_text_color(arclabelOnder, textColor2, LV_PART_MAIN);
  lv_obj_set_style_text_font(arclabelOnder, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_arclabel_set_text_static(arclabelOnder, onderBuf);
  lv_arclabel_set_angle_start(arclabelOnder, 0);    // onderboog, midden op 90°
  lv_arclabel_set_angle_size(arclabelOnder, 180);
  lv_arclabel_set_radius(arclabelOnder, 95);        // iets kleiner dan 105 i.v.m. grotere font
  lv_arclabel_set_offset(arclabelOnder, 0);        // geen offset = tekst gecentreerd, letters rechtop
  lv_arclabel_set_dir(arclabelOnder, LV_ARCLABEL_DIR_COUNTER_CLOCKWISE);  // letters rechtop aan onderkant
  lv_arclabel_set_text_vertical_align(arclabelOnder, LV_ARCLABEL_TEXT_ALIGN_LEADING);
  lv_arclabel_set_text_horizontal_align(arclabelOnder, LV_ARCLABEL_TEXT_ALIGN_CENTER);
  lv_obj_center(arclabelOnder);
}

static void maanstandUpdate() {
  if (!arclabelFase || !arclabelOnder || !shadowObj || !shadowImg) return;
  float fase = maanfase();
  int fi = faseIndex(fase);
  // Schaduw: wassend = rechterkant verlicht, afnemend = linkerkant. Cosinus voor betere %-weergave.
  float phaseT = (fase < 0.5f) ? (2.0f * fase) : (2.0f * (1.0f - fase));
  float offset = (MOON_R * 0.5f) * (1.0f - cosf(3.14159265f * phaseT));
  int sx = (fase < 0.5f) ? (32 - (int)offset) : (32 + (int)offset);

  lv_obj_set_x(shadowObj, sx);
  lv_obj_set_x(shadowImg, 32 - sx);

  static char bovenBuf[56];
  struct tm t;
  int si = 0;
  if (getLocalTime(&t)) si = seizoenIndex(t.tm_mon + 1, t.tm_mday);
  float illum = illuminatiePct(fase);
  snprintf(bovenBuf, sizeof(bovenBuf), "%s   %.0f%%   %s", seizoenNamen[si], illum, faseNamen[fi]);
  lv_arclabel_set_text(arclabelFase, bovenBuf);

  static char onderBuf[56];
  float leeftijd = maanLeeftijdDagen(fase);
  if (getLocalTime(&t)) {
    char dt[24];
    strftime(dt, sizeof(dt), "%d-%m-%Y   %H:%M:%S", &t);
    snprintf(onderBuf, sizeof(onderBuf), "Dag %.2f   %s", leeftijd, dt);
  } else {
    snprintf(onderBuf, sizeof(onderBuf), "Dag %.2f   --", leeftijd);
  }
  lv_arclabel_set_text_static(arclabelOnder, onderBuf);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Maanstand LVGL – start");

  lv_init();
  lv_tick_set_cb(my_tick);

  lv_display_t * disp;
#if LV_USE_TFT_ESPI
  disp = lv_tft_espi_create(TFT_HOR_RES, TFT_VER_RES, draw_buf, sizeof(draw_buf));
  lv_display_set_rotation(disp, TFT_ROTATION);
#else
  disp = lv_display_create(TFT_HOR_RES, TFT_VER_RES);
  lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
#endif

  lv_obj_t * screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_make(32, 8, 48), LV_PART_MAIN);

  labelStatus = lv_label_create(screen);
  lv_label_set_text(labelStatus, "WiFi verbinden...");
  lv_obj_set_style_text_color(labelStatus, lv_color_white(), LV_PART_MAIN);
  lv_obj_center(labelStatus);

  for (int i = 0; i < 5; i++) {
    lv_timer_handler();
    delay(10);
  }

  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setConfigPortalBlocking(true);
  bool connected = wm.autoConnect(WIFI_AP_NAAM);
  if (!connected) {
    lv_label_set_text(labelStatus, "Geen WiFi");
    Serial.println("Geen WiFi / config geannuleerd");
    return;
  }
  Serial.println("WiFi OK: " + WiFi.SSID());

  lv_label_set_text(labelStatus, "Tijd ophalen...");
  for (int i = 0; i < 5; i++) {
    lv_timer_handler();
    delay(10);
  }

  configTime(0, 0, NTP_SERVER);   // Eerst NTP (UTC) ophalen
  struct tm t;
  int n = 0;
  while (n < 20) {
    if (getLocalTime(&t)) break;
    delay(500);
    lv_timer_handler();
    delay(5);
    n++;
  }
  setenv("TZ", TZ_NEDERLAND, 1);  // Daarna TZ voor lokale tijd (NL)
  tzset();
  if (!getLocalTime(&t)) {
    lv_label_set_text(labelStatus, "Geen NTP-tijd");
    Serial.println("Geen NTP-tijd");
    return;
  }
  Serial.println("Tijd gesynchroniseerd (NL)");

  lv_obj_delete(labelStatus);
  labelStatus = NULL;
  maanstandUI(screen);
  // Hele scherm invalideren zodat boven- en onderrand meegenomen worden (geen dode band).
  lv_obj_invalidate(screen);
  Serial.println("Setup done");
}

void loop() {
  lv_timer_handler();
  static uint32_t last = 0;
  if (millis() - last >= 1000) {   // elke seconde refreshen zodat de tijd meeloopt
    last = millis();
    if (WiFi.status() == WL_CONNECTED)
      maanstandUpdate();
  }
  delay(5);
}
