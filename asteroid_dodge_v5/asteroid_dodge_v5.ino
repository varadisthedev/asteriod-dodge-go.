#include <SPI.h>

#define SD_SCK  18
#define SD_MISO 19
#define SD_MOSI 23
#define SD_CS   5

SPIClass spi(VSPI);

uint8_t sendCmd(uint8_t cmd, uint32_t arg, uint8_t crc) {
  digitalWrite(SD_CS, LOW);

  spi.transfer(0x40 | cmd);
  spi.transfer((arg >> 24) & 0xFF);
  spi.transfer((arg >> 16) & 0xFF);
  spi.transfer((arg >> 8) & 0xFF);
  spi.transfer(arg & 0xFF);
  spi.transfer(crc);

  uint8_t response = 0xFF;
  for (int i = 0; i < 8; i++) {
    response = spi.transfer(0xFF);
    if (response != 0xFF) break;
  }

  digitalWrite(SD_CS, HIGH);
  spi.transfer(0xFF); // extra clock after deselect
  return response;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- Raw SPI SD Diagnostic ---");

  pinMode(SD_CS, OUTPUT);

  digitalWrite(SD_CS, HIGH);

  spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
pinMode(SD_MISO, INPUT_PULLUP);   // now placed AFTER spi.begin(), so it actually sticks
spi.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  // Send 80 dummy clocks with CS high to let card power up
  digitalWrite(SD_CS, HIGH);
  for (int i = 0; i < 10; i++) {
    spi.transfer(0xFF);
  }

  Serial.println("Sending CMD0 (GO_IDLE_STATE)...");
  uint8_t r1 = 0xFF;
  for (int attempt = 0; attempt < 5; attempt++) {
    r1 = sendCmd(0, 0, 0x95); // valid fixed CRC for CMD0
    Serial.printf("Attempt %d: R1 response = 0x%02X\n", attempt + 1, r1);
    if (r1 == 0x01) break;
    delay(100);
  }

  spi.endTransaction();

  Serial.println("---------------------------");
  if (r1 == 0x01) {
    Serial.println("SUCCESS: Card responded correctly (0x01 = idle state).");
    Serial.println("-> Wiring/electrical path is fine. Problem is likely in SD library layer.");
  } else if (r1 == 0xFF) {
    Serial.println("FAIL: No response at all (0xFF = MISO stuck high / no data).");
    Serial.println("-> Check MISO wiring, card seating, or card is dead.");
  } else {
    Serial.println("PARTIAL: Got a response, but not the expected 0x01.");
    Serial.printf("-> Got 0x%02X instead. Check CS timing, CRC, or clock polarity.\n", r1);
  }
}

void loop() {}