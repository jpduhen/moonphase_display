/**
 * Implementatie maan–planeet samenstanden.
 * Planeetposities: vereenvoudigde mean-element ephemeris (zie kPlanetElements).
 * Voor hogere nauwkeurigheid: vervang getPlanetTopocentricPosition() door
 * een echte ephemeris (bijv. JPL, VSOP87 of library) en behoud de rest van de flow.
 */
#include "PlanetConjunctions.h"
#include <stdlib.h>
#include <stdio.h>

#ifndef PLANET_CONJ_DEBUG
#define PLANET_CONJ_DEBUG 0  /* 1 = Serial output per samenstand */
#endif
#if PLANET_CONJ_DEBUG
#include <Arduino.h>
#endif

static const double D2R = M_PI / 180.0;
static const double R2D = 180.0 / M_PI;

/* --- Hoekafstand (great-circle) in graden --- */
float planetAngularSeparationDeg(double az1_deg, double el1_deg, double az2_deg, double el2_deg) {
  double a1 = az1_deg * D2R, e1 = el1_deg * D2R;
  double a2 = az2_deg * D2R, e2 = el2_deg * D2R;
  double c1 = cos(e1), s1 = sin(e1);
  double c2 = cos(e2), s2 = sin(e2);
  double daz = a2 - a1;
  double cos_sep = s1 * s2 + c1 * c2 * cos(daz);
  if (cos_sep > 1.0) cos_sep = 1.0;
  if (cos_sep < -1.0) cos_sep = -1.0;
  return (float)(acos(cos_sep) * R2D);
}

/* --- Richting maan → planeet: positiehoek (rad). 0 = noord (omhoog), π/2 = oost (rechts). --- */
float planetDirectionFromMoonRad(double moon_az_deg, double moon_el_deg,
                                  double planet_az_deg, double planet_el_deg) {
  double mel = moon_el_deg * D2R;
  double d_el = (planet_el_deg - moon_el_deg) * D2R;  /* noord-component (tangentvlak) */
  double d_az = (planet_az_deg - moon_az_deg) * D2R;
  double east = d_az * cos(mel);  /* oost-component */
  return (float)atan2(east, d_el);
}

/* --- Julian Day uit time_t (UTC) --- */
static double timeToJD(time_t t) {
  return (double)(t) / 86400.0 + 2440587.5;
}

/* --- GMST in uren (UT1 ≈ UTC voor onze doeleinden) --- */
static double jdToGMST(double jd) {
  double T = (jd - 2451545.0) / 36525.0;
  double g = 280.46061837 + 360.98564736629 * (jd - 2451545.0) + 0.000387933*T*T;
  while (g < 0) g += 360.0;
  while (g >= 360.0) g -= 360.0;
  return g / 15.0; /* uren */
}

/* Vereenvoudigde geocentrische eclipticaal: alleen lengte (graden), breedte ≈ 0. */
struct PlanetMeanElements { double L0; double n; };
static const PlanetMeanElements kPlanetElements[] = {
  { 252.0,  4.09 },  /* Mercury */
  { 181.0,  1.60 },  /* Venus */
  { 356.0,  0.52 },  /* Mars */
  {  34.0,  0.08 },  /* Jupiter */
  {  50.0,  0.03 },  /* Saturn */
  { 314.0,  0.01 },  /* Uranus */
  { 304.0,  0.006 }, /* Neptune */
};

static void eclipticToEquatorial(double lon_deg, double lat_deg, double T,
                                  double* ra_deg, double* dec_deg) {
  double obliq = (23.439 - 0.013 * T) * D2R;
  double lon = lon_deg * D2R, lat = lat_deg * D2R;
  double cl = cos(lon), sl = sin(lon), cb = cos(lat), sb = sin(lat);
  double ce = cos(obliq), se = sin(obliq);
  double ra = atan2(sl * ce - sb * se / cb, cl);
  double dec = asin(sb * ce + cb * se * sl);
  *ra_deg = ra * R2D; if (*ra_deg < 0) *ra_deg += 360.0;
  *dec_deg = dec * R2D;
}

static void equatorialToHorizontal(double ra_deg, double dec_deg,
                                    double lst_hours, double lat_deg,
                                    double* az_deg, double* el_deg) {
  double lat = lat_deg * D2R;
  double ha_deg = lst_hours * 15.0 - ra_deg;
  while (ha_deg > 180.0) ha_deg -= 360.0;
  while (ha_deg < -180.0) ha_deg += 360.0;
  double ha = ha_deg * D2R;
  double dec = dec_deg * D2R;
  double sh = sin(ha), ch = cos(ha);
  double sd = sin(dec), cd = cos(dec);
  double sl = sin(lat), cl = cos(lat);
  double sin_el = sd * sl + cd * cl * ch;
  if (sin_el > 1.0) sin_el = 1.0; if (sin_el < -1.0) sin_el = -1.0;
  *el_deg = asin(sin_el) * R2D;
  double cos_az = (sd - sl * sin_el) / (cl * cos(asin(sin_el) + 1e-10));
  if (cl < 1e-10) cos_az = ch;
  double tan_az = sh / (ch * sl - cd * cl * tan(dec));
  *az_deg = atan2(sh, ch * sl - cd * cl * tan(dec)) * R2D;
  if (*az_deg < 0) *az_deg += 360.0;
}

bool getPlanetTopocentricPosition(PlanetId planet_id, time_t utc_time,
                                   const PlanetObserver* observer,
                                   SkyPosition* out_az_el) {
  if (!observer || !out_az_el || (int)planet_id < 0 || (int)planet_id >= PLANET_COUNT)
    return false;
  double jd = timeToJD(utc_time);
  double T = (jd - 2451545.0) / 36525.0;
  const PlanetMeanElements* e = &kPlanetElements[(int)planet_id];
  double lon_deg = e->L0 + e->n * T * 36525.0;
  while (lon_deg < 0) lon_deg += 360.0;
  while (lon_deg >= 360.0) lon_deg -= 360.0;
  double lat_deg = 0.0; /* vereenvoudigd */
  double ra_deg, dec_deg;
  eclipticToEquatorial(lon_deg, lat_deg, T, &ra_deg, &dec_deg);
  double lst_h = jdToGMST(jd) + observer->lon_deg / 15.0;
  while (lst_h < 0) lst_h += 24.0;
  while (lst_h >= 24.0) lst_h -= 24.0;
  equatorialToHorizontal(ra_deg, dec_deg, lst_h, observer->lat_deg,
                         &out_az_el->az_deg, &out_az_el->el_deg);
  return true;
}

int planetFindConjunctions(double moon_az_deg, double moon_el_deg,
                            time_t utc_time, const PlanetObserver* observer,
                            float max_sep_deg,
                            ConjunctionEvent* events) {
  if (!observer || !events) return 0;
  int n = 0;
  SkyPosition planet_pos;
  for (int i = 0; i < PLANET_COUNT && n < PLANET_CONJ_MAX; i++) {
    PlanetId id = (PlanetId)i;
    if (!getPlanetTopocentricPosition(id, utc_time, observer, &planet_pos))
      continue;
    float sep = planetAngularSeparationDeg(
      moon_az_deg, moon_el_deg, planet_pos.az_deg, planet_pos.el_deg);
    if (sep >= max_sep_deg) continue;
    ConjunctionEvent* ev = &events[n++];
    ev->planet_id = id;
    ev->separation_deg = sep;
    ev->position_angle_rad = planetDirectionFromMoonRad(
      moon_az_deg, moon_el_deg, planet_pos.az_deg, planet_pos.el_deg);
    ev->is_close = (sep < PLANET_CONJ_CLOSE_DEG);
#if PLANET_CONJ_DEBUG
    Serial.printf("[Conj] %s sep=%.2f° PA=%.2f rad\n", planetName(id), (double)sep, (double)ev->position_angle_rad);
#endif
  }
  return n;
}

/* Minimale overlap-afstand (rad); anders radiale offset (naar binnen of buiten). */
static const float RIM_OVERLAP_ANGLE_RAD = 0.35f;  /* ~20° */
#define RIM_RADIAL_OFFSET_PX   6
/* Max offset t.o.v. maanrand (naar binnen en naar buiten). */
#define RIM_RADIAL_MAX_OFFSET_PX  12

void planetComputeRimPositions(const ConjunctionEvent* events, int num_events,
                                int moon_cx, int moon_cy, int moon_radius_px,
                                PlanetRimRenderInfo* out_render) {
  int r_min = moon_radius_px - RIM_RADIAL_MAX_OFFSET_PX;
  int r_max = moon_radius_px + RIM_RADIAL_MAX_OFFSET_PX;
  for (int i = 0; i < num_events; i++) {
    const ConjunctionEvent* ev = &events[i];
    float pa = ev->position_angle_rad;
    /* Scherm: x naar rechts, y naar beneden. PA 0 = noord = omhoog → (0,-1); PA π/2 = oost = rechts → (1,0).
       Offset van center: dx = sin(pa)*R, dy = -cos(pa)*R. */
    int overlap_count = 0;
    for (int j = 0; j < i; j++) {
      float dpa = (float)fabs(ev->position_angle_rad - events[j].position_angle_rad);
      if (dpa > (float)M_PI) dpa = (float)(2.0*M_PI - dpa);
      if (dpa < RIM_OVERLAP_ANGLE_RAD)
        overlap_count++;
    }
    /* Afwisselend naar buiten (+6) en naar binnen (-6) bij overlap, zodat iconen niet allemaal naar buiten schuiven. */
    int delta = 0;
    if (overlap_count >= 1)
      delta = (overlap_count % 2 == 1) ? RIM_RADIAL_OFFSET_PX : -RIM_RADIAL_OFFSET_PX;
    int r = moon_radius_px + delta;
    if (r < r_min) r = r_min;
    if (r > r_max) r = r_max;
    float c = (float)cos(pa), s = (float)sin(pa);
    int dx = (int)(s * (float)r);
    int dy = (int)(-c * (float)r);
    out_render[i].planet_id = ev->planet_id;
    out_render[i].separation_deg = ev->separation_deg;
    out_render[i].position_angle_rad = pa;
    out_render[i].screen_x = moon_cx + dx;
    out_render[i].screen_y = moon_cy + dy;
    out_render[i].visible = true;
#if PLANET_CONJ_DEBUG
    Serial.printf("[Rim] %s x=%d y=%d\n", planetName(ev->planet_id), out_render[i].screen_x, out_render[i].screen_y);
#endif
  }
}

const char* planetName(PlanetId id) {
  static const char* names[] = { "Mercury", "Venus", "Mars", "Jupiter", "Saturn", "Uranus", "Neptune" };
  if ((int)id < 0 || (int)id >= PLANET_COUNT) return "?";
  return names[(int)id];
}

void planetConjunctionLabel(const ConjunctionEvent* events, int num_events, char* buf, size_t buf_len) {
  if (!buf || buf_len == 0) return;
  buf[0] = '\0';
  size_t pos = 0;
  for (int i = 0; i < num_events && pos < buf_len - 1; i++) {
    if (i > 0 && pos < buf_len - 1) { buf[pos++] = ' '; buf[pos] = '\0'; }
    char part[28];
    snprintf(part, sizeof(part), "+ %s %.1f°", planetName(events[i].planet_id), (double)events[i].separation_deg);
    for (const char* s = part; *s && pos < buf_len - 1; s++) buf[pos++] = *s;
    buf[pos] = '\0';
  }
}
