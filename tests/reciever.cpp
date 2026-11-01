
#include <Arduino.h>
#include <RadioLib.h>

// SPI: ESP32 DevKit V1
#define LORA_SCK   18
#define LORA_MISO  19
#define LORA_MOSI  23
#define LORA_CS     5

// E22-900M30S
#define LORA_RST   14
#define LORA_BUSY  26
#define LORA_DIO1  33
#define LORA_RXEN  32
#define LORA_TXEN  25

const float FREQUENCY_MHZ = 868.0;
const int TOTAL_MESSAGES = 100;

SX1262 radio = new Module(
    LORA_CS,
    LORA_DIO1,
    LORA_RST,
    LORA_BUSY
);

bool received[TOTAL_MESSAGES + 1] = {false};
int receivedCount = 0;
int duplicateCount = 0;

void setup() {
    Serial.begin(115200);
    delay(1000);

    radio.setRfSwitchPins(LORA_RXEN, LORA_TXEN);

    int state = radio.begin(
        FREQUENCY_MHZ,
        125.0,
        9,
        7,
        RADIOLIB_SX126X_SYNC_WORD_PRIVATE,
        4,
        8,
        1.8
    );

    if (state != RADIOLIB_ERR_NONE) {
        Serial.print("Radio init failed: ");
        Serial.println(state);

        while (true) {
            delay(1000);
        }
    }

    Serial.println("Radio initialized!");
    Serial.println("Receiver ready.");
}

void loop() {
    String message;

    int state = radio.receive(message);

    if (state != RADIOLIB_ERR_NONE) {
        return;
    }

    Serial.print("Received: ");
    Serial.println(message);

    if (!message.startsWith("DATA|")) {
        Serial.println("Invalid message.");
        return;
    }

    String number = message.substring(5);
    int id = number.toInt();

    if (id < 1 || id > TOTAL_MESSAGES ||
        number.length() == 0) {
        Serial.println("Invalid sequence number.");
        return;
    }

    // Учитываем только уникальные сообщения.
    if (!received[id]) {
        received[id] = true;
        receivedCount++;

        Serial.print("RX,");
        Serial.print(id);
        Serial.println(",OK");
    } else {
        duplicateCount++;

        Serial.print("Duplicate DATA,");
        Serial.println(id);
    }

    String ack = "ACK|" + String(id);

    Serial.print("Sending: ");
    Serial.println(ack);

    int txState = radio.transmit(ack);

    if (txState == RADIOLIB_ERR_NONE) {
        Serial.println("ACK sent.");
    } else {
        Serial.print("ACK send failed: ");
        Serial.println(txState);
    }

    // Итог печатаем при первом получении DATA|100.
    if (id == TOTAL_MESSAGES) {
        Serial.println("\n=== RECEIVER SUMMARY ===");
        Serial.print("Unique messages received: ");
        Serial.println(receivedCount);
        Serial.print("Duplicates: ");
        Serial.println(duplicateCount);

        Serial.print("Missing IDs: ");
        int missing = 0;

        for (int i = 1; i <= TOTAL_MESSAGES; i++) {
            if (!received[i]) {
                Serial.print(i);
                Serial.print(" ");
                missing++;
            }
        }

        if (missing == 0) {
            Serial.print("none");
        }

        Serial.println();
        Serial.print("Unique delivery rate: ");
        Serial.print(100.0 * receivedCount / TOTAL_MESSAGES);
        Serial.println("%");
    }
}
