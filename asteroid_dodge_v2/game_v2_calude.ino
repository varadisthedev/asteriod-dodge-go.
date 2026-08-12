#include <Wire.h>
#include <U8g2lib.h>

// ── DISPLAY: page-buffer + HW_I2C = fastest possible ──────────────────
// If your SH1106 is on GPIO21/22 (ESP32 default HW I2C), use this:
U8G2_SH1106_128X64_NONAME_1_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
// If HW I2C doesn't work on your board, fall back to SW page-mode:
// U8G2_SH1106_128X64_NONAME_1_SW_I2C u8g2(U8G2_R0,22,21,U8X8_PIN_NONE);

// ── PINS ───────────────────────────────────────────────────────────────
#define VRX     34
#define VRY     35
#define SW      25
#define BTN1    26
#define BTN2    27
#define LED     18
#define BUZZER  19

// ── TUNING ─────────────────────────────────────────────────────────────
#define TICK_MS      30    // ~33 FPS
#define MAX_ROCKS    4
#define JOY_DEAD     400   // joystick dead zone (centred at 2048)
#define PLAYER_SPD   4
#define BOOST_SPD    7
#define ROCK_W       7
#define ROCK_H       7
#define SHIP_W       7
#define SHIP_H       9
#define SCR_W       128
#define SCR_H        64
#define HUD_H        9

// ── STATE ──────────────────────────────────────────────────────────────
struct Rock {
  int8_t  x;
  int8_t  y;
  int8_t  spd;
  bool    active;
};

Rock   rocks[MAX_ROCKS];
int16_t px, py;         // player position (int, not float — faster)
uint16_t score;
uint8_t  lives;
bool     gameOver;
bool     lastSW;
uint8_t  flashLED;
uint8_t  spawnTimer;
uint8_t  spawnInterval;
uint32_t lastTick;

// stars (static bg — computed once, never randomised in loop)
const uint8_t STAR_COUNT = 12;
uint8_t starX[STAR_COUNT];
uint8_t starY[STAR_COUNT];

// ── HELPERS ────────────────────────────────────────────────────────────
inline bool hits(int ax,int ay,int aw,int ah,
                  int bx,int by,int bw,int bh) {
  return ax<bx+bw && ax+aw>bx && ay<by+bh && ay+ah>by;
}

void spawnRock() {
  for (uint8_t i=0;i<MAX_ROCKS;i++) {
    if (!rocks[i].active) {
      rocks[i].x      = random(1, SCR_W - ROCK_W - 1);
      rocks[i].y      = HUD_H;
      rocks[i].spd    = 2 + score/8;           // faster as score grows
      if (rocks[i].spd > 7) rocks[i].spd = 7;   // cap speed
      rocks[i].active = true;
      return;
    }
  }
}

void resetGame() {
  px = (SCR_W - SHIP_W) / 2;
  py = SCR_H - SHIP_H - 2;
  score = 0; lives = 3;
  gameOver = false;
  flashLED = 0; spawnTimer = 0;
  spawnInterval = 50;
  for (uint8_t i=0;i<MAX_ROCKS;i++) rocks[i].active=false;
  digitalWrite(LED, LOW);
}

// Draw rocket: nose up, two side fins
void drawRocket(int x, int y, bool boost) {
  // body
  u8g2.drawBox(x+2, y+2, 3, 5);
  // nose cone (two pixels)
  u8g2.drawPixel(x+3, y);
  u8g2.drawPixel(x+2, y+1);
  u8g2.drawPixel(x+4, y+1);
  // left fin
  u8g2.drawBox(x, y+5, 2, 3);
  // right fin
  u8g2.drawBox(x+5, y+5, 2, 3);
  // exhaust flame (only when boosting or alternates each frame)
  if (boost) {
    u8g2.drawPixel(x+2, y+8);
    u8g2.drawPixel(x+4, y+8);
    u8g2.drawPixel(x+3, y+9);
  } else {
    u8g2.drawPixel(x+3, y+8);
  }
}

// Draw asteroid: irregular box cluster (no curves = fast)
void drawAsteroid(int x, int y) {
  u8g2.drawBox(x+1, y,   5, 2);
  u8g2.drawBox(x,   y+2, 7, 3);
  u8g2.drawBox(x+1, y+5, 5, 2);
}

// ── SETUP ──────────────────────────────────────────────────────────────
void setup() {
  randomSeed(analogRead(36));  // floating pin for entropy

  pinMode(SW,    INPUT_PULLUP);
  pinMode(BTN1,  INPUT_PULLUP);
  pinMode(BTN2,  INPUT_PULLUP);
  pinMode(LED,   OUTPUT);
  pinMode(BUZZER,OUTPUT);

  // HW I2C on ESP32 defaults to GPIO21(SDA) GPIO22(SCL)
  Wire.begin();
  Wire.setClock(400000);  // 400kHz fast mode — cuts display time in half
  u8g2.begin();
  u8g2.setFont(u8g2_font_5x7_tr);  // set once, never change in loop

  // generate static star field once
  for (uint8_t i=0;i<STAR_COUNT;i++) {
    starX[i] = random(SCR_W);
    starY[i] = random(HUD_H+1, SCR_H);
  }

  resetGame();
  lastSW = true;

  // splash
  u8g2.firstPage();
  do {
    u8g2.drawStr(18,28,"ASTEROID DODGE");
    u8g2.drawStr(10,44,"Tilt to dodge  BTN2=boost");
  } while (u8g2.nextPage());
  delay(1800);
}

// ── MAIN LOOP ──────────────────────────────────────────────────────────
void loop() {

  // strict timing — exit early if tick not reached
  uint32_t now = millis();
  if (now - lastTick < TICK_MS) return;
  lastTick = now;

  // read all inputs ONCE at top of frame
  int  xRaw  = analogRead(VRX);
  int  yRaw  = analogRead(VRY);
  bool boost = (digitalRead(BTN2) == LOW);
  bool sw    = (digitalRead(SW)   == LOW);

  // ── GAME OVER SCREEN ──────────────────────────────────────────────
  if (gameOver) {
    u8g2.firstPage();
    do {
      u8g2.drawStr(22,20,"GAME OVER");
      u8g2.setCursor(22,34);
      u8g2.print("Score: "); u8g2.print(score);
      u8g2.drawStr(16,50,"[SW] to restart");
    } while (u8g2.nextPage());

    if (sw && !lastSW) resetGame();
    lastSW = sw;
    return;
  }

  // ── PLAYER MOVEMENT ───────────────────────────────────────────────
  int spd = boost ? BOOST_SPD : PLAYER_SPD;
  int dx = xRaw - 2048;
  int dy = yRaw - 2048;

  if (dx < -JOY_DEAD) px -= spd;
  else if (dx >  JOY_DEAD) px += spd;
  if (dy < -JOY_DEAD) py -= spd;
  else if (dy >  JOY_DEAD) py += spd;

  if (px < 0)              px = 0;
  if (px > SCR_W - SHIP_W) px = SCR_W - SHIP_W;
  if (py < HUD_H)          py = HUD_H;
  if (py > SCR_H - SHIP_H) py = SCR_H - SHIP_H;

  // ── ASTEROIDS ─────────────────────────────────────────────────────
  for (uint8_t i=0;i<MAX_ROCKS;i++) {
    if (!rocks[i].active) continue;
    rocks[i].y += rocks[i].spd;

    if (rocks[i].y > SCR_H) {        // dodged — score!
      rocks[i].active = false;
      score++;
      tone(BUZZER, 1000, 30);
      if (score % 5 == 0 && spawnInterval > 18)
        spawnInterval -= 4;           // ramp up spawn rate
      continue;
    }

    if (hits(px,py,SHIP_W,SHIP_H, rocks[i].x,rocks[i].y,ROCK_W,ROCK_H)) {
      rocks[i].active = false;
      lives--;
      flashLED = 6;
      tone(BUZZER, 220, 250);
      if (lives == 0) {
        gameOver = true;
        digitalWrite(LED, HIGH);
        tone(BUZZER, 150, 700);
      }
    }
  }

  // ── SPAWN ─────────────────────────────────────────────────────────
  if (++spawnTimer >= spawnInterval) {
    spawnTimer = 0;
    spawnRock();
  }

  // ── LED FLASH ─────────────────────────────────────────────────────
  if (flashLED) {
    digitalWrite(LED, flashLED-- & 1 ? HIGH : LOW);
  }

  // ── DRAW (page-buffer mode) ────────────────────────────────────────
  u8g2.firstPage();
  do {

    // stars (static positions, no recompute)
    for (uint8_t i=0;i<STAR_COUNT;i++)
      u8g2.drawPixel(starX[i], starY[i]);

    // HUD line
    u8g2.drawHLine(0, HUD_H-1, SCR_W);
    u8g2.setCursor(0,7);  u8g2.print(score);
    // lives as small filled squares
    for (uint8_t i=0;i<lives;i++)
      u8g2.drawBox(100+i*9, 1, 6, 6);

    // asteroids
    for (uint8_t i=0;i<MAX_ROCKS;i++)
      if (rocks[i].active) drawAsteroid(rocks[i].x, rocks[i].y);

    // rocket
    drawRocket(px, py, boost);

  } while (u8g2.nextPage());
}