/**
 * Maan–planeet samenstanden: bereken en visueel weergeven.
 * Topocentrisch (waarnemer lat/lon), hoekafstand < 5°, iconen op maanrand.
 *
 * Koppeling met SunMoonCalc: gebruik de topocentrische maan-azimut en -elevatie
 * uit SunMoonCalc::Result (r.moon.azimuth, r.moon.elevation in graden) als
 * moon_az_deg en moon_el_deg in planetFindConjunctions().
 */
#pragma once

#include <Arduino.h>
#include <time.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/** Maximale aantal samenstanden tegelijk (7 planeten). */
#define PLANET_CONJ_MAX 7

/** Standaard samenstanddrempel (graden). */
#define PLANET_CONJ_SEP_LIMIT_DEG 5.0f

/** Optioneel: nauwe samenstand voor extra accent (< 2°). */
#define PLANET_CONJ_CLOSE_DEG 2.0f

/** Waarnemer: locatie (en optioneel hoogte). */
struct PlanetObserver {
  double lat_deg;   ///< Breedtegraad (graden)
  double lon_deg;   ///< Lengtegraad (graden)
  double alt_m;     ///< Hoogte t.o.v. zeeniveau (m), optioneel, 0 = standaard
};

/** Hemelpositie (topocentrisch): azimut en elevatie in graden. */
struct SkyPosition {
  double az_deg;    ///< Azimut (graden, 0 = N, 90 = O)
  double el_deg;    ///< Elevatie (graden)
};

/** Planeet-ID (index voor arrays). */
enum PlanetId {
  PLANET_MERCURY = 0,
  PLANET_VENUS,
  PLANET_MARS,
  PLANET_JUPITER,
  PLANET_SATURN,
  PLANET_URANUS,
  PLANET_NEPTUNE,
  PLANET_COUNT
};

/** Eén samenstand-event: planeet binnen drempel van de maan. */
struct ConjunctionEvent {
  PlanetId planet_id;
  float separation_deg;   ///< Hoekafstand (graden)
  float position_angle_rad; ///< Richting t.o.v. maan op hemel (rad), 0 = noord, π/2 = oost
  bool is_close;          ///< true als separation_deg < PLANET_CONJ_CLOSE_DEG
};

/** Render-info: schermpositie voor een planeeticoon op de maanrand. */
struct PlanetRimRenderInfo {
  PlanetId planet_id;
  float separation_deg;
  int screen_x;            ///< Scherm-x (middelpunt icoon)
  int screen_y;            ///< Scherm-y (middelpunt icoon)
  float position_angle_rad; ///< Gebruikt voor overlap-sortering
  bool visible;            ///< false = niet tekenen
};

/** Vul observer uit lat/lon (alt_m = 0). */
inline void planetObserverSet(PlanetObserver* obs, double lat_deg, double lon_deg, double alt_m = 0.0) {
  obs->lat_deg = lat_deg;
  obs->lon_deg = lon_deg;
  obs->alt_m   = alt_m;
}

/**
 * Hoekafstand tussen twee posities aan de hemel (graden).
 * Gebruikt sin/cos voor numerieke stabiliteit.
 */
float planetAngularSeparationDeg(double az1_deg, double el1_deg, double az2_deg, double el2_deg);

/**
 * Richting van maan naar planeet in het beeldvlak (positiehoek).
 * Retourneert positiehoek in radialen: 0 = noord (omhoog), π/2 = oost (rechts).
 * LVGL: scherm (x naar rechts, y naar beneden) → zie planetRimScreenOffset.
 */
float planetDirectionFromMoonRad(double moon_az_deg, double moon_el_deg,
                                  double planet_az_deg, double planet_el_deg);

/**
 * Topocentrische positie van een planeet (azimut, elevatie in graden).
 * Retourneert true bij succes; bij false zijn az_deg/el_deg onbepaald.
 * Implementatie kan placeholder zijn; vervang door echte ephemeriden.
 */
bool getPlanetTopocentricPosition(PlanetId planet_id, time_t utc_time,
                                   const PlanetObserver* observer,
                                   SkyPosition* out_az_el);

/**
 * Zoek alle samenstanden: maan + alle planeten met separatie < max_sep_deg.
 * moon_az_deg, moon_el_deg = topocentrische maanpositie (uit SunMoonCalc).
 * Vul events[] en retourneer aantal (max PLANET_CONJ_MAX).
 */
int planetFindConjunctions(double moon_az_deg, double moon_el_deg,
                            time_t utc_time, const PlanetObserver* observer,
                            float max_sep_deg,
                            ConjunctionEvent* events);

/**
 * Bepaal schermposities voor planeeticonen op de maanrand.
 * moon_cx, moon_cy = pixel-middelpunt van de maan; moon_radius_px = straal in pixels.
 * Iconen worden op de rand geplaatst; bij overlap kleine tangentiële of radiale offset.
 */
void planetComputeRimPositions(const ConjunctionEvent* events, int num_events,
                                int moon_cx, int moon_cy, int moon_radius_px,
                                PlanetRimRenderInfo* out_render);

/** Planeetnaam voor debug (kort). */
const char* planetName(PlanetId id);

/** Optioneel: format string voor labelregel, bv. "☽ + ♃ 2.3°". */
void planetConjunctionLabel(const ConjunctionEvent* events, int num_events, char* buf, size_t buf_len);
