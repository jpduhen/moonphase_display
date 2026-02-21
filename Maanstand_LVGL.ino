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
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include "moon_image.h"
#include "SunMoonCalc.h"

/** 1 = samenstanden maan–planeet tonen (iconen op maanrand).
 *  Vereist: planet_images.h (alle 7 planeet-iconen in één bestand, zoals zodiac_images.h). */
#define USE_PLANET_CONJUNCTIONS 1
#if USE_PLANET_CONJUNCTIONS
#include "PlanetConjunctions.h"
#include "planet_images.h"
#endif

#define WIFI_AP_NAAM "Maanstand_WiFi"   // AP-naam bij eerste keer / geen opgeslagen netwerk
#define NTP_SERVER "pool.ntp.org"
#define PREF_NAAM   "maanstand"
#define DEFAULT_LAT "51.98"   // Arnhem (51.98, 5.91)
#define DEFAULT_LON "5.91"
// NL: wintertijd UTC+1, zomertijd UTC+2; expliciete offset voor CEST (sommige ESP32-cores)
#define TZ_NEDERLAND "CET-1CEST-2,M3.5.0,M10.5.0/3"
#define DEFAULT_TZ   TZ_NEDERLAND

static WebServer server(80);

/** UI-taal: 1 = Nederlands, 0 = English (compile-time default; runtime overschrijfbaar via web-instellingen) */
#define UI_LANG_NL 1

#define PREF_LANG      "lang"
#define PREF_SHOW_ZODIAC "showZodiac"
#define PREF_SHOW_CONJUNCTIONS "showConj"
#define PREF_CONJ_TEST "conjTest"
#define DEFAULT_LANG   "nl"

/** Zodiac-icoontjes op de maan: 1 = aan (gebruik zodiac_images.h), 0 = uit (alleen placeholder) */
#define USE_ZODIAC_IMAGES 1

#define LUNAR_CYCLE_DAYS 29.530588853f

#define TFT_HOR_RES   240
#define TFT_VER_RES   240
#define TFT_ROTATION  LV_DISPLAY_ROTATION_180

#define CX 120
#define CY 120
#define MOON_R 88
#define MOON_SZ (MOON_R * 2)
/** Straal voor planeeticonen op de maanrand (12 px kleiner dan maan), zodat alle iconen binnen de maancirkel vallen. */
#define PLANET_RIM_R  (MOON_R - 12)

#define DRAW_BUF_SIZE (TFT_HOR_RES * TFT_VER_RES / 10 * (LV_COLOR_DEPTH / 8))
static uint32_t draw_buf[DRAW_BUF_SIZE / 4];

static uint32_t my_tick(void) { return millis(); }

static const char* const faseNamenNL[] = {
  "Nieuwe  maan", "Wassende  sikkel", "Eerste  kwartier", "Wassende  maan",
  "Volle  maan", "Krimpende  maan", "Laatste  kwartier", "Afnemende  sikkel"
};
static const char* const faseNamenEN[] = {
  "New  moon", "Waxing  crescent", "First  quarter", "Waxing  gibbous",
  "Full  moon", "Waning  gibbous", "Last  quarter", "Waning  crescent"
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
static lv_obj_t * arclabelFase = NULL;   // bovenboog: seizoen, %, fase (gele maantint)
static lv_obj_t * arclabelRise  = NULL;  // maan opkomst HH:MM @ 225° (links-onder)
static lv_obj_t * arclabelOnder = NULL;  // huidige tijd HH:MM:SS @ 180° (onder, midden)
static lv_obj_t * arclabelSet   = NULL;  // maan ondergang HH:MM @ 135° (rechts-onder)
static lv_obj_t * labelZodiacName = NULL;  // Ram, Stier, …
static lv_obj_t * zodiacImg = NULL;       // overlay-afbeelding met alpha over de maan

#if USE_PLANET_CONJUNCTIONS
static lv_obj_t* planetIcons[PLANET_COUNT] = { NULL };  // planeeticonen op maanrand
#endif

// Zodiac-afbeeldingen: standaard een onzichtbare placeholder. Zie zodiac_images.h om 12 PNG's (met alpha) toe te voegen.
#ifndef USE_ZODIAC_IMAGES
static const uint16_t _zodiac_ph_pixel = 0;
static const lv_image_dsc_t _zodiac_ph_dsc = {
  .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .flags = 0, .w = 1, .h = 1, .stride = 2, .reserved_2 = 0 },
  .data_size = 2, .data = (const uint8_t*)&_zodiac_ph_pixel, .reserved = NULL, .reserved_2 = NULL
};
static const lv_image_dsc_t* const zodiacImgDsc[] = {
  &_zodiac_ph_dsc, &_zodiac_ph_dsc, &_zodiac_ph_dsc, &_zodiac_ph_dsc, &_zodiac_ph_dsc, &_zodiac_ph_dsc,
  &_zodiac_ph_dsc, &_zodiac_ph_dsc, &_zodiac_ph_dsc, &_zodiac_ph_dsc, &_zodiac_ph_dsc, &_zodiac_ph_dsc
};
#else
#include "zodiac_images.h"
static const lv_image_dsc_t* const zodiacImgDsc[] = {
  &zodiac_0, &zodiac_1, &zodiac_2, &zodiac_3, &zodiac_4, &zodiac_5,
  &zodiac_6, &zodiac_7, &zodiac_8, &zodiac_9, &zodiac_10, &zodiac_11
};
#endif

// Sterrenbeelden (tropische zonnetekens), volgorde Aries(0) t/m Pisces(11).
static const char* const zodiacNamenNL[] = {
  "Ram", "Stier", "Tweelingen", "Kreeft", "Leeuw", "Maagd",
  "Weegschaal", "Schorpioen", "Boogschutter", "Steenbok", "Waterman", "Vissen"
};
static const char* const zodiacNamenEN[] = {
  "Aries", "Taurus", "Gemini", "Cancer", "Leo", "Virgo",
  "Libra", "Scorpio", "Sagittarius", "Capricorn", "Aquarius", "Pisces"
};

/** Sterrenbeeldindex 0..11 (Aries=0 … Pisces=11) voor gegeven maand (1..12) en dag. */
static int zodiacIndex(int month, int day) {
  int d = month * 32 + day;
  if (d >= 12*32+22 || d <= 1*32+19)  return 9;   // Capricorn
  if (d <= 2*32+18) return 10;   // Aquarius
  if (d <= 3*32+20) return 11;   // Pisces
  if (d <= 4*32+19) return 0;    // Aries
  if (d <= 5*32+20) return 1;    // Taurus
  if (d <= 6*32+20) return 2;    // Gemini
  if (d <= 7*32+22) return 3;    // Cancer
  if (d <= 8*32+22) return 4;    // Leo
  if (d <= 9*32+22) return 5;    // Virgo
  if (d <= 10*32+22) return 6;   // Libra
  if (d <= 11*32+21) return 7;   // Scorpio
  return 8;   // Sagittarius
}

#if UI_LANG_NL
static WiFiManagerParameter paramLat("lat", "Breedtegraad (bijv. 52.37)", DEFAULT_LAT, 12);
static WiFiManagerParameter paramLon("lon", "Lengtegraad (bijv. 4.89)", DEFAULT_LON, 12);
#else
static WiFiManagerParameter paramLat("lat", "Latitude (e.g. 52.37)", DEFAULT_LAT, 12);
static WiFiManagerParameter paramLon("lon", "Longitude (e.g. 4.89)", DEFAULT_LON, 12);
#endif

static void saveParamsCallback() {
  Preferences prefs;
  prefs.begin(PREF_NAAM, false);
  prefs.putString("lat", paramLat.getValue());
  prefs.putString("lon", paramLon.getValue());
  prefs.end();
  Serial.println("Locatie opgeslagen: " + String(paramLat.getValue()) + ", " + String(paramLon.getValue()));
}

/** Haalt opgeslagen lat/lon op (standaard Arnhem). */
static void getLatLon(float& lat, float& lon) {
  Preferences prefs;
  prefs.begin(PREF_NAAM, true);
  String slat = prefs.getString("lat", DEFAULT_LAT);
  String slon = prefs.getString("lon", DEFAULT_LON);
  prefs.end();
  lat = slat.toFloat();
  lon = slon.toFloat();
}

/** Haalt displayvoorkeuren op: taal (langNL), zodiac/conjuncties, conjunctie-testmodus. */
static void getDisplayPrefs(bool& showZodiac, bool& showConjunctions, bool& conjTestMode, bool& langNL) {
  Preferences prefs;
  prefs.begin(PREF_NAAM, true);
  showZodiac       = (prefs.getString(PREF_SHOW_ZODIAC, "1") == "1");
  showConjunctions = (prefs.getString(PREF_SHOW_CONJUNCTIONS, "1") == "1");
  conjTestMode     = (prefs.getString(PREF_CONJ_TEST, "0") == "1");
  langNL           = (prefs.getString(PREF_LANG, DEFAULT_LANG) != "en");
  prefs.end();
}

/** Astronomische maandata via SunMoonCalc (locatie-afhankelijk). vult illumPct, fi, leeftijd, fase.
 *  Optioneel: riseStr/setStr/transitStr, angleDeg, moon_az_deg/moon_el_deg (voor samenstanden). */
static void getMoonData(float& illumPct, int& fi, float& leeftijd, float& fase,
                        char* riseStr, size_t riseLen, char* setStr, size_t setLen,
                        char* transitStr, size_t transitLen, float* angleDeg,
                        float* moon_az_deg = nullptr, float* moon_el_deg = nullptr) {
  time_t now;
  time(&now);
  float lat, lon;
  getLatLon(lat, lon);
  SunMoonCalc calc(now, (double)lat, (double)lon);
  SunMoonCalc::Result r = calc.calculateSunAndMoonData();
  illumPct = (float)(r.moon.illumination * 100.0);
  fi = (int)r.moon.phase.index;
  leeftijd = (float)r.moon.age;
  fase = (float)(r.moon.age / LUNAR_CYCLE_DAYS);
  if (fase >= 1.0f) fase -= 1.0f;
  if (riseStr && riseLen > 0 && r.moon.rise > 0) {
    struct tm* lt = localtime(&r.moon.rise);
    if (lt) strftime(riseStr, riseLen, "%H:%M", lt);
    else riseStr[0] = '\0';
  }
  if (setStr && setLen > 0 && r.moon.set > 0) {
    struct tm* lt = localtime(&r.moon.set);
    if (lt) strftime(setStr, setLen, "%H:%M", lt);
    else setStr[0] = '\0';
  }
  if (transitStr && transitLen > 0 && r.moon.transit > 0) {
    struct tm* lt = localtime(&r.moon.transit);
    if (lt) strftime(transitStr, transitLen, "%H:%M", lt);
    else transitStr[0] = '\0';
  }
  if (angleDeg) *angleDeg = (float)(r.moon.brightLimbAngle * 57.29577951308232);  // rad naar graden
  if (moon_az_deg) *moon_az_deg = (float)r.moon.azimuth;
  if (moon_el_deg) *moon_el_deg = (float)r.moon.elevation;
}

/** Format time_t als "DD-MM-YYYY HH:MM" of "—" als ongeldig. */
static void formatTime(time_t ts, char* buf, size_t len) {
  if (ts <= 0 || len == 0) { buf[0] = '\0'; return; }
  struct tm* lt = localtime(&ts);
  if (!lt) { buf[0] = '\0'; return; }
  strftime(buf, len, "%d-%m-%Y %H:%M", lt);
}

/* --- Web-UI: hoofdpagina = data; instellingen; opslaan --- */
static const char WEB_CSS[] PROGMEM = "*{box-sizing:border-box}body{margin:0;font-family:'Segoe UI',system-ui,sans-serif;background:linear-gradient(145deg,#1a0a1f 0%,#0d0512 100%);min-height:100vh;color:#e8e4ec}.wrap{max-width:720px;margin:0 auto;padding:1rem}h1{font-size:1.5rem;font-weight:600;margin:0 0 1rem;color:#c4b8e0}nav{display:flex;align-items:center;justify-content:space-between;margin-bottom:1.25rem;padding-bottom:0.75rem;border-bottom:1px solid rgba(196,184,224,0.25)}nav a{color:#a89dd4;text-decoration:none;font-size:0.9rem}nav a:hover{color:#c4b8e0}.card{background:rgba(40,25,55,0.6);border-radius:12px;padding:1.25rem;margin-bottom:1rem;border:1px solid rgba(196,184,224,0.15)}.card h2{font-size:1.1rem;margin:0 0 0.75rem;color:#b8a8d8}table{width:100%;border-collapse:collapse;font-size:0.9rem}th,td{border:1px solid rgba(196,184,224,0.2);padding:0.5rem 0.75rem;text-align:left}th{background:rgba(0,0,0,0.2);color:#c4b8e0;font-weight:500}tr:nth-child(even){background:rgba(0,0,0,0.1)}label{display:block;margin-bottom:0.5rem;font-size:0.9rem}input[type=text]{width:100%;max-width:20rem;padding:0.5rem;border:1px solid rgba(196,184,224,0.3);border-radius:6px;background:rgba(20,10,30,0.8);color:#e8e4ec;font-size:1rem}select{padding:0.5rem;border:1px solid rgba(196,184,224,0.3);border-radius:6px;background:rgba(20,10,30,0.8);color:#e8e4ec;font-size:1rem;min-width:10rem}.row{margin-bottom:1rem}.chk{display:flex;align-items:center;gap:0.5rem;margin-bottom:0.5rem}.chk input{width:1.1rem;height:1.1rem;accent-color:#7b6ba8}button{background:#6B4E9E;color:#fff;border:none;padding:0.6rem 1.2rem;border-radius:8px;font-size:1rem;cursor:pointer}button:hover{background:#7b5eae}";

static void serveSettings() {
  Preferences prefs;
  prefs.begin(PREF_NAAM, true);
  String lat = prefs.getString("lat", DEFAULT_LAT);
  String lon = prefs.getString("lon", DEFAULT_LON);
  String tz  = prefs.getString("tz", DEFAULT_TZ);
  String lang = prefs.getString(PREF_LANG, DEFAULT_LANG);
  String sz = prefs.getString(PREF_SHOW_ZODIAC, "1");
  String sc = prefs.getString(PREF_SHOW_CONJUNCTIONS, "1");
  String st = prefs.getString(PREF_CONJ_TEST, "0");
  prefs.end();
  bool isNL = (lang != "en");
  String html = F("<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>");
  html += isNL ? F("Instellingen — Maanstand") : F("Settings — Moon phase");
  html += F("</title><style>");
  html += FPSTR(WEB_CSS);
  html += F("</style></head><body><div class='wrap'><nav><span>");
  html += isNL ? F("Maanstand") : F("Moon phase");
  html += F("</span><a href='/'>Data</a></nav><h1>");
  html += isNL ? F("Instellingen") : F("Settings");
  html += F("</h1><div class='card'><form method='post' action='/save'>");
  html += F("<div class='row'><label>");
  html += isNL ? F("Breedtegraad (lat)") : F("Latitude (lat)");
  html += F("</label><input type='text' name='lat' value='");
  html += lat;
  html += F("' /></div><div class='row'><label>");
  html += isNL ? F("Lengtegraad (lon)") : F("Longitude (lon)");
  html += F("</label><input type='text' name='lon' value='");
  html += lon;
  html += F("' /></div><div class='row'><label>");
  html += isNL ? F("Tijdzone (TZ)") : F("Time zone (TZ)");
  html += F("</label><input type='text' name='tz' value='");
  html += tz;
  html += F("' style='max-width:100%' /></div><div class='row'><label>");
  html += isNL ? F("Taal") : F("Language");
  html += F("</label><select name='lang'><option value='nl'");
  if (lang == "nl") html += F(" selected");
  html += F(">Nederlands</option><option value='en'");
  if (lang == "en") html += F(" selected");
  html += F(">English</option></select></div><div class='row'><label>");
  html += isNL ? F("Op het scherm tonen") : F("Show on screen");
  html += F("</label><div class='chk'><input type='checkbox' name='showZodiac' id='showZodiac' value='1'");
  if (sz == "1") html += F(" checked");
  html += F(" /><label for='showZodiac' style='display:inline;margin:0'>");
  html += isNL ? F("Zodiac / sterrenbeeld") : F("Zodiac / sign");
  html += F("</label></div><div class='chk'><input type='checkbox' name='showConj' id='showConj' value='1'");
  if (sc == "1") html += F(" checked");
  html += F(" /><label for='showConj' style='display:inline;margin:0'>");
  html += isNL ? F("Conjuncties (planeten)") : F("Conjunctions (planets)");
  html += F("</label></div><div class='chk'><input type='checkbox' name='conjTest' id='conjTest' value='1'");
  if (st == "1") html += F(" checked");
  html += F(" /><label for='conjTest' style='display:inline;margin:0'>");
  html += isNL ? F("Conjuncties testmodus (alle planeten op positie)") : F("Conjunctions test mode (all planets at position)");
  html += F("</label></div></div><button type='submit'>");
  html += isNL ? F("Opslaan") : F("Save");
  html += F("</button></form></div></div></body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

static void handleSave() {
  if (server.method() != HTTP_POST) { server.send(405, "text/plain", "Method Not Allowed"); return; }
  String lat = server.hasArg("lat") ? server.arg("lat") : String(DEFAULT_LAT);
  String lon = server.hasArg("lon") ? server.arg("lon") : String(DEFAULT_LON);
  String tz  = server.hasArg("tz")  ? server.arg("tz")  : String(DEFAULT_TZ);
  String lang = server.hasArg("lang") ? server.arg("lang") : String(DEFAULT_LANG);
  if (lang != "en") lang = "nl";
  String showZodiac = server.hasArg("showZodiac") ? "1" : "0";
  String showConjunctions = server.hasArg("showConj") ? "1" : "0";
  String conjTest = server.hasArg("conjTest") ? "1" : "0";
  Preferences prefs;
  prefs.begin(PREF_NAAM, false);
  prefs.putString("lat", lat);
  prefs.putString("lon", lon);
  prefs.putString("tz", tz);
  prefs.putString(PREF_LANG, lang);
  prefs.putString(PREF_SHOW_ZODIAC, showZodiac);
  prefs.putString(PREF_SHOW_CONJUNCTIONS, showConjunctions);
  prefs.putString(PREF_CONJ_TEST, conjTest);
  prefs.end();
  Serial.println("Web: instellingen opgeslagen");
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "See Other");
}

static void serveData() {
  Preferences prefs;
  prefs.begin(PREF_NAAM, true);
  bool isNL = (prefs.getString(PREF_LANG, DEFAULT_LANG) != "en");
  prefs.end();
  float lat, lon;
  getLatLon(lat, lon);
  time_t now;
  time(&now);
  SunMoonCalc calc(now, (double)lat, (double)lon);
  SunMoonCalc::Result r = calc.calculateSunAndMoonData();
  char buf[32];
  String html = F("<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>");
  html += isNL ? F("Maanstand — Data") : F("Moon phase — Data");
  html += F("</title><style>");
  html += FPSTR(WEB_CSS);
  html += F("</style></head><body><div class='wrap'><nav><span>");
  html += isNL ? F("Maanstand") : F("Moon phase");
  html += F("</span><a href='/settings'>");
  html += isNL ? F("Instellingen") : F("Settings");
  html += F("</a></nav><h1>");
  html += isNL ? F("Berekende data") : F("Computed data");
  html += F("</h1><div class='card'><p style='margin:0 0 0.75rem'><strong>");
  html += isNL ? F("Locatie") : F("Location");
  html += F("</strong> ");
  html += String(lat, 4) + ", " + String(lon, 4);
  html += F(" &nbsp;·&nbsp; <strong>");
  html += isNL ? F("Lunare cyclus") : F("Lunar cycle");
  html += F("</strong> ");
  html += String(LUNAR_CYCLE_DAYS, 4);
  html += isNL ? F(" dagen</p><h2>Zon</h2><table><tr><th>Veld</th><th>Waarde</th></tr>") : F(" days</p><h2>Sun</h2><table><tr><th>Field</th><th>Value</th></tr>");
  formatTime(r.sun.rise, buf, sizeof(buf));
  html += F("<tr><td>"); html += isNL ? F("Opkomst") : F("Rise"); html += F("</td><td id='v_sun_rise'>"); html += buf; html += F("</td></tr>");
  formatTime(r.sun.transit, buf, sizeof(buf));
  html += F("<tr><td>"); html += isNL ? F("Doorgang") : F("Transit"); html += F("</td><td id='v_sun_transit'>"); html += buf; html += F("</td></tr>");
  formatTime(r.sun.set, buf, sizeof(buf));
  html += F("<tr><td>"); html += isNL ? F("Ondergang") : F("Set"); html += F("</td><td id='v_sun_set'>"); html += buf; html += F("</td></tr>");
  html += F("<tr><td>"); html += isNL ? F("Azimut (°)") : F("Azimuth (°)"); html += F("</td><td id='v_sun_az'>"); html += String(r.sun.azimuth, 2) + F("</td></tr>");
  html += F("<tr><td>"); html += isNL ? F("Elevatie (°)") : F("Elevation (°)"); html += F("</td><td id='v_sun_el'>"); html += String(r.sun.elevation, 2) + F("</td></tr>");
  html += F("<tr><td>"); html += isNL ? F("Elevatie doorgang (°)") : F("Transit elevation (°)"); html += F("</td><td id='v_sun_el_tr'>"); html += String(r.sun.transitElevation, 2) + F("</td></tr>");
  html += F("<tr><td>"); html += isNL ? F("Afstand (km)") : F("Distance (km)"); html += F("</td><td id='v_sun_dist'>"); html += String(r.sun.distance, 0) + F("</td></tr></table></div>");
  html += F("<div class='card'><h2>"); html += isNL ? F("Maan") : F("Moon"); html += F("</h2><table><tr><th>"); html += isNL ? F("Veld") : F("Field"); html += F("</th><th>"); html += isNL ? F("Waarde") : F("Value"); html += F("</th></tr>");
  formatTime(r.moon.rise, buf, sizeof(buf));
  html += F("<tr><td>"); html += isNL ? F("Opkomst") : F("Rise"); html += F("</td><td id='v_moon_rise'>"); html += buf; html += F("</td></tr>");
  formatTime(r.moon.transit, buf, sizeof(buf));
  html += F("<tr><td>"); html += isNL ? F("Doorgang") : F("Transit"); html += F("</td><td id='v_moon_transit'>"); html += buf; html += F("</td></tr>");
  formatTime(r.moon.set, buf, sizeof(buf));
  html += F("<tr><td>"); html += isNL ? F("Ondergang") : F("Set"); html += F("</td><td id='v_moon_set'>"); html += buf; html += F("</td></tr>");
  html += F("<tr><td>"); html += isNL ? F("Azimut (°)") : F("Azimuth (°)"); html += F("</td><td id='v_moon_az'>"); html += String(r.moon.azimuth, 2) + F("</td></tr>");
  html += F("<tr><td>"); html += isNL ? F("Elevatie (°)") : F("Elevation (°)"); html += F("</td><td id='v_moon_el'>"); html += String(r.moon.elevation, 2) + F("</td></tr>");
  html += F("<tr><td>"); html += isNL ? F("Elevatie doorgang (°)") : F("Transit elevation (°)"); html += F("</td><td id='v_moon_el_tr'>"); html += String(r.moon.transitElevation, 2) + F("</td></tr>");
  html += F("<tr><td>"); html += isNL ? F("Leeftijd (dagen)") : F("Age (days)"); html += F("</td><td id='v_moon_age'>"); html += String(r.moon.age, 3) + F("</td></tr>");
  html += F("<tr><td>"); html += isNL ? F("Fase in cyclus (0–1)") : F("Phase in cycle (0–1)"); html += F("</td><td id='v_moon_phase_cycle'>"); html += String((float)(r.moon.age / (double)LUNAR_CYCLE_DAYS), 4) + F("</td></tr>");
  html += F("<tr><td>"); html += isNL ? F("Verlichting (0–1)") : F("Illumination (0–1)"); html += F("</td><td id='v_moon_illum'>"); html += String(r.moon.illumination, 4) + F("</td></tr>");
  html += F("<tr><td>"); html += isNL ? F("Fase-index") : F("Phase index"); html += F("</td><td id='v_moon_phase'>"); html += String(r.moon.phase.index) + " " + r.moon.phase.name + F("</td></tr>");
  html += F("<tr><td>"); html += isNL ? F("Afstand (km)") : F("Distance (km)"); html += F("</td><td id='v_moon_dist'>"); html += String(r.moon.distance, 0) + F("</td></tr>");
  double deg = r.moon.axisPositionAngle * 57.29577951308232;
  html += F("<tr><td>Axis position angle (°)</td><td id='v_moon_axis'>"); html += String(deg, 2) + F("</td></tr>");
  deg = r.moon.brightLimbAngle * 57.29577951308232;
  html += F("<tr><td>Bright limb angle (°)</td><td id='v_moon_bright'>"); html += String(deg, 2) + F("</td></tr>");
  deg = r.moon.parallacticAngle * 57.29577951308232;
  html += F("<tr><td>Parallactic angle (°)</td><td id='v_moon_parallactic'>"); html += String(deg, 2) + F("</td></tr></table></div>");
#if USE_PLANET_CONJUNCTIONS
  {
    PlanetObserver obs;
    planetObserverSet(&obs, (double)lat, (double)lon, 0.0);
    ConjunctionEvent conj[PLANET_CONJ_MAX];
    int n_conj = planetFindConjunctions(r.moon.azimuth, r.moon.elevation, now, &obs, PLANET_CONJ_SEP_LIMIT_DEG, conj);
    static const char* planetNamesNL[] = { "Mercurius", "Venus", "Mars", "Jupiter", "Saturnus", "Uranus", "Neptunus" };
    static const char* planetNamesEN[] = { "Mercury", "Venus", "Mars", "Jupiter", "Saturn", "Uranus", "Neptune" };
    for (int p = 0; p < PLANET_COUNT; p++) {
      SkyPosition sky;
      bool has_pos = getPlanetTopocentricPosition((PlanetId)p, now, &obs, &sky);
      float sep_deg = -1.0f;
      for (int c = 0; c < n_conj; c++) { if (conj[c].planet_id == (PlanetId)p) { sep_deg = conj[c].separation_deg; break; } }
      html += F("<div class='card'><h2>");
      html += isNL ? planetNamesNL[p] : planetNamesEN[p];
      html += F("</h2><table><tr><th>"); html += isNL ? F("Veld") : F("Field"); html += F("</th><th>"); html += isNL ? F("Waarde") : F("Value"); html += F("</th></tr>");
      if (has_pos) {
        html += F("<tr><td>"); html += isNL ? F("Azimut (°)") : F("Azimuth (°)"); html += F("</td><td id='v_p"); html += String(p); html += F("_az'>"); html += String(sky.az_deg, 2) + F("</td></tr>");
        html += F("<tr><td>"); html += isNL ? F("Elevatie (°)") : F("Elevation (°)"); html += F("</td><td id='v_p"); html += String(p); html += F("_el'>"); html += String(sky.el_deg, 2) + F("</td></tr>");
      } else {
        html += F("<tr><td>"); html += isNL ? F("Azimut (°)") : F("Azimuth (°)"); html += F("</td><td id='v_p"); html += String(p); html += F("_az'>—</td></tr><tr><td>"); html += isNL ? F("Elevatie (°)") : F("Elevation (°)"); html += F("</td><td id='v_p"); html += String(p); html += F("_el'>—</td></tr>");
      }
      html += F("<tr><td>"); html += isNL ? F("Samenstand met maan") : F("Conjunction with moon"); html += F("</td><td id='v_p"); html += String(p); html += F("_conj'>");
      if (sep_deg >= 0.0f) {
        html += String(sep_deg, 1) + (isNL ? F("° (binnen ") : F("° (within "));
        html += String((float)PLANET_CONJ_SEP_LIMIT_DEG, 0) + F("°)</td></tr>");
      } else {
        html += isNL ? F("nee</td></tr>") : F("no</td></tr>");
      }
      html += F("</table></div>");
    }
  }
#endif
  html += F("<script>function upd(){fetch('/api/data').then(r=>r.json()).then(d=>{for(var k in d){var e=document.getElementById(k);if(e)e.textContent=d[k];}}).catch(()=>{});}setInterval(upd,30000);</script></div></body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

/** API: alleen waarden als JSON voor automatische verversing zonder pagina-reload. */
static void serveDataJson() {
  Preferences prefs;
  prefs.begin(PREF_NAAM, true);
  bool isNL = (prefs.getString(PREF_LANG, DEFAULT_LANG) != "en");
  prefs.end();
  float lat, lon;
  getLatLon(lat, lon);
  time_t now;
  time(&now);
  SunMoonCalc calc(now, (double)lat, (double)lon);
  SunMoonCalc::Result r = calc.calculateSunAndMoonData();
  char buf[32];
  String json = F("{\"v_sun_rise\":\"");
  formatTime(r.sun.rise, buf, sizeof(buf)); json += buf;
  json += F("\",\"v_sun_transit\":\""); formatTime(r.sun.transit, buf, sizeof(buf)); json += buf;
  json += F("\",\"v_sun_set\":\""); formatTime(r.sun.set, buf, sizeof(buf)); json += buf;
  json += "\",\"v_sun_az\":\"" + String(r.sun.azimuth, 2) + F("\",\"v_sun_el\":\"") + String(r.sun.elevation, 2) +
          F("\",\"v_sun_el_tr\":\"") + String(r.sun.transitElevation, 2) + F("\",\"v_sun_dist\":\"") + String(r.sun.distance, 0) + "\"";
  json += F(",\"v_moon_rise\":\"");
  formatTime(r.moon.rise, buf, sizeof(buf)); json += buf;
  json += F("\",\"v_moon_transit\":\""); formatTime(r.moon.transit, buf, sizeof(buf)); json += buf;
  json += F("\",\"v_moon_set\":\""); formatTime(r.moon.set, buf, sizeof(buf)); json += buf;
  json += "\",\"v_moon_az\":\"" + String(r.moon.azimuth, 2) + F("\",\"v_moon_el\":\"") + String(r.moon.elevation, 2) +
          F("\",\"v_moon_el_tr\":\"") + String(r.moon.transitElevation, 2) + F("\",\"v_moon_age\":\"") + String(r.moon.age, 3) +
          F("\",\"v_moon_phase_cycle\":\"") + String((float)(r.moon.age / (double)LUNAR_CYCLE_DAYS), 4) +
          F("\",\"v_moon_illum\":\"") + String(r.moon.illumination, 4) + F("\",\"v_moon_phase\":\"") + String(r.moon.phase.index) + " " + String(r.moon.phase.name) +
          "\",\"v_moon_dist\":\"" + String(r.moon.distance, 0) + "\"";
  double deg = r.moon.axisPositionAngle * 57.29577951308232;
  json += F(",\"v_moon_axis\":\"");
  json += String(deg, 2);
  json += "\"";
  deg = r.moon.brightLimbAngle * 57.29577951308232;
  json += F(",\"v_moon_bright\":\"");
  json += String(deg, 2);
  json += "\"";
  deg = r.moon.parallacticAngle * 57.29577951308232;
  json += F(",\"v_moon_parallactic\":\"");
  json += String(deg, 2);
  json += "\"";
#if USE_PLANET_CONJUNCTIONS
  {
    PlanetObserver obs;
    planetObserverSet(&obs, (double)lat, (double)lon, 0.0);
    ConjunctionEvent conj[PLANET_CONJ_MAX];
    int n_conj = planetFindConjunctions(r.moon.azimuth, r.moon.elevation, now, &obs, PLANET_CONJ_SEP_LIMIT_DEG, conj);
    const char* noConj = isNL ? "nee" : "no";
    for (int p = 0; p < PLANET_COUNT; p++) {
      SkyPosition sky;
      bool has_pos = getPlanetTopocentricPosition((PlanetId)p, now, &obs, &sky);
      float sep_deg = -1.0f;
      for (int c = 0; c < n_conj; c++) { if (conj[c].planet_id == (PlanetId)p) { sep_deg = conj[c].separation_deg; break; } }
      String pid = "v_p" + String(p);
      if (has_pos) {
        json += ",\"" + pid + "_az\":\"" + String(sky.az_deg, 2) + "\",\"" + pid + "_el\":\"" + String(sky.el_deg, 2) + "\"";
      } else {
        json += ",\"" + pid + "_az\":\"—\",\"" + pid + "_el\":\"—\"";
      }
      if (sep_deg >= 0.0f)
        json += ",\"" + pid + "_conj\":\"" + String(sep_deg, 1) + "°\"";
      else
        json += ",\"" + pid + "_conj\":\"" + String(noConj) + "\"";
    }
  }
#endif
  json += F("}");
  server.send(200, "application/json", json);
}

/** Gegeven gewenste verlichte fractie L (0..1), retourneer afstand tussen middelpunten
 *  maan en schaduw (beide straal MOON_R) zodat de overlap precies (1-L)*oppervlakte geeft.
 *  (Alleen gebruikt als USE_SPHERE_TERMINATOR niet gedefinieerd is.) */
static float offsetUitLitFraction(float L) {
  if (L <= 0.0f) return 0.0f;
  if (L >= 1.0f) return (float)(MOON_R * 2);
  const float R = (float)MOON_R;
  const float R2 = R * R;
  const float area = 3.14159265f * R2;
  const float targetOverlap = (1.0f - L) * area;
  float lo = 0.0f, hi = 2.0f * R;
  for (int i = 0; i < 24; i++) {
    float d = (lo + hi) * 0.5f;
    float x = d / (2.0f * R);
    if (x >= 1.0f) { hi = d; continue; }
    float overlap = 2.0f * R2 * acosf(x) - (d * 0.5f) * sqrtf(4.0f * R2 - d * d);
    if (overlap > targetOverlap)
      lo = d;
    else
      hi = d;
  }
  return (lo + hi) * 0.5f;
}

/* --- Exacte terminator via bolprojectie (USE_SPHERE_TERMINATOR) ---
 * Verlichte zijde = punten op de bol waar P·S > 0 (S = richting maan→zon).
 * Coördinaten: scherm (x,y), bol z = sqrt(R² - x² - y²). */
#define USE_SPHERE_TERMINATOR 1
#if USE_SPHERE_TERMINATOR
#define MOON_COMPOSED_STRIDE (MOON_SZ * 2)
static uint16_t moon_composed_buf[MOON_SZ * MOON_SZ];
static const lv_image_dsc_t moon_composed_dsc = {
  .header = {
    .magic = LV_IMAGE_HEADER_MAGIC,
    .cf = LV_COLOR_FORMAT_RGB565,
    .flags = 0,
    .w = MOON_SZ,
    .h = MOON_SZ,
    .stride = MOON_COMPOSED_STRIDE,
    .reserved_2 = 0,
  },
  .data_size = (uint32_t)(MOON_SZ * MOON_SZ * 2),
  .data = (const uint8_t *)moon_composed_buf,
  .reserved = NULL,
  .reserved_2 = NULL,
};
/* Donkere schaduwkleur (past bij sky), RGB565. */
static const uint16_t MOON_SHADOW_RGB565 = 0x0842;  /* ~#0a0810 */

/** Vul moon_composed_buf met maanbeeld: verlichte kant uit texture, schaduwkant donker.
 *  angleDeg = positiehoek heldere rand (graden), phase = maanfase 0..1 (0=nieuw, 0.5=vol). */
static void updateMoonTerminator(float angleDeg, float phase) {
  const float R = (float)MOON_R;
  const float R2 = R * R;
  /* Zonrichting (eenheidsvector maan→zon). Scherm: x=rechts, y=omlaag; Noord=omhoog.
   * phase 0 = nieuwe maan (zon vóór maan) → Sz = -1; phase 0.5 = volle maan → Sz = +1. */
  float Sz = -cosf(phase * 6.28318530718f);
  float angleRad = angleDeg * 0.01745329252f;
  float h = sqrtf(1.0f - Sz * Sz);
  if (h < 1e-6f) h = 0.0f;
  /* Sx gespiegeld t.o.v. scherm zodat sikkel niet over verticale as gespiegeld is */
  float Sx = -sinf(angleRad) * h;
  float Sy = -cosf(angleRad) * h;
  for (int j = 0; j < MOON_SZ; j++) {
    float y = (float)(j - MOON_R);
    for (int i = 0; i < MOON_SZ; i++) {
      float x = (float)(i - MOON_R);
      float d2 = x * x + y * y;
      if (d2 >= R2) {
        moon_composed_buf[j * MOON_SZ + i] = 0x0000;
        continue;
      }
      float z = sqrtf(R2 - d2);
      float dot = x * Sx + y * Sy + z * Sz;
      if (dot > 0.0f) {
        moon_composed_buf[j * MOON_SZ + i] = image_data_176x176x16[j * MOON_IMG_W + i];
      } else {
        /* Schaduwkant: donkere maantextuur (blend texture + schaduwkleur) zodat kraters zichtbaar blijven */
        uint16_t tex = image_data_176x176x16[j * MOON_IMG_W + i];
        uint16_t r = (uint16_t)(((tex >> 11) * 20 + (MOON_SHADOW_RGB565 >> 11) * 80) / 100);
        uint16_t g = (uint16_t)((((tex >> 5) & 0x3F) * 20 + ((MOON_SHADOW_RGB565 >> 5) & 0x3F) * 80) / 100);
        uint16_t b = (uint16_t)(((tex & 0x1F) * 20 + (MOON_SHADOW_RGB565 & 0x1F) * 80) / 100);
        moon_composed_buf[j * MOON_SZ + i] = (uint16_t)((r << 11) | (g << 5) | b);
      }
    }
  }
}
#endif /* USE_SPHERE_TERMINATOR */

static void maanstandUI(lv_obj_t * screen) {
  float illumPct, leeftijd, fase, angleDeg;
  int fi;
  char riseStr[8], setStr[8], transitStr[8];
  riseStr[0] = setStr[0] = transitStr[0] = '\0';
  getMoonData(illumPct, fi, leeftijd, fase, riseStr, sizeof(riseStr), setStr, sizeof(setStr),
              transitStr, sizeof(transitStr), &angleDeg);

  lv_color_t skyColor = lv_color_make(32, 8, 48);
  lv_color_t moonColor = lv_color_make(230, 230, 200);
  /* Gele/maan tint voor kop- en voetregel (komt overeen met maan in afbeelding) */
  lv_color_t arcLabelColor = lv_color_make(230, 222, 175);
  lv_color_t textColor2 = lv_color_make(192, 192, 192);

  lv_obj_set_style_bg_color(screen, skyColor, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(screen, 0, LV_PART_MAIN);

#if USE_SPHERE_TERMINATOR
  updateMoonTerminator(angleDeg, fase);
  moonObj = lv_image_create(screen);
  lv_obj_set_size(moonObj, MOON_SZ, MOON_SZ);
  lv_obj_set_pos(moonObj, 32, 32);
  lv_image_set_src(moonObj, &moon_composed_dsc);
  lv_image_set_inner_align(moonObj, LV_IMAGE_ALIGN_STRETCH);
  lv_obj_set_style_radius(moonObj, MOON_R, LV_PART_MAIN);
  lv_obj_set_style_clip_corner(moonObj, true, LV_PART_MAIN);
  lv_obj_set_style_border_width(moonObj, 0, LV_PART_MAIN);
  lv_obj_clear_flag(moonObj, LV_OBJ_FLAG_SCROLLABLE);
  shadowObj = NULL;
  shadowImg = NULL;
#else
  float L = illumPct / 100.0f;
  float offset = offsetUitLitFraction(L);
  float angleRad = angleDeg * 0.01745329252f;
  float dx =  offset * sinf(angleRad);
  float dy =  offset * cosf(angleRad);
  int sx = (int)(32.0f + dx);
  int sy = (int)(32.0f + dy);
  int imgOx = (int)(-dx);
  int imgOy = (int)(-dy);
  moonObj = lv_image_create(screen);
  lv_obj_set_size(moonObj, MOON_SZ, MOON_SZ);
  lv_obj_set_pos(moonObj, 32, 32);
  lv_image_set_src(moonObj, &moon_img_dsc);
  lv_image_set_inner_align(moonObj, LV_IMAGE_ALIGN_STRETCH);
  lv_obj_set_style_radius(moonObj, MOON_R, LV_PART_MAIN);
  lv_obj_set_style_clip_corner(moonObj, true, LV_PART_MAIN);
  lv_obj_set_style_border_width(moonObj, 0, LV_PART_MAIN);
  lv_obj_clear_flag(moonObj, LV_OBJ_FLAG_SCROLLABLE);
  lv_color_t shadowBg = lv_color_hex(0x0a0810);
  shadowObj = lv_obj_create(screen);
  lv_obj_set_size(shadowObj, MOON_SZ, MOON_SZ);
  lv_obj_set_pos(shadowObj, sx, sy);
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
  lv_obj_set_pos(shadowImg, imgOx, imgOy);
  lv_obj_set_style_pad_all(shadowImg, 0, LV_PART_MAIN);
  lv_image_set_src(shadowImg, &moon_img_dsc);
  lv_image_set_inner_align(shadowImg, LV_IMAGE_ALIGN_STRETCH);
  lv_obj_set_style_image_opa(shadowImg, LV_OPA_20, LV_PART_MAIN);
  lv_obj_set_style_border_width(shadowImg, 0, LV_PART_MAIN);
  lv_obj_clear_flag(shadowImg, LV_OBJ_FLAG_SCROLLABLE);
#endif

  // Bovenste regel: "Seizoen - DD-MM-YY - Maanfase", gebogen BOVENAAN.
  bool showZodiac, showConjunctions, conjTestMode, langNL;
  getDisplayPrefs(showZodiac, showConjunctions, conjTestMode, langNL);
  const char* const* faseNamen = langNL ? faseNamenNL : faseNamenEN;
  static char bovenBuf[56];
  struct tm t;
  int si = 0;
  if (getLocalTime(&t)) si = seizoenIndex(t.tm_mon + 1, t.tm_mday);
  if (getLocalTime(&t))
    snprintf(bovenBuf, sizeof(bovenBuf), "%s   %02d-%02d-%02d   %s", seizoenNamen[si], t.tm_mday, t.tm_mon + 1, (t.tm_year + 1900) % 100, faseNamen[fi]);
  else
    snprintf(bovenBuf, sizeof(bovenBuf), "%s   --/--/--   %s", seizoenNamen[si], faseNamen[fi]);

  arclabelFase = lv_arclabel_create(screen);
  lv_obj_set_size(arclabelFase, 240, 240);
  lv_obj_set_style_bg_opa(arclabelFase, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(arclabelFase, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(arclabelFase, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(arclabelFase, 0, LV_PART_MAIN);
  lv_obj_set_style_text_color(arclabelFase, arcLabelColor, LV_PART_MAIN);
  lv_obj_set_style_text_font(arclabelFase, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_arclabel_set_text(arclabelFase, bovenBuf);
  /* Boog gecentreerd op top (270°): start = 270 - size/2. Size 220° geeft ruimte voor lange teksten. */
  lv_arclabel_set_angle_start(arclabelFase, 160);   // 270 - 110 = 160
  lv_arclabel_set_angle_size(arclabelFase, 220);
  lv_arclabel_set_radius(arclabelFase, 95);
  lv_arclabel_set_offset(arclabelFase, 0);
  lv_arclabel_set_dir(arclabelFase, LV_ARCLABEL_DIR_CLOCKWISE);
  lv_arclabel_set_text_vertical_align(arclabelFase, LV_ARCLABEL_TEXT_ALIGN_TRAILING);
  lv_arclabel_set_text_horizontal_align(arclabelFase, LV_ARCLABEL_TEXT_ALIGN_CENTER);
  lv_obj_center(arclabelFase);

  // Voetregel: drie gebogen labels. LVGL 0°=rechts, 90°=onder. User top=0 → 225° links-onder, 180° onder, 135° rechts-onder.
  const char* r = (riseStr[0]) ? riseStr : "--:--";
  const char* s = (setStr[0]) ? setStr : "--:--";
  static char tijdBuf[16];
  if (getLocalTime(&t)) snprintf(tijdBuf, sizeof(tijdBuf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  else snprintf(tijdBuf, sizeof(tijdBuf), "--:--:--");

  arclabelRise = lv_arclabel_create(screen);
  lv_obj_set_size(arclabelRise, 240, 240);
  lv_obj_set_style_bg_opa(arclabelRise, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(arclabelRise, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(arclabelRise, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(arclabelRise, 0, LV_PART_MAIN);
  lv_obj_set_style_text_color(arclabelRise, lv_color_hex(0x6B4E9E), LV_PART_MAIN);  // paarsblauw = zodiac
  lv_obj_set_style_text_font(arclabelRise, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_arclabel_set_text(arclabelRise, r);
  lv_arclabel_set_angle_start(arclabelRise, 110);   // centrum 135° (user 225° = links-onder)
  lv_arclabel_set_angle_size(arclabelRise, 50);
  lv_arclabel_set_radius(arclabelRise, 95);
  lv_arclabel_set_offset(arclabelRise, 0);
  lv_arclabel_set_dir(arclabelRise, LV_ARCLABEL_DIR_COUNTER_CLOCKWISE);
  lv_arclabel_set_text_vertical_align(arclabelRise, LV_ARCLABEL_TEXT_ALIGN_LEADING);
  lv_arclabel_set_text_horizontal_align(arclabelRise, LV_ARCLABEL_TEXT_ALIGN_CENTER);
  lv_obj_center(arclabelRise);

  arclabelOnder = lv_arclabel_create(screen);
  lv_obj_set_size(arclabelOnder, 240, 240);
  lv_obj_set_style_bg_opa(arclabelOnder, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(arclabelOnder, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(arclabelOnder, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(arclabelOnder, 0, LV_PART_MAIN);
  lv_obj_set_style_text_color(arclabelOnder, arcLabelColor, LV_PART_MAIN);
  lv_obj_set_style_text_font(arclabelOnder, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_arclabel_set_text(arclabelOnder, tijdBuf);
  lv_arclabel_set_angle_start(arclabelOnder, 65);   // centrum 90° (user 180° = onder, midden)
  lv_arclabel_set_angle_size(arclabelOnder, 50);
  lv_arclabel_set_radius(arclabelOnder, 95);
  lv_arclabel_set_offset(arclabelOnder, 0);
  lv_arclabel_set_dir(arclabelOnder, LV_ARCLABEL_DIR_COUNTER_CLOCKWISE);
  lv_arclabel_set_text_vertical_align(arclabelOnder, LV_ARCLABEL_TEXT_ALIGN_LEADING);
  lv_arclabel_set_text_horizontal_align(arclabelOnder, LV_ARCLABEL_TEXT_ALIGN_CENTER);
  lv_obj_center(arclabelOnder);

  arclabelSet = lv_arclabel_create(screen);
  lv_obj_set_size(arclabelSet, 240, 240);
  lv_obj_set_style_bg_opa(arclabelSet, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(arclabelSet, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(arclabelSet, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(arclabelSet, 0, LV_PART_MAIN);
  lv_obj_set_style_text_color(arclabelSet, lv_color_hex(0x6B4E9E), LV_PART_MAIN);  // paarsblauw = zodiac
  lv_obj_set_style_text_font(arclabelSet, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_arclabel_set_text(arclabelSet, s);
  lv_arclabel_set_angle_start(arclabelSet, 20);   // centrum 45° (user 135° = rechts-onder)
  lv_arclabel_set_angle_size(arclabelSet, 50);
  lv_arclabel_set_radius(arclabelSet, 95);
  lv_arclabel_set_offset(arclabelSet, 0);
  lv_arclabel_set_dir(arclabelSet, LV_ARCLABEL_DIR_COUNTER_CLOCKWISE);
  lv_arclabel_set_text_vertical_align(arclabelSet, LV_ARCLABEL_TEXT_ALIGN_LEADING);
  lv_arclabel_set_text_horizontal_align(arclabelSet, LV_ARCLABEL_TEXT_ALIGN_CENTER);
  lv_obj_center(arclabelSet);

  // Zodiac en tijd: zichtbaarheid wordt in maanstandUpdate() uit prefs gezet (direct doorgevoerd).
  const char* const* zodiacNamen = langNL ? zodiacNamenNL : zodiacNamenEN;
  int zi = getLocalTime(&t) ? zodiacIndex(t.tm_mon + 1, t.tm_mday) : 0;
  zodiacImg = lv_image_create(screen);
  lv_image_set_src(zodiacImg, zodiacImgDsc[zi]);
  lv_obj_set_style_bg_opa(zodiacImg, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_align(zodiacImg, LV_ALIGN_CENTER, 0, -8);
#if USE_ZODIAC_IMAGES
  lv_obj_set_style_image_recolor(zodiacImg, lv_color_hex(0x6B4E9E), LV_PART_MAIN);
  lv_obj_set_style_image_recolor_opa(zodiacImg, LV_OPA_COVER, LV_PART_MAIN);
#else
  lv_obj_set_style_image_opa(zodiacImg, LV_OPA_TRANSP, LV_PART_MAIN);
#endif
  if (!showZodiac) lv_obj_add_flag(zodiacImg, LV_OBJ_FLAG_HIDDEN);
  labelZodiacName = lv_label_create(screen);
  lv_label_set_text(labelZodiacName, zodiacNamen[zi]);
  lv_obj_set_style_text_color(labelZodiacName, lv_color_hex(0x6B4E9E), LV_PART_MAIN);
  lv_obj_set_style_text_font(labelZodiacName, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(labelZodiacName, LV_ALIGN_CENTER, 0, 40);
  if (!showZodiac) lv_obj_add_flag(labelZodiacName, LV_OBJ_FLAG_HIDDEN);

#if USE_PLANET_CONJUNCTIONS
  for (int i = 0; i < PLANET_COUNT; i++) {
    planetIcons[i] = lv_image_create(screen);
    lv_image_set_src(planetIcons[i], planetImgDsc[i]);
    lv_obj_set_size(planetIcons[i], 24, 40);
    lv_obj_set_style_bg_opa(planetIcons[i], LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_image_recolor(planetIcons[i], lv_color_hex(0x6B4E9E), LV_PART_MAIN);
    lv_obj_set_style_image_recolor_opa(planetIcons[i], LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(planetIcons[i], LV_OBJ_FLAG_HIDDEN);
  }
#endif

#if USE_SPHERE_TERMINATOR
  lv_obj_move_foreground(zodiacImg);
  lv_obj_move_foreground(labelZodiacName);
#if USE_PLANET_CONJUNCTIONS
  for (int i = 0; i < PLANET_COUNT; i++) if (planetIcons[i]) lv_obj_move_foreground(planetIcons[i]);
#endif
#endif
}

static void maanstandUpdate() {
#if USE_SPHERE_TERMINATOR
  if (!arclabelFase || !arclabelRise || !arclabelOnder || !arclabelSet || !moonObj) return;
#else
  if (!arclabelFase || !arclabelRise || !arclabelOnder || !arclabelSet || !shadowObj || !shadowImg) return;
#endif
  float illumPct, leeftijd, fase, angleDeg;
  float moon_az_deg = 0.0f, moon_el_deg = 0.0f;
  int fi;
  char riseStr[8], setStr[8], transitStr[8];
  riseStr[0] = setStr[0] = transitStr[0] = '\0';
  getMoonData(illumPct, fi, leeftijd, fase, riseStr, sizeof(riseStr), setStr, sizeof(setStr),
              transitStr, sizeof(transitStr), &angleDeg, &moon_az_deg, &moon_el_deg);
  bool showZodiac, showConjunctions, conjTestMode, langNL;
  getDisplayPrefs(showZodiac, showConjunctions, conjTestMode, langNL);
  const char* const* faseNamen = langNL ? faseNamenNL : faseNamenEN;
  const char* const* zodiacNamen = langNL ? zodiacNamenNL : zodiacNamenEN;

  /* Zichtbaarheid zodiac direct uit instellingen (zonder herstart) */
  if (zodiacImg) { if (showZodiac) lv_obj_clear_flag(zodiacImg, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(zodiacImg, LV_OBJ_FLAG_HIDDEN); }
  if (labelZodiacName) { if (showZodiac) lv_obj_clear_flag(labelZodiacName, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(labelZodiacName, LV_OBJ_FLAG_HIDDEN); }

#if USE_PLANET_CONJUNCTIONS
  if (showConjunctions) {
    PlanetObserver obs;
    float lat, lon;
    getLatLon(lat, lon);
    planetObserverSet(&obs, (double)lat, (double)lon, 0.0);
    time_t now; time(&now);
    ConjunctionEvent events[PLANET_CONJ_MAX];
    float maxSep = conjTestMode ? 180.0f : (float)PLANET_CONJ_SEP_LIMIT_DEG;
    int n = planetFindConjunctions((double)moon_az_deg, (double)moon_el_deg, now, &obs,
                                   maxSep, events);
    PlanetRimRenderInfo render[PLANET_CONJ_MAX];
    planetComputeRimPositions(events, n, CX, CY, PLANET_RIM_R, render);
    for (int i = 0; i < PLANET_COUNT; i++) {
      if (planetIcons[i]) lv_obj_add_flag(planetIcons[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < n && render[i].visible && planetIcons[(int)render[i].planet_id]; i++) {
      lv_obj_t* icon = planetIcons[(int)render[i].planet_id];
      int x = render[i].screen_x - 12;  /* 24/2: center icoon op rand */
      int y = render[i].screen_y - 20;  /* 40/2: iconen tot 39 px hoog */
      lv_obj_set_pos(icon, x, y);
      lv_obj_clear_flag(icon, LV_OBJ_FLAG_HIDDEN);
    }
  } else {
    for (int i = 0; i < PLANET_COUNT; i++) {
      if (planetIcons[i]) lv_obj_add_flag(planetIcons[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
#endif

#if USE_SPHERE_TERMINATOR
  updateMoonTerminator(angleDeg, fase);
  lv_image_set_src(moonObj, &moon_composed_dsc);
#else
  float L = illumPct / 100.0f;
  float offset = offsetUitLitFraction(L);
  float angleRad = angleDeg * 0.01745329252f;
  float dx =  offset * sinf(angleRad);
  float dy =  offset * cosf(angleRad);
  int sx = (int)(32.0f + dx);
  int sy = (int)(32.0f + dy);
  int imgOx = (int)(-dx);
  int imgOy = (int)(-dy);
  lv_obj_set_pos(shadowObj, sx, sy);
  lv_obj_set_pos(shadowImg, imgOx, imgOy);
#endif

  static char bovenBuf[56];
  struct tm t;
  int si = 0;
  if (getLocalTime(&t)) si = seizoenIndex(t.tm_mon + 1, t.tm_mday);
  if (getLocalTime(&t))
    snprintf(bovenBuf, sizeof(bovenBuf), "%s   %02d-%02d-%02d   %s", seizoenNamen[si], t.tm_mday, t.tm_mon + 1, (t.tm_year + 1900) % 100, faseNamen[fi]);
  else
    snprintf(bovenBuf, sizeof(bovenBuf), "%s   --/--/--   %s", seizoenNamen[si], faseNamen[fi]);
  lv_arclabel_set_text(arclabelFase, bovenBuf);

  /* Voetregel: opkomst @ 225°, huidige tijd @ 180°, ondergang @ 135° */
  const char* r = (riseStr[0]) ? riseStr : "--:--";
  const char* s = (setStr[0]) ? setStr : "--:--";
  static char tijdBuf[16];
  if (getLocalTime(&t)) snprintf(tijdBuf, sizeof(tijdBuf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  else snprintf(tijdBuf, sizeof(tijdBuf), "--:--:--");
  lv_arclabel_set_text(arclabelRise, r);
  lv_arclabel_set_text(arclabelOnder, tijdBuf);
  lv_arclabel_set_text(arclabelSet, s);
  /* Tijd in voetregel: geel als maan zichtbaar (boven horizon), paars/blauw als maan niet zichtbaar */
  if (arclabelOnder) {
    lv_color_t tijdKleur = (moon_el_deg > 0.0f) ? lv_color_make(230, 222, 175) : lv_color_hex(0x6B4E9E);
    lv_obj_set_style_text_color(arclabelOnder, tijdKleur, LV_PART_MAIN);
  }

  int zi = getLocalTime(&t) ? zodiacIndex(t.tm_mon + 1, t.tm_mday) : 0;
  if (zodiacImg) lv_image_set_src(zodiacImg, zodiacImgDsc[zi]);
  if (labelZodiacName) lv_label_set_text(labelZodiacName, zodiacNamen[zi]);
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
#if UI_LANG_NL
  lv_label_set_text(labelStatus, "WiFi verbinden...");
#else
  lv_label_set_text(labelStatus, "Connecting WiFi...");
#endif
  lv_obj_set_style_text_color(labelStatus, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_align(labelStatus, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_center(labelStatus);

  for (int i = 0; i < 5; i++) {
    lv_timer_handler();
    delay(10);
  }

  WiFi.mode(WIFI_STA);
  Preferences prefs;
  prefs.begin(PREF_NAAM, true);
  paramLat.setValue(prefs.getString("lat", DEFAULT_LAT).c_str(), 12);
  paramLon.setValue(prefs.getString("lon", DEFAULT_LON).c_str(), 12);
  String tzPref = prefs.getString("tz", DEFAULT_TZ);
  prefs.end();
  WiFiManager wm;
  wm.addParameter(&paramLat);
  wm.addParameter(&paramLon);
  wm.setSaveParamsCallback(saveParamsCallback);
  wm.setConfigPortalBlocking(true);
  bool connected = wm.autoConnect(WIFI_AP_NAAM);
  if (!connected) {
#if UI_LANG_NL
    lv_label_set_text(labelStatus, "Geen WiFi");
    Serial.println("Geen WiFi / config geannuleerd");
#else
    lv_label_set_text(labelStatus, "No WiFi");
    Serial.println("No WiFi / config cancelled");
#endif
    return;
  }
  Serial.println("WiFi OK: " + WiFi.SSID());

  {
    String ipStr = WiFi.localIP().toString();
#if UI_LANG_NL
    String msg = String("Tijd ophalen...\nWebUI via:\n") + ipStr;
#else
    String msg = String("Fetching time...\nWebUI via:\n") + ipStr;
#endif
    lv_label_set_text(labelStatus, msg.c_str());
    lv_obj_center(labelStatus);
  }
  uint32_t statusScreenStart = millis();
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
  setenv("TZ", tzPref.c_str(), 1);  // TZ uit Preferences (standaard NL)
  tzset();
  if (!getLocalTime(&t)) {
#if UI_LANG_NL
    lv_label_set_text(labelStatus, "Geen NTP-tijd");
    Serial.println("Geen NTP-tijd");
#else
    lv_label_set_text(labelStatus, "No NTP time");
    Serial.println("No NTP time");
#endif
    return;
  }
  Serial.println("Tijd gesynchroniseerd (NL)");

  server.on("/", serveData);
  server.on("/api/data", serveDataJson);
  server.on("/settings", serveSettings);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  Serial.println("Webserver gestart op http://" + WiFi.localIP().toString());

  /* Scherm "Tijd ophalen..." + IP minimaal 2 s tonen voordat we naar het hoofdscherm gaan */
  while ((uint32_t)(millis() - statusScreenStart) < 2000) {
    lv_timer_handler();
    delay(10);
  }

  lv_obj_delete(labelStatus);
  labelStatus = NULL;
  maanstandUI(screen);
  // Hele scherm invalideren zodat boven- en onderrand meegenomen worden (geen dode band).
  lv_obj_invalidate(screen);
  Serial.println("Setup done");
}

void loop() {
  server.handleClient();
  lv_timer_handler();
  static uint32_t last = 0;
  if (millis() - last >= 1000) {   // elke seconde scherm verversen, seconden lopen mee
    last = millis();
    maanstandUpdate();
  }
  delay(5);
}
