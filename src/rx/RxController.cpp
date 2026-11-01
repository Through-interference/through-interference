#include "rx/RxController.h"

#include <RadioLib.h>
#include "crypto.h"
#include "protocol.h"
#include "radio_config.h"
#include "radio_hardware.h"
#include "RadioChannelManager.h"

namespace {
const uint8_t TEST_DROP_ACK_COUNT = 2;

const unsigned long ACK_TX_TIMEOUT_MS = 5000UL;
const unsigned long RX_STATS_INTERVAL_MS = 50000UL;
}

RxController::RxController()
: radioRecovery_(transmitFlag_, operationDone) {
}


void RxController::begin() {
Serial.begin(115200);
delay(1000);

if (!cryptoInit()) {
Serial.println("AES-GCM initialization failed!");
while (true) delay(1000);
}

if (!messageStore_.begin()) {
Serial.println(
"[RX] Cannot open NVS namespace for transaction state"
);
while (true) delay(1000);
}

if (!messageStore_.load()) {
Serial.println(
"[RX][FATAL] Could not load persistent transaction state; "
"stopping for safety"
);
while (true) delay(1000);
}

Serial.print("[RX] Node ID: ");
Serial.println(RX_NODE_ID);

Serial.print("[RX] Expected source node ID: ");
Serial.println(TX_NODE_ID);

Serial.print("[RX] Last committed messageId: ");
Serial.println(messageStore_.lastMessageId());

Serial.print("[SX1262] Initializing ... ");

if (radioRecovery_.initializeRadio()) {
Serial.println("success!");
Serial.println("Encrypted receiver started");

radioRecovery_.startReceiveWithRecovery();
} else {
Serial.println(
"initialization failed; recovery will retry automatically"
);

radioRecovery_.scheduleFullRecovery(
"startup initialization failed"
);
}

lastStatsAt_ = millis();
lastDataAt_ = millis();

duplicateStreak_ = 0;
scanAfterAck_ = false;
}


void RxController::update() {
handleRadioEvent();

// If ACK TX is stuck and its completion IRQ never arrives,
// schedule radio reinitialization.
if (transmitFlag_ &&
(unsigned long)(millis() - ackTxStartedAt_) >=
ACK_TX_TIMEOUT_MS) {

Serial.println(
"[RX] ACK TX timeout; scheduling radio reinitialization"
);

radio.standby();

transmitFlag_ = false;
operationDone = false;

radioRecovery_.scheduleFullRecovery(
"ACK transmission timeout"
);
}

radioRecovery_.update();

// Handle frequency scanning after radio recovery processing.
updateChannelSearch();

printPeriodicStats();

yield();
}


void RxController::handleRadioEvent() {
if (!operationDone) return;

operationDone = false;

// ACK transmission has completed: return to receive mode.
if (transmitFlag_) {
transmissionState_ = radio.finishTransmit();

if (transmissionState_ == RADIOLIB_ERR_NONE) {
ackTransmissionsCompleted_++;
Serial.println("[RX] ACK transmission finished");
} else {
ackTransmissionFailures_++;

Serial.print("[RX] ACK transmission error: ");
Serial.println(transmissionState_);
}

transmitFlag_ = false;

radioRecovery_.startReceiveWithRecovery();
return;
}

handleReceivedData();
}


void RxController::handleReceivedData() {
String encryptedFrame;

const int state = radio.readData(encryptedFrame);

if (state != RADIOLIB_ERR_NONE) {
Serial.print("[RX] readData failed: ");
Serial.println(state);

radioRecovery_.startReceiveWithRecovery();
return;
}

Serial.print("[RX] Received encrypted frame: ");
Serial.println(encryptedFrame);

Serial.print("[RX] RSSI: ");
Serial.print(radio.getRSSI());
Serial.println(" dBm");

Serial.print("[RX] SNR: ");
Serial.print(radio.getSNR());
Serial.println(" dB");

String plaintext;

uint32_t cryptoSenderId = 0;
uint64_t cryptoCounter = 0;

const CryptoPacketStatus status = decryptPacketEx(
encryptedFrame,
plaintext,
cryptoSenderId,
cryptoCounter
);

if (status == CRYPTO_PACKET_ERROR) {
decryptFailures_++;

Serial.println(
"[RX] Decryption/authentication FAILED; ignored"
);

radioRecovery_.startReceiveWithRecovery();

} else if (status == CRYPTO_PACKET_REPLAY) {
decryptFailures_++;

Serial.println(
"[RX] Old crypto frame/replay; ignored"
);

radioRecovery_.startReceiveWithRecovery();

} else {
Serial.print("[RX] Authenticated plaintext: ");
Serial.println(plaintext);

processApplicationMessage(
status,
plaintext,
cryptoSenderId
);
}
}


void RxController::processApplicationMessage(
CryptoPacketStatus cryptoStatus,
const String& plaintext,
uint32_t cryptoSenderId
) {
if (cryptoStatus != CRYPTO_PACKET_NEW &&
cryptoStatus != CRYPTO_PACKET_DUPLICATE) {

decryptFailures_++;

Serial.println(
"[RX] Invalid crypto status; message ignored"
);

radioRecovery_.startReceiveWithRecovery();
return;
}

// Explicit cryptographic peer check.
if (cryptoSenderId != static_cast<uint32_t>(CRYPTO_PEER_ID)) {
Serial.println(
"[RX] Unexpected cryptographic peer; ignored"
);

radioRecovery_.startReceiveWithRecovery();
return;
}

RadioMessage message;

if (!decodeRadioMessage(plaintext, message)) {
Serial.println(
"[RX] Invalid application header; ignored"
);

radioRecovery_.startReceiveWithRecovery();
return;
}

// Check application message type and addressing.
if (message.type != RADIO_MSG_DATA ||
message.senderId != TX_NODE_ID ||
message.destinationId != RX_NODE_ID) {

Serial.print("[RX] Wrong type/address. sender=");
Serial.print(message.senderId);

Serial.print(" destination=");
Serial.println(message.destinationId);

radioRecovery_.startReceiveWithRecovery();
return;
}

// A crypto duplicate is valid only for the already committed
// logical message with exactly the same contents.
if (cryptoStatus == CRYPTO_PACKET_DUPLICATE &&
(!messageStore_.hasMessage() ||
message.messageId != messageStore_.lastMessageId() ||
message.senderId != messageStore_.lastSourceId() ||
message.destinationId != messageStore_.lastDestinationId() ||
message.payload != messageStore_.lastPayload())) {

Serial.println(
"[RX] Crypto duplicate does not match the committed "
"logical transaction; rejected"
);

radioRecovery_.startReceiveWithRecovery();
return;
}

// We received an authenticated DATA from the expected TX.
lastDataAt_ = millis();

// If RX was searching, the transmitting channel has been found.
if (radioChannelManager.isSearching()) {
radioChannelManager.stopSearch();

duplicateStreak_ = 0;
scanAfterAck_ = false;

Serial.print("[RX][CHANNEL] Found TX on channel ");
Serial.println(radioChannelManager.currentChannel());
}

receivedDataFrames_++;

Serial.print("[RX] DATA messageId=");
Serial.print(message.messageId);

Serial.print(" from node ");
Serial.print(message.senderId);

Serial.print(" to node ");
Serial.println(message.destinationId);

// Reject stale logical transactions.
if (messageStore_.hasMessage() &&
message.messageId < messageStore_.lastMessageId()) {

Serial.println(
"[RX] Stale logical transaction; not executed or ACKed"
);

radioRecovery_.startReceiveWithRecovery();
return;
}

// Handle a repeated logical transaction.
if (messageStore_.hasMessage() &&
message.messageId == messageStore_.lastMessageId()) {

if (message.senderId != messageStore_.lastSourceId() ||
message.destinationId != messageStore_.lastDestinationId() ||
message.payload != messageStore_.lastPayload()) {

Serial.println(
"[RX] Message ID reused with different content; rejected"
);

radioRecovery_.startReceiveWithRecovery();
return;
}

duplicateDataFrames_++;

if (duplicateStreak_ < 255) {
duplicateStreak_++;
}

Serial.print("[RX] Consecutive duplicate DATA frames: ");
Serial.println(duplicateStreak_);

// Multiple retransmissions indicate a possible connection problem.
if (duplicateStreak_ >= RX_DUPLICATES_BEFORE_SCAN) {
scanAfterAck_ = true;
}

Serial.println(
"[RX] Duplicate logical transaction; "
"action NOT repeated, ACK resent"
);

startAck(message.messageId);
return;
}

// A new logical message was received.
// Persist it before acknowledging it.
if (!messageStore_.save(message)) {
Serial.println(
"[RX] Cannot persist transaction state; "
"action and ACK suppressed"
);

radioRecovery_.startReceiveWithRecovery();
return;
}

// New DATA resets the duplicate detection streak.
duplicateStreak_ = 0;
scanAfterAck_ = false;

// Commit messageId before the application action (at-most-once gate).
newDataMessages_++;

Serial.print("[RX] New DATA accepted once. Payload: ");
Serial.println(message.payload);

startAck(message.messageId);
}


void RxController::startAck(uint32_t messageId) {
// Test mode: deliberately drop the first ACK.
// Simulate loss of the first N ACK transmissions.
if (testAckDrops_ < TEST_DROP_ACK_COUNT) {
testAckDrops_++;

Serial.print("[RX][TEST] Simulating lost ACK ");
Serial.print(testAckDrops_);
Serial.print("/");
Serial.println(TEST_DROP_ACK_COUNT);

radioRecovery_.startReceiveWithRecovery();
return;
}
RadioMessage ack;

ack.type = RADIO_MSG_ACK;
ack.senderId = RX_NODE_ID;
ack.destinationId = TX_NODE_ID;
ack.messageId = messageId;
ack.payload = "OK";

String plaintext;
String encryptedFrame;

if (!encodeRadioMessage(ack, plaintext) ||
!encryptPacket(plaintext, encryptedFrame)) {

Serial.println("[RX] ACK encoding/encryption failed!");

radioRecovery_.startReceiveWithRecovery();
return;
}

radio.standby();
delay(50);

Serial.print("[RX] Sending ACK for messageId=");
Serial.println(messageId);

operationDone = false;

transmissionState_ = radio.startTransmit(encryptedFrame);

if (transmissionState_ == RADIOLIB_ERR_NONE) {
transmitFlag_ = true;

ackTxStartedAt_ = millis();
ackTransmissionsStarted_++;

} else {
ackTransmissionFailures_++;

Serial.print("[RX] startTransmit failed: ");
Serial.println(transmissionState_);

transmitFlag_ = false;

radioRecovery_.startReceiveWithRecovery();
}
}


// Start channel search when requested by duplicate detection
// or when no DATA has arrived for a configured period.
void RxController::startChannelSearch() {
if (radioChannelManager.isSearching() ||
transmitFlag_ ||
radioRecovery_.isRecovering()) {
return;
}

Serial.println("[RX][CHANNEL] Starting channel search");

if (!radioChannelManager.startSearch(RX_SCAN_DWELL_MS)) {
Serial.println("[RX][CHANNEL] Cannot start search");

// Avoid retrying a failed search on every update() iteration.
scanAfterAck_ = false;
lastDataAt_ = millis();

return;
}

scanAfterAck_ = false;

// The manager has already switched to the first search channel.
radioRecovery_.startReceiveWithRecovery();

Serial.print("[RX][CHANNEL] Listening on channel ");
Serial.println(radioChannelManager.currentChannel());
}


// Maintain the channel search state.
void RxController::updateChannelSearch() {
if (transmitFlag_ || radioRecovery_.isRecovering()) {
return;
}

// Start scanning after the ACK has finished transmitting.
// If the test deliberately dropped the ACK, scanning can begin
// immediately because the radio is already in receive mode.
if (scanAfterAck_) {
startChannelSearch();
return;
}

// Start scanning if no valid DATA has arrived for too long.
if (!radioChannelManager.isSearching()) {
if ((unsigned long)(millis() - lastDataAt_) >=
RX_SCAN_AFTER_IDLE_MS) {

startChannelSearch();
}

return;
}

// Switch to the next channel after the listening interval.
if (radioChannelManager.updateSearch()) {
Serial.print("[RX][CHANNEL] Listening on channel ");
Serial.println(radioChannelManager.currentChannel());

radioRecovery_.startReceiveWithRecovery();
}
}


void RxController::printPeriodicStats() {
if ((unsigned long)(millis() - lastStatsAt_) <
RX_STATS_INTERVAL_MS) {
return;
}

lastStatsAt_ = millis();

Serial.println("[RX] === PERIODIC STATISTICS ===");

Serial.print("DATA frames received: ");
Serial.println(receivedDataFrames_);

Serial.print("New application messages: ");
Serial.println(newDataMessages_);

Serial.print("Duplicate DATA frames: ");
Serial.println(duplicateDataFrames_);

Serial.print("ACK transmissions started: ");
Serial.println(ackTransmissionsStarted_);

Serial.print("ACK transmissions completed: ");
Serial.println(ackTransmissionsCompleted_);

Serial.print("ACK transmission failures: ");
Serial.println(ackTransmissionFailures_);

Serial.print("Decryption/replay errors: ");
Serial.println(decryptFailures_);

Serial.print("startReceive failures: ");
Serial.println(radioRecovery_.receiveStartFailures());

Serial.print("Successful radio recoveries: ");
Serial.println(radioRecovery_.successfulRecoveries());

Serial.print("Failed radio recovery attempts: ");
Serial.println(radioRecovery_.failedRecoveryAttempts());

Serial.print("Current channel: ");
Serial.println(radioChannelManager.currentChannel());

Serial.print("Channel search active: ");
Serial.println(
radioChannelManager.isSearching() ? "YES" : "NO"
);
}