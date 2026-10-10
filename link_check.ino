/*
  Проверка связи между двумя ESP32 + SX1262.
  Одна и та же прошивка на обе платы.
  Антенны разнести на 3 метра. Мощность передачи низкая.

  VCC -> VIN, GND -> GND
  SCK 18, MISO 19, MOSI 23, NSS 5
  NRST 14, BUSY 26, DIO1 33, RXEN 32, TXEN 25

  Плата: ESP32 Dev Module. Upload Speed: 115200. Монитор: 115200.
*/

#include <SPI.h>
#include <esp_system.h>

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
const uint8_t CMD_SET_TX = 0x83;
const uint8_t CMD_SET_RX = 0x82;
const uint8_t CMD_SET_PACKET_TYPE = 0x8A;
const uint8_t CMD_SET_RF_FREQUENCY = 0x86;
const uint8_t CMD_SET_TX_PARAMS = 0x8E;
const uint8_t CMD_SET_PA_CONFIG = 0x95;
const uint8_t CMD_SET_MODULATION = 0x8B;
const uint8_t CMD_SET_PACKET = 0x8C;
const uint8_t CMD_SET_BUFFER_BASE = 0x8F;
const uint8_t CMD_SET_DIO_IRQ = 0x08;
const uint8_t CMD_GET_IRQ = 0x12;
const uint8_t CMD_CLEAR_IRQ = 0x02;
const uint8_t CMD_WRITE_BUFFER = 0x0E;
const uint8_t CMD_READ_BUFFER = 0x1E;
const uint8_t CMD_GET_RX_BUFFER = 0x13;
const uint8_t CMD_GET_PACKET_STATUS = 0x14;
const uint8_t CMD_SET_TCXO = 0x97;
const uint8_t CMD_CALIBRATE = 0x89;
const uint8_t CMD_CALIBRATE_IMAGE = 0x98;
const uint8_t CMD_SET_REGULATOR = 0x96;
const uint8_t CMD_SET_FALLBACK = 0x93;
const uint8_t CMD_WRITE_REGISTER = 0x0D;
const uint8_t CMD_GET_STATUS = 0xC0;

const uint16_t IRQ_TX_DONE = 0x0001;
const uint16_t IRQ_RX_DONE = 0x0002;
const uint16_t IRQ_CRC_ERR = 0x0040;
const uint16_t IRQ_TIMEOUT = 0x0200;

const int8_t TX_POWER_DBM = 10;

uint16_t myId = 0;
uint16_t txNumber = 0;
uint32_t txCount = 0;
uint32_t rxCount = 0;
uint32_t nextTx = 0;
bool radioReady = false;
bool tcxoOn = false;

void rfIdle() {
  digitalWrite(PIN_TXEN, LOW);
  digitalWrite(PIN_RXEN, LOW);
}

void rfTx() {
  digitalWrite(PIN_RXEN, LOW);
  digitalWrite(PIN_TXEN, HIGH);
}

void rfRx() {
  digitalWrite(PIN_TXEN, LOW);
  digitalWrite(PIN_RXEN, HIGH);
}

bool waitReady(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (digitalRead(PIN_BUSY) == HIGH) {
    if (millis() - start > timeoutMs) {
      return false;
    }
  }
  return true;
}

void radioCommand(uint8_t opcode, uint8_t *data, uint8_t len) {
  waitReady(30);
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW);
  SPI.transfer(opcode);
  for (uint8_t i = 0; i < len; i++) {
    SPI.transfer(data[i]);
  }
  digitalWrite(PIN_NSS, HIGH);
  SPI.endTransaction();
  waitReady(30);
}

void radioCommand1(uint8_t opcode, uint8_t value) {
  radioCommand(opcode, &value, 1);
}

void radioRead(uint8_t opcode, uint8_t *out, uint8_t len) {
  waitReady(30);
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW);
  SPI.transfer(opcode);
  // На этом модуле первый байт ответа повторяет статус, данные идут следом.
  SPI.transfer(0x00);
  for (uint8_t i = 0; i < len; i++) {
    out[i] = SPI.transfer(0x00);
  }
  digitalWrite(PIN_NSS, HIGH);
  SPI.endTransaction();
}

uint8_t getStatus() {
  waitReady(30);
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW);
  uint8_t first = SPI.transfer(CMD_GET_STATUS);
  uint8_t second = SPI.transfer(0x00);
  digitalWrite(PIN_NSS, HIGH);
  SPI.endTransaction();
  uint8_t secondMode = (second >> 4) & 0x07;
  bool secondOk = second != 0x00 && second != 0xFF && secondMode >= 2 && secondMode <= 6;
  uint8_t firstMode = (first >> 4) & 0x07;
  bool firstOk = first != 0x00 && first != 0xFF && firstMode >= 2 && firstMode <= 6;
  if (secondOk && !firstOk) {
    return second;
  }
  return first;
}

bool statusLooksReal(uint8_t status) {
  if (status == 0x00 || status == 0xFF) {
    return false;
  }
  uint8_t mode = (status >> 4) & 0x07;
  return mode >= 2 && mode <= 6;
}

void writeRegister(uint16_t addr, uint8_t value) {
  uint8_t data[3];
  data[0] = addr >> 8;
  data[1] = addr & 0xFF;
  data[2] = value;
  radioCommand(CMD_WRITE_REGISTER, data, 3);
}

void clearIrq() {
  uint8_t mask[2];
  mask[0] = 0xFF;
  mask[1] = 0xFF;
  radioCommand(CMD_CLEAR_IRQ, mask, 2);
}

uint16_t getIrq() {
  uint8_t raw[2];
  radioRead(CMD_GET_IRQ, raw, 2);
  return ((uint16_t)raw[0] << 8) | raw[1];
}

void resetChip() {
  digitalWrite(PIN_RST, LOW);
  delay(2);
  digitalWrite(PIN_RST, HIGH);
  waitReady(100);
}

bool enableTcxo(uint8_t voltage) {
  uint8_t data[4];
  data[0] = voltage;
  data[1] = 0x00;
  data[2] = 0x02;
  data[3] = 0x80;
  waitReady(50);
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW);
  SPI.transfer(CMD_SET_TCXO);
  for (uint8_t i = 0; i < 4; i++) {
    SPI.transfer(data[i]);
  }
  digitalWrite(PIN_NSS, HIGH);
  SPI.endTransaction();
  return waitReady(150);
}

void configureRadio() {
  uint8_t image[2];
  image[0] = 0xD7;
  image[1] = 0xDB;
  radioCommand(CMD_CALIBRATE_IMAGE, image, 2);

  radioCommand1(CMD_SET_REGULATOR, tcxoOn ? 0x01 : 0x00);
  radioCommand1(CMD_SET_PACKET_TYPE, 0x01);

  uint8_t freq[4];
  freq[0] = 0x36;
  freq[1] = 0x4C;
  freq[2] = 0xCC;
  freq[3] = 0xCC;
  radioCommand(CMD_SET_RF_FREQUENCY, freq, 4);

  uint8_t pa[4];
  pa[0] = 0x02;
  pa[1] = 0x02;
  pa[2] = 0x00;
  pa[3] = 0x01;
  radioCommand(CMD_SET_PA_CONFIG, pa, 4);
  radioCommand1(0x9D, 0x00);

  uint8_t tx[2];
  tx[0] = (uint8_t)TX_POWER_DBM;
  tx[1] = 0x04;
  radioCommand(CMD_SET_TX_PARAMS, tx, 2);

  uint8_t mod[4];
  mod[0] = 0x07;
  mod[1] = 0x04;
  mod[2] = 0x01;
  mod[3] = 0x00;
  radioCommand(CMD_SET_MODULATION, mod, 4);

  uint8_t packet[6];
  packet[0] = 0x00;
  packet[1] = 0x08;
  packet[2] = 0x00;
  packet[3] = 4;
  packet[4] = 0x01;
  packet[5] = 0x00;
  radioCommand(CMD_SET_PACKET, packet, 6);

  uint8_t base[2];
  base[0] = 0x00;
  base[1] = 0x00;
  radioCommand(CMD_SET_BUFFER_BASE, base, 2);

  uint8_t irq[8];
  irq[0] = 0x02;
  irq[1] = 0x43;
  irq[2] = 0x02;
  irq[3] = 0x43;
  irq[4] = 0x00;
  irq[5] = 0x00;
  irq[6] = 0x00;
  irq[7] = 0x00;
  radioCommand(CMD_SET_DIO_IRQ, irq, 8);

  radioCommand1(CMD_SET_FALLBACK, tcxoOn ? 0x30 : 0x20);
  writeRegister(0x0740, 0x14);
  writeRegister(0x0741, 0x24);
  clearIrq();
}

bool radioBegin(uint8_t tcxoVoltage) {
  resetChip();
  radioCommand1(CMD_SET_STANDBY, 0x00);
  radioCommand1(CMD_CALIBRATE, 0x7F);
  waitReady(200);
  tcxoOn = false;
  if (tcxoVoltage != 0) {
    tcxoOn = enableTcxo(tcxoVoltage);
    if (!tcxoOn) {
      return false;
    }
  }
  configureRadio();
  return statusLooksReal(getStatus());
}

void clearErrors() {
  waitReady(50);
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW);
  SPI.transfer(0x07);
  digitalWrite(PIN_NSS, HIGH);
  SPI.endTransaction();
  waitReady(30);
}

uint16_t readErrors() {
  uint8_t raw[2];
  radioRead(0x17, raw, 2);
  return ((uint16_t)raw[0] << 8) | raw[1];
}

void startReceive() {
  rfRx();
  clearIrq();
  uint8_t timeout[3];
  timeout[0] = 0xFF;
  timeout[1] = 0xFF;
  timeout[2] = 0xFF;
  radioCommand(CMD_SET_RX, timeout, 3);
}

bool sendPacket(bool diagnostic) {
  uint8_t data[4];
  data[0] = myId >> 8;
  data[1] = myId & 0xFF;
  data[2] = txNumber >> 8;
  data[3] = txNumber & 0xFF;

  clearErrors();
  radioCommand1(CMD_SET_STANDBY, tcxoOn ? 0x01 : 0x00);
  if (!waitReady(200)) {
    if (diagnostic) {
      Serial.println("  oscillator did not start");
    }
    rfIdle();
    return false;
  }

  rfTx();
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW);
  SPI.transfer(CMD_WRITE_BUFFER);
  SPI.transfer(0x00);
  for (uint8_t i = 0; i < 4; i++) {
    SPI.transfer(data[i]);
  }
  digitalWrite(PIN_NSS, HIGH);
  SPI.endTransaction();
  waitReady(50);

  clearIrq();
  uint8_t timeout[3];
  timeout[0] = 0x00;
  timeout[1] = 0x4B;
  timeout[2] = 0x00;
  waitReady(50);
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW);
  SPI.transfer(CMD_SET_TX);
  SPI.transfer(timeout[0]);
  SPI.transfer(timeout[1]);
  SPI.transfer(timeout[2]);
  digitalWrite(PIN_NSS, HIGH);
  SPI.endTransaction();

  uint32_t start = millis();
  bool done = false;
  while (millis() - start < 1000) {
    if (digitalRead(PIN_BUSY) == HIGH) {
      delay(2);
      continue;
    }
    uint16_t irq = getIrq();
    if (irq & IRQ_TX_DONE) {
      done = true;
      break;
    }
    if ((irq & IRQ_TIMEOUT) && (millis() - start > 80)) {
      break;
    }
    delay(2);
  }

  if (!done && diagnostic) {
    uint16_t irq = getIrq();
    uint16_t errors = readErrors();
    Serial.print("  busy=");
    Serial.print(digitalRead(PIN_BUSY));
    Serial.print(" status=0x");
    Serial.print(getStatus(), HEX);
    Serial.print(" irq=0x");
    Serial.print(irq, HEX);
    Serial.print(" err=0x");
    Serial.println(errors, HEX);
  }

  clearIrq();
  rfIdle();
  radioCommand1(CMD_SET_STANDBY, tcxoOn ? 0x01 : 0x00);
  if (done) {
    Serial.print("TX #");
    Serial.println(txNumber);
    txNumber++;
    txCount++;
  }
  return done;
}

void printRssi() {
  uint8_t status[3];
  radioRead(CMD_GET_PACKET_STATUS, status, 3);
  int rssi = -((int)status[0]) / 2;
  int snr = ((int)(int8_t)status[1]) / 4;
  Serial.print(" rssi=");
  Serial.print(rssi);
  Serial.print(" dBm snr=");
  Serial.print(snr);
  Serial.println(" dB");
}

void pollReceive() {
  uint16_t irq = getIrq();
  if (irq & IRQ_CRC_ERR) {
    Serial.println("RX crc error");
    clearIrq();
    startReceive();
    return;
  }
  if ((irq & IRQ_RX_DONE) == 0) {
    return;
  }

  uint8_t info[2];
  radioRead(CMD_GET_RX_BUFFER, info, 2);
  uint8_t length = info[0];
  uint8_t offset = info[1];
  if (length > 8) {
    length = 8;
  }

  uint8_t data[8];
  waitReady(30);
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW);
  SPI.transfer(CMD_READ_BUFFER);
  SPI.transfer(offset);
  SPI.transfer(0x00);
  for (uint8_t i = 0; i < length; i++) {
    data[i] = SPI.transfer(0x00);
  }
  digitalWrite(PIN_NSS, HIGH);
  SPI.endTransaction();

  if (length >= 4) {
    uint16_t remoteId = ((uint16_t)data[0] << 8) | data[1];
    uint16_t number = ((uint16_t)data[2] << 8) | data[3];
    Serial.print("RX id=");
    Serial.print(remoteId, HEX);
    Serial.print(" #");
    Serial.print(number);
    printRssi();
    rxCount++;
  } else {
    Serial.println("RX short packet");
  }

  clearIrq();
  startReceive();
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
  rfIdle();

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);

  myId = (uint16_t)(ESP.getEfuseMac() & 0xFFFF);
  if (myId == 0) {
    myId = (uint16_t)(esp_random() & 0xFFFF);
    if (myId == 0) {
      myId = 1;
    }
  }

  Serial.println();
  Serial.println("=== Link check ===");
  Serial.println("868.8 MHz, SF7, BW125, power 10 dBm");
  Serial.println("Antennas 3 meters apart. Same sketch on both boards.");
  Serial.print("My id: ");
  Serial.println(myId, HEX);

  const uint8_t clocks[] = {0, 0x02, 0x07};
  const char* clockNames[] = {"XTAL", "TCXO 1.8V", "TCXO 3.3V"};
  radioReady = false;
  for (uint8_t i = 0; i < 3; i++) {
    Serial.print("Try ");
    Serial.println(clockNames[i]);
    if (!radioBegin(clocks[i])) {
      Serial.println("  init failed");
      continue;
    }
    if (sendPacket(true)) {
      Serial.print("Clock OK: ");
      Serial.println(clockNames[i]);
      radioReady = true;
      break;
    }
  }
  if (!radioReady) {
    Serial.println("Transmit failed on all clock modes.");
    return;
  }

  startReceive();
  nextTx = millis() + 400 + (myId % 1500);
  Serial.println("Listening.");
}

void loop() {
  if (!radioReady) {
    delay(1000);
    return;
  }

  pollReceive();

  if (millis() >= nextTx) {
    if (!sendPacket(false)) {
      Serial.println("TX timeout");
    }
    startReceive();
    nextTx = millis() + 2000;
    if (rxCount == 0 && txCount == 8) {
      Serial.println("No RX yet. Check antenna, RXEN, TXEN and that the other board is powered.");
    }
  }
}
