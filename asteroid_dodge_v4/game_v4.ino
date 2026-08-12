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
// ESP32 ADC: 12-bit (0–4095), centre ≈ 2048.
#define JOY_CENTRE    2048
#define JOY_DEAD       400   // ±counts around centre with ZERO output

// Normal / boost max speed (pixels / frame at full deflection)
#define JOY_MAX_SPD      3
#define JOY_BOOST_SPD    5

// Sub-pixel accumulator — 1 real pixel = SUBPIX units (integer only, no floats)
#define SUBPIX           4

// ADC multi-sample count — averages out low-rail noise (fixes LEFT axis)
#define ADC_SAMPLES      4

// Diagonal boost factor × 128 (integer fixed-point).
// A physical thumb-stick gate clips diagonal travel to ~71% of cardinal range.
// Multiplying by DIAG_BOOST / 128 ≈ 1.35 corrects this so diagonal feels
// exactly as responsive as up/down/left/right.
// Applied only when BOTH axes are active simultaneously.
#define DIAG_BOOST      173   // 173/128 ≈ 1.35

// ── GAME OBJECTS ────────────────────────────────────────────────────────────
#define MAX_ROCKS    5
#define ROCK_W      11    // collision width (slightly wider for larger sprite)
#define ROCK_H      11    // collision height
#define SHIP_W       7
#define SHIP_H       9

#define MAX_BULLETS  4
#define BULLET_W     1
#define BULLET_H     3
#define BULLET_SPD   5

// ── STARS (scrolling parallax) ───────────────────────────────────────────────
#define STAR_FAR_COUNT   10
#define STAR_NEAR_COUNT   6
#define STAR_FAR_SPD      2   // sub-px / frame → 0.5 real px/frame
#define STAR_NEAR_SPD     5   // sub-px / frame → 1.25 real px/frame

// ── STRUCTS ─────────────────────────────────────────────────────────────────
struct Rock   { int8_t x, y, spd; bool active; };
struct Bullet { int8_t x, y;      bool active; };

// ── GLOBAL STATE ─────────────────────────────────────────────────────────────
Rock     rocks[MAX_ROCKS];
Bullet   bullets[MAX_BULLETS];
int32_t  pxSub, pySub;          // player pos in sub-pixel units
uint16_t score;
uint8_t  lives;
bool     gameOver;
bool     onTitleScreen;          // true while showing title
uint8_t  flashLED;
uint8_t  spawnTimer;
uint8_t  spawnInterval;
uint32_t lastTick;

bool prevSW   = HIGH;
bool prevBTN1 = HIGH;
bool prevBTN2 = HIGH;

uint8_t fireCooldown = 0;
bool    boostFlicker = false;

// Scrolling star arrays — Y in sub-pixel units
int16_t farStarX[STAR_FAR_COUNT];
int16_t farStarY[STAR_FAR_COUNT];
int16_t nearStarX[STAR_NEAR_COUNT];
int16_t nearStarY[STAR_NEAR_COUNT];

// ── HELPERS ──────────────────────────────────────────────────────────────────
inline bool hits(int ax, int ay, int aw, int ah,
                  int bx, int by, int bw, int bh) {
  return ax < bx+bw && ax+aw > bx && ay < by+bh && ay+ah > by;
}

// ─────────────────────────────────────────────────────────────────────────────
// readADCAvg() — averages ADC_SAMPLES reads to kill low-rail noise.
// The ESP32 ADC is least accurate near 0 V (joystick pushed LEFT), so a
// single read can swing ±300 counts.  Averaging 4 reads cuts that in half.
// ─────────────────────────────────────────────────────────────────────────────
int readADCAvg(uint8_t pin) {
  int32_t sum = 0;
  for (uint8_t s = 0; s < ADC_SAMPLES; s++) sum += analogRead(pin);
  return (int)(sum / ADC_SAMPLES);
}

// ─────────────────────────────────────────────────────────────────────────────
// readAxisSub() — proportional velocity in sub-pixel units.
//   Dead zone   → 0
//   0–100% push → 0 to maxSpdReal*SUBPIX  (linear)
// ─────────────────────────────────────────────────────────────────────────────
int32_t readAxisSub(uint8_t axisPin, int maxSpdReal, bool invert = false) {
  int delta = readADCAvg(axisPin) - JOY_CENTRE;
  if (delta > -JOY_DEAD && delta < JOY_DEAD) return 0;

  int range = JOY_CENTRE - JOY_DEAD;   // usable counts per side = 1648
  int sign, magnitude;
  if (delta > 0) {
    sign = invert ? -1 : +1;  magnitude = delta - JOY_DEAD;
  } else {
    sign = invert ? +1 : -1;  magnitude = -delta - JOY_DEAD;
  }
  if (magnitude > range) magnitude = range;

  int32_t vel = ((int32_t)magnitude * maxSpdReal * SUBPIX) / range;
  return sign * vel;
}

// ─────────────────────────────────────────────────────────────────────────────
// readJoystick() — reads both axes with diagonal correction.
//
// WHY DIAGONAL FEELS SLOW NEAR CORNERS:
//   A physical thumb-stick has a circular or square gate.  At a pure diagonal
//   (45°) the stick physically can't reach the same radius as it can when
//   pushed straight up/left/right/down — the gate clips it at ~71% of max.
//   Both ADC values are therefore ~71% of their cardinal maximum, so both
//   proportional velocities come out at ~71% → the ship feels sluggish.
//
// FIX — Diagonal Boost:
//   When BOTH axes are outside the dead zone at the same time (diagonal move),
//   multiply both velocities by DIAG_BOOST/128 ≈ 1.35.
//   1/0.71 ≈ 1.41, so 1.35 gives a slight under-correction — enough to feel
//   natural without over-shoot at partial diagonals.
//   The result is then clamped to maxSpdReal*SUBPIX so a pure cardinal push
//   at full deflection is unchanged.
// ─────────────────────────────────────────────────────────────────────────────
void readJoystick(int maxSpdReal, int32_t &vx, int32_t &vy) {
  vx = readAxisSub(VRX, maxSpdReal, false);
  vy = readAxisSub(VRY, maxSpdReal, false);

  if (vx != 0 && vy != 0) {
    // Both axes active → apply diagonal boost
    int32_t cap = (int32_t)maxSpdReal * SUBPIX;
    vx = constrain((vx * DIAG_BOOST) / 128, -cap, cap);
    vy = constrain((vy * DIAG_BOOST) / 128, -cap, cap);
  }
}

// ── SPAWN / INIT ─────────────────────────────────────────────────────────────
void spawnRock() {
  for (uint8_t i = 0; i < MAX_ROCKS; i++) {
    if (!rocks[i].active) {
      rocks[i].x      = random(1, SCR_W - ROCK_W - 1);
      rocks[i].y      = HUD_H;
      rocks[i].spd    = 1 + score / 20;
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
  for (uint8_t i = 0; i < STAR_FAR_COUNT; i++) {
    farStarX[i] = random(SCR_W);
    farStarY[i] = random(HUD_H + 1, SCR_H) * SUBPIX;
  }
  for (uint8_t i = 0; i < STAR_NEAR_COUNT; i++) {
    nearStarX[i] = random(SCR_W);
    nearStarY[i] = random(HUD_H + 1, SCR_H) * SUBPIX;
  }
}

void scrollStars() {
  int16_t farBottom = (int16_t)SCR_H   * SUBPIX;
  int16_t hudTop    = (int16_t)(HUD_H+1) * SUBPIX;
  for (uint8_t i = 0; i < STAR_FAR_COUNT; i++) {
    farStarY[i] += STAR_FAR_SPD;
    if (farStarY[i] >= farBottom) { farStarY[i] = hudTop + random(8)*SUBPIX; farStarX[i] = random(SCR_W); }
  }
  for (uint8_t i = 0; i < STAR_NEAR_COUNT; i++) {
    nearStarY[i] += STAR_NEAR_SPD;
    if (nearStarY[i] >= farBottom) { nearStarY[i] = hudTop + random(4)*SUBPIX; nearStarX[i] = random(SCR_W); }
  }
}

void resetGame() {
  pxSub = (int32_t)((SCR_W - SHIP_W) / 2) * SUBPIX;
  pySub = (int32_t)(SCR_H - SHIP_H - 2)   * SUBPIX;
  score = 0;  lives = 3;
  gameOver = false;  flashLED = 0;
  spawnTimer = 0;  spawnInterval = 65;
  fireCooldown = 0;  boostFlicker = false;
  for (uint8_t i = 0; i < MAX_ROCKS;   i++) rocks[i].active   = false;
  for (uint8_t i = 0; i < MAX_BULLETS; i++) bullets[i].active = false;
  digitalWrite(LED, LOW);
}

// ── DRAW PRIMITIVES ──────────────────────────────────────────────────────────
void drawStars() {
  for (uint8_t i = 0; i < STAR_FAR_COUNT; i++) {
    int sy = (int)(farStarY[i] / SUBPIX), sx = farStarX[i];
    if (sy > HUD_H && sy < SCR_H) u8g2.drawPixel(sx, sy);
  }
  for (uint8_t i = 0; i < STAR_NEAR_COUNT; i++) {
    int sy = (int)(nearStarY[i] / SUBPIX), sx = nearStarX[i];
    if (sy > HUD_H && sy < SCR_H) {
      u8g2.drawPixel(sx, sy);
      if (sy + 1 < SCR_H) u8g2.drawPixel(sx, sy + 1);
    }
  }
}

void drawRocket(int x, int y, bool boost) {
  u8g2.drawBox(x+2, y+2, 3, 5);    // body
  u8g2.drawPixel(x+3, y);           // nose tip
  u8g2.drawPixel(x+2, y+1);
  u8g2.drawPixel(x+4, y+1);
  u8g2.drawBox(x,   y+5, 2, 3);    // left fin
  u8g2.drawBox(x+5, y+5, 2, 3);    // right fin
  if (boost && boostFlicker) {
    u8g2.drawPixel(x+2, y+8);
    u8g2.drawPixel(x+4, y+8);
    u8g2.drawPixel(x+3, y+9);
  } else {
    u8g2.drawPixel(x+3, y+8);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// drawAsteroid() — jagged polygon asteroid using drawLine.
// The outline is an irregular 8-point polygon that looks like a real rock.
// Points are relative to (x, y) top-left of the 11×11 bounding box:
//
//      (3,0)───(7,0)
//     /              \
//  (1,2)           (10,3)
//  |                    |
//  (0,6)           (10,7)
//     \               /
//      (2,9)──(7,10)
//       \      |
//        (1,10)
//
// (the interior is filled by a box, the outline adds the craggy silhouette)
// ─────────────────────────────────────────────────────────────────────────────
void drawAsteroid(int x, int y) {
  // Filled interior — quick solid body
  u8g2.drawBox(x+2, y+1, 7, 9);   // central filled mass
  u8g2.drawBox(x+1, y+3, 9, 5);   // wider between notches

  // Craggy outline pixels to break the rectangular silhouette
  // Top edge jaggies
  u8g2.drawPixel(x+1, y+2);
  u8g2.drawPixel(x+9, y+2);
  // Bottom edge jaggies
  u8g2.drawPixel(x,   y+5);
  u8g2.drawPixel(x+10,y+5);
  u8g2.drawPixel(x,   y+6);
  u8g2.drawPixel(x+10,y+6);
  // Notch cuts on corners (erase 2 corner pixels for irregular look)
  u8g2.setDrawColor(0);   // draw in BLACK to "cut" corners
  u8g2.drawPixel(x+2, y+1);       // top-left bevel
  u8g2.drawPixel(x+8, y+1);       // top-right bevel
  u8g2.drawPixel(x+3, y+9);       // bottom-left bevel
  u8g2.drawPixel(x+7, y+9);       // bottom-right notch
  u8g2.drawPixel(x+1, y+3);       // left upper notch
  u8g2.drawPixel(x+9, y+7);       // right lower notch
  u8g2.setDrawColor(1);   // restore WHITE
}

// ─────────────────────────────────────────────────────────────────────────────
// drawTitleScreen() — full title screen with pixel-art style large letters.
// Uses u8g2 large font for the title, small font for instructions.
// Drawn inside a firstPage/nextPage loop called from showTitleScreen().
// blink: controls whether the "PRESS JOYSTICK" line is visible this frame.
// ─────────────────────────────────────────────────────────────────────────────
void drawTitleScreen(bool blink) {
  drawStars();

  // ── Big decorative border ─────────────────────────────────────────────────
  u8g2.drawFrame(0, 0, SCR_W, SCR_H);       // outer border
  u8g2.drawHLine(2, 2,  SCR_W-4);           // top inner line
  u8g2.drawHLine(2, SCR_H-3, SCR_W-4);      // bottom inner line

  // ── Large title "ASTEROID" ────────────────────────────────────────────────
  u8g2.setFont(u8g2_font_7x14B_tr);         // bold 7-wide font
  u8g2.drawStr(10, 20, "ASTEROID");

  // ── Subtitle "DODGE" next line, slightly offset for visual rhythm ─────────
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(36, 34, "D O D G E");

  // ── Decorative divider ────────────────────────────────────────────────────
  u8g2.drawHLine(10, 38, SCR_W - 20);

  // ── Blinking prompt ───────────────────────────────────────────────────────
  u8g2.setFont(u8g2_font_5x7_tr);
  if (blink) {
    u8g2.drawStr(14, 50, "PRESS JOYSTICK TO START");
  }

  // ── Corner decorations (tiny pixel star clusters) ────────────────────────
  // Top-left
  u8g2.drawPixel(4, 5);  u8g2.drawPixel(6, 4);  u8g2.drawPixel(5, 6);
  // Top-right
  u8g2.drawPixel(SCR_W-5, 5); u8g2.drawPixel(SCR_W-7, 4); u8g2.drawPixel(SCR_W-6, 6);
  // Bottom-left
  u8g2.drawPixel(4, SCR_H-6); u8g2.drawPixel(6, SCR_H-5);
  // Bottom-right
  u8g2.drawPixel(SCR_W-5, SCR_H-6); u8g2.drawPixel(SCR_W-7, SCR_H-5);

  // ── Small rocket icon next to title ──────────────────────────────────────
  // tiny 5-pixel rocket silhouette in top-right area
  int rx = 95, ry = 8;
  u8g2.drawPixel(rx+1, ry);
  u8g2.drawBox(rx, ry+1, 3, 4);
  u8g2.drawPixel(rx-1, ry+3); u8g2.drawPixel(rx+3, ry+3);
  u8g2.drawPixel(rx+1, ry+5);

  // Restore small font for game use
  u8g2.setFont(u8g2_font_5x7_tr);
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

  onTitleScreen = true;
  prevSW        = HIGH;
  lastTick      = millis();
}

// ── MAIN LOOP ────────────────────────────────────────────────────────────────
void loop() {
  uint32_t now = millis();
  if (now - lastTick < TICK_MS) return;
  lastTick = now;

  // ── Read buttons (needed for title screen too) ───────────────────────────
  bool rawSW   = (digitalRead(SW)   == LOW);
  bool rawBTN1 = (digitalRead(BTN1) == LOW);
  bool rawBTN2 = (digitalRead(BTN2) == LOW);
  bool pressedSW   = (rawSW   && !prevSW);
  bool pressedBTN1 = (rawBTN1 && !prevBTN1);
  bool boost       = rawBTN2;

  prevSW   = rawSW;
  prevBTN1 = rawBTN1;
  prevBTN2 = rawBTN2;

  boostFlicker = !boostFlicker;
  scrollStars();

  // ── TITLE SCREEN ─────────────────────────────────────────────────────────
  if (onTitleScreen) {
    // Blink "PRESS JOYSTICK" every ~500 ms = every 10 frames at 20 FPS
    static uint8_t blinkCounter = 0;
    blinkCounter++;
    bool blink = (blinkCounter / 10) & 1;   // toggles every 10 frames

    u8g2.firstPage();
    do { drawTitleScreen(blink); } while (u8g2.nextPage());

    if (pressedSW) {
      onTitleScreen = false;
      resetGame();
      // Short ready-tone
      tone(BUZZER, 880, 80);
      delay(100);
      tone(BUZZER, 1100, 80);
    }
    return;
  }

  // ── GAME OVER SCREEN ─────────────────────────────────────────────────────
  if (gameOver) {
    u8g2.firstPage();
    do {
      drawStars();
      u8g2.drawFrame(10, 10, SCR_W-20, SCR_H-20);
      u8g2.drawStr(22, 26, "GAME  OVER");
      u8g2.setCursor(18, 38);
      u8g2.print("Score: "); u8g2.print(score);
      u8g2.drawStr(14, 52, "[SW] to restart");
    } while (u8g2.nextPage());

    if (pressedSW) { onTitleScreen = true; }   // go back to title
    return;
  }

  // ── READ JOYSTICK (with diagonal correction) ──────────────────────────────
  int  maxSpd = boost ? JOY_BOOST_SPD : JOY_MAX_SPD;
  int32_t vx, vy;
  readJoystick(maxSpd, vx, vy);

  // ── PLAYER MOVEMENT ───────────────────────────────────────────────────────
  pxSub += vx;
  pySub += vy;

  int32_t pxMin = 0,                          pxMax = (int32_t)(SCR_W-SHIP_W)*SUBPIX;
  int32_t pyMin = (int32_t)HUD_H * SUBPIX,   pyMax = (int32_t)(SCR_H-SHIP_H)*SUBPIX;
  if (pxSub < pxMin) pxSub = pxMin;
  if (pxSub > pxMax) pxSub = pxMax;
  if (pySub < pyMin) pySub = pyMin;
  if (pySub > pyMax) pySub = pyMax;

  int px = (int)(pxSub / SUBPIX);
  int py = (int)(pySub / SUBPIX);

  // ── FIRE ─────────────────────────────────────────────────────────────────
  if (fireCooldown > 0) fireCooldown--;
  if (pressedBTN1 && fireCooldown == 0) { fireBullet(); fireCooldown = 6; }

  // ── UPDATE BULLETS ────────────────────────────────────────────────────────
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
        digitalWrite(LED, HIGH); delay(30); digitalWrite(LED, LOW);
        break;
      }
    }
  }

  // ── ASTEROIDS ─────────────────────────────────────────────────────────────
  for (uint8_t i = 0; i < MAX_ROCKS; i++) {
    if (!rocks[i].active) continue;
    rocks[i].y += rocks[i].spd;

    if (rocks[i].y > SCR_H) {
      rocks[i].active = false;
      score++;
      tone(BUZZER, 1000, 30);
      if (score % 5 == 0 && spawnInterval > 25) spawnInterval -= 2;
      continue;
    }

    if (hits(px, py, SHIP_W, SHIP_H, rocks[i].x, rocks[i].y, ROCK_W, ROCK_H)) {
      rocks[i].active = false;
      lives--;
      tone(BUZZER, 220, 250);
      if (lives == 0) { gameOver = true; tone(BUZZER, 150, 700); }
    }
  }

  // ── SPAWN ─────────────────────────────────────────────────────────────────
  if (++spawnTimer >= spawnInterval) { spawnTimer = 0; spawnRock(); }

  // ── DRAW ──────────────────────────────────────────────────────────────────
  u8g2.firstPage();
  do {
    drawStars();

    // HUD
    u8g2.drawHLine(0, HUD_H-1, SCR_W);
    u8g2.setCursor(0, 7);  u8g2.print(score);
    for (uint8_t i = 0; i < lives; i++)
      u8g2.drawBox(100 + i*9, 1, 6, 6);

    // Bullets
    for (uint8_t i = 0; i < MAX_BULLETS; i++)
      if (bullets[i].active)
        u8g2.drawBox(bullets[i].x, bullets[i].y, BULLET_W, BULLET_H);

    // Asteroids
    for (uint8_t i = 0; i < MAX_ROCKS; i++)
      if (rocks[i].active) drawAsteroid(rocks[i].x, rocks[i].y);

    // Rocket
    drawRocket(px, py, boost);

  } while (u8g2.nextPage());
}