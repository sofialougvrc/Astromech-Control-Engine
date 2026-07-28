#include <Adafruit_PWMServoDriver.h>
#include <Wire.h>

#include "servo_calibration.hpp"

constexpr unsigned long BAUD_RATE = 115200;
constexpr uint8_t PCA9685_ADDRESS = 0x40;
constexpr uint8_t PCA9685_SERVO_HZ = 50;
constexpr uint8_t PCA9685_CHANNELS = 16;
constexpr size_t MAX_LINE_LENGTH = 96;
constexpr uint8_t TELEMETRY_CAPACITY = 16;
constexpr size_t TELEMETRY_LINE_LENGTH = 160;
constexpr uint8_t I2C_NOT_CHECKED = 255;

Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver(PCA9685_ADDRESS);
String inputLine;
bool pcaReady = false;

ServoProfile servoProfiles[PCA9685_CHANNELS];
char telemetryLines[TELEMETRY_CAPACITY][TELEMETRY_LINE_LENGTH];
uint8_t telemetryNext = 0;
uint8_t telemetryCount = 0;
uint32_t commandSequence = 0;

struct ServoDriveResult {
  bool ok;
  int appliedAngle;
  uint16_t pulseUs;
  uint16_t ticks;
  uint8_t i2cStatus;
};

void receiveSerialCommands();
void handleCommand(const String& rawLine);
void handleServoAngleCommand(uint32_t sequence, unsigned long receivedAtMs, const String& channelText, const String& angleText);
ServoDriveResult driveServoAngle(uint8_t channel, int requestedAngle);
uint16_t angleToPulseUs(int angleDeg, const ServoProfile& profile);
uint16_t pulseUsToPca9685Ticks(uint16_t pulseUs);
bool isIntegerText(const String& value);
void printCalibration(uint8_t channel);
uint8_t checkPca9685I2c();
bool initializePca9685();
void recordTelemetry(uint32_t sequence, unsigned long receivedAtMs, unsigned long executedAtMs, const char* command, const char* status, int channel, int requestedAngle, int appliedAngle, uint16_t pulseUs, uint16_t ticks, uint8_t i2cStatus);
void printTelemetry();

void setup() {
  Serial.begin(BAUD_RATE);
  inputLine.reserve(MAX_LINE_LENGTH);
  loadServoCalibration(servoProfiles, PCA9685_CHANNELS);
  Serial.println("OK:ACE_SERIAL_READY");
  recordTelemetry(0, millis(), millis(), "BOOT", "SERIAL_READY", -1, -1, -1, 0, 0, I2C_NOT_CHECKED);

  Wire.begin();
  pcaReady = initializePca9685();
  if (pcaReady) {
    Serial.println("OK:ACE_PCA9685_READY");
    recordTelemetry(0, millis(), millis(), "BOOT", "PCA9685_READY", -1, -1, -1, 0, 0, 0);
  } else {
    Serial.println("ERR:PCA9685_INIT");
    recordTelemetry(0, millis(), millis(), "BOOT", "PCA9685_INIT_FAIL", -1, -1, -1, 0, 0, checkPca9685I2c());
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
  unsigned long receivedAtMs = millis();
  uint32_t sequence = ++commandSequence;
  String line = rawLine;
  line.trim();

  if (line.length() == 0) return;

  if (line == "PING") {
    Serial.println("OK:PONG");
    recordTelemetry(sequence, receivedAtMs, millis(), "PING", "OK", -1, -1, -1, 0, 0, I2C_NOT_CHECKED);
    return;
  }

  if (line == "STATUS") {
    // STATUS deliberately stays serial-only. If this fails, debug USB serial,
    // baud rate, line endings, or parsing before touching I2C or servo wiring.
    Serial.println("OK:ACE_SERIAL_READY");
    recordTelemetry(sequence, receivedAtMs, millis(), "STATUS", "OK", -1, -1, -1, 0, 0, I2C_NOT_CHECKED);
    return;
  }

  if (line == "PCASTATUS") {
    // PCASTATUS is the first command that reports the hardware layer. Keeping
    // it separate from STATUS makes bring-up failures easier to isolate.
    uint8_t i2cStatus = checkPca9685I2c();
    pcaReady = (i2cStatus == 0) && (pcaReady || initializePca9685());
    Serial.println(pcaReady ? "OK:ACE_PCA9685_READY" : "ERR:PCA9685_INIT");
    recordTelemetry(sequence, receivedAtMs, millis(), "PCASTATUS", pcaReady ? "OK" : "I2C_ERR", -1, -1, -1, 0, 0, i2cStatus);
    return;
  }

  if (line == "CALSTATUS") {
    printCalibration(0);
    printCalibration(1);
    recordTelemetry(sequence, receivedAtMs, millis(), "CALSTATUS", "OK", -1, -1, -1, 0, 0, I2C_NOT_CHECKED);
    return;
  }

  if (line == "TELEMETRY") {
    recordTelemetry(sequence, receivedAtMs, millis(), "TELEMETRY", "OK", -1, -1, -1, 0, 0, I2C_NOT_CHECKED);
    printTelemetry();
    return;
  }

  int first = line.indexOf(':');
  int second = line.indexOf(':', first + 1);

  if (first < 0 || second < 0) {
    Serial.println("ERR:BAD_FRAME");
    recordTelemetry(sequence, receivedAtMs, millis(), "PARSE", "BAD_FRAME", -1, -1, -1, 0, 0, I2C_NOT_CHECKED);
    return;
  }

  String commandType = line.substring(0, first);
  String channelText = line.substring(first + 1, second);
  String valueText = line.substring(second + 1);

  commandType.toUpperCase();
  valueText.trim();

  if (commandType == "SERVO") {
    handleServoAngleCommand(sequence, receivedAtMs, channelText, valueText);
    return;
  }

  Serial.println("ERR:UNKNOWN_COMMAND");
  recordTelemetry(sequence, receivedAtMs, millis(), commandType.c_str(), "UNKNOWN_COMMAND", -1, -1, -1, 0, 0, I2C_NOT_CHECKED);
}

void handleServoAngleCommand(uint32_t sequence, unsigned long receivedAtMs, const String& channelText, const String& angleText) {
  if (!pcaReady) {
    Serial.println("ERR:PCA9685_NOT_READY");
    recordTelemetry(sequence, receivedAtMs, millis(), "SERVO", "PCA9685_NOT_READY", -1, -1, -1, 0, 0, checkPca9685I2c());
    return;
  }

  int channel = channelText.toInt();
  if (!isIntegerText(channelText) || channel < 0 || channel >= PCA9685_CHANNELS) {
    Serial.println("ERR:SERVO_CHANNEL");
    recordTelemetry(sequence, receivedAtMs, millis(), "SERVO", "BAD_CHANNEL", channel, -1, -1, 0, 0, I2C_NOT_CHECKED);
    return;
  }

  int requestedAngle = angleText.toInt();
  if (!isIntegerText(angleText)) {
    Serial.println("ERR:SERVO_ANGLE");
    recordTelemetry(sequence, receivedAtMs, millis(), "SERVO", "BAD_ANGLE", channel, -1, -1, 0, 0, I2C_NOT_CHECKED);
    return;
  }

  ServoDriveResult result = driveServoAngle(static_cast<uint8_t>(channel), requestedAngle);
  if (!result.ok) {
    Serial.println("ERR:PCA9685_I2C");
    recordTelemetry(sequence, receivedAtMs, millis(), "SERVO", "I2C_ERR", channel, requestedAngle, -1, 0, 0, result.i2cStatus);
    return;
  }

  Serial.print("OK:SERVO:");
  Serial.print(channel);
  Serial.print(":");
  Serial.print(result.appliedAngle);
  Serial.print(":");
  Serial.println(servoProfiles[channel].name);
  recordTelemetry(sequence, receivedAtMs, millis(), "SERVO", requestedAngle == result.appliedAngle ? "OK" : "CLAMPED", channel, requestedAngle, result.appliedAngle, result.pulseUs, result.ticks, result.i2cStatus);
}

ServoDriveResult driveServoAngle(uint8_t channel, int requestedAngle) {
  ServoDriveResult result;
  result.ok = false;
  result.appliedAngle = -1;
  result.pulseUs = 0;
  result.ticks = 0;
  result.i2cStatus = checkPca9685I2c();
  if (result.i2cStatus != 0) {
    pcaReady = false;
    return result;
  }

  const ServoProfile& profile = servoProfiles[channel];
  int clampedAngle = constrain(requestedAngle, profile.minAngleDeg, profile.maxAngleDeg);
  uint16_t pulseUs = angleToPulseUs(clampedAngle, profile);
  uint16_t ticks = pulseUsToPca9685Ticks(pulseUs);

  pwm.setPWM(channel, 0, ticks);
  result.ok = true;
  result.appliedAngle = clampedAngle;
  result.pulseUs = pulseUs;
  result.ticks = ticks;
  return result;
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

uint8_t checkPca9685I2c() {
  Wire.beginTransmission(PCA9685_ADDRESS);
  return Wire.endTransmission();
}

bool initializePca9685() {
  if (!pwm.begin()) {
    return false;
  }
  pwm.setOscillatorFrequency(27000000);
  pwm.setPWMFreq(PCA9685_SERVO_HZ);
  delay(10);
  return true;
}

void recordTelemetry(uint32_t sequence, unsigned long receivedAtMs, unsigned long executedAtMs, const char* command, const char* status, int channel, int requestedAngle, int appliedAngle, uint16_t pulseUs, uint16_t ticks, uint8_t i2cStatus) {
  snprintf(
    telemetryLines[telemetryNext],
    TELEMETRY_LINE_LENGTH,
    "TEL:%lu:rx_ms=%lu:exec_ms=%lu:cmd=%s:status=%s:ch=%d:req=%d:applied=%d:pulse_us=%u:ticks=%u:i2c=%u",
    static_cast<unsigned long>(sequence),
    receivedAtMs,
    executedAtMs,
    command,
    status,
    channel,
    requestedAngle,
    appliedAngle,
    pulseUs,
    ticks,
    i2cStatus
  );

  telemetryNext = (telemetryNext + 1) % TELEMETRY_CAPACITY;
  if (telemetryCount < TELEMETRY_CAPACITY) {
    telemetryCount++;
  }
}

void printTelemetry() {
  uint8_t start = (telemetryNext + TELEMETRY_CAPACITY - telemetryCount) % TELEMETRY_CAPACITY;
  for (uint8_t i = 0; i < telemetryCount; ++i) {
    uint8_t index = (start + i) % TELEMETRY_CAPACITY;
    Serial.println(telemetryLines[index]);
  }
  Serial.print("OK:TELEMETRY:");
  Serial.println(telemetryCount);
}
