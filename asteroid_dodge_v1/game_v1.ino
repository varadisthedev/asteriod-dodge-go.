#include <Wire.h>
#include <U8g2lib.h>

// ================= OLED (YOUR EXACT CONFIG) =================
U8G2_SH1106_128X64_NONAME_F_SW_I2C u8g2(
  U8G2_R0,
  /* clock=*/ 22,
  /* data=*/ 21,
  U8X8_PIN_NONE
);

// ================= PINS =================
#define VRX 34
#define VRY 35
#define SW 25
#define BTN1 26
#define BTN2 27
#define LED 18
#define BUZZER 19

// ================= GAME =================
int playerX = 60;
int obstacleX = random(0, 120);
int obstacleY = 0;

int score = 0;
bool gameOver = false;

unsigned long lastUpdate = 0;

void setup() {
  Serial.begin(115200);
  

  pinMode(SW, INPUT_PULLUP);
  pinMode(BTN1, INPUT_PULLUP);
  pinMode(BTN2, INPUT_PULLUP);
  pinMode(LED, OUTPUT);
  pinMode(BUZZER, OUTPUT);

  Serial.println("Booting...");



u8g2.begin();
Serial.println("U8G2 begun");



  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(10, 30, "GAME START");
  u8g2.sendBuffer();
  delay(1000);
}

void loop() {

  if (gameOver) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_7x13B_tr);
    u8g2.drawStr(15, 25, "GAME OVER");

    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.setCursor(30, 45);
    u8g2.print("Score: ");
    u8g2.print(score);

    u8g2.sendBuffer();

    digitalWrite(BUZZER, HIGH);
    delay(200);
    digitalWrite(BUZZER, LOW);

    if (digitalRead(SW) == LOW) {
      resetGame();
    }
    return;
  }

  if (millis() - lastUpdate > 50) {
    lastUpdate = millis();

    // 🎮 joystick
    int xVal = analogRead(VRX);

    if (xVal < 1500) playerX -= 3;
    if (xVal > 2500) playerX += 3;

    if (playerX < 0) playerX = 0;
    if (playerX > 120) playerX = 120;

    // obstacle
    obstacleY += 4;

    if (obstacleY > 64) {
      obstacleY = 0;
      obstacleX = random(0, 120);
      score++;
    }

    // collision
    if (abs(playerX - obstacleX) < 8 && obstacleY > 50) {
      gameOver = true;
      digitalWrite(LED, HIGH);
    }

    // 🖥️ DRAW (U8g2 style)
    u8g2.clearBuffer();

    // player
    u8g2.drawBox(playerX, 56, 8, 8);

    // obstacle
    u8g2.drawBox(obstacleX, obstacleY, 6, 6);

    // score
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.setCursor(0, 10);
    u8g2.print("Score: ");
    u8g2.print(score);

    u8g2.sendBuffer();
  }
}

void resetGame() {
  playerX = 60;
  obstacleY = 0;
  obstacleX = random(0, 120);
  score = 0;
  gameOver = false;
  digitalWrite(LED, LOW);
}