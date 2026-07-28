#include <Adafruit_PWMServoDriver.h>
#include <Wire.h>

#include "servo_calibration.hpp"

constexpr unsigned long BAUD_RATE = 115200;
constexpr uint8_t PCA9685_ADDRESS = 0x40;
constexpr uint8_t PCA9685_SERVO_HZ = 50;
constexpr uint8_t PCA9685_CHANNELS = 16;
constexpr size_t MAX_LINE_LENGTH = 96;

Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver(PCA9685_ADDRESS);
String inputLine;
bool pcaReady = false;

ServoProfile servoProfiles[PCA9685_CHANNELS];

void receiveSerialCommands();
void handleCommand(const String& rawLine);
void handleServoAngleCommand(const String& channelText, const String& angleText);
int driveServoAngle(uint8_t channel, int requestedAngle);
uint16_t angleToPulseUs(int angleDeg, const ServoProfile& profile);
uint16_t pulseUsToPca9685Ticks(uint16_t pulseUs);
bool isIntegerText(const String& value);
void printCalibration(uint8_t channel);

void setup() {
  Serial.begin(BAUD_RATE);
  inputLine.reserve(MAX_LINE_LENGTH);
  loadServoCalibration(servoProfiles, PCA9685_CHANNELS);
  Serial.println("OK:ACE_SERIAL_READY");

  Wire.begin();
  pcaReady = pwm.begin();
  if (pcaReady) {
    pwm.setOscillatorFrequency(27000000);
    pwm.setPWMFreq(PCA9685_SERVO_HZ);
    delay(10);
    Serial.println("OK:ACE_PCA9685_READY");
  } else {
    Serial.println("ERR:PCA9685_INIT");
  }
}

void loop() {
  receiveSerialCommands();
}

void receiveSerialCommands() {
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;

    if (c == '\n') {
      handleCommand(inputLine);
      inputLine = "";
      continue;
    }

    if (inputLine.length() < MAX_LINE_LENGTH - 1) {
      inputLine += c;
    } else {
      inputLine = "";
      Serial.println("ERR:LINE_TOO_LONG");
    }
  }
}

void handleCommand(const String& rawLine) {
  String line = rawLine;
  line.trim();

  if (line.length() == 0) return;

  if (line == "PING") {
    Serial.println("OK:PONG");
    return;
  }

  if (line == "STATUS") {
    // STATUS deliberately stays serial-only. If this fails, debug USB serial,
    // baud rate, line endings, or parsing before touching I2C or servo wiring.
    Serial.println("OK:ACE_SERIAL_READY");
    return;
  }

  if (line == "PCASTATUS") {
    // PCASTATUS is the first command that reports the hardware layer. Keeping
    // it separate from STATUS makes bring-up failures easier to isolate.
    Serial.println(pcaReady ? "OK:ACE_PCA9685_READY" : "ERR:PCA9685_INIT");
    return;
  }

  if (line == "CALSTATUS") {
    printCalibration(0);
    printCalibration(1);
    return;
  }

  int first = line.indexOf(':');
  int second = line.indexOf(':', first + 1);

  if (first < 0 || second < 0) {
    Serial.println("ERR:BAD_FRAME");
    return;
  }

  String commandType = line.substring(0, first);
  String channelText = line.substring(first + 1, second);
  String valueText = line.substring(second + 1);

  commandType.toUpperCase();
  valueText.trim();

  if (commandType == "SERVO") {
    handleServoAngleCommand(channelText, valueText);
    return;
  }

  Serial.println("ERR:UNKNOWN_COMMAND");
}

void handleServoAngleCommand(const String& channelText, const String& angleText) {
  if (!pcaReady) {
    Serial.println("ERR:PCA9685_NOT_READY");
    return;
  }

  int channel = channelText.toInt();
  if (!isIntegerText(channelText) || channel < 0 || channel >= PCA9685_CHANNELS) {
    Serial.println("ERR:SERVO_CHANNEL");
    return;
  }

  int requestedAngle = angleText.toInt();
  if (!isIntegerText(angleText)) {
    Serial.println("ERR:SERVO_ANGLE");
    return;
  }

  int appliedAngle = driveServoAngle(static_cast<uint8_t>(channel), requestedAngle);

  Serial.print("OK:SERVO:");
  Serial.print(channel);
  Serial.print(":");
  Serial.print(appliedAngle);
  Serial.print(":");
  Serial.println(servoProfiles[channel].name);
}

int driveServoAngle(uint8_t channel, int requestedAngle) {
  const ServoProfile& profile = servoProfiles[channel];
  int clampedAngle = constrain(requestedAngle, profile.minAngleDeg, profile.maxAngleDeg);
  uint16_t pulseUs = angleToPulseUs(clampedAngle, profile);
  uint16_t ticks = pulseUsToPca9685Ticks(pulseUs);

  pwm.setPWM(channel, 0, ticks);
  return clampedAngle;
}

uint16_t angleToPulseUs(int angleDeg, const ServoProfile& profile) {
  long pulse = map(
    angleDeg,
    profile.minAngleDeg,
    profile.maxAngleDeg,
    profile.minPulseUs,
    profile.maxPulseUs
  );

  return static_cast<uint16_t>(constrain(pulse, profile.minPulseUs, profile.maxPulseUs));
}

uint16_t pulseUsToPca9685Ticks(uint16_t pulseUs) {
  const uint32_t periodUs = 1000000UL / PCA9685_SERVO_HZ;
  uint32_t ticks = (static_cast<uint32_t>(pulseUs) * 4096UL + periodUs / 2) / periodUs;

  return static_cast<uint16_t>(constrain(ticks, 0UL, 4095UL));
}

bool isIntegerText(const String& value) {
  if (value.length() == 0) return false;

  for (unsigned int i = 0; i < value.length(); ++i) {
    if (i == 0 && (value[i] == '-' || value[i] == '+')) continue;
    if (!isDigit(value[i])) return false;
  }

  return true;
}

void printCalibration(uint8_t channel) {
  if (channel >= PCA9685_CHANNELS) return;

  const ServoProfile& profile = servoProfiles[channel];
  Serial.print("OK:CAL:");
  Serial.print(channel);
  Serial.print(":");
  Serial.print(profile.name);
  Serial.print(":");
  Serial.print(profile.minPulseUs);
  Serial.print(":");
  Serial.print(profile.maxPulseUs);
  Serial.print(":");
  Serial.print(profile.minAngleDeg);
  Serial.print(":");
  Serial.print(profile.maxAngleDeg);
  Serial.print(":");
  Serial.println(profile.homeAngleDeg);
}
