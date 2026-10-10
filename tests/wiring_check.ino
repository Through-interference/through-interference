/*
  Проверка проводов ESP32 - SX1262.
  Передатчик не включается: только сброс, SPI и запись регистра.

  VCC модуля -> VIN (5 В), GND -> GND
  SCK 18, MISO 19, MOSI 23, NSS 5
  NRST 14, BUSY 26, DIO1 33, RXEN 32, TXEN 25

  Плата: ESP32 Dev Module. Монитор порта: 115200.
*/

#include <SPI.h>

const int PIN_SCK = 18;
const int PIN_MISO = 19;
const int PIN_MOSI = 23;
const int PIN_NSS = 5;
const int PIN_RST = 14;
const int PIN_BUSY = 26;
const int PIN_DIO1 = 33;
const int PIN_RXEN = 32;
const int PIN_TXEN = 25;

const uint8_t CMD_SET_STANDBY = 0x80;
const uint8_t CMD_SET_PACKET_TYPE = 0x8A;
const uint8_t CMD_WRITE_REGISTER = 0x0D;
const uint8_t CMD_READ_REGISTER = 0x1D;
const uint8_t CMD_GET_STATUS = 0xC0;
const uint16_t REG_SYNC_MSB = 0x0740;

bool spiOk = false;
bool busyOk = false;

void rfSwitchOff() {
  digitalWrite(PIN_TXEN, LOW);
  digitalWrite(PIN_RXEN, LOW);
}

bool waitBusy(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (digitalRead(PIN_BUSY) == HIGH) {
    if (millis() - start > timeoutMs) {
      return false;
    }
  }
  return true;
}

void resetChip() {
  digitalWrite(PIN_RST, LOW);
  delay(2);
  digitalWrite(PIN_RST, HIGH);
}

void pauseForBusy() {
  waitBusy(20);
}

bool statusLooksReal(uint8_t status) {
  if (status == 0x00 || status == 0xFF) {
    return false;
  }
  uint8_t mode = (status >> 4) & 0x07;
  return mode >= 2 && mode <= 6;
}

uint8_t getStatus() {
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW);
  uint8_t first = SPI.transfer(CMD_GET_STATUS);
  uint8_t second = SPI.transfer(0x00);
  digitalWrite(PIN_NSS, HIGH);
  SPI.endTransaction();
  if (statusLooksReal(second) && !statusLooksReal(first)) {
    return second;
  }
  return first;
}

bool command(uint8_t opcode, uint8_t value) {
  pauseForBusy();
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW);
  SPI.transfer(opcode);
  SPI.transfer(value);
  digitalWrite(PIN_NSS, HIGH);
  SPI.endTransaction();
  return waitBusy(20);
}

uint8_t readRegister(uint16_t addr) {
  uint8_t value = 0x00;
  pauseForBusy();
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW);
  SPI.transfer(CMD_READ_REGISTER);
  SPI.transfer(addr >> 8);
  SPI.transfer(addr & 0xFF);
  SPI.transfer(0x00);
  value = SPI.transfer(0x00);
  digitalWrite(PIN_NSS, HIGH);
  SPI.endTransaction();
  waitBusy(20);
  return value;
}

void writeRegister(uint16_t addr, uint8_t value) {
  pauseForBusy();
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW);
  SPI.transfer(CMD_WRITE_REGISTER);
  SPI.transfer(addr >> 8);
  SPI.transfer(addr & 0xFF);
  SPI.transfer(value);
  digitalWrite(PIN_NSS, HIGH);
  SPI.endTransaction();
  waitBusy(20);
}

const char* chipModeName(uint8_t status) {
  switch ((status >> 4) & 0x07) {
    case 2: return "STDBY_RC";
    case 3: return "STDBY_XOSC";
    case 4: return "FS";
    case 5: return "RX";
    case 6: return "TX";
    default: return "unknown";
  }
}

uint8_t readSx1276Version() {
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW);
  SPI.transfer(0x42);
  uint8_t value = SPI.transfer(0x00);
  digitalWrite(PIN_NSS, HIGH);
  SPI.endTransaction();
  return value;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_NSS, OUTPUT);
  pinMode(PIN_RST, OUTPUT);
  pinMode(PIN_BUSY, INPUT);
  pinMode(PIN_DIO1, INPUT);
  pinMode(PIN_RXEN, OUTPUT);
  pinMode(PIN_TXEN, OUTPUT);
  digitalWrite(PIN_NSS, HIGH);
  digitalWrite(PIN_RST, HIGH);
  rfSwitchOff();

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);

  Serial.println();
  Serial.println("=== ESP32 - SX1262 wiring check ===");
  Serial.println("No transmit. Module VCC must be on VIN.");
  Serial.println();

  Serial.println("[1] Reset, BUSY pin");
  resetChip();
  uint32_t start = millis();
  busyOk = waitBusy(100);
  if (busyOk) {
    Serial.print("    BUSY went low in ");
    Serial.print(millis() - start);
    Serial.println(" ms. OK");
  } else if (digitalRead(PIN_BUSY) == HIGH) {
    Serial.println("    BUSY stays HIGH. Check VIN, GND, NRST GPIO14, BUSY GPIO26.");
  } else {
    Serial.println("    BUSY is already low.");
  }

  Serial.println("[2] SPI answer");
  command(CMD_SET_STANDBY, 0x00);
  uint8_t status = getStatus();
  Serial.print("    GetStatus = 0x");
  Serial.print(status, HEX);
  Serial.print(", mode ");
  Serial.println(chipModeName(status));
  spiOk = statusLooksReal(status);
  if (spiOk) {
    Serial.println("    Chip answered. SCK, MISO, MOSI, NSS and power are OK.");
  } else {
    Serial.println("    No answer. Check SCK 18, MISO 19, MOSI 23, NSS 5, GND, VIN.");
    uint8_t version = readSx1276Version();
    if (version == 0x12) {
      Serial.println("    Reg 0x42 = 0x12: this is RFM95/SX1276, not SX1262.");
    }
  }

  Serial.println("[3] Register write and read");
  bool memoryOk = false;
  if (spiOk) {
    command(CMD_SET_PACKET_TYPE, 0x01);
    uint8_t original = readRegister(REG_SYNC_MSB);
    Serial.print("    0x0740 was 0x");
    Serial.println(original, HEX);

    writeRegister(REG_SYNC_MSB, 0x5A);
    bool first = readRegister(REG_SYNC_MSB) == 0x5A;
    writeRegister(REG_SYNC_MSB, 0xA5);
    bool second = readRegister(REG_SYNC_MSB) == 0xA5;
    writeRegister(REG_SYNC_MSB, original);
    uint8_t restored = readRegister(REG_SYNC_MSB);

    Serial.println(first ? "    Write 0x5A read back. OK" : "    Write 0x5A mismatch");
    Serial.println(second ? "    Write 0xA5 read back. OK" : "    Write 0xA5 mismatch");
    Serial.print("    Restored 0x");
    Serial.print(original, HEX);
    Serial.print(", now 0x");
    Serial.println(restored, HEX);
    memoryOk = first && second && restored == original;
    if (!memoryOk) {
      Serial.println("    Write did not stick. MOSI and MISO are often swapped.");
    }
  } else {
    Serial.println("    Skipped: SPI is not answering.");
  }

  Serial.println();
  if (spiOk && memoryOk && busyOk) {
    Serial.println("Result: power, SPI, NRST and BUSY are wired correctly.");
  } else if (spiOk && memoryOk) {
    Serial.println("Result: SPI works. Check BUSY GPIO26 and NRST GPIO14.");
  } else {
    Serial.println("Result: module did not answer. Recheck the wires in the header comment.");
  }
  Serial.println("DIO1 GPIO33 is not tested.");
  Serial.println("RXEN and TXEN are held low. The amplifier is off.");
}

void loop() {
}
