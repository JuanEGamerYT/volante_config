#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include "soc/soc_caps.h"

#define ENABLE_USB_HID 1
#define ENABLE_SERIAL_DEBUG 1

#ifndef CONFIG_TINYUSB_HID_ENABLED
#define CONFIG_TINYUSB_HID_ENABLED 0
#endif

#if ENABLE_USB_HID && SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_HID_ENABLED
#include "USB.h"
#include "USBHIDGamepad.h"
USBHIDGamepad Gamepad;
#define USB_HID_GAMEPAD_AVAILABLE 1
#else
#define USB_HID_GAMEPAD_AVAILABLE 0
#define HAT_CENTER     0
#define HAT_UP         1
#define HAT_UP_RIGHT   2
#define HAT_RIGHT      3
#define HAT_DOWN_RIGHT 4
#define HAT_DOWN       5
#define HAT_DOWN_LEFT  6
#define HAT_LEFT       7
#define HAT_UP_LEFT    8
#endif

// =========================
// ESP32-S3 bridge/receptor
// =========================
//
// Recibe tramas del Nano por UART (binario + CRC).
// Lee botones/pots locales del ESP32-S3.
// Publica estado por USB Serial para validar.
//
// Ajusta pines segun tu placa SuperMini.

constexpr uint32_t USB_BAUD = 115200;
constexpr uint32_t NANO_UART_BAUD = 115200;
constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 10000;
constexpr uint16_t OTA_PORT = 3232;
constexpr uint8_t CONFIG_VERSION = 2;

constexpr uint8_t FRAME_SYNC_1 = 0xA5;
constexpr uint8_t FRAME_SYNC_2 = 0x5A;
constexpr uint8_t NANO_MAX_BUTTONS = 40;
constexpr uint8_t NANO_MAX_BYTES = (NANO_MAX_BUTTONS + 7) / 8;
constexpr uint8_t EXPECTED_NANO_BUTTONS = 19;  // Nano sketch actual: D2..D12 + A0..A7

const char *const NANO_BUTTON_LABELS[EXPECTED_NANO_BUTTONS] = {
  "D2", "D3", "D4", "D5", "D6", "D7", "D8", "D9", "D10", "D11",
  "D12", "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7"
};

// UART fisica con el Nano (tu cableado actual)
constexpr int NANO_RX_PIN = 13;  // ESP32 RX <- Nano TX (con divisor 10k/20k)
constexpr int NANO_TX_PIN = 12;  // ESP32 TX -> Nano RX

// Motores en ESP32-S3 (control PWM por MOSFET en low-side).
constexpr uint8_t MOTOR_PINS[] = {7, 6};
constexpr uint8_t MOTOR_PIN_COUNT = sizeof(MOTOR_PINS) / sizeof(MOTOR_PINS[0]);
constexpr uint8_t MOTOR_PWM_MAX = 255;
constexpr uint32_t MOTOR_HOLD_MS = 1000;  // 1s a 255
constexpr uint32_t MOTOR_RAMP_MS = 2000;  // 2s de 255 a 0
constexpr bool MOTOR_TEST_PATTERN_ENABLED = false;
constexpr uint8_t MOTOR_DEFAULT_LEFT = 0;
constexpr uint8_t MOTOR_DEFAULT_RIGHT = 0;
constexpr uint8_t MOTOR_DEFAULT_MIN_LEFT = 120;   // GPIO7: vence zona muerta del motor.
constexpr uint8_t MOTOR_DEFAULT_MIN_RIGHT = 100;  // GPIO6: vence zona muerta del motor.

// Botones locales del ESP32-S3 (a GND con INPUT_PULLUP)
constexpr uint8_t LOCAL_BUTTON_PINS[] = {8, 5, 48, 47};
constexpr bool LOCAL_BUTTON_ACTIVE_LOW[] = {true, true, true, true};
constexpr uint8_t LOCAL_BUTTON_COUNT = sizeof(LOCAL_BUTTON_PINS) / sizeof(LOCAL_BUTTON_PINS[0]);
constexpr uint8_t LOCAL_BUTTON_BYTES = (LOCAL_BUTTON_COUNT + 7) / 8;

// Potenciometros locales (0..4095 en ESP32)
// Orden: pedalera en GPIO2, pedalera en GPIO3, volante en GPIO4.
constexpr uint8_t POT_PINS[] = {2, 3, 4};
const char *const POT_LABELS[] = {"PEDAL_GPIO2", "PEDAL_GPIO3", "WHEEL4"};
constexpr uint8_t POT_COUNT = sizeof(POT_PINS) / sizeof(POT_PINS[0]);
constexpr uint8_t PEDAL_1_INDEX = 0;
constexpr uint8_t PEDAL_2_INDEX = 1;
constexpr uint8_t WHEEL_INDEX = 2;
constexpr bool DEFAULT_PEDALS_USE_PULLUP = true;  // Pedales a GND: usar pullup interno.
constexpr uint16_t ADC_MAX_VALUE = 4095;
constexpr uint16_t DEFAULT_WHEEL_DEADZONE = 45;
constexpr uint32_t HID_REPORT_INTERVAL_MS = 5;
constexpr uint32_t DEBUG_PRINT_INTERVAL_MS = 100;
constexpr uint8_t POT_FILTER_SHIFT = 3;  // 1/8 smoothing
constexpr uint8_t DEFAULT_AXIS_SENSITIVITY = 100;  // 100 = lineal normal
constexpr uint8_t MIN_AXIS_SENSITIVITY = 20;
constexpr uint8_t MAX_AXIS_SENSITIVITY = 200;
constexpr bool DEFAULT_INVERT_WHEEL = true;
constexpr bool DEFAULT_INVERT_PEDAL_1 = false;
constexpr bool DEFAULT_INVERT_PEDAL_2 = false;

constexpr bool DEFAULT_ENABLE_HID_POV = true;
// Windows muestra botones desde 1; el bit HID/Nano empieza desde 0.
// Mapeo actual visto en "Dispositivos de juego USB":
// Boton 5=arriba, 6=abajo, 7=derecha, 8=izquierda.
constexpr uint8_t POV_UP_INDEX = 4;
constexpr uint8_t POV_DOWN_INDEX = 5;
constexpr uint8_t POV_RIGHT_INDEX = 6;
constexpr uint8_t POV_LEFT_INDEX = 7;

// LED RGB segun tu mapeo:
// GPIO9 = Rojo, GPIO10 = Verde, GPIO11 = Azul
constexpr int RGB_R_PIN = 9;
constexpr int RGB_G_PIN = 10;
constexpr int RGB_B_PIN = 11;
constexpr bool DEFAULT_RGB_ACTIVE_LOW = false;  // true: anodo comun, false: catodo comun
constexpr bool RGB_DEBUG_BLINK = false;  // true: prueba colores local; false: color fijo/manual.
constexpr uint8_t RGB_DEFAULT_R = 0;
constexpr uint8_t RGB_DEFAULT_G = 0;
constexpr uint8_t RGB_DEFAULT_B = 0;
constexpr uint32_t RGB_STEP_MS = 250;
constexpr uint8_t RGB_DEFAULT_MODE = 1;   // 0 off, 1 fijo, 2 ciclo, 3 respiracion, 4 onda, 5 pulso.
constexpr uint8_t RGB_DEFAULT_SPEED = 100;  // 10..200, porcentaje.
constexpr uint8_t RGB_MIN_SPEED = 10;
constexpr uint8_t RGB_MAX_SPEED = 200;
const char *const DEFAULT_OTA_HOSTNAME = "volante-s3";

HardwareSerial NanoSerial(1);
Preferences preferences;

struct WheelConfig {
  uint8_t rgbR;
  uint8_t rgbG;
  uint8_t rgbB;
  uint8_t rgbMode;
  uint8_t rgbSpeed;
  bool wifiEnabled;
  bool enableHidPov;
  bool rgbActiveLow;
  bool pedalsUsePullup;
  uint16_t wheelCenter;
  uint16_t wheelDeadzone;
  uint8_t wheelSensitivity;
  uint16_t pedalMin[2];
  uint16_t pedalMax[2];
  uint8_t pedalSensitivity[2];
  uint8_t motorMinPwm[2];
  bool invertWheel;
  bool invertPedal[2];
};

enum MotorState : uint8_t {
  MOTOR_HOLD_FULL,
  MOTOR_RAMP_DOWN
};

enum RxState : uint8_t {
  WAIT_SYNC_1,
  WAIT_SYNC_2,
  WAIT_SEQ,
  WAIT_COUNT,
  WAIT_PAYLOAD,
  WAIT_CRC
};

RxState rxState = WAIT_SYNC_1;
uint8_t rxSeq = 0;
uint8_t rxCount = 0;
uint8_t rxPayload[NANO_MAX_BYTES];
uint8_t rxPayloadIndex = 0;
uint8_t rxCrc = 0;

uint8_t nanoButtons[NANO_MAX_BYTES];
uint8_t nanoButtonCount = 0;
uint8_t nanoSeq = 0;
uint32_t lastNanoFrameMs = 0;
uint32_t rxByteCount = 0;
uint32_t rxFrameOkCount = 0;
uint32_t rxFrameCrcErrCount = 0;
uint32_t rxFrameInvalidCount = 0;

uint8_t localButtons[LOCAL_BUTTON_BYTES];
uint16_t pots[POT_COUNT];
uint16_t wheelCenter = ADC_MAX_VALUE / 2;
WheelConfig config;
bool configLoadedFromFlash = false;
MotorState motorState = MOTOR_HOLD_FULL;
uint32_t motorStateStartedMs = 0;
uint8_t motorLeftPwm = MOTOR_DEFAULT_LEFT;
uint8_t motorRightPwm = MOTOR_DEFAULT_RIGHT;
uint8_t motorLeftActualPwm = 0;
uint8_t motorRightActualPwm = 0;
uint8_t rgbR = RGB_DEFAULT_R;
uint8_t rgbG = RGB_DEFAULT_G;
uint8_t rgbB = RGB_DEFAULT_B;
String wifiSsid;
String wifiPassword;
String otaHostname = DEFAULT_OTA_HOSTNAME;
String otaPassword;
bool otaStarted = false;
uint32_t lastWifiAttemptMs = 0;

#if ENABLE_SERIAL_DEBUG
bool serialDebugOutputEnabled = true;
char serialCommandLine[160];
uint8_t serialCommandLen = 0;
#endif

uint16_t clampAdc(int32_t value) {
  if (value < 0) {
    return 0;
  }
  if (value > ADC_MAX_VALUE) {
    return ADC_MAX_VALUE;
  }
  return static_cast<uint16_t>(value);
}

uint8_t clampSensitivity(int32_t value) {
  if (value < MIN_AXIS_SENSITIVITY) {
    return MIN_AXIS_SENSITIVITY;
  }
  if (value > MAX_AXIS_SENSITIVITY) {
    return MAX_AXIS_SENSITIVITY;
  }
  return static_cast<uint8_t>(value);
}

uint8_t clampRgbSpeed(int32_t value) {
  if (value < RGB_MIN_SPEED) {
    return RGB_MIN_SPEED;
  }
  if (value > RGB_MAX_SPEED) {
    return RGB_MAX_SPEED;
  }
  return static_cast<uint8_t>(value);
}

void setDefaultConfig() {
  config.rgbR = RGB_DEFAULT_R;
  config.rgbG = RGB_DEFAULT_G;
  config.rgbB = RGB_DEFAULT_B;
  config.rgbMode = RGB_DEFAULT_MODE;
  config.rgbSpeed = RGB_DEFAULT_SPEED;
  config.wifiEnabled = false;
  config.enableHidPov = DEFAULT_ENABLE_HID_POV;
  config.rgbActiveLow = DEFAULT_RGB_ACTIVE_LOW;
  config.pedalsUsePullup = DEFAULT_PEDALS_USE_PULLUP;
  config.wheelCenter = ADC_MAX_VALUE / 2;
  config.wheelDeadzone = DEFAULT_WHEEL_DEADZONE;
  config.wheelSensitivity = DEFAULT_AXIS_SENSITIVITY;
  config.pedalMin[0] = 0;
  config.pedalMax[0] = ADC_MAX_VALUE;
  config.pedalMin[1] = 0;
  config.pedalMax[1] = ADC_MAX_VALUE;
  config.pedalSensitivity[0] = DEFAULT_AXIS_SENSITIVITY;
  config.pedalSensitivity[1] = DEFAULT_AXIS_SENSITIVITY;
  config.motorMinPwm[0] = MOTOR_DEFAULT_MIN_LEFT;
  config.motorMinPwm[1] = MOTOR_DEFAULT_MIN_RIGHT;
  config.invertWheel = DEFAULT_INVERT_WHEEL;
  config.invertPedal[0] = DEFAULT_INVERT_PEDAL_1;
  config.invertPedal[1] = DEFAULT_INVERT_PEDAL_2;
  wifiSsid = "";
  wifiPassword = "";
  otaHostname = DEFAULT_OTA_HOSTNAME;
  otaPassword = "";
}

bool loadConfigFromFlash() {
  setDefaultConfig();

  preferences.begin("wheelcfg", true);
  const bool valid = preferences.getBool("valid", false);
  const uint8_t storedVersion = preferences.getUChar("ver", 0);
  if (valid) {
    config.rgbR = preferences.getUChar("rr", config.rgbR);
    config.rgbG = preferences.getUChar("rg", config.rgbG);
    config.rgbB = preferences.getUChar("rb", config.rgbB);
    config.rgbMode = preferences.getUChar("rm", config.rgbMode);
    config.rgbSpeed = clampRgbSpeed(preferences.getUChar("rs", config.rgbSpeed));
    config.wifiEnabled = preferences.getBool("we", config.wifiEnabled);
    config.enableHidPov = preferences.getBool("pov", config.enableHidPov);
    config.rgbActiveLow = preferences.getBool("ral", config.rgbActiveLow);
    config.pedalsUsePullup = preferences.getBool("ppu", config.pedalsUsePullup);
    wifiSsid = preferences.getString("wssid", wifiSsid);
    wifiPassword = preferences.getString("wpass", wifiPassword);
    otaHostname = preferences.getString("ohost", otaHostname);
    otaPassword = preferences.getString("opass", otaPassword);
    config.wheelCenter = preferences.getUShort("wc", config.wheelCenter);
    config.wheelDeadzone = preferences.getUShort("wdz", config.wheelDeadzone);
    config.wheelSensitivity = clampSensitivity(preferences.getUChar("ws", config.wheelSensitivity));
    config.pedalMin[0] = preferences.getUShort("p1mn", config.pedalMin[0]);
    config.pedalMax[0] = preferences.getUShort("p1mx", config.pedalMax[0]);
    config.pedalSensitivity[0] = clampSensitivity(preferences.getUChar("p1s", config.pedalSensitivity[0]));
    config.pedalMin[1] = preferences.getUShort("p2mn", config.pedalMin[1]);
    config.pedalMax[1] = preferences.getUShort("p2mx", config.pedalMax[1]);
    config.pedalSensitivity[1] = clampSensitivity(preferences.getUChar("p2s", config.pedalSensitivity[1]));
    config.motorMinPwm[0] = preferences.getUChar("m1min", config.motorMinPwm[0]);
    config.motorMinPwm[1] = preferences.getUChar("m2min", config.motorMinPwm[1]);
    config.invertWheel = preferences.getBool("iw", config.invertWheel);
    config.invertPedal[0] = preferences.getBool("ip1", config.invertPedal[0]);
    config.invertPedal[1] = preferences.getBool("ip2", config.invertPedal[1]);

    config.pedalsUsePullup = DEFAULT_PEDALS_USE_PULLUP;
  }
  preferences.end();

  config.wheelCenter = clampAdc(config.wheelCenter);
  config.wheelDeadzone = clampAdc(config.wheelDeadzone);
  config.pedalMin[0] = clampAdc(config.pedalMin[0]);
  config.pedalMax[0] = clampAdc(config.pedalMax[0]);
  config.pedalMin[1] = clampAdc(config.pedalMin[1]);
  config.pedalMax[1] = clampAdc(config.pedalMax[1]);
  if (config.rgbMode > 5) {
    config.rgbMode = RGB_DEFAULT_MODE;
  }
  if (otaHostname.length() == 0) {
    otaHostname = DEFAULT_OTA_HOSTNAME;
  }

  rgbR = config.rgbR;
  rgbG = config.rgbG;
  rgbB = config.rgbB;
  wheelCenter = config.wheelCenter;
  return valid;
}

void saveConfigToFlash() {
  preferences.begin("wheelcfg", false);
  preferences.putBool("valid", true);
  preferences.putUChar("ver", CONFIG_VERSION);
  preferences.putUChar("rr", config.rgbR);
  preferences.putUChar("rg", config.rgbG);
  preferences.putUChar("rb", config.rgbB);
  preferences.putUChar("rm", config.rgbMode);
  preferences.putUChar("rs", config.rgbSpeed);
  preferences.putBool("we", config.wifiEnabled);
  preferences.putBool("pov", config.enableHidPov);
  preferences.putBool("ral", config.rgbActiveLow);
  preferences.putBool("ppu", config.pedalsUsePullup);
  preferences.putString("wssid", wifiSsid);
  preferences.putString("wpass", wifiPassword);
  preferences.putString("ohost", otaHostname);
  preferences.putString("opass", otaPassword);
  preferences.putUShort("wc", config.wheelCenter);
  preferences.putUShort("wdz", config.wheelDeadzone);
  preferences.putUChar("ws", config.wheelSensitivity);
  preferences.putUShort("p1mn", config.pedalMin[0]);
  preferences.putUShort("p1mx", config.pedalMax[0]);
  preferences.putUChar("p1s", config.pedalSensitivity[0]);
  preferences.putUShort("p2mn", config.pedalMin[1]);
  preferences.putUShort("p2mx", config.pedalMax[1]);
  preferences.putUChar("p2s", config.pedalSensitivity[1]);
  preferences.putUChar("m1min", config.motorMinPwm[0]);
  preferences.putUChar("m2min", config.motorMinPwm[1]);
  preferences.putBool("iw", config.invertWheel);
  preferences.putBool("ip1", config.invertPedal[0]);
  preferences.putBool("ip2", config.invertPedal[1]);
  preferences.end();
  configLoadedFromFlash = true;
}

uint8_t motorCommandToActualPwm(uint8_t commandPwm, uint8_t minPwm) {
  if (commandPwm == 0) {
    return 0;
  }
  if (minPwm >= MOTOR_PWM_MAX) {
    return MOTOR_PWM_MAX;
  }

  return static_cast<uint8_t>(
      minPwm + ((static_cast<uint16_t>(commandPwm) * (MOTOR_PWM_MAX - minPwm)) / MOTOR_PWM_MAX));
}

void applyMotorPwm(uint8_t leftPwm, uint8_t rightPwm) {
  motorLeftPwm = leftPwm;
  motorRightPwm = rightPwm;
  motorLeftActualPwm = motorCommandToActualPwm(motorLeftPwm, config.motorMinPwm[0]);
  motorRightActualPwm = motorCommandToActualPwm(motorRightPwm, config.motorMinPwm[1]);
  analogWrite(MOTOR_PINS[0], motorLeftActualPwm);
  analogWrite(MOTOR_PINS[1], motorRightActualPwm);
}

void setMotorRumble(uint8_t leftPwm, uint8_t rightPwm) {
  applyMotorPwm(leftPwm, rightPwm);
}

void updateMotorPattern() {
  if (!MOTOR_TEST_PATTERN_ENABLED) {
    applyMotorPwm(motorLeftPwm, motorRightPwm);
    return;
  }

  const uint32_t now = millis();
  const uint32_t elapsed = now - motorStateStartedMs;

  if (motorState == MOTOR_HOLD_FULL) {
    applyMotorPwm(MOTOR_PWM_MAX, MOTOR_PWM_MAX);
    if (elapsed >= MOTOR_HOLD_MS) {
      motorState = MOTOR_RAMP_DOWN;
      motorStateStartedMs = now;
    }
    return;
  }

  if (elapsed >= MOTOR_RAMP_MS) {
    motorState = MOTOR_HOLD_FULL;
    motorStateStartedMs = now;
    applyMotorPwm(MOTOR_PWM_MAX, MOTOR_PWM_MAX);
    return;
  }

  const uint8_t pwm = static_cast<uint8_t>(
      (static_cast<uint32_t>(MOTOR_PWM_MAX) * (MOTOR_RAMP_MS - elapsed)) / MOTOR_RAMP_MS);
  applyMotorPwm(pwm, pwm);
}

void writeRgbChannel(uint8_t pin, uint8_t value) {
  analogWrite(pin, config.rgbActiveLow ? (255 - value) : value);
}

void writeRgbColor(uint8_t r, uint8_t g, uint8_t b) {
  rgbR = r;
  rgbG = g;
  rgbB = b;
  writeRgbChannel(RGB_R_PIN, rgbR);
  writeRgbChannel(RGB_G_PIN, rgbG);
  writeRgbChannel(RGB_B_PIN, rgbB);
}

void setRgbColor(uint8_t r, uint8_t g, uint8_t b) {
  config.rgbR = r;
  config.rgbG = g;
  config.rgbB = b;
  writeRgbColor(config.rgbR, config.rgbG, config.rgbB);
}

void setRgb(bool rOn, bool gOn, bool bOn) {
  writeRgbColor(rOn ? 255 : 0, gOn ? 255 : 0, bOn ? 255 : 0);
}

void hsvToRgb(uint8_t hue, uint8_t sat, uint8_t val, uint8_t &r, uint8_t &g, uint8_t &b) {
  const uint8_t region = hue / 43;
  const uint8_t remainder = (hue - (region * 43)) * 6;
  const uint8_t p = (static_cast<uint16_t>(val) * (255 - sat)) >> 8;
  const uint8_t q = (static_cast<uint16_t>(val) * (255 - ((static_cast<uint16_t>(sat) * remainder) >> 8))) >> 8;
  const uint8_t t = (static_cast<uint16_t>(val) * (255 - ((static_cast<uint16_t>(sat) * (255 - remainder)) >> 8))) >> 8;

  switch (region) {
    case 0: r = val; g = t; b = p; break;
    case 1: r = q; g = val; b = p; break;
    case 2: r = p; g = val; b = t; break;
    case 3: r = p; g = q; b = val; break;
    case 4: r = t; g = p; b = val; break;
    default: r = val; g = p; b = q; break;
  }
}

uint8_t triangleWave(uint32_t phase, uint32_t period) {
  if (period == 0) {
    return 255;
  }
  phase %= period;
  const uint32_t half = period / 2;
  if (half == 0) {
    return 255;
  }
  if (phase < half) {
    return static_cast<uint8_t>((phase * 255UL) / half);
  }
  return static_cast<uint8_t>(((period - phase) * 255UL) / half);
}

uint32_t scaledPeriod(uint32_t baseMs) {
  return (baseMs * 100UL) / clampRgbSpeed(config.rgbSpeed);
}

void updateRgbBlink() {
  const uint32_t now = millis();
  uint8_t r = config.rgbR;
  uint8_t g = config.rgbG;
  uint8_t b = config.rgbB;

  switch (config.rgbMode) {
    case 0:
      writeRgbColor(0, 0, 0);
      return;

    case 1:
      writeRgbColor(config.rgbR, config.rgbG, config.rgbB);
      return;

    case 2: {
      const uint32_t period = scaledPeriod(5000);
      const uint8_t hue = static_cast<uint8_t>((now % period) * 255UL / period);
      hsvToRgb(hue, 255, 255, r, g, b);
      writeRgbColor(r, g, b);
      return;
    }

    case 3: {
      const uint32_t period = scaledPeriod(2400);
      const uint8_t level = triangleWave(now, period);
      writeRgbColor((static_cast<uint16_t>(config.rgbR) * level) / 255,
                    (static_cast<uint16_t>(config.rgbG) * level) / 255,
                    (static_cast<uint16_t>(config.rgbB) * level) / 255);
      return;
    }

    case 4: {
      const uint32_t period = scaledPeriod(3500);
      const uint8_t hue = static_cast<uint8_t>((now % period) * 255UL / period);
      const uint8_t level = 90 + ((triangleWave(now + period / 4, period) * 165U) / 255U);
      hsvToRgb(hue, 255, level, r, g, b);
      writeRgbColor(r, g, b);
      return;
    }

    case 5: {
      const uint32_t period = scaledPeriod(1100);
      uint8_t level = triangleWave(now, period);
      level = (static_cast<uint16_t>(level) * level) / 255;
      writeRgbColor((static_cast<uint16_t>(config.rgbR) * level) / 255,
                    (static_cast<uint16_t>(config.rgbG) * level) / 255,
                    (static_cast<uint16_t>(config.rgbB) * level) / 255);
      return;
    }
  }

  static uint32_t lastStepMs = 0;
  static uint8_t step = 0;

  if (now - lastStepMs < RGB_STEP_MS) {
    return;
  }
  lastStepMs = now;

  switch (step) {
    case 0: setRgb(true, false, false); break;   // rojo
    case 1: setRgb(false, false, false); break;  // off
    case 2: setRgb(false, true, false); break;   // verde
    case 3: setRgb(false, false, false); break;  // off
    case 4: setRgb(false, false, true); break;   // azul
    case 5: setRgb(false, false, false); break;  // off
    default: setRgb(true, true, true); break;    // blanco
  }

  step = (step + 1) % 7;
}

String wifiStatusText() {
  if (!config.wifiEnabled || WiFi.getMode() == WIFI_OFF) {
    return "OFF";
  }

  switch (WiFi.status()) {
    case WL_CONNECTED: return "CONNECTED";
    case WL_NO_SSID_AVAIL: return "NO_SSID";
    case WL_CONNECT_FAILED: return "FAILED";
    case WL_CONNECTION_LOST: return "LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    case WL_IDLE_STATUS: return "IDLE";
    default: return "UNKNOWN";
  }
}

void stopOtaService() {
  if (!otaStarted) {
    return;
  }
  ArduinoOTA.end();
  otaStarted = false;
}

void startOtaService() {
  if (otaStarted || !config.wifiEnabled || WiFi.status() != WL_CONNECTED) {
    return;
  }

  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.setHostname(otaHostname.c_str());
  if (otaPassword.length() > 0) {
    ArduinoOTA.setPassword(otaPassword.c_str());
  } else {
    ArduinoOTA.setPassword("");
  }
  ArduinoOTA.setMdnsEnabled(true);
  ArduinoOTA.onStart([]() {
    setMotorRumble(0, 0);
#if ENABLE_SERIAL_DEBUG
    Serial.println(F("OTA START"));
#endif
  });
  ArduinoOTA.onEnd([]() {
#if ENABLE_SERIAL_DEBUG
    Serial.println(F("OTA END"));
#endif
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
#if ENABLE_SERIAL_DEBUG
    static uint8_t lastPercent = 255;
    const uint8_t percent = total == 0 ? 0 : static_cast<uint8_t>((progress * 100U) / total);
    if (percent != lastPercent && (percent % 10 == 0 || percent == 100)) {
      lastPercent = percent;
      Serial.print(F("OTA PROGRESS "));
      Serial.println(percent);
    }
#endif
  });
  ArduinoOTA.onError([](ota_error_t error) {
#if ENABLE_SERIAL_DEBUG
    Serial.print(F("OTA ERROR "));
    Serial.println(static_cast<int>(error));
#endif
  });
  ArduinoOTA.begin();
  otaStarted = true;
#if ENABLE_SERIAL_DEBUG
  Serial.print(F("OTA READY IP="));
  Serial.print(WiFi.localIP());
  Serial.print(F(" HOST="));
  Serial.println(otaHostname);
#endif
}

void beginWifiConnection(bool forceRestart) {
  if (!config.wifiEnabled || wifiSsid.length() == 0) {
    return;
  }

  if (forceRestart) {
    stopOtaService();
    WiFi.disconnect(false);
    delay(20);
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  lastWifiAttemptMs = millis();
}

void disableWifi() {
  stopOtaService();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void updateWifiAndOta() {
  if (!config.wifiEnabled) {
    if (WiFi.getMode() != WIFI_OFF) {
      disableWifi();
    }
    return;
  }

  if (wifiSsid.length() == 0) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    startOtaService();
    if (otaStarted) {
      ArduinoOTA.handle();
    }
    return;
  }

  stopOtaService();
  const uint32_t now = millis();
  if (lastWifiAttemptMs == 0 || now - lastWifiAttemptMs >= WIFI_RECONNECT_INTERVAL_MS) {
    beginWifiConnection(true);
  }
}

#if ENABLE_SERIAL_DEBUG
bool commandEquals(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    const char ca = static_cast<char>(toupper(static_cast<unsigned char>(*a)));
    const char cb = static_cast<char>(toupper(static_cast<unsigned char>(*b)));
    if (ca != cb) {
      return false;
    }
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

bool parseIntegerToken(const char *token, int32_t &value) {
  if (token == nullptr) {
    return false;
  }

  char *end = nullptr;
  const long parsed = strtol(token, &end, 10);
  if (end == token) {
    return false;
  }

  value = parsed;
  return true;
}

char *skipSerialArgSeparators(char *cursor) {
  while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') {
    ++cursor;
  }
  return cursor;
}

bool readSerialArg(char *&cursor, String &out) {
  cursor = skipSerialArgSeparators(cursor);
  out = "";
  if (*cursor == '\0') {
    return false;
  }

  if (*cursor == '"') {
    ++cursor;
    while (*cursor != '\0' && *cursor != '"') {
      if (*cursor == '\\' && *(cursor + 1) != '\0') {
        ++cursor;
      }
      out += *cursor;
      ++cursor;
    }
    if (*cursor == '"') {
      ++cursor;
    }
    return true;
  }

  while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' && *cursor != ',') {
    out += *cursor;
    ++cursor;
  }
  return true;
}

uint8_t parsePedalIndex(const char *token) {
  if (token == nullptr) {
    return 255;
  }
  if (commandEquals(token, "P1") || commandEquals(token, "PEDAL1") || commandEquals(token, "GPIO2")) {
    return 0;
  }
  if (commandEquals(token, "P2") || commandEquals(token, "PEDAL2") || commandEquals(token, "GPIO3")) {
    return 1;
  }
  return 255;
}

uint8_t parseRgbMode(const char *token) {
  if (token == nullptr) {
    return 255;
  }
  if (commandEquals(token, "0") || commandEquals(token, "OFF") || commandEquals(token, "DESACTIVADO")) {
    return 0;
  }
  if (commandEquals(token, "1") || commandEquals(token, "FIXED") || commandEquals(token, "FIJO")) {
    return 1;
  }
  if (commandEquals(token, "2") || commandEquals(token, "CYCLE") || commandEquals(token, "CICLO")) {
    return 2;
  }
  if (commandEquals(token, "3") || commandEquals(token, "BREATH") || commandEquals(token, "RESPIRACION")) {
    return 3;
  }
  if (commandEquals(token, "4") || commandEquals(token, "WAVE") || commandEquals(token, "ONDA")) {
    return 4;
  }
  if (commandEquals(token, "5") || commandEquals(token, "PULSE") || commandEquals(token, "PUNTO")) {
    return 5;
  }
  return 255;
}

const __FlashStringHelper *rgbModeName(uint8_t mode) {
  switch (mode) {
    case 0: return F("OFF");
    case 1: return F("FIXED");
    case 2: return F("CYCLE");
    case 3: return F("BREATH");
    case 4: return F("WAVE");
    case 5: return F("PULSE");
    default: return F("UNKNOWN");
  }
}

void printConfigLine();
void printStatusLine();
void printWifiLine();

void handleSerialCommand(char *line) {
  char originalLine[sizeof(serialCommandLine)];
  strncpy(originalLine, line, sizeof(originalLine) - 1);
  originalLine[sizeof(originalLine) - 1] = '\0';

  char *cmd = strtok(line, " \t,");
  if (cmd == nullptr) {
    return;
  }

  if (commandEquals(cmd, "R") || commandEquals(cmd, "RGB")) {
    int32_t r = 0;
    int32_t g = 0;
    int32_t b = 0;
    if (!parseIntegerToken(strtok(nullptr, " \t,"), r)
        || !parseIntegerToken(strtok(nullptr, " \t,"), g)
        || !parseIntegerToken(strtok(nullptr, " \t,"), b)) {
      Serial.println(F("ERR RGB usage: RGB r g b"));
      return;
    }

    config.rgbR = static_cast<uint8_t>(constrain(r, 0, 255));
    config.rgbG = static_cast<uint8_t>(constrain(g, 0, 255));
    config.rgbB = static_cast<uint8_t>(constrain(b, 0, 255));
    setRgbColor(config.rgbR, config.rgbG, config.rgbB);
    saveConfigToFlash();
    Serial.println(F("OK RGB"));
    printConfigLine();
    return;
  }

  if (commandEquals(cmd, "RGBMODE")) {
    const uint8_t mode = parseRgbMode(strtok(nullptr, " \t,"));
    if (mode > 5) {
      Serial.println(F("ERR RGBMODE usage: RGBMODE OFF/FIXED/CYCLE/BREATH/WAVE/PULSE"));
      return;
    }
    config.rgbMode = mode;
    saveConfigToFlash();
    Serial.println(F("OK RGBMODE"));
    printConfigLine();
    return;
  }

  if (commandEquals(cmd, "RGBSPEED")) {
    int32_t speed = RGB_DEFAULT_SPEED;
    if (!parseIntegerToken(strtok(nullptr, " \t,"), speed)) {
      Serial.println(F("ERR RGBSPEED usage: RGBSPEED 10..200"));
      return;
    }
    config.rgbSpeed = clampRgbSpeed(speed);
    saveConfigToFlash();
    Serial.println(F("OK RGBSPEED"));
    printConfigLine();
    return;
  }

  if (commandEquals(cmd, "M") || commandEquals(cmd, "MOTOR")) {
    int32_t left = 0;
    int32_t right = 0;
    if (!parseIntegerToken(strtok(nullptr, " \t,"), left)
        || !parseIntegerToken(strtok(nullptr, " \t,"), right)) {
      Serial.println(F("ERR MOTOR usage: M left right"));
      return;
    }

    setMotorRumble(static_cast<uint8_t>(constrain(left, 0, 255)),
                   static_cast<uint8_t>(constrain(right, 0, 255)));
    Serial.println(F("OK MOTOR"));
    return;
  }

  if (commandEquals(cmd, "MMIN") || commandEquals(cmd, "MOTOR_MIN")) {
    int32_t left = 0;
    int32_t right = 0;
    if (!parseIntegerToken(strtok(nullptr, " \t,"), left)
        || !parseIntegerToken(strtok(nullptr, " \t,"), right)) {
      Serial.println(F("ERR MOTOR_MIN usage: MMIN left right"));
      return;
    }

    config.motorMinPwm[0] = static_cast<uint8_t>(constrain(left, 0, 255));
    config.motorMinPwm[1] = static_cast<uint8_t>(constrain(right, 0, 255));
    applyMotorPwm(motorLeftPwm, motorRightPwm);
    saveConfigToFlash();
    Serial.println(F("OK MOTOR_MIN"));
    printConfigLine();
    return;
  }

  if (commandEquals(cmd, "CFG?") || commandEquals(cmd, "CONFIG?") || commandEquals(cmd, "CONFIG")) {
    printConfigLine();
    return;
  }

  if (commandEquals(cmd, "STAT?") || commandEquals(cmd, "STATUS?") || commandEquals(cmd, "STATUS")) {
    printStatusLine();
    return;
  }

  if (commandEquals(cmd, "WIFI?")) {
    printWifiLine();
    return;
  }

  if (commandEquals(cmd, "WIFI")) {
    char *cursor = originalLine;
    String parsedCommand;
    String parsedSsid;
    String parsedPassword;
    readSerialArg(cursor, parsedCommand);
    if (!readSerialArg(cursor, parsedSsid) || parsedSsid.length() == 0) {
      printWifiLine();
      return;
    }
    if (!readSerialArg(cursor, parsedPassword)) {
      parsedPassword = "";
    }

    wifiSsid = parsedSsid;
    wifiPassword = parsedPassword;
    config.wifiEnabled = true;
    saveConfigToFlash();
    beginWifiConnection(true);
    Serial.println(F("OK WIFI"));
    printWifiLine();
    return;
  }

  if (commandEquals(cmd, "WIFIOFF")) {
    config.wifiEnabled = false;
    saveConfigToFlash();
    disableWifi();
    Serial.println(F("OK WIFIOFF"));
    printWifiLine();
    return;
  }

  if (commandEquals(cmd, "WIFION")) {
    if (wifiSsid.length() == 0) {
      Serial.println(F("ERR WIFION needs saved SSID"));
      return;
    }
    config.wifiEnabled = true;
    saveConfigToFlash();
    beginWifiConnection(true);
    Serial.println(F("OK WIFION"));
    printWifiLine();
    return;
  }

  if (commandEquals(cmd, "HOST") || commandEquals(cmd, "OTAHOST")) {
    char *host = strtok(nullptr, " \t,");
    if (host == nullptr || strlen(host) == 0) {
      Serial.println(F("ERR HOST usage: HOST name"));
      return;
    }
    otaHostname = host;
    saveConfigToFlash();
    if (WiFi.status() == WL_CONNECTED) {
      stopOtaService();
      startOtaService();
    }
    Serial.println(F("OK HOST"));
    printWifiLine();
    return;
  }

  if (commandEquals(cmd, "OTAPASS")) {
    char *password = strtok(nullptr, " \t,");
    otaPassword = (password == nullptr || commandEquals(password, "-")) ? "" : password;
    saveConfigToFlash();
    if (WiFi.status() == WL_CONNECTED) {
      stopOtaService();
      startOtaService();
    }
    Serial.println(F("OK OTAPASS"));
    printWifiLine();
    return;
  }

  if (commandEquals(cmd, "BOOL")) {
    char *target = strtok(nullptr, " \t,");
    int32_t value = 0;
    if (target == nullptr || !parseIntegerToken(strtok(nullptr, " \t,"), value)) {
      Serial.println(F("ERR BOOL usage: BOOL POV/RGBAL/PULLUP 0/1"));
      return;
    }

    const bool enabled = value != 0;
    if (commandEquals(target, "POV") || commandEquals(target, "HAT")) {
      config.enableHidPov = enabled;
    } else if (commandEquals(target, "RGBAL") || commandEquals(target, "RGB_ACTIVE_LOW")) {
      config.rgbActiveLow = enabled;
      setRgbColor(config.rgbR, config.rgbG, config.rgbB);
    } else if (commandEquals(target, "PULLUP") || commandEquals(target, "PEDAL_PULLUP")) {
      config.pedalsUsePullup = enabled;
      initPotsAndWheelCenter();
    } else {
      Serial.println(F("ERR BOOL target must be POV, RGBAL or PULLUP"));
      return;
    }

    saveConfigToFlash();
    Serial.println(F("OK BOOL"));
    printConfigLine();
    return;
  }

  if (commandEquals(cmd, "QUIET")) {
    int32_t enabled = 1;
    parseIntegerToken(strtok(nullptr, " \t,"), enabled);
    serialDebugOutputEnabled = (enabled == 0);
    Serial.print(F("OK QUIET "));
    Serial.println(serialDebugOutputEnabled ? 0 : 1);
    return;
  }

  if (commandEquals(cmd, "CAL")) {
    char *target = strtok(nullptr, " \t,");
    char *point = strtok(nullptr, " \t,");
    if (target == nullptr) {
      Serial.println(F("ERR CAL usage: CAL WHEEL | CAL P1 MIN | CAL P1 MAX"));
      return;
    }

    if (commandEquals(target, "W") || commandEquals(target, "WHEEL")) {
      config.wheelCenter = readAnalogAverage(POT_PINS[WHEEL_INDEX], 32);
      wheelCenter = config.wheelCenter;
      saveConfigToFlash();
      Serial.println(F("OK CAL WHEEL"));
      printConfigLine();
      return;
    }

    const uint8_t pedal = parsePedalIndex(target);
    if (pedal > 1 || point == nullptr) {
      Serial.println(F("ERR CAL usage: CAL P1 MIN | CAL P1 MAX | CAL P2 MIN | CAL P2 MAX"));
      return;
    }

    const uint16_t raw = readAnalogAverage(POT_PINS[pedal], 16);
    if (commandEquals(point, "MIN")) {
      config.pedalMin[pedal] = raw;
    } else if (commandEquals(point, "MAX")) {
      config.pedalMax[pedal] = raw;
    } else {
      Serial.println(F("ERR CAL point must be MIN or MAX"));
      return;
    }
    saveConfigToFlash();
    Serial.println(F("OK CAL PEDAL"));
    printConfigLine();
    return;
  }

  if (commandEquals(cmd, "SENS")) {
    char *target = strtok(nullptr, " \t,");
    int32_t value = 100;
    if (target == nullptr || !parseIntegerToken(strtok(nullptr, " \t,"), value)) {
      Serial.println(F("ERR SENS usage: SENS W 100 | SENS P1 100 | SENS P2 100"));
      return;
    }

    if (commandEquals(target, "W") || commandEquals(target, "WHEEL")) {
      config.wheelSensitivity = clampSensitivity(value);
    } else {
      const uint8_t pedal = parsePedalIndex(target);
      if (pedal > 1) {
        Serial.println(F("ERR SENS target must be W, P1 or P2"));
        return;
      }
      config.pedalSensitivity[pedal] = clampSensitivity(value);
    }
    saveConfigToFlash();
    Serial.println(F("OK SENS"));
    printConfigLine();
    return;
  }

  if (commandEquals(cmd, "DEADZONE") || commandEquals(cmd, "DZ")) {
    int32_t value = DEFAULT_WHEEL_DEADZONE;
    if (!parseIntegerToken(strtok(nullptr, " \t,"), value)) {
      Serial.println(F("ERR DEADZONE usage: DEADZONE value"));
      return;
    }
    config.wheelDeadzone = clampAdc(value);
    saveConfigToFlash();
    Serial.println(F("OK DEADZONE"));
    printConfigLine();
    return;
  }

  if (commandEquals(cmd, "INV") || commandEquals(cmd, "INVERT")) {
    char *target = strtok(nullptr, " \t,");
    int32_t value = 0;
    if (target == nullptr || !parseIntegerToken(strtok(nullptr, " \t,"), value)) {
      Serial.println(F("ERR INV usage: INV W 0/1 | INV P1 0/1 | INV P2 0/1"));
      return;
    }

    const bool enabled = value != 0;
    if (commandEquals(target, "W") || commandEquals(target, "WHEEL")) {
      config.invertWheel = enabled;
    } else {
      const uint8_t pedal = parsePedalIndex(target);
      if (pedal > 1) {
        Serial.println(F("ERR INV target must be W, P1 or P2"));
        return;
      }
      config.invertPedal[pedal] = enabled;
    }
    saveConfigToFlash();
    Serial.println(F("OK INVERT"));
    printConfigLine();
    return;
  }

  if (commandEquals(cmd, "SAVE")) {
    saveConfigToFlash();
    Serial.println(F("OK SAVE"));
    printConfigLine();
    return;
  }

  if (commandEquals(cmd, "RESETCFG")) {
    setDefaultConfig();
    disableWifi();
    config.wheelCenter = readAnalogAverage(POT_PINS[WHEEL_INDEX], 32);
    wheelCenter = config.wheelCenter;
    setRgbColor(config.rgbR, config.rgbG, config.rgbB);
    saveConfigToFlash();
    Serial.println(F("OK RESETCFG"));
    printConfigLine();
    return;
  }

  if (commandEquals(cmd, "HELP") || commandEquals(cmd, "?")) {
    Serial.println(F("CMDS: RGB r g b | RGBMODE OFF/FIXED/CYCLE/BREATH/WAVE/PULSE | RGBSPEED 10..200 | M l r | MMIN l r | WIFI ssid pass | WIFION | WIFIOFF | WIFI? | HOST name | OTAPASS pass/- | BOOL POV/RGBAL/PULLUP 0/1 | CAL WHEEL | CAL P1 MIN/MAX | CAL P2 MIN/MAX | SENS W/P1/P2 value | DEADZONE value | INV W/P1/P2 0/1 | CFG? | STATUS? | QUIET 0/1 | RESETCFG"));
    return;
  }

  Serial.print(F("ERR unknown command: "));
  Serial.println(cmd);
}

void processSerialCommands() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') {
      continue;
    }

    if (c == '\n') {
      serialCommandLine[serialCommandLen] = '\0';
      handleSerialCommand(serialCommandLine);
      serialCommandLen = 0;
      continue;
    }

    if (serialCommandLen + 1 < sizeof(serialCommandLine)) {
      serialCommandLine[serialCommandLen++] = c;
    } else {
      serialCommandLen = 0;
      Serial.println(F("ERR command too long"));
    }
  }
}
#endif

uint8_t crc8Update(uint8_t crc, uint8_t data) {
  crc ^= data;
  for (uint8_t i = 0; i < 8; ++i) {
    if (crc & 0x80) {
      crc = static_cast<uint8_t>((crc << 1) ^ 0x07);
    } else {
      crc <<= 1;
    }
  }
  return crc;
}

void resetParser() {
  rxState = WAIT_SYNC_1;
  rxSeq = 0;
  rxCount = 0;
  rxPayloadIndex = 0;
  rxCrc = 0;
}

void onNanoFrame(uint8_t seq, uint8_t count, const uint8_t *payload) {
  const uint8_t bytes = (count + 7) / 8;
  nanoButtonCount = count;
  nanoSeq = seq;
  for (uint8_t i = 0; i < bytes; ++i) {
    nanoButtons[i] = payload[i];
  }
  for (uint8_t i = bytes; i < NANO_MAX_BYTES; ++i) {
    nanoButtons[i] = 0;
  }
  lastNanoFrameMs = millis();
}

void processNanoSerial() {
  while (NanoSerial.available() > 0) {
    const uint8_t b = static_cast<uint8_t>(NanoSerial.read());
    ++rxByteCount;

    switch (rxState) {
      case WAIT_SYNC_1:
        if (b == FRAME_SYNC_1) {
          rxState = WAIT_SYNC_2;
        }
        break;

      case WAIT_SYNC_2:
        if (b == FRAME_SYNC_2) {
          rxState = WAIT_SEQ;
        } else {
          rxState = WAIT_SYNC_1;
        }
        break;

      case WAIT_SEQ:
        rxSeq = b;
        rxCrc = 0;
        rxCrc = crc8Update(rxCrc, rxSeq);
        rxState = WAIT_COUNT;
        break;

      case WAIT_COUNT:
        rxCount = b;
        if (rxCount == 0 || rxCount > NANO_MAX_BUTTONS) {
          ++rxFrameInvalidCount;
          resetParser();
          break;
        }
        rxCrc = crc8Update(rxCrc, rxCount);
        rxPayloadIndex = 0;
        rxState = WAIT_PAYLOAD;
        break;

      case WAIT_PAYLOAD: {
        rxPayload[rxPayloadIndex++] = b;
        rxCrc = crc8Update(rxCrc, b);
        const uint8_t needed = (rxCount + 7) / 8;
        if (rxPayloadIndex >= needed) {
          rxState = WAIT_CRC;
        }
      } break;

      case WAIT_CRC:
        if (b == rxCrc) {
          ++rxFrameOkCount;
          onNanoFrame(rxSeq, rxCount, rxPayload);
        } else {
          ++rxFrameCrcErrCount;
        }
        resetParser();
        break;
    }
  }
}

void readLocalButtons() {
  for (uint8_t i = 0; i < LOCAL_BUTTON_BYTES; ++i) {
    localButtons[i] = 0;
  }

  for (uint8_t i = 0; i < LOCAL_BUTTON_COUNT; ++i) {
    const int state = digitalRead(LOCAL_BUTTON_PINS[i]);
    const bool pressed = LOCAL_BUTTON_ACTIVE_LOW[i] ? (state == LOW) : (state == HIGH);
    if (pressed) {
      localButtons[i >> 3] |= static_cast<uint8_t>(1U << (i & 7U));
    }
  }
}

void readPots() {
  for (uint8_t i = 0; i < POT_COUNT; ++i) {
    const uint16_t raw = static_cast<uint16_t>(analogRead(POT_PINS[i]));
    pots[i] = static_cast<uint16_t>(
        ((static_cast<uint32_t>(pots[i]) * ((1U << POT_FILTER_SHIFT) - 1U)) + raw) >> POT_FILTER_SHIFT);
  }
}

bool isBitSet(const uint8_t *packed, uint8_t index) {
  return (packed[index >> 3] & static_cast<uint8_t>(1U << (index & 7U))) != 0;
}

uint16_t readAnalogAverage(uint8_t pin, uint8_t samples) {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < samples; ++i) {
    sum += analogRead(pin);
    delay(2);
  }
  return static_cast<uint16_t>(sum / samples);
}

void initPotsAndWheelCenter() {
  for (uint8_t i = 0; i < POT_COUNT; ++i) {
    const bool isPedal = (i == PEDAL_1_INDEX) || (i == PEDAL_2_INDEX);
    pinMode(POT_PINS[i], (config.pedalsUsePullup && isPedal) ? INPUT_PULLUP : INPUT);
    pots[i] = readAnalogAverage(POT_PINS[i], 8);
  }

  // Si todavia no hay config guardada, el primer arranque toma el centro actual.
  if (!configLoadedFromFlash) {
    config.wheelCenter = readAnalogAverage(POT_PINS[WHEEL_INDEX], 64);
  }
  wheelCenter = config.wheelCenter;
}

int8_t clampAxis(int32_t value) {
  if (value > 127) {
    return 127;
  }
  if (value < -127) {
    return -127;
  }
  return static_cast<int8_t>(value);
}

int8_t mapPedalAxis(uint16_t value, bool invert) {
  int32_t axis = (static_cast<int32_t>(value) * 254L / ADC_MAX_VALUE) - 127L;
  if (invert) {
    axis = -axis;
  }
  return clampAxis(axis);
}

int8_t applyAxisSensitivity(int32_t axis, uint8_t sensitivity) {
  axis = axis * static_cast<int32_t>(sensitivity) / 100L;
  return clampAxis(axis);
}

int8_t mapPedalAxisCalibrated(uint8_t pedalIndex, uint16_t value) {
  const int32_t minValue = config.pedalMin[pedalIndex];
  const int32_t maxValue = config.pedalMax[pedalIndex];
  const int32_t range = maxValue - minValue;
  if (abs(range) < 8) {
    return 0;
  }

  int32_t axis = ((static_cast<int32_t>(value) - minValue) * 254L / range) - 127L;
  if (config.invertPedal[pedalIndex]) {
    axis = -axis;
  }
  return applyAxisSensitivity(axis, config.pedalSensitivity[pedalIndex]);
}

int8_t mapWheelAxis(uint16_t value) {
  int32_t delta = static_cast<int32_t>(value) - static_cast<int32_t>(wheelCenter);
  if (abs(delta) <= config.wheelDeadzone) {
    return 0;
  }

  int32_t span = (delta > 0)
      ? static_cast<int32_t>(ADC_MAX_VALUE - wheelCenter)
      : static_cast<int32_t>(wheelCenter);
  if (span < 1) {
    span = 1;
  }
  int32_t axis = delta * 127L / span;
  if (config.invertWheel) {
    axis = -axis;
  }
  return applyAxisSensitivity(axis, config.wheelSensitivity);
}

uint32_t buildHidButtons() {
  uint32_t buttons = 0;

  const uint8_t nanoLimit =
      (nanoButtonCount < EXPECTED_NANO_BUTTONS) ? nanoButtonCount : EXPECTED_NANO_BUTTONS;
  for (uint8_t i = 0; i < nanoLimit; ++i) {
    if (config.enableHidPov
        && (i == POV_UP_INDEX || i == POV_RIGHT_INDEX || i == POV_DOWN_INDEX || i == POV_LEFT_INDEX)) {
      continue;
    }
    if (isBitSet(nanoButtons, i)) {
      buttons |= (1UL << i);
    }
  }

  for (uint8_t i = 0; i < LOCAL_BUTTON_COUNT; ++i) {
    if (isBitSet(localButtons, i)) {
      buttons |= (1UL << (EXPECTED_NANO_BUTTONS + i));
    }
  }

  return buttons;
}

uint8_t buildHidHat() {
  if (!config.enableHidPov) {
    return HAT_CENTER;
  }

  const bool up = isBitSet(nanoButtons, POV_UP_INDEX);
  const bool right = isBitSet(nanoButtons, POV_RIGHT_INDEX);
  const bool down = isBitSet(nanoButtons, POV_DOWN_INDEX);
  const bool left = isBitSet(nanoButtons, POV_LEFT_INDEX);

  if (up && right) {
    return HAT_UP_RIGHT;
  }
  if (down && right) {
    return HAT_DOWN_RIGHT;
  }
  if (down && left) {
    return HAT_DOWN_LEFT;
  }
  if (up && left) {
    return HAT_UP_LEFT;
  }
  if (up) {
    return HAT_UP;
  }
  if (right) {
    return HAT_RIGHT;
  }
  if (down) {
    return HAT_DOWN;
  }
  if (left) {
    return HAT_LEFT;
  }
  return HAT_CENTER;
}

void updateHidGamepad() {
#if USB_HID_GAMEPAD_AVAILABLE
  static uint32_t lastHidReportMs = 0;
  const uint32_t now = millis();
  if (now - lastHidReportMs < HID_REPORT_INTERVAL_MS) {
    return;
  }
  lastHidReportMs = now;

  const int8_t wheel = mapWheelAxis(pots[WHEEL_INDEX]);
  const int8_t pedal1 = mapPedalAxisCalibrated(PEDAL_1_INDEX, pots[PEDAL_1_INDEX]);
  const int8_t pedal2 = mapPedalAxisCalibrated(PEDAL_2_INDEX, pots[PEDAL_2_INDEX]);
  const uint32_t buttons = buildHidButtons();
  const uint8_t hat = buildHidHat();

  // Pedales solo como triggers Rx/Ry. No duplicar en Z/Rz porque varios juegos
  // interpretan Z/Rz como stick derecho.
  Gamepad.send(wheel, 0, 0, 0, pedal1, pedal2, hat, buttons);
#endif
}

#if ENABLE_SERIAL_DEBUG
void printStringOrDash(const String &value) {
  if (value.length() == 0) {
    Serial.print('-');
    return;
  }
  Serial.print(value);
}

void printConfigLine() {
  Serial.print(F("CONFIG RGB="));
  Serial.print(config.rgbR);
  Serial.print(',');
  Serial.print(config.rgbG);
  Serial.print(',');
  Serial.print(config.rgbB);
  Serial.print(F(" RGB_MODE="));
  Serial.print(rgbModeName(config.rgbMode));
  Serial.print(F(" RGB_SPEED="));
  Serial.print(config.rgbSpeed);
  Serial.print(F(" WIFI_EN="));
  Serial.print(config.wifiEnabled ? 1 : 0);
  Serial.print(F(" POV="));
  Serial.print(config.enableHidPov ? 1 : 0);
  Serial.print(F(" RGB_AL="));
  Serial.print(config.rgbActiveLow ? 1 : 0);
  Serial.print(F(" PULLUP="));
  Serial.print(config.pedalsUsePullup ? 1 : 0);
  Serial.print(F(" WIFI_SSID="));
  printStringOrDash(wifiSsid);
  Serial.print(F(" OTA_HOST="));
  Serial.print(otaHostname);
  Serial.print(F(" OTA_PASS="));
  Serial.print(otaPassword.length() > 0 ? 1 : 0);
  Serial.print(F(" WC="));
  Serial.print(config.wheelCenter);
  Serial.print(F(" WDZ="));
  Serial.print(config.wheelDeadzone);
  Serial.print(F(" WSENS="));
  Serial.print(config.wheelSensitivity);
  Serial.print(F(" WINV="));
  Serial.print(config.invertWheel ? 1 : 0);
  Serial.print(F(" P1MIN="));
  Serial.print(config.pedalMin[0]);
  Serial.print(F(" P1MAX="));
  Serial.print(config.pedalMax[0]);
  Serial.print(F(" P1SENS="));
  Serial.print(config.pedalSensitivity[0]);
  Serial.print(F(" P1INV="));
  Serial.print(config.invertPedal[0] ? 1 : 0);
  Serial.print(F(" P2MIN="));
  Serial.print(config.pedalMin[1]);
  Serial.print(F(" P2MAX="));
  Serial.print(config.pedalMax[1]);
  Serial.print(F(" P2SENS="));
  Serial.print(config.pedalSensitivity[1]);
  Serial.print(F(" P2INV="));
  Serial.print(config.invertPedal[1] ? 1 : 0);
  Serial.print(F(" MMIN="));
  Serial.print(config.motorMinPwm[0]);
  Serial.print(',');
  Serial.print(config.motorMinPwm[1]);
  Serial.print(F(" SAVED="));
  Serial.println(configLoadedFromFlash ? 1 : 0);
}

void printStatusLine() {
  const int8_t wheelAxis = mapWheelAxis(pots[WHEEL_INDEX]);
  const int8_t pedal1Axis = mapPedalAxisCalibrated(PEDAL_1_INDEX, pots[PEDAL_1_INDEX]);
  const int8_t pedal2Axis = mapPedalAxisCalibrated(PEDAL_2_INDEX, pots[PEDAL_2_INDEX]);
  const uint32_t age = (lastNanoFrameMs == 0) ? 0xFFFFFFFFUL : (millis() - lastNanoFrameMs);
  const bool linkUp = (age != 0xFFFFFFFFUL) && (age < 300);

  Serial.print(F("STATUS WRAW="));
  Serial.print(pots[WHEEL_INDEX]);
  Serial.print(F(" WAXIS="));
  Serial.print(wheelAxis);
  Serial.print(F(" P1RAW="));
  Serial.print(pots[PEDAL_1_INDEX]);
  Serial.print(F(" P1AXIS="));
  Serial.print(pedal1Axis);
  Serial.print(F(" P2RAW="));
  Serial.print(pots[PEDAL_2_INDEX]);
  Serial.print(F(" P2AXIS="));
  Serial.print(pedal2Axis);
  Serial.print(F(" BTN=0x"));
  Serial.print(buildHidButtons(), HEX);
  Serial.print(F(" HAT="));
  Serial.print(buildHidHat());
  Serial.print(F(" RGB="));
  Serial.print(rgbR);
  Serial.print(',');
  Serial.print(rgbG);
  Serial.print(',');
  Serial.print(rgbB);
  Serial.print(F(" MOTOR="));
  Serial.print(motorLeftPwm);
  Serial.print(',');
  Serial.print(motorRightPwm);
  Serial.print(F(" MOTOR_OUT="));
  Serial.print(motorLeftActualPwm);
  Serial.print(',');
  Serial.print(motorRightActualPwm);
  Serial.print(F(" WIFI="));
  Serial.print(wifiStatusText());
  Serial.print(F(" IP="));
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(WiFi.localIP());
  } else {
    Serial.print('-');
  }
  Serial.print(F(" RSSI="));
  Serial.print(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  Serial.print(F(" OTA="));
  Serial.print(otaStarted ? 1 : 0);
  Serial.print(F(" LINK="));
  Serial.println(linkUp ? F("UP") : F("DOWN"));
}

void printWifiLine() {
  Serial.print(F("WIFI EN="));
  Serial.print(config.wifiEnabled ? 1 : 0);
  Serial.print(F(" SSID="));
  printStringOrDash(wifiSsid);
  Serial.print(F(" STATUS="));
  Serial.print(wifiStatusText());
  Serial.print(F(" IP="));
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(WiFi.localIP());
  } else {
    Serial.print('-');
  }
  Serial.print(F(" RSSI="));
  Serial.print(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  Serial.print(F(" HOST="));
  Serial.print(otaHostname);
  Serial.print(F(" OTA="));
  Serial.print(otaStarted ? 1 : 0);
  Serial.print(F(" PORT="));
  Serial.print(OTA_PORT);
  Serial.print(F(" AUTH="));
  Serial.println(otaPassword.length() > 0 ? 1 : 0);
}
#endif

void printHexBytes(const uint8_t *data, uint8_t len) {
  for (uint8_t i = 0; i < len; ++i) {
    if (data[i] < 16) {
      Serial.print('0');
    }
    Serial.print(data[i], HEX);
    if (i + 1 < len) {
      Serial.print(' ');
    }
  }
}

bool packedChanged(const uint8_t *current, const uint8_t *previous, uint8_t len) {
  for (uint8_t i = 0; i < len; ++i) {
    if (current[i] != previous[i]) {
      return true;
    }
  }
  return false;
}

void copyPacked(uint8_t *dst, const uint8_t *src, uint8_t len) {
  for (uint8_t i = 0; i < len; ++i) {
    dst[i] = src[i];
  }
}

void printNanoEdgeEvents(const uint8_t *current, const uint8_t *previous, uint8_t bitCount) {
  bool any = false;
  for (uint8_t i = 0; i < bitCount; ++i) {
    const bool cur = isBitSet(current, i);
    const bool prev = isBitSet(previous, i);
    if (cur == prev) {
      continue;
    }

    if (!any) {
      Serial.print(F(" EVT_N=["));
      any = true;
    } else {
      Serial.print(',');
    }

    Serial.print(cur ? '+' : '-');
    Serial.print(i);
    Serial.print(':');
    if (i < EXPECTED_NANO_BUTTONS) {
      Serial.print(NANO_BUTTON_LABELS[i]);
    } else {
      Serial.print('?');
    }
  }

  if (any) {
    Serial.print(']');
  }
}

void printPressedNano(const uint8_t *packed, uint8_t bitCount) {
  Serial.print(F(" N_PRESSED=["));
  bool any = false;
  for (uint8_t i = 0; i < bitCount; ++i) {
    if ((packed[i >> 3] & static_cast<uint8_t>(1U << (i & 7U))) == 0) {
      continue;
    }
    if (any) {
      Serial.print(',');
    }
    Serial.print(i);
    Serial.print(':');
    if (i < EXPECTED_NANO_BUTTONS) {
      Serial.print(NANO_BUTTON_LABELS[i]);
    } else {
      Serial.print('?');
    }
    any = true;
  }
  if (!any) {
    Serial.print('-');
  }
  Serial.print(']');
}

void printNanoButtonMap() {
  Serial.println(F("Nano button map (index:pin):"));
  for (uint8_t i = 0; i < EXPECTED_NANO_BUTTONS; ++i) {
    Serial.print(i);
    Serial.print(':');
    Serial.println(NANO_BUTTON_LABELS[i]);
  }
  Serial.println(F("Tip: ahora el indice 11 corresponde a A0 (D13 fue removido)."));
}

void printPressedLocalPins(const uint8_t *packed, uint8_t bitCount) {
  Serial.print(F(" L_PRESSED=["));
  bool any = false;
  for (uint8_t i = 0; i < bitCount; ++i) {
    if ((packed[i >> 3] & static_cast<uint8_t>(1U << (i & 7U))) == 0) {
      continue;
    }
    if (any) {
      Serial.print(',');
    }
    Serial.print(F("GPIO"));
    Serial.print(LOCAL_BUTTON_PINS[i]);
    any = true;
  }
  if (!any) {
    Serial.print('-');
  }
  Serial.print(']');
}

void setup() {
#if USB_HID_GAMEPAD_AVAILABLE
  Gamepad.begin();
  USB.begin();
#endif
#if ENABLE_SERIAL_DEBUG
  Serial.begin(USB_BAUD);
#endif
  configLoadedFromFlash = loadConfigFromFlash();
  NanoSerial.begin(NANO_UART_BAUD, SERIAL_8N1, NANO_RX_PIN, NANO_TX_PIN);

  for (uint8_t i = 0; i < LOCAL_BUTTON_COUNT; ++i) {
    pinMode(LOCAL_BUTTON_PINS[i], LOCAL_BUTTON_ACTIVE_LOW[i] ? INPUT_PULLUP : INPUT_PULLDOWN);
  }
  for (uint8_t i = 0; i < MOTOR_PIN_COUNT; ++i) {
    pinMode(MOTOR_PINS[i], OUTPUT);
  }
  motorStateStartedMs = millis();
  setMotorRumble(MOTOR_DEFAULT_LEFT, MOTOR_DEFAULT_RIGHT);

  // Reservados para RGB
  pinMode(RGB_R_PIN, OUTPUT);
  pinMode(RGB_G_PIN, OUTPUT);
  pinMode(RGB_B_PIN, OUTPUT);
  setRgbColor(config.rgbR, config.rgbG, config.rgbB);

  analogReadResolution(12);
  initPotsAndWheelCenter();
  beginWifiConnection(false);
  resetParser();
#if ENABLE_SERIAL_DEBUG
  printNanoButtonMap();
  Serial.print(F("USB HID available="));
  Serial.print(USB_HID_GAMEPAD_AVAILABLE);
  Serial.print(F(" enabled="));
  Serial.println(ENABLE_USB_HID);
  Serial.print(F("Wheel center calibrated at ADC="));
  Serial.println(wheelCenter);
  printConfigLine();
#endif
}

void loop() {
  processNanoSerial();
#if ENABLE_SERIAL_DEBUG
  processSerialCommands();
#endif
  updateWifiAndOta();
  updateRgbBlink();
  updateMotorPattern();
  readLocalButtons();
  readPots();
  updateHidGamepad();

  static uint32_t lastPrintMs = 0;
  static uint8_t prevNanoButtons[NANO_MAX_BYTES] = {0};
  static uint8_t prevLocalButtons[LOCAL_BUTTON_BYTES] = {0};
  static uint8_t prevNanoCount = 0;

  const uint32_t now = millis();
  const uint8_t nanoBytes = (nanoButtonCount + 7) / 8;
  const bool nanoChanged =
      (nanoButtonCount != prevNanoCount) || packedChanged(nanoButtons, prevNanoButtons, nanoBytes);
  const bool localChanged = packedChanged(localButtons, prevLocalButtons, LOCAL_BUTTON_BYTES);
  const bool timedPrint = (now - lastPrintMs >= DEBUG_PRINT_INTERVAL_MS);

#if ENABLE_SERIAL_DEBUG
  if (serialDebugOutputEnabled && (nanoChanged || localChanged || timedPrint)) {
    lastPrintMs = now;

    const uint32_t age = (lastNanoFrameMs == 0) ? 0xFFFFFFFFUL : (now - lastNanoFrameMs);

    Serial.print(F("SEQ="));
    Serial.print(nanoSeq);

    Serial.print(F(" NBTN="));
    Serial.print(nanoButtonCount);
    Serial.print(F(" N=["));
    printHexBytes(nanoButtons, nanoBytes);
    Serial.print(F("]"));
    printPressedNano(nanoButtons, nanoButtonCount);
    if (nanoChanged && prevNanoCount == nanoButtonCount) {
      printNanoEdgeEvents(nanoButtons, prevNanoButtons, nanoButtonCount);
    }

    Serial.print(F(" L=["));
    printHexBytes(localButtons, LOCAL_BUTTON_BYTES);
    Serial.print(F("]"));
    printPressedLocalPins(localButtons, LOCAL_BUTTON_COUNT);

    Serial.print(F(" P=["));
    for (uint8_t i = 0; i < POT_COUNT; ++i) {
      Serial.print(POT_LABELS[i]);
      Serial.print('=');
      Serial.print(pots[i]);
      if (i + 1 < POT_COUNT) {
        Serial.print(',');
      }
    }
    Serial.print(F("]"));
    Serial.print(F(" M=[L="));
    Serial.print(motorLeftPwm);
    Serial.print(F(",R="));
    Serial.print(motorRightPwm);
    Serial.print(F(",OUTL="));
    Serial.print(motorLeftActualPwm);
    Serial.print(F(",OUTR="));
    Serial.print(motorRightActualPwm);
    Serial.print(']');
    Serial.print(F(" HID=[X="));
    const int8_t wheelAxis = mapWheelAxis(pots[WHEEL_INDEX]);
    const int8_t pedal1Axis = mapPedalAxisCalibrated(PEDAL_1_INDEX, pots[PEDAL_1_INDEX]);
    const int8_t pedal2Axis = mapPedalAxisCalibrated(PEDAL_2_INDEX, pots[PEDAL_2_INDEX]);
    Serial.print(wheelAxis);
    Serial.print(F(",Z="));
    Serial.print(0);
    Serial.print(F(",RZ="));
    Serial.print(0);
    Serial.print(F(",RX="));
    Serial.print(pedal1Axis);
    Serial.print(F(",RY="));
    Serial.print(pedal2Axis);
    Serial.print(F(",BTN=0x"));
    Serial.print(buildHidButtons(), HEX);
    Serial.print(F(",HAT="));
    Serial.print(buildHidHat());
    Serial.print(']');
    Serial.print(F(" AXIS_CHECK=[P1_GPIO2_RAW="));
    Serial.print(pots[PEDAL_1_INDEX]);
    Serial.print(F(",P1_GPIO2_DIRECT="));
    Serial.print(analogRead(POT_PINS[PEDAL_1_INDEX]));
    Serial.print(F(",GPIO2_DIG="));
    Serial.print(digitalRead(2));
    Serial.print(F(",P1_GPIO2_AXIS="));
    Serial.print(pedal1Axis);
    Serial.print(F(",P2_GPIO3_RAW="));
    Serial.print(pots[PEDAL_2_INDEX]);
    Serial.print(F(",P2_GPIO3_DIRECT="));
    Serial.print(analogRead(POT_PINS[PEDAL_2_INDEX]));
    Serial.print(F(",GPIO3_DIG="));
    Serial.print(digitalRead(3));
    Serial.print(F(",P2_GPIO3_AXIS="));
    Serial.print(pedal2Axis);
    Serial.print(F(",GPIO1_DIRECT="));
    Serial.print(analogRead(1));
    Serial.print(F(",GPIO1_DIG="));
    Serial.print(digitalRead(1));
    Serial.print(F(",WHEEL4_RAW="));
    Serial.print(pots[WHEEL_INDEX]);
    Serial.print(F(",WHEEL4_CENTER="));
    Serial.print(wheelCenter);
    Serial.print(F(",WHEEL4_AXIS="));
    Serial.print(wheelAxis);
    Serial.print(']');

    Serial.print(F(" AGEms="));
    if (age == 0xFFFFFFFFUL) {
      Serial.print(F("NA"));
    } else {
      Serial.print(age);
    }

    const bool linkUp = (age != 0xFFFFFFFFUL) && (age < 300);
    Serial.print(F(" LINK="));
    Serial.print(linkUp ? F("UP") : F("DOWN"));
    Serial.print(F(" RXB="));
    Serial.print(rxByteCount);
    Serial.print(F(" OK="));
    Serial.print(rxFrameOkCount);
    Serial.print(F(" CRCERR="));
    Serial.print(rxFrameCrcErrCount);
    Serial.print(F(" INV="));
    Serial.println(rxFrameInvalidCount);

    copyPacked(prevNanoButtons, nanoButtons, nanoBytes);
    copyPacked(prevLocalButtons, localButtons, LOCAL_BUTTON_BYTES);
    prevNanoCount = nanoButtonCount;
  }
#else
  if (nanoChanged || localChanged || timedPrint) {
    lastPrintMs = now;
    copyPacked(prevNanoButtons, nanoButtons, nanoBytes);
    copyPacked(prevLocalButtons, localButtons, LOCAL_BUTTON_BYTES);
    prevNanoCount = nanoButtonCount;
  }
#endif
}
