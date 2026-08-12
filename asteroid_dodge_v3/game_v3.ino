#include <Wire.h>
#include <U8g2lib.h>

// ── DISPLAY ─────────────────────────────────────────────────────────────────
U8G2_SH1106_128X64_NONAME_1_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
// SW I2C fallback: U8G2_SH1106_128X64_NONAME_1_SW_I2C u8g2(U8G2_R0,22,21,U8X8_PIN_NONE);

// ── PINS ────────────────────────────────────────────────────────────────────
#define VRX     34    // joystick X-axis ADC
#define VRY     35    // joystick Y-axis ADC
#define SW      25    // joystick push-button (active LOW, pull-up)
#define BTN1    26    // fire  (active LOW, pull-up)
#define BTN2    27    // boost (active LOW, pull-up)
#define LED     18
#define BUZZER  19

// ── TIMING ──────────────────────────────────────────────────────────────────
#define TICK_MS       50    // 20 FPS

// ── SCREEN ──────────────────────────────────────────────────────────────────
#define SCR_W  128
#define SCR_H   64
#define HUD_H    9

// ── JOYSTICK TUNING ─────────────────────────────────────────────────────────
// The ESP32 ADC is 12-bit: 0–4095, centre ≈ 2048.
// A real player almost never returns fully to centre while playing —
// they sweep R-R-R-L-L-U-D continuously.
// We handle this with PROPORTIONAL velocity:
//   • Inside JOY_DEAD (±400 counts)  →  zero velocity (true rest only)
//   • Outside JOY_DEAD               →  velocity scales linearly from 0 to JOY_MAX_SPD
// This gives a smooth "glide" feel: a gentle push = slow drift,
// a hard push = full speed — exactly how a human plays.
#define JOY_CENTRE    2048
#define JOY_DEAD       400   // counts around centre with ZERO output
#define JOY_MAX_SPD      3   // max pixels/frame at full deflection (normal)
#define JOY_BOOST_SPD    5   // max pixels/frame when BTN2 held

// Sub-pixel accumulator precision: we work in 1/4-pixel units internally
// so the ship moves smoothly at fractional speeds (e.g. 0.5 px/frame).
// Int arithmetic only — no floats, ESP32-friendly.
#define SUBPIX         4     // 1 real pixel = SUBPIX sub-pixel units

// ── GAME OBJECTS ────────────────────────────────────────────────────────────
#define MAX_ROCKS    5    // one extra asteroid on-screen
#define ROCK_W      10    // bigger than before (was 7)
#define ROCK_H      10
#define SHIP_W       7
#define SHIP_H       9

#define MAX_BULLETS  4
#define BULLET_W     1
#define BULLET_H     3
#define BULLET_SPD   5

// ── STARS (scrolling parallax layers) ───────────────────────────────────────
// Two layers: far (slow, dim = single pixel) and near (faster, bright).
// Both scroll downward so the ship appears to fly upward through space.
#define STAR_FAR_COUNT   10
#define STAR_NEAR_COUNT   6
// Speeds in sub-pixel units per frame (SUBPIX=4 → 4 sub-px = 1 real px/frame)
#define STAR_FAR_SPD      2   // 0.5 real px / frame
#define STAR_NEAR_SPD     5   // 1.25 real px / frame

// ── STRUCTS ─────────────────────────────────────────────────────────────────
struct Rock {
  int8_t  x, y, spd;
  bool    active;
};

struct Bullet {
  int8_t  x, y;
  bool    active;
};

// ── STATE ────────────────────────────────────────────────────────────────────
Rock     rocks[MAX_ROCKS];
Bullet   bullets[MAX_BULLETS];

// Player position stored in sub-pixel units (divide by SUBPIX to draw)
int32_t  pxSub, pySub;

uint16_t score;
uint8_t  lives;
bool     gameOver;
uint8_t  flashLED;
uint8_t  spawnTimer;
uint8_t  spawnInterval;
uint32_t lastTick;

// Button debounce (previous raw state)
bool prevSW   = HIGH;
bool prevBTN1 = HIGH;
bool prevBTN2 = HIGH;

uint8_t fireCooldown = 0;
bool    boostFlicker = false;

// Scrolling star arrays — Y stored in sub-pixel units
int16_t farStarX[STAR_FAR_COUNT];
int16_t farStarY[STAR_FAR_COUNT];    // sub-pixel Y
int16_t nearStarX[STAR_NEAR_COUNT];
int16_t nearStarY[STAR_NEAR_COUNT];  // sub-pixel Y

// ── HELPERS ──────────────────────────────────────────────────────────────────
inline bool hits(int ax, int ay, int aw, int ah,
                  int bx, int by, int bw, int bh) {
  return ax < bx+bw && ax+aw > bx && ay < by+bh && ay+ah > by;
}

// ─────────────────────────────────────────────────────────────────────────────
// readADCAvg():
//   The ESP32 ADC has well-known non-linearity near the voltage rails (0 V
//   and 3.3 V).  Pushing the joystick fully LEFT or fully UP drives the pin
//   close to 0 V, which produces wildly inconsistent single samples — this
//   is exactly why left/up felt unreliable compared to right/down.
//
//   Fix: take SAMPLES consecutive reads and return their average.
//   4 samples cuts peak noise by ~50% and is fast enough to fit inside a
//   50 ms tick without any perceptible delay.
// ─────────────────────────────────────────────────────────────────────────────
#define ADC_SAMPLES 4
int readADCAvg(uint8_t pin) {
  int32_t sum = 0;
  for (uint8_t s = 0; s < ADC_SAMPLES; s++) sum += analogRead(pin);
  return (int)(sum / ADC_SAMPLES);
}

// ─────────────────────────────────────────────────────────────────────────────
// readAxisSub():
//   Returns a velocity in sub-pixel units per frame for one joystick axis.
//   • Dead zone: returns 0 inside ±JOY_DEAD counts of centre.
//   • Outside dead zone: linearly interpolates from 0 to maxSpdReal*SUBPIX.
//   • invert: flip sign if your module's axis runs backwards.
//
//   Uses readADCAvg() so near-rail noise is averaged out — this makes
//   LEFT (which pulls toward 0 V) as smooth as RIGHT (which pulls toward
//   3.3 V, where the ADC is more accurate).
// ─────────────────────────────────────────────────────────────────────────────
int32_t readAxisSub(uint8_t axisPin, int maxSpdReal, bool invert = false) {
  int raw   = readADCAvg(axisPin);   // averaged — kills low-rail noise
  int delta = raw - JOY_CENTRE;

  if (delta > -JOY_DEAD && delta < JOY_DEAD) return 0;  // dead zone → no move

  // Available range outside dead zone on each side
  int range = JOY_CENTRE - JOY_DEAD;   // = 1648 counts

  int magnitude;
  int sign;
  if (delta > 0) {
    sign      = invert ? -1 : +1;
    magnitude = delta - JOY_DEAD;
  } else {
    sign      = invert ? +1 : -1;
    magnitude = -delta - JOY_DEAD;
  }
  if (magnitude > range) magnitude = range;  // clamp

  // Linear map: magnitude ∈ [0, range] → velocity ∈ [0, maxSpdReal * SUBPIX]
  int32_t vel = ((int32_t)magnitude * maxSpdReal * SUBPIX) / range;
  return sign * vel;
}

// ── SPAWN / RESET ────────────────────────────────────────────────────────────
void spawnRock() {
  for (uint8_t i = 0; i < MAX_ROCKS; i++) {
    if (!rocks[i].active) {
      rocks[i].x   = random(1, SCR_W - ROCK_W - 1);
      rocks[i].y   = HUD_H;
      // Speed: starts at 1 px/frame, max 2 — gentle ramp every 20 pts
      rocks[i].spd = 1 + score / 20;
      if (rocks[i].spd > 2) rocks[i].spd = 2;
      rocks[i].active = true;
      return;
    }
  }
}

void fireBullet() {
  for (uint8_t i = 0; i < MAX_BULLETS; i++) {
    if (!bullets[i].active) {
      bullets[i].x      = (pxSub / SUBPIX) + SHIP_W / 2;
      bullets[i].y      = (pySub / SUBPIX) - BULLET_H;
      bullets[i].active = true;
      tone(BUZZER, 1800, 15);
      return;
    }
  }
}

void initStars() {
  // Far layer — spread across full screen height, random X
  for (uint8_t i = 0; i < STAR_FAR_COUNT; i++) {
    farStarX[i] = random(SCR_W);
    farStarY[i] = random(HUD_H + 1, SCR_H) * SUBPIX;
  }
  // Near layer — spread across full screen height, random X
  for (uint8_t i = 0; i < STAR_NEAR_COUNT; i++) {
    nearStarX[i] = random(SCR_W);
    nearStarY[i] = random(HUD_H + 1, SCR_H) * SUBPIX;
  }
}

void resetGame() {
  pxSub = (int32_t)((SCR_W - SHIP_W) / 2) * SUBPIX;
  pySub = (int32_t)(SCR_H - SHIP_H - 2)   * SUBPIX;
  score = 0;  lives = 3;
  gameOver      = false;
  flashLED      = 0;
  spawnTimer    = 0;
  spawnInterval = 65;
  fireCooldown  = 0;
  boostFlicker  = false;
  for (uint8_t i = 0; i < MAX_ROCKS;   i++) rocks[i].active   = false;
  for (uint8_t i = 0; i < MAX_BULLETS; i++) bullets[i].active = false;
  digitalWrite(LED, LOW);
}

// ── DRAW ROUTINES ────────────────────────────────────────────────────────────
void drawRocket(int x, int y, bool boost) {
  u8g2.drawBox(x+2, y+2, 3, 5);   // body
  u8g2.drawPixel(x+3, y);          // nose tip
  u8g2.drawPixel(x+2, y+1);
  u8g2.drawPixel(x+4, y+1);
  u8g2.drawBox(x,   y+5, 2, 3);   // left fin
  u8g2.drawBox(x+5, y+5, 2, 3);   // right fin
  // exhaust flame flickers when boosting
  if (boost && boostFlicker) {
    u8g2.drawPixel(x+2, y+8);
    u8g2.drawPixel(x+4, y+8);
    u8g2.drawPixel(x+3, y+9);
  } else {
    u8g2.drawPixel(x+3, y+8);
  }
}

// Larger 10×10 asteroid (was 7×7)
void drawAsteroid(int x, int y) {
  u8g2.drawBox(x+2, y,     6, 2);   // top cap
  u8g2.drawBox(x,   y+2,  10, 6);   // wide middle band
  u8g2.drawBox(x+2, y+8,   6, 2);   // bottom cap
}

// ── SETUP ────────────────────────────────────────────────────────────────────
void setup() {
  randomSeed(analogRead(36));

  pinMode(SW,    INPUT_PULLUP);
  pinMode(BTN1,  INPUT_PULLUP);
  pinMode(BTN2,  INPUT_PULLUP);
  pinMode(LED,   OUTPUT);
  pinMode(BUZZER,OUTPUT);

  Wire.begin();
  Wire.setClock(400000);
  u8g2.begin();
  u8g2.setFont(u8g2_font_5x7_tr);

  initStars();
  resetGame();

  // Splash screen
  u8g2.firstPage();
  do {
    u8g2.drawStr(18, 20, "ASTEROID DODGE");
    u8g2.drawStr(10, 34, "Joy=move  BTN1=fire");
    u8g2.drawStr(10, 46, "BTN2=boost  SW=start");
  } while (u8g2.nextPage());
  delay(2000);

  lastTick = millis();
}

// ── MAIN LOOP ────────────────────────────────────────────────────────────────
void loop() {
  uint32_t now = millis();
  if (now - lastTick < TICK_MS) return;
  lastTick = now;

  // ── READ INPUTS ────────────────────────────────────────────────────────────
  // boost flag used to choose max speed for readAxisSub
  bool rawBTN2 = (digitalRead(BTN2) == LOW);
  bool boost   = rawBTN2;
  int  maxSpd  = boost ? JOY_BOOST_SPD : JOY_MAX_SPD;

  // Proportional sub-pixel velocities per frame
  // If your physical module has X/Y swapped, swap VRX↔VRY below.
  // If an axis is inverted, set the last argument to true.
  int32_t vx = readAxisSub(VRX, maxSpd, false);  // +right, -left
  int32_t vy = readAxisSub(VRY, maxSpd, false);  // +down,  -up

  bool rawSW   = (digitalRead(SW)   == LOW);
  bool rawBTN1 = (digitalRead(BTN1) == LOW);

  // Edge-detect for momentary actions
  bool pressedSW   = (rawSW   && !prevSW);
  bool pressedBTN1 = (rawBTN1 && !prevBTN1);

  prevSW   = rawSW;
  prevBTN1 = rawBTN1;
  prevBTN2 = rawBTN2;

  boostFlicker = !boostFlicker;

  // ── SCROLL STARS ─────────────────────────────────────────────────────────
  // Stars move downward (ship flies "up") at two parallax speeds.
  // When a star scrolls past the bottom it wraps to just above the HUD.
  int16_t farBottom  = (int16_t)(SCR_H)   * SUBPIX;
  int16_t hudTop     = (int16_t)(HUD_H+1) * SUBPIX;

  for (uint8_t i = 0; i < STAR_FAR_COUNT; i++) {
    farStarY[i] += STAR_FAR_SPD;
    if (farStarY[i] >= farBottom) {
      farStarY[i] = hudTop + random(8) * SUBPIX;
      farStarX[i] = random(SCR_W);
    }
  }
  for (uint8_t i = 0; i < STAR_NEAR_COUNT; i++) {
    nearStarY[i] += STAR_NEAR_SPD;
    if (nearStarY[i] >= farBottom) {
      nearStarY[i] = hudTop + random(4) * SUBPIX;
      nearStarX[i] = random(SCR_W);
    }
  }

  // ── GAME OVER SCREEN ───────────────────────────────────────────────────────
  if (gameOver) {
    u8g2.firstPage();
    do {
      u8g2.drawStr(22, 20, "GAME OVER");
      u8g2.setCursor(22, 34);
      u8g2.print("Score: "); u8g2.print(score);
      u8g2.drawStr(12, 50, "[SW] to restart");
    } while (u8g2.nextPage());

    if (pressedSW) resetGame();
    return;
  }

  // ── PLAYER MOVEMENT ────────────────────────────────────────────────────────
  // Add proportional velocity to sub-pixel position, then clamp.
  pxSub += vx;
  pySub += vy;

  int32_t pxMin = 0;
  int32_t pxMax = (int32_t)(SCR_W - SHIP_W) * SUBPIX;
  int32_t pyMin = (int32_t)(HUD_H)           * SUBPIX;
  int32_t pyMax = (int32_t)(SCR_H - SHIP_H)  * SUBPIX;
  if (pxSub < pxMin) pxSub = pxMin;
  if (pxSub > pxMax) pxSub = pxMax;
  if (pySub < pyMin) pySub = pyMin;
  if (pySub > pyMax) pySub = pyMax;

  // Integer pixel position for drawing and collision
  int px = (int)(pxSub / SUBPIX);
  int py = (int)(pySub / SUBPIX);

  // ── FIRE ───────────────────────────────────────────────────────────────────
  if (fireCooldown > 0) fireCooldown--;
  if (pressedBTN1 && fireCooldown == 0) {
    fireBullet();
    fireCooldown = 6;
  }

  // ── UPDATE BULLETS ─────────────────────────────────────────────────────────
  for (uint8_t i = 0; i < MAX_BULLETS; i++) {
    if (!bullets[i].active) continue;
    bullets[i].y -= BULLET_SPD;
    if (bullets[i].y + BULLET_H < HUD_H) { bullets[i].active = false; continue; }
    for (uint8_t r = 0; r < MAX_ROCKS; r++) {
      if (!rocks[r].active) continue;
      if (hits(bullets[i].x, bullets[i].y, BULLET_W, BULLET_H,
               rocks[r].x,   rocks[r].y,   ROCK_W,   ROCK_H)) {
        bullets[i].active = false;
        rocks[r].active   = false;
        score++;
        tone(BUZZER, 1200, 40);
        // LED flashes only on a successful bullet hit
        digitalWrite(LED, HIGH);
        delay(30);
        digitalWrite(LED, LOW);
        break;
      }
    }
  }

  // ── ASTEROIDS ──────────────────────────────────────────────────────────────
  for (uint8_t i = 0; i < MAX_ROCKS; i++) {
    if (!rocks[i].active) continue;
    rocks[i].y += rocks[i].spd;

    if (rocks[i].y > SCR_H) {
      rocks[i].active = false;
      score++;
      tone(BUZZER, 1000, 30);
      if (score % 5 == 0 && spawnInterval > 25)
        spawnInterval -= 2;
      continue;
    }

    if (hits(px, py, SHIP_W, SHIP_H,
             rocks[i].x, rocks[i].y, ROCK_W, ROCK_H)) {
      rocks[i].active = false;
      lives--;
      tone(BUZZER, 220, 250);  // buzz only — no LED on ship hit
      if (lives == 0) {
        gameOver = true;
        tone(BUZZER, 150, 700);
      }
    }
  }

  // ── SPAWN ──────────────────────────────────────────────────────────────────
  if (++spawnTimer >= spawnInterval) {
    spawnTimer = 0;
    spawnRock();
  }

  // LED is now driven directly on bullet hit (see bullet-collision block above).
  // No frame-by-frame flashLED ticker needed.

  // ── DRAW ───────────────────────────────────────────────────────────────────
  u8g2.firstPage();
  do {
    // -- Far star layer (single pixels, slow scroll)
    for (uint8_t i = 0; i < STAR_FAR_COUNT; i++) {
      int sy = (int)(farStarY[i]  / SUBPIX);
      int sx = (int)farStarX[i];
      if (sy > HUD_H && sy < SCR_H)
        u8g2.drawPixel(sx, sy);
    }

    // -- Near star layer (2×1 pixels, faster scroll — looks closer)
    for (uint8_t i = 0; i < STAR_NEAR_COUNT; i++) {
      int sy = (int)(nearStarY[i] / SUBPIX);
      int sx = (int)nearStarX[i];

      if (sy > HUD_H && sy < SCR_H) {
        u8g2.drawPixel(sx, sy);
        if (sy + 1 < SCR_H) u8g2.drawPixel(sx, sy + 1); // 2px tall = "brighter"
      }
    }

    // -- HUD
    u8g2.drawHLine(0, HUD_H - 1, SCR_W);
    u8g2.setCursor(0, 7);  u8g2.print(score);
    for (uint8_t i = 0; i < lives; i++)
      u8g2.drawBox(100 + i * 9, 1, 6, 6);

    // -- Bullets
    for (uint8_t i = 0; i < MAX_BULLETS; i++)
      if (bullets[i].active)
        u8g2.drawBox(bullets[i].x, bullets[i].y, BULLET_W, BULLET_H);

    // -- Asteroids
    for (uint8_t i = 0; i < MAX_ROCKS; i++)
      if (rocks[i].active) drawAsteroid(rocks[i].x, rocks[i].y);

    // -- Rocket
    drawRocket(px, py, boost);

  } while (u8g2.nextPage());
}