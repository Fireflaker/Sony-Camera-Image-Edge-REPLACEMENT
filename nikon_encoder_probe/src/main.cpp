/*
 * Absolute Encoder Probe v4 — ESP32-S3
 *
 * NOW ON 5V — encoder draws ~3.5mA at idle (confirmed alive)
 * Blue LEDs on each signal pin → GND clamp 5V to ~3V (safe for ESP32)
 *
 * REVISED PIN MAP (from short-circuit test):
 *   4-pin connector:
 *     Red      → 5V supply
 *     Black    → GND
 *     Dark Red → GPIO 4  = Encoder TX+ OUTPUT (sinks 1A → active driver)
 *     Blue     → GPIO 5  = Encoder RX+ INPUT  (high-Z → we send here)
 *
 *   2-pin connector:
 *     Purple   → GPIO 6  = Encoder TX- OUTPUT (sinks 1A → active driver)
 *     White    → GPIO 7  = Encoder RX- INPUT  (high-Z → inverted complement)
 *
 *   OLED SSD1306 128x64:
 *     SDA → GPIO 8
 *     SCL → GPIO 9
 *
 *   Full-duplex RS-422: encoder TX pair = DarkRed(4)+Purple(6)
 *                        encoder RX pair = Blue(5)+White(7)
 *   186 ohm between DarkRed and Blue = cross-termination
 *
 *   ESP32-S3 GPIO Matrix trick: route UART1 TX to GPIO5 (normal)
 *   AND GPIO7 (inverted) simultaneously for pseudo-differential TX.
 *
 * Commands (115200 baud):
 *   '1' - Passive listen (5 sec)
 *   '2' - REQ pulse + capture (various widths)
 *   '3' - Tamagawa T-format (multi-baud, multi-cmd)
 *   '4' - Clock & read bit-bang
 *   '5' - UART-style probe (async serial)
 *   '6' - Pin diagnostics & ADC
 *   '7' - BRUTE FORCE (all of the above + creative combos)
 *   'd' - PSEUDO-DIFFERENTIAL Tamagawa (GPIO matrix, best approach)
 *   'n' - Nikon A-format CDF sweep (18-bit frame brute guidance)
 *   's' - SSI/BiSS clock-sync read (encoder may need clock, not UART!)
 *   'a' - All modes 1-6
 *   'c' - Continuous read loop
 *   'h' - Toggle BAT+ (GPIO7) HIGH/LOW
 *   'u' - HardwareSerial UART mode (proper serial frames)
 *   'p' - Toggle internal pull-ups on data lines
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "soc/gpio_sig_map.h"
#include "soc/gpio_struct.h"
#include "esp_rom_gpio.h"
#include "driver/gpio.h"
#include <SPI.h>
#include <math.h>

// === OLED ===
#define OLED_SDA  8
#define OLED_SCL  9
#define OLED_ADDR 0x3C
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
static bool oledOk = false;

// === Encoder pins (RE-REVISED v4: from isolation ohmmeter data) ===
// Nikon R2EA04008 = RS-485 half-duplex (A-format), 6-wire:
//   Red(VCC +5V), Black(GND), DarkRed(RS-485 B/DATA-), Blue(RS-485 A/DATA+),
//   Orange/White(Battery+ 3.6V — leave floating), Purple(Battery GND — tie to GND)
// WIRING CHANGE REQUIRED before using mode 'h':
//   Purple (GPIO6) → connect to GND (or disconnect from ESP32)
//   Orange/White (GPIO7) → disconnect from ESP32 (floating = no battery, that's OK)
static const int PIN_ENC_TXP  = 4;  // Dark Red — RS-485 B/DATA- (inverting terminal)
static const int PIN_ENC_RXP  = 5;  // Blue     — RS-485 A/DATA+ (non-inverting terminal)
static const int PIN_ENC_TXN  = 6;  // Purple   — Battery GND (= same as Black GND)
static const int PIN_ENC_RXN  = 7;  // Orange/White — Battery+ (leave floating, no battery)

// Motor verification drive outputs (user requested):
// Use GPIO40/GPIO41/GPIO42 as 3-phase commutation steps to nudge spindle.
static const int PIN_MOTOR_DRV_A = 40;
static const int PIN_MOTOR_DRV_B = 41;
static const int PIN_MOTOR_DRV_C = 42;

// Backwards-compatible aliases for old modes
#define PIN_DATA_P PIN_ENC_TXP
#define PIN_DATA_N PIN_ENC_RXP
#define PIN_REQ    PIN_ENC_TXN
#define PIN_BAT    PIN_ENC_RXN

// === Capture buffer ===
static const int CAPTURE_LEN = 512;
static uint8_t captureBuf[CAPTURE_LEN];
static uint32_t captureTimesUs[CAPTURE_LEN];

static bool batHigh = false;
static bool pullUpsEnabled = false;
static uint32_t lastVpWord18 = 0;
static int lastVpWordCount = 0;
static bool vpAngleInit = false;
static float vpLastDeg = 0.0f;
static float vpTotalDeg = 0.0f;
static uint32_t vpAngleUpdates = 0;
static uint32_t vpLastUpdateUs = 0;
static float vpSpeedDps = 0.0f;
static float vpFiltSpeedDps = 0.0f;
static float vpLastDeltaDeg = 0.0f;
static uint32_t vpTrustedSamples = 0;
static uint32_t vpRejectedSamples = 0;

static int vpTrustedPercent() {
  uint32_t total = vpTrustedSamples + vpRejectedSamples;
  if (total == 0) return 100;
  return (int)((vpTrustedSamples * 100UL) / total);
}

static const char* vpDirectionTag() {
  if (vpFiltSpeedDps > 1.0f) return "CW";
  if (vpFiltSpeedDps < -1.0f) return "CCW";
  return "ST";
}

// Second UART for encoder communication
HardwareSerial EncoderSerial(1);  // UART1

// ============================================================
void oledShow(const char* l0, const char* l1 = nullptr,
              const char* l2 = nullptr, const char* l3 = nullptr) {
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);  if (l0) oled.println(l0);
  if (l1) { oled.setCursor(0, 16); oled.println(l1); }
  if (l2) { oled.setCursor(0, 32); oled.println(l2); }
  if (l3) { oled.setCursor(0, 48); oled.println(l3); }
  oled.display();
}

// ============================================================
void updateVpAngleFromWord(uint32_t word18, bool trustedMotion = true) {
  uint32_t data12 = (word18 >> 6) & 0x0FFFU;
  float curDeg = data12 * 360.0f / 4096.0f;
  uint32_t nowUs = micros();

  if (!vpAngleInit) {
    vpAngleInit = true;
    vpLastDeg = curDeg;
    vpTotalDeg = 0.0f;
    vpAngleUpdates = 1;
    vpLastUpdateUs = nowUs;
    vpSpeedDps = 0.0f;
    vpFiltSpeedDps = 0.0f;
    vpLastDeltaDeg = 0.0f;
    vpTrustedSamples = 0;
    vpRejectedSamples = 0;
    return;
  }

  float delta = curDeg - vpLastDeg;
  if (delta > 180.0f) delta -= 360.0f;
  else if (delta < -180.0f) delta += 360.0f;

  uint32_t dtUs = nowUs - vpLastUpdateUs;
  bool plausible = true;
  if (dtUs > 0 && dtUs < 40000U && fabsf(delta) > 120.0f) {
    plausible = false;
  }
  if (dtUs >= 1000U) {
    float dtSec = (float)dtUs / 1000000.0f;
    vpSpeedDps = delta / dtSec;
    if (fabsf(vpSpeedDps) > 1800.0f) {
      plausible = false;
    }
    // Low-pass filter to stabilize display/readout.
    vpFiltSpeedDps = (0.25f * vpSpeedDps) + (0.75f * vpFiltSpeedDps);
  }

  bool useSample = trustedMotion && plausible;
  if (useSample) {
    vpTotalDeg += delta;
    vpLastDeltaDeg = delta;
    vpTrustedSamples++;
  } else {
    // Keep readout calm on low-confidence samples.
    vpSpeedDps = 0.0f;
    vpFiltSpeedDps *= 0.92f;
    vpRejectedSamples++;
  }
  vpLastDeg = curDeg;
  vpLastUpdateUs = nowUs;
  vpAngleUpdates++;
}

// ============================================================
void oledShowVp485Candidate(uint32_t word18, int profile, int score) {
  uint32_t data12 = (word18 >> 6) & 0x0FFFU;
  float deg12 = data12 * 360.0f / 4096.0f;
  float turns = vpTotalDeg / 360.0f;

  char l1[24], l2[24], l3[24];
  snprintf(l1, sizeof(l1), "W:%05lX P:%d", (unsigned long)word18, profile);
  snprintf(l2, sizeof(l2), "Now:%6.2f Q:%3d%%", deg12, vpTrustedPercent());
  snprintf(l3, sizeof(l3), "%s %6.1fd/s T:%4.2f", vpDirectionTag(), vpFiltSpeedDps, turns);
  oledShow("VP485 Candidate", l1, l2, l3);
}

// ============================================================
void motorVerifyNudge(int cycles, int halfUs) {
  pinMode(PIN_MOTOR_DRV_A, OUTPUT);
  pinMode(PIN_MOTOR_DRV_B, OUTPUT);
  pinMode(PIN_MOTOR_DRV_C, OUTPUT);

  for (int i = 0; i < cycles; i++) {
    // 3-phase single-hot sequence: A -> B -> C
    digitalWrite(PIN_MOTOR_DRV_A, HIGH);
    digitalWrite(PIN_MOTOR_DRV_B, LOW);
    digitalWrite(PIN_MOTOR_DRV_C, LOW);
    delayMicroseconds(halfUs);

    digitalWrite(PIN_MOTOR_DRV_A, LOW);
    digitalWrite(PIN_MOTOR_DRV_B, HIGH);
    digitalWrite(PIN_MOTOR_DRV_C, LOW);
    delayMicroseconds(halfUs);

    digitalWrite(PIN_MOTOR_DRV_A, LOW);
    digitalWrite(PIN_MOTOR_DRV_B, LOW);
    digitalWrite(PIN_MOTOR_DRV_C, HIGH);
    delayMicroseconds(halfUs);
  }

  // Return to safe idle low-low-low.
  digitalWrite(PIN_MOTOR_DRV_A, LOW);
  digitalWrite(PIN_MOTOR_DRV_B, LOW);
  digitalWrite(PIN_MOTOR_DRV_C, LOW);
}

// ============================================================
int captureDataBurst(int durationUs) {
  int count = 0;
  unsigned long startUs = micros();
  noInterrupts();
  while (count < CAPTURE_LEN && (micros() - startUs) < (unsigned long)durationUs) {
    captureBuf[count] = digitalRead(PIN_DATA_P) | (digitalRead(PIN_DATA_N) << 1);
    captureTimesUs[count] = micros() - startUs;
    count++;
  }
  interrupts();
  return count;
}

// ============================================================
void printCapture(uint8_t* buf, int len) {
  if (len == 0) { Serial.println("  (empty)"); return; }
  bool allSame = true;
  for (int i = 1; i < len; i++) {
    if (buf[i] != buf[0]) { allSame = false; break; }
  }
  if (allSame) {
    Serial.printf("  All %d samples = D+:%d D-:%d (static)\n",
                  len, buf[0] & 1, (buf[0] >> 1) & 1);
    return;
  }
  int trans = 0;
  Serial.printf("  t=0us D+:%d D-:%d\n", buf[0] & 1, (buf[0] >> 1) & 1);
  for (int i = 1; i < len && trans < 100; i++) {
    if (buf[i] != buf[i - 1]) {
      Serial.printf("  t=%luus D+:%d D-:%d\n",
                    captureTimesUs[i], buf[i] & 1, (buf[i] >> 1) & 1);
      trans++;
    }
  }
  Serial.printf("  (%d transitions in %d samples)\n", trans, len);
}

// ============================================================
bool hasActivity(int count) {
  for (int i = 1; i < count; i++) {
    if (captureBuf[i] != captureBuf[0]) return true;
  }
  return false;
}

// ============================================================
void printPinStates(const char* label) {
  Serial.printf("[%s] D+=%d D-=%d REQ=%d BAT=%d\n", label,
    digitalRead(PIN_DATA_P), digitalRead(PIN_DATA_N),
    digitalRead(PIN_REQ), digitalRead(PIN_BAT));
}

// ============================================================
// Mode 1: Passive listen 5s
// ============================================================
void mode1_passive() {
  Serial.println("\n--- Mode 1: Passive Listen (5 sec) ---");
  oledShow("Mode 1: Passive", "Listening 5s...");
  printPinStates("IDLE");

  int transitions = 0;
  int lastP = digitalRead(PIN_DATA_P);
  int lastN = digitalRead(PIN_DATA_N);
  unsigned long start = millis();

  while (millis() - start < 5000) {
    int p = digitalRead(PIN_DATA_P);
    int n = digitalRead(PIN_DATA_N);
    if (p != lastP || n != lastN) {
      transitions++;
      if (transitions <= 50) {
        Serial.printf("  %lu ms: D+ %d->%d  D- %d->%d\n",
                      millis() - start, lastP, p, lastN, n);
      }
      lastP = p; lastN = n;
    }
  }
  Serial.printf("Total transitions: %d\n", transitions);
  char buf[32];
  snprintf(buf, sizeof(buf), "Trans: %d", transitions);
  oledShow("Mode 1 Done", transitions ? "Activity!" : "Silent", buf);
}

// ============================================================
// Mode 2: REQ pulse + capture (many widths)
// ============================================================
void mode2_req_toggle() {
  Serial.println("\n--- Mode 2: REQ Pulse + Capture ---");
  oledShow("Mode 2: REQ", "Pulse tests...");

  int pulseUs[] = {1, 5, 10, 50, 100, 500, 1000, 5000};
  const char* labels[] = {"1us","5us","10us","50us","100us","500us","1ms","5ms"};

  for (int i = 0; i < 8; i++) {
    Serial.printf("\n  REQ HIGH %s: ", labels[i]);
    digitalWrite(PIN_REQ, LOW);
    delayMicroseconds(100);

    digitalWrite(PIN_REQ, HIGH);
    if (pulseUs[i] >= 1000) delay(pulseUs[i] / 1000);
    else delayMicroseconds(pulseUs[i]);
    digitalWrite(PIN_REQ, LOW);

    int c = captureDataBurst(5000);
    if (hasActivity(c)) { Serial.println("ACTIVITY!"); printCapture(captureBuf, c); }
    else Serial.printf("silent (%d samp, D+:%d D-:%d)\n", c, captureBuf[0]&1, (captureBuf[0]>>1)&1);
    delay(30);
  }

  // REQ held HIGH
  Serial.println("\n  REQ held HIGH 10ms:");
  digitalWrite(PIN_REQ, HIGH);
  delay(1);
  int c = captureDataBurst(10000);
  digitalWrite(PIN_REQ, LOW);
  if (hasActivity(c)) { Serial.println("  ACTIVITY!"); printCapture(captureBuf, c); }
  else Serial.printf("  static D+:%d D-:%d\n", captureBuf[0]&1, (captureBuf[0]>>1)&1);

  // Falling edge
  Serial.println("\n  Falling edge (H->L):");
  digitalWrite(PIN_REQ, HIGH); delay(10);
  digitalWrite(PIN_REQ, LOW);
  c = captureDataBurst(5000);
  if (hasActivity(c)) { Serial.println("  ACTIVITY!"); printCapture(captureBuf, c); }
  else Serial.println("  silent");

  oledShow("Mode 2 Done", "REQ pulse tests", "complete");
}

// ============================================================
// Mode 3: Tamagawa T-format multi-baud multi-cmd
// ============================================================
void mode3_tamagawa() {
  Serial.println("\n--- Mode 3: Tamagawa T-format ---");
  oledShow("Mode 3: Tamagawa", "Multi-baud scan");

  uint8_t cmds[] = {0x1A, 0x02, 0x8A, 0x92, 0xA2, 0xBA, 0xEA};
  const char* cmdN[] = {"0x1A(pos)","0x02(mtrn)","0x8A(ID)","0x92(stat)",
                        "0xA2(err)","0xBA(EErd)","0xEA(posEE)"};
  int bitTimes[] = {1, 2, 4, 10, 20, 100};
  const char* baudL[] = {"~1MHz","~500k","~250k","~100k","~50k","~10k"};

  for (int b = 0; b < 6; b++) {
    int bt = bitTimes[b];
    Serial.printf("\n=== Baud %s (bit=%dus) ===\n", baudL[b], bt);

    for (int ci = 0; ci < 7; ci++) {
      uint8_t cmd = cmds[ci];
      Serial.printf("  %s: ", cmdN[ci]);

      noInterrupts();
      // Start bit
      digitalWrite(PIN_REQ, HIGH);
      delayMicroseconds(bt);
      // 8 data bits MSB
      for (int i = 7; i >= 0; i--) {
        digitalWrite(PIN_REQ, (cmd >> i) & 1 ? HIGH : LOW);
        delayMicroseconds(bt);
      }
      digitalWrite(PIN_REQ, LOW);
      int c = captureDataBurst(bt * 80);
      interrupts();

      if (hasActivity(c)) {
        Serial.println("*** ACTIVITY ***");
        printCapture(captureBuf, c);
      } else {
        Serial.printf("silent\n");
      }
      delay(5);
    }
  }

  // Inverted polarity (idle HIGH)
  Serial.println("\n=== Inverted polarity (idle HIGH, start LOW) ===");
  oledShow("Mode 3: Tamagawa", "Inverted polarity");
  digitalWrite(PIN_REQ, HIGH);
  delay(10);

  for (int b = 1; b < 4; b++) {
    int bt = bitTimes[b];
    Serial.printf("\n  Inverted @ %dus/bit:\n", bt);
    for (int ci = 0; ci < 3; ci++) {
      uint8_t cmd = cmds[ci];
      Serial.printf("    0x%02X: ", cmd);
      noInterrupts();
      digitalWrite(PIN_REQ, LOW); delayMicroseconds(bt);
      for (int i = 7; i >= 0; i--) {
        digitalWrite(PIN_REQ, (cmd >> i) & 1 ? LOW : HIGH);
        delayMicroseconds(bt);
      }
      digitalWrite(PIN_REQ, HIGH);
      int c = captureDataBurst(bt * 80);
      interrupts();
      if (hasActivity(c)) { Serial.println("ACTIVITY!"); printCapture(captureBuf, c); }
      else Serial.println("silent");
      delay(5);
    }
  }
  digitalWrite(PIN_REQ, LOW);
  oledShow("Mode 3 Done", "Tamagawa complete");
}

// ============================================================
// Mode 4: Clock & read bit-bang
// ============================================================
void mode4_clock_read() {
  Serial.println("\n--- Mode 4: Clock & Read ---");
  oledShow("Mode 4: Clock", "Bit-bang...");

  int periods[] = {200, 100, 20, 10, 4, 2};
  const char* lbl[] = {"5k","10k","50k","100k","250k","500k"};

  for (int s = 0; s < 6; s++) {
    int hp = periods[s] / 2;
    if (hp < 1) hp = 1;
    Serial.printf("\nClock ~%sHz:\n", lbl[s]);
    Serial.print("  D+: ");

    noInterrupts();
    uint64_t bitsP = 0, bitsN = 0;
    for (int bit = 0; bit < 48; bit++) {
      digitalWrite(PIN_REQ, HIGH); delayMicroseconds(hp);
      bitsP = (bitsP << 1) | digitalRead(PIN_DATA_P);
      bitsN = (bitsN << 1) | digitalRead(PIN_DATA_N);
      digitalWrite(PIN_REQ, LOW); delayMicroseconds(hp);
    }
    interrupts();

    for (int bit = 47; bit >= 0; bit--) Serial.print((int)((bitsP >> bit) & 1));
    Serial.println();
    Serial.print("  D-: ");
    for (int bit = 47; bit >= 0; bit--) Serial.print((int)((bitsN >> bit) & 1));
    Serial.println();

    uint64_t x = bitsP ^ bitsN;
    int diff = 0;
    for (int i = 0; i < 48; i++) if ((x >> i) & 1) diff++;
    Serial.printf("  XOR: %d/48 differ", diff);
    if (diff == 48) Serial.print(" (perfect differential)");
    else if (diff > 40) Serial.print(" (mostly diff)");
    else if (diff == 0) Serial.print(" (identical=NOT diff)");
    Serial.println();
    delay(30);
  }
  digitalWrite(PIN_REQ, LOW);
  oledShow("Mode 4 Done", "Clock read done");
}

// ============================================================
// Mode 5: UART probe (async serial at many bauds)
// ============================================================
void mode5_uart_probe() {
  Serial.println("\n--- Mode 5: UART-style Probe ---");
  oledShow("Mode 5: UART", "Baud scan...");

  long bauds[] = {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
  const char* bn[] = {"9600","19200","38400","57600","115200","230400","460800","921600"};

  for (int b = 0; b < 8; b++) {
    int bitUs = 1000000 / bauds[b];
    if (bitUs < 1) bitUs = 1;
    Serial.printf("\n  Baud %s (%dus/bit): ", bn[b], bitUs);

    // REQ pulse then capture
    digitalWrite(PIN_REQ, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_REQ, LOW);

    int dur = bitUs * 100;
    if (dur < 500) dur = 500;
    if (dur > 50000) dur = 50000;
    int c = captureDataBurst(dur);

    if (hasActivity(c)) {
      Serial.println("ACTIVITY!");
      printCapture(captureBuf, c);
    } else {
      Serial.printf("silent (%d samp)\n", c);
    }
    delay(10);
  }
  oledShow("Mode 5 Done", "UART scan done");
}

// ============================================================
// Mode 6: Pin diagnostics & ADC
// ============================================================
void mode6_diagnostics() {
  Serial.println("\n--- Mode 6: Pin Diagnostics ---");
  oledShow("Mode 6: Diag", "Reading ADC...");

  // Temporarily make REQ and BAT inputs for reading
  pinMode(PIN_REQ, INPUT);
  pinMode(PIN_BAT, INPUT);
  delay(5);

  int adcP = analogRead(PIN_DATA_P);
  int adcN = analogRead(PIN_DATA_N);
  int adcR = analogRead(PIN_REQ);
  int adcB = analogRead(PIN_BAT);

  float vP = adcP * 3.3 / 4095.0;
  float vN = adcN * 3.3 / 4095.0;
  float vR = adcR * 3.3 / 4095.0;
  float vB = adcB * 3.3 / 4095.0;

  Serial.printf("  DATA+ (GPIO%d): ADC=%d ~%.2fV dig=%d\n", PIN_DATA_P, adcP, vP, digitalRead(PIN_DATA_P));
  Serial.printf("  DATA- (GPIO%d): ADC=%d ~%.2fV dig=%d\n", PIN_DATA_N, adcN, vN, digitalRead(PIN_DATA_N));
  Serial.printf("  REQ   (GPIO%d): ADC=%d ~%.2fV dig=%d\n", PIN_REQ, adcR, vR, digitalRead(PIN_REQ));
  Serial.printf("  BAT+  (GPIO%d): ADC=%d ~%.2fV dig=%d\n", PIN_BAT, adcB, vB, digitalRead(PIN_BAT));
  Serial.printf("  Diff (D+ - D-) = %.2fV\n", vP - vN);

  // Restore outputs
  pinMode(PIN_REQ, OUTPUT);
  digitalWrite(PIN_REQ, LOW);
  pinMode(PIN_BAT, OUTPUT);
  digitalWrite(PIN_BAT, batHigh ? HIGH : LOW);

  // Voltage analysis
  if (vP < 0.3 && vN < 0.3) {
    Serial.println("  !! Both data lines near 0V — encoder not driving");
  } else if (vP > 2.0 && vN > 2.0) {
    Serial.println("  Both lines HIGH — normal RS-422 idle");
  } else if (vP > 2.0 && vN < 1.0) {
    Serial.println("  D+ HIGH, D- LOW — mark state (idle bit = 1)");
  } else if (vP < 1.0 && vN > 2.0) {
    Serial.println("  D+ LOW, D- HIGH — space state (idle bit = 0)");
  }

  // Quick impedance check
  Serial.println("  Impedance probe:");
  int before = analogRead(PIN_DATA_P);
  pinMode(PIN_DATA_P, OUTPUT);
  digitalWrite(PIN_DATA_P, LOW);
  delayMicroseconds(10);
  pinMode(PIN_DATA_P, INPUT);
  delayMicroseconds(50);
  int after = analogRead(PIN_DATA_P);
  Serial.printf("    D+ before=%d after=%d ", before, after);
  if (after > before - 200) Serial.println("(quick recovery = encoder driving)");
  else Serial.println("(slow recovery = floating/high-Z)");

  before = analogRead(PIN_DATA_N);
  pinMode(PIN_DATA_N, OUTPUT);
  digitalWrite(PIN_DATA_N, LOW);
  delayMicroseconds(10);
  pinMode(PIN_DATA_N, INPUT);
  delayMicroseconds(50);
  after = analogRead(PIN_DATA_N);
  Serial.printf("    D- before=%d after=%d ", before, after);
  if (after > before - 200) Serial.println("(quick recovery)");
  else Serial.println("(slow/floating)");

  char b1[32], b2[32], b3[32];
  snprintf(b1, sizeof(b1), "D+:%.2fV D-:%.2fV", vP, vN);
  snprintf(b2, sizeof(b2), "REQ:%.2fV BAT:%.2fV", vR, vB);
  snprintf(b3, sizeof(b3), "Diff:%.2fV", vP - vN);
  oledShow("Mode 6: Diag", b1, b2, b3);
}

// ============================================================
// Mode 7: BRUTE FORCE
// ============================================================
void mode7_bruteforce() {
  Serial.println("\n========================================");
  Serial.println("  MODE 7: BRUTE FORCE — TRY EVERYTHING");
  Serial.println("========================================\n");
  oledShow("BRUTE FORCE", "Phase 1: Diag");

  // Phase 1: Diagnostics
  Serial.println(">>> Phase 1: Diagnostics");
  mode6_diagnostics();
  delay(200);

  // Phase 2: BAT+ HIGH experiments
  Serial.println("\n>>> Phase 2: BAT+ HIGH tests");
  oledShow("BRUTE", "Phase 2: BAT HIGH");
  pinMode(PIN_BAT, OUTPUT);
  digitalWrite(PIN_BAT, HIGH);
  delay(100);
  printPinStates("BAT_HIGH");

  int trans = 0, lastP = digitalRead(PIN_DATA_P);
  unsigned long start = millis();
  while (millis() - start < 2000) {
    int p = digitalRead(PIN_DATA_P);
    if (p != lastP) { trans++; lastP = p; }
  }
  Serial.printf("  Passive with BAT+ HIGH: %d transitions\n", trans);
  if (trans > 0) {
    Serial.println("  *** BAT+ triggered activity! ***");
    int c = captureDataBurst(10000);
    printCapture(captureBuf, c);
  }

  // Phase 3: REQ + BAT HIGH combos
  Serial.println("\n>>> Phase 3: REQ + BAT_HIGH combos");
  oledShow("BRUTE", "Phase 3: REQ+BAT");
  int pw[] = {5, 50, 500, 5000};
  for (int i = 0; i < 4; i++) {
    Serial.printf("  REQ %dus (BAT+HIGH): ", pw[i]);
    digitalWrite(PIN_REQ, HIGH);
    if (pw[i] >= 1000) delay(pw[i]/1000); else delayMicroseconds(pw[i]);
    digitalWrite(PIN_REQ, LOW);
    int c = captureDataBurst(5000);
    if (hasActivity(c)) { Serial.println("ACTIVITY!"); printCapture(captureBuf, c); }
    else Serial.println("silent");
    delay(20);
  }

  // Phase 4: Half-duplex TX on DATA+ (Tamagawa half-duplex)
  Serial.println("\n>>> Phase 4: Half-duplex TX on DATA+");
  oledShow("BRUTE", "Phase 4: TX D+");
  uint8_t txCmds[] = {0x1A, 0x02, 0x8A, 0x92};
  int txBits[] = {2, 4, 10};

  for (int bt = 0; bt < 3; bt++) {
    int bitUs = txBits[bt];
    Serial.printf("\n  Half-duplex TX @ %dus/bit on DATA+:\n", bitUs);

    for (int ci = 0; ci < 4; ci++) {
      uint8_t cmd = txCmds[ci];
      Serial.printf("    TX 0x%02X: ", cmd);

      pinMode(PIN_DATA_P, OUTPUT);
      noInterrupts();

      // Start bit HIGH
      digitalWrite(PIN_DATA_P, HIGH);
      delayMicroseconds(bitUs);
      // 8 data bits MSB
      for (int i = 7; i >= 0; i--) {
        digitalWrite(PIN_DATA_P, (cmd >> i) & 1 ? HIGH : LOW);
        delayMicroseconds(bitUs);
      }
      // back to idle LOW
      digitalWrite(PIN_DATA_P, LOW);
      delayMicroseconds(bitUs);
      // switch to input & capture
      pinMode(PIN_DATA_P, INPUT);
      int c = captureDataBurst(bitUs * 100);
      interrupts();

      if (hasActivity(c)) { Serial.println("*** RESPONSE ***"); printCapture(captureBuf, c); }
      else Serial.println("silent");
      delay(5);
    }
  }
  pinMode(PIN_DATA_P, INPUT);

  // Phase 5: TX on DATA- (maybe D- is command line)
  Serial.println("\n>>> Phase 5: TX on DATA- (D- as cmd?)");
  oledShow("BRUTE", "Phase 5: TX D-");
  for (int bt = 0; bt < 2; bt++) {
    int bitUs = (bt == 0) ? 4 : 10;
    Serial.printf("  TX 0x1A on D- @ %dus: ", bitUs);

    pinMode(PIN_DATA_N, OUTPUT);
    noInterrupts();
    digitalWrite(PIN_DATA_N, HIGH); delayMicroseconds(bitUs);
    uint8_t cmd = 0x1A;
    for (int i = 7; i >= 0; i--) {
      digitalWrite(PIN_DATA_N, (cmd >> i) & 1 ? HIGH : LOW);
      delayMicroseconds(bitUs);
    }
    digitalWrite(PIN_DATA_N, LOW); delayMicroseconds(bitUs);
    pinMode(PIN_DATA_N, INPUT);
    int c = captureDataBurst(bitUs * 100);
    interrupts();

    if (hasActivity(c)) { Serial.println("RESPONSE!"); printCapture(captureBuf, c); }
    else Serial.println("silent");
    delay(10);
  }

  // Phase 6: BAT+ LOW, fast 2.5MHz Tamagawa on REQ
  Serial.println("\n>>> Phase 6: BAT LOW + fast Tamagawa");
  oledShow("BRUTE", "Phase 6: Fast");
  digitalWrite(PIN_BAT, LOW);
  delay(50);

  Serial.println("  Fast 0x1A on REQ (~ets_delay_us):");
  noInterrupts();
  digitalWrite(PIN_REQ, HIGH); ets_delay_us(1);
  // 0x1A = 00011010 MSB first
  digitalWrite(PIN_REQ, LOW); ets_delay_us(1);  // 0
  digitalWrite(PIN_REQ, LOW); ets_delay_us(1);  // 0
  digitalWrite(PIN_REQ, LOW); ets_delay_us(1);  // 0
  digitalWrite(PIN_REQ, HIGH); ets_delay_us(1); // 1
  digitalWrite(PIN_REQ, HIGH); ets_delay_us(1); // 1
  digitalWrite(PIN_REQ, LOW); ets_delay_us(1);  // 0
  digitalWrite(PIN_REQ, HIGH); ets_delay_us(1); // 1
  digitalWrite(PIN_REQ, LOW); ets_delay_us(1);  // 0
  digitalWrite(PIN_REQ, LOW);
  int c = captureDataBurst(500);
  interrupts();
  if (hasActivity(c)) { Serial.println("  ACTIVITY!"); printCapture(captureBuf, c); }
  else Serial.printf("  silent (%d samp)\n", c);

  // Phase 7: Read all pins as inputs (pin swap hypothesis)
  Serial.println("\n>>> Phase 7: All pins as inputs");
  oledShow("BRUTE", "Phase 7: Pin swap");
  pinMode(PIN_REQ, INPUT);
  delay(5);
  Serial.printf("  GPIO4(D+): dig=%d adc=%d\n", digitalRead(PIN_DATA_P), analogRead(PIN_DATA_P));
  Serial.printf("  GPIO5(D-): dig=%d adc=%d\n", digitalRead(PIN_DATA_N), analogRead(PIN_DATA_N));
  Serial.printf("  GPIO6(REQ): dig=%d adc=%d\n", digitalRead(PIN_REQ), analogRead(PIN_REQ));
  Serial.printf("  GPIO7(BAT): dig=%d adc=%d\n", digitalRead(PIN_BAT), analogRead(PIN_BAT));

  // Phase 8: TX Tamagawa on BAT+ wire
  Serial.println("\n>>> Phase 8: TX on BAT+ (GPIO7)");
  oledShow("BRUTE", "Phase 8: TX BAT+");
  pinMode(PIN_REQ, INPUT); // read REQ too
  pinMode(PIN_BAT, OUTPUT);

  for (int bt = 0; bt < 2; bt++) {
    int bitUs = (bt == 0) ? 4 : 10;
    Serial.printf("  TX 0x1A on BAT @ %dus: ", bitUs);
    noInterrupts();
    digitalWrite(PIN_BAT, HIGH); delayMicroseconds(bitUs);
    uint8_t cmd2 = 0x1A;
    for (int i = 7; i >= 0; i--) {
      digitalWrite(PIN_BAT, (cmd2 >> i) & 1 ? HIGH : LOW);
      delayMicroseconds(bitUs);
    }
    digitalWrite(PIN_BAT, LOW);

    // Capture D+, D-, REQ
    int cnt = 0;
    unsigned long su = micros();
    while (cnt < CAPTURE_LEN && (micros() - su) < 1000) {
      captureBuf[cnt] = digitalRead(PIN_DATA_P)
                      | (digitalRead(PIN_DATA_N) << 1)
                      | (digitalRead(PIN_REQ) << 2);
      captureTimesUs[cnt] = micros() - su;
      cnt++;
    }
    interrupts();

    bool act = false;
    for (int i = 1; i < cnt; i++) {
      if (captureBuf[i] != captureBuf[0]) { act = true; break; }
    }
    if (act) {
      Serial.println("ACTIVITY!");
      for (int i = 0; i < cnt; i++) {
        if (i == 0 || captureBuf[i] != captureBuf[i-1]) {
          Serial.printf("  t=%luus D+:%d D-:%d REQ:%d\n",
            captureTimesUs[i], captureBuf[i]&1, (captureBuf[i]>>1)&1, (captureBuf[i]>>2)&1);
        }
      }
    } else Serial.println("silent");
    delay(10);
  }

  // Phase 9: Try differential — TX on D+, read D- (true half-duplex)
  Serial.println("\n>>> Phase 9: Differential half-duplex (TX D+, RX D-)");
  oledShow("BRUTE", "Phase 9: Diff TX");
  pinMode(PIN_REQ, OUTPUT); digitalWrite(PIN_REQ, LOW);
  pinMode(PIN_BAT, OUTPUT); digitalWrite(PIN_BAT, LOW);

  for (int bt = 0; bt < 3; bt++) {
    int bitUs = txBits[bt];
    Serial.printf("  TX 0x1A on D+ @ %dus, read D-: ", bitUs);

    pinMode(PIN_DATA_P, OUTPUT);
    noInterrupts();
    // idle LOW then start
    digitalWrite(PIN_DATA_P, HIGH); delayMicroseconds(bitUs);
    uint8_t cmd3 = 0x1A;
    for (int i = 7; i >= 0; i--) {
      digitalWrite(PIN_DATA_P, (cmd3 >> i) & 1 ? HIGH : LOW);
      delayMicroseconds(bitUs);
    }
    digitalWrite(PIN_DATA_P, LOW); delayMicroseconds(bitUs);
    // Don't switch back yet — capture D- while still driving D+
    int cnt = 0;
    unsigned long su = micros();
    while (cnt < CAPTURE_LEN && (micros() - su) < (unsigned long)(bitUs * 80)) {
      captureBuf[cnt] = digitalRead(PIN_DATA_N);
      captureTimesUs[cnt] = micros() - su;
      cnt++;
    }
    interrupts();
    pinMode(PIN_DATA_P, INPUT);

    bool act = false;
    for (int i = 1; i < cnt; i++) {
      if (captureBuf[i] != captureBuf[0]) { act = true; break; }
    }
    if (act) {
      Serial.println("RESPONSE ON D-!");
      int tr = 0;
      for (int i = 0; i < cnt && tr < 60; i++) {
        if (i == 0 || captureBuf[i] != captureBuf[i-1]) {
          Serial.printf("  t=%luus D-=%d\n", captureTimesUs[i], captureBuf[i]);
          tr++;
        }
      }
    } else Serial.println("silent");
    delay(10);
  }

  // Cleanup
  pinMode(PIN_DATA_P, INPUT);
  pinMode(PIN_DATA_N, INPUT);
  pinMode(PIN_REQ, OUTPUT); digitalWrite(PIN_REQ, LOW);
  pinMode(PIN_BAT, OUTPUT); digitalWrite(PIN_BAT, LOW);
  batHigh = false;

  Serial.println("\n========================================");
  Serial.println("  BRUTE FORCE COMPLETE");
  Serial.println("========================================");
  Serial.println("If ALL silent:");
  Serial.println("  1) Need MAX485 for proper RS-422 signaling");
  Serial.println("  2) Encoder may use proprietary clock/data protocol");
  Serial.println("  3) Try swapping Red/DarkRed (maybe data is on Red)");
  Serial.println("  4) Maybe need pull-ups on data lines");

  oledShow("BRUTE DONE", "Check serial log", "for results");
}

// ============================================================
// Continuous read
// ============================================================
void continuous_read() {
  Serial.println("\n--- Continuous Read (any key to stop) ---");
  oledShow("Continuous", "Reading...");

  uint32_t lastVal = 0xFFFFFFFF;
  int reads = 0;

  while (!Serial.available()) {
    digitalWrite(PIN_REQ, HIGH); delayMicroseconds(10);
    digitalWrite(PIN_REQ, LOW);

    noInterrupts();
    uint64_t bits = 0;
    for (int b = 0; b < 40; b++) {
      digitalWrite(PIN_REQ, HIGH); delayMicroseconds(1);
      bits = (bits << 1) | digitalRead(PIN_DATA_P);
      digitalWrite(PIN_REQ, LOW); delayMicroseconds(1);
    }
    interrupts();

    uint32_t val = (uint32_t)(bits >> 8);
    if (val != lastVal) {
      Serial.printf("[%d] raw=0x%010llX pos=0x%08X\n", reads, bits, val);
      lastVal = val;
      if (oledOk && (reads % 5 == 0)) {
        char buf[22];
        snprintf(buf, sizeof(buf), "0x%08X", val);
        oledShow("Continuous", buf);
      }
    }
    reads++;
    delay(50);
  }
  while (Serial.available()) Serial.read();
  Serial.printf("Stopped after %d reads\n", reads);
}

// ============================================================
void toggleBat() {
  batHigh = !batHigh;
  pinMode(PIN_BAT, OUTPUT);
  digitalWrite(PIN_BAT, batHigh ? HIGH : LOW);
  Serial.printf("BAT+ (GPIO%d) = %s\n", PIN_BAT, batHigh ? "HIGH" : "LOW");
  char buf[32];
  snprintf(buf, sizeof(buf), "BAT+: %s", batHigh ? "HIGH" : "LOW");
  oledShow("BAT Toggle", buf);
}

// ============================================================
// Toggle internal pull-ups on data lines
// ============================================================
void togglePullups() {
  pullUpsEnabled = !pullUpsEnabled;
  if (pullUpsEnabled) {
    pinMode(PIN_DATA_P, INPUT_PULLUP);
    pinMode(PIN_DATA_N, INPUT_PULLUP);
    pinMode(PIN_REQ, INPUT_PULLUP);
    pinMode(PIN_BAT, INPUT_PULLUP);
    Serial.println("Pull-ups ENABLED on all encoder pins");
  } else {
    pinMode(PIN_DATA_P, INPUT);
    pinMode(PIN_DATA_N, INPUT);
    pinMode(PIN_REQ, OUTPUT);
    digitalWrite(PIN_REQ, LOW);
    pinMode(PIN_BAT, OUTPUT);
    digitalWrite(PIN_BAT, batHigh ? HIGH : LOW);
    Serial.println("Pull-ups DISABLED (pins back to normal)");
  }
  delay(10);

  // Read ADC with new state
  int aP = analogRead(PIN_DATA_P);
  int aN = analogRead(PIN_DATA_N);
  int aR = analogRead(PIN_REQ);
  int aB = analogRead(PIN_BAT);
  Serial.printf("  D+=%.2fV D-=%.2fV REQ=%.2fV BAT=%.2fV\n",
    aP*3.3/4095.0, aN*3.3/4095.0, aR*3.3/4095.0, aB*3.3/4095.0);
  Serial.printf("  D+ dig=%d D- dig=%d REQ dig=%d BAT dig=%d\n",
    digitalRead(PIN_DATA_P), digitalRead(PIN_DATA_N),
    digitalRead(PIN_REQ), digitalRead(PIN_BAT));

  // If pull-ups are on and any line gets pulled LOW, the encoder is sinking current
  if (pullUpsEnabled) {
    Serial.println("  Interpreting: if a pin reads LOW with pullup, encoder is sinking that line");
    if (digitalRead(PIN_DATA_P) == 0) Serial.println("  -> D+ is being pulled LOW by encoder");
    if (digitalRead(PIN_DATA_N) == 0) Serial.println("  -> D- is being pulled LOW by encoder");
    if (digitalRead(PIN_REQ) == 0) Serial.println("  -> REQ is being pulled LOW by encoder");
    if (digitalRead(PIN_BAT) == 0) Serial.println("  -> BAT is being pulled LOW by encoder");
  }

  char buf[32], buf2[32];
  snprintf(buf, sizeof(buf), "Pullups: %s", pullUpsEnabled ? "ON" : "OFF");
  snprintf(buf2, sizeof(buf2), "D+:%.1fV D-:%.1fV", aP*3.3/4095.0, aN*3.3/4095.0);
  oledShow("Pull-up Test", buf, buf2);
}

// ============================================================
// Hardware UART serial mode — proper async serial frames
// Uses ESP32-S3 UART1 on data pins
// Tamagawa T-format uses 2.5Mbps, half-duplex RS-485
// ============================================================
void mode_uart_serial() {
  Serial.println("\n--- UART Serial Mode ---");
  oledShow("UART Mode", "HW serial probe");

  // Tamagawa baud rates to try
  long bauds[] = {2500000, 1000000, 500000, 250000, 115200, 57600, 19200, 9600};
  const char* bn[] = {"2.5M","1M","500k","250k","115200","57600","19200","9600"};

  // Tamagawa data ID commands (CF field, 4 bits)
  // Data ID 0 (CC=0x1A): Read absolute (17+17 bit)
  // Data ID 3 (CC=0x02): Read multi-turn + absolute
  // Data ID C (CC=0x8A): Read encoder ID
  // Data ID 6 (CC=0x92): Read absolute + status
  struct { uint8_t cc; const char* name; } cmds[] = {
    {0x1A, "ID0:abs"},
    {0x02, "ID3:multi"},
    {0x8A, "IDC:encID"},
    {0x92, "ID6:stat"},
  };

  for (int b = 0; b < 8; b++) {
    Serial.printf("\n=== Baud %s ===\n", bn[b]);

    for (int c = 0; c < 4; c++) {
      // --- TX on D+ (GPIO4), RX on D- (GPIO5) ---
      Serial.printf("  %s via D+TX/D-RX: ", cmds[c].name);

      EncoderSerial.begin(bauds[b], SERIAL_8N1, PIN_DATA_N, PIN_DATA_P);
      EncoderSerial.flush();
      while (EncoderSerial.available()) EncoderSerial.read();

      // Send the command byte (Tamagawa CF byte)
      EncoderSerial.write(cmds[c].cc);
      EncoderSerial.flush();

      // Wait for response (Tamagawa response is typically 5-10 bytes)
      delay(5);

      int rxCount = 0;
      uint8_t rxBuf[32];
      unsigned long deadline = millis() + 20;
      while (millis() < deadline && rxCount < 32) {
        if (EncoderSerial.available()) {
          rxBuf[rxCount++] = EncoderSerial.read();
          deadline = millis() + 5;
        }
      }
      EncoderSerial.end();

      if (rxCount > 0) {
        Serial.printf("GOT %d bytes! ", rxCount);
        for (int i = 0; i < rxCount; i++) Serial.printf("%02X ", rxBuf[i]);
        Serial.println();
      } else {
        Serial.println("silent");
      }

      delay(5);

      // --- TX on D- (GPIO5), RX on D+ (GPIO4) ---
      Serial.printf("  %s via D-TX/D+RX: ", cmds[c].name);

      EncoderSerial.begin(bauds[b], SERIAL_8N1, PIN_DATA_P, PIN_DATA_N);
      EncoderSerial.flush();
      while (EncoderSerial.available()) EncoderSerial.read();

      EncoderSerial.write(cmds[c].cc);
      EncoderSerial.flush();

      delay(5);
      rxCount = 0;
      deadline = millis() + 20;
      while (millis() < deadline && rxCount < 32) {
        if (EncoderSerial.available()) {
          rxBuf[rxCount++] = EncoderSerial.read();
          deadline = millis() + 5;
        }
      }
      EncoderSerial.end();

      if (rxCount > 0) {
        Serial.printf("GOT %d bytes! ", rxCount);
        for (int i = 0; i < rxCount; i++) Serial.printf("%02X ", rxBuf[i]);
        Serial.println();
      } else {
        Serial.println("silent");
      }

      delay(5);

      // --- TX on REQ (GPIO6), RX on D+ (GPIO4) ---
      Serial.printf("  %s via REQ_TX/D+RX: ", cmds[c].name);

      EncoderSerial.begin(bauds[b], SERIAL_8N1, PIN_DATA_P, PIN_REQ);
      EncoderSerial.flush();
      while (EncoderSerial.available()) EncoderSerial.read();

      EncoderSerial.write(cmds[c].cc);
      EncoderSerial.flush();

      delay(5);
      rxCount = 0;
      deadline = millis() + 20;
      while (millis() < deadline && rxCount < 32) {
        if (EncoderSerial.available()) {
          rxBuf[rxCount++] = EncoderSerial.read();
          deadline = millis() + 5;
        }
      }
      EncoderSerial.end();

      if (rxCount > 0) {
        Serial.printf("GOT %d bytes! ", rxCount);
        for (int i = 0; i < rxCount; i++) Serial.printf("%02X ", rxBuf[i]);
        Serial.println();
      } else {
        Serial.println("silent");
      }

      delay(5);

      // --- TX on BAT (GPIO7), RX on D+ (GPIO4) ---
      Serial.printf("  %s via BAT_TX/D+RX: ", cmds[c].name);

      EncoderSerial.begin(bauds[b], SERIAL_8N1, PIN_DATA_P, PIN_BAT);
      EncoderSerial.flush();
      while (EncoderSerial.available()) EncoderSerial.read();

      EncoderSerial.write(cmds[c].cc);
      EncoderSerial.flush();

      delay(5);
      rxCount = 0;
      deadline = millis() + 20;
      while (millis() < deadline && rxCount < 32) {
        if (EncoderSerial.available()) {
          rxBuf[rxCount++] = EncoderSerial.read();
          deadline = millis() + 5;
        }
      }
      EncoderSerial.end();

      if (rxCount > 0) {
        Serial.printf("GOT %d bytes! ", rxCount);
        for (int i = 0; i < rxCount; i++) Serial.printf("%02X ", rxBuf[i]);
        Serial.println();
      } else {
        Serial.println("silent");
      }

      delay(5);
    }
  }

  // Restore pin modes
  pinMode(PIN_DATA_P, INPUT);
  pinMode(PIN_DATA_N, INPUT);
  pinMode(PIN_REQ, OUTPUT);
  digitalWrite(PIN_REQ, LOW);
  pinMode(PIN_BAT, OUTPUT);
  digitalWrite(PIN_BAT, batHigh ? HIGH : LOW);

  oledShow("UART Done", "Check serial log");
}

// ============================================================
// Tamagawa CRC8 — poly 0x01 (from gjwmotor/gjw_enc_arduino)
// ============================================================
static uint8_t tmg_crc8(uint8_t *data, int len) {
  uint8_t crc = 0;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      if (crc & 0x80) crc = (crc << 1) ^ 0x01;
      else crc <<= 1;
    }
  }
  return crc;
}

// ============================================================
// Mode D: Pseudo-Differential Tamagawa via ESP32-S3 GPIO matrix
// Uses UART1 TX → GPIO5(Blue) normal + GPIO7(White) inverted
// Reads UART1 RX ← GPIO4(DarkRed) or GPIO6(Purple)
// Protocol from gjwmotor/gjw_enc_arduino (MIT, Tamagawa T-format)
// ============================================================
void mode_diff_tamagawa() {
  Serial.println("\n============================================");
  Serial.println("  MODE D: PSEUDO-DIFFERENTIAL TAMAGAWA");
  Serial.println("  ESP32-S3 GPIO Matrix Trick");
  Serial.println("============================================\n");

  Serial.println("Pin mapping (from short-circuit test):");
  Serial.printf("  GPIO%d (DarkRed) = Encoder TX+ OUTPUT (sinks 1A)\n", PIN_ENC_TXP);
  Serial.printf("  GPIO%d (Blue)    = Encoder RX+ INPUT  (high-Z) <- we TX here\n", PIN_ENC_RXP);
  Serial.printf("  GPIO%d (Purple)  = Encoder TX- OUTPUT (sinks 1A)\n", PIN_ENC_TXN);
  Serial.printf("  GPIO%d (White)   = Encoder RX- INPUT  (high-Z) <- inverted TX\n", PIN_ENC_RXN);
  Serial.println("  Blue LEDs on all pins clamp 5V -> ~3V\n");
  Serial.println("  Pseudo-diff: UART1 TX -> GPIO5 (normal) + GPIO7 (inverted)");
  Serial.println("  This creates ~3.3V differential swing on encoder RX pair\n");

  oledShow("Mode D: Diff", "Pseudo-diff TX", "GPIO matrix trick");

  // Tamagawa T-format commands (from gjw library)
  struct TmgCmd {
    uint8_t cf;          // Command Field byte
    int responseLen;     // Expected response length including CF echo + CRC
    const char* name;
  };
  // Response format: CF(1) + payload + CRC(1)
  TmgCmd cmds[] = {
    {0x1A, 11, "ID0:pos+ID+alm"},  // ABS(3)+ENID(1)+ABM(2)+ALMC(1) +SF+CRC
    {0x02,  6, "ID3:pos"},          // ABS(3) +SF+CRC
    {0x92, 10, "ID6:hwID"},         // HDIDL(2)+HDIDH(2)+SFIDL(2)+SFIDH(2) +CRC
    {0x4A,  3, "ID5:status"},       // SF +CRC
    {0x8A,  6, "IDC:encID"},        // Try this variant too
    {0xEA,  4, "EEPROM:rd"},        // EEPROM read (addr 0x00)
  };
  const int NUM_CMDS = 6;

  // Baud rates: 2.5M is standard Tamagawa, but try lower ones too
  long bauds[] = {2500000, 1000000, 500000, 250000, 115200, 57600, 19200, 9600};
  const char* bn[] = {"2.5M","1M","500k","250k","115200","57600","19200","9600"};
  const int NUM_BAUDS = 8;

  // Configurations to test
  struct Config {
    const char* label;
    int txPin;    // UART1 TX (normal)
    int rxPin;    // UART1 RX
    int invPin;   // Inverted TX copy (-1 = none)
    bool hdEcho;  // true = half-duplex: strip TX echo from RX buffer
  };
  Config configs[] = {
    // *** CORRECT RS-485 HD config (v4 pinout): A=Blue(5) B=DarkRed(4) ***
    // TX on A(GPIO5), inverted B on GPIO4, RX on A(GPIO5 same pin = hd loopback)
    // Purple(6) must be tied to GND; Orange/White(7) must be disconnected
    {"RS485-HD A:5=TX+RX Binv:4",  PIN_ENC_RXP, PIN_ENC_RXP, PIN_ENC_TXP, true},
    // Previous attempts kept for reference:
    // Best guess: pseudo-diff TX to encoder RX pair, read from encoder TX+
    {"PseudoDiff TX:5+7inv RX:4",  PIN_ENC_RXP, PIN_ENC_TXP, PIN_ENC_RXN, false},
    // Same TX but read from encoder TX- (Purple)
    {"PseudoDiff TX:5+7inv RX:6",  PIN_ENC_RXP, PIN_ENC_TXN, PIN_ENC_RXN, false},
    // Single-ended TX on Blue, RX on DarkRed (no complement)
    {"SingleEnd TX:5 RX:4",        PIN_ENC_RXP, PIN_ENC_TXP, -1, false},
    // Swapped: TX on White, RX on DarkRed
    {"Swapped TX:7 RX:4 inv:5",    PIN_ENC_RXN, PIN_ENC_TXP, PIN_ENC_RXP, false},
    // Half-duplex attempt with wrong inv pin (archived)
    {"HalfDpx TX:5 RX:4 inv:6",   PIN_ENC_RXP, PIN_ENC_TXP, PIN_ENC_TXN, false},
  };
  const int NUM_CONFIGS = sizeof(configs) / sizeof(configs[0]);

  int totalSuccess = 0;
  int bestConfig = -1;
  int bestBaud = -1;

  for (int cfg = 0; cfg < NUM_CONFIGS; cfg++) {
    Serial.printf("\n>>> Config %d: %s\n", cfg, configs[cfg].label);
    char oledBuf[22];
    snprintf(oledBuf, sizeof(oledBuf), "Cfg %d/%d", cfg+1, NUM_CONFIGS);
    oledShow("Mode D", configs[cfg].label, oledBuf);

    for (int b = 0; b < NUM_BAUDS; b++) {
      // Initialize UART1 with specified TX/RX pins
      EncoderSerial.begin(bauds[b], SERIAL_8N1, configs[cfg].rxPin, configs[cfg].txPin);

      // Route inverted TX copy if configured
      if (configs[cfg].invPin >= 0) {
        pinMode(configs[cfg].invPin, OUTPUT);
        esp_rom_gpio_connect_out_signal(configs[cfg].invPin, U1TXD_OUT_IDX, true, false);
      }

      // Flush any garbage
      EncoderSerial.flush();
      delayMicroseconds(100);
      while (EncoderSerial.available()) EncoderSerial.read();

      bool gotAnything = false;
      int baudSuccess = 0;

      for (int c = 0; c < NUM_CMDS; c++) {
        // Clear RX buffer
        while (EncoderSerial.available()) EncoderSerial.read();

        uint8_t txFrame[3];
        int txLen = 0;

        // For EEPROM read, we need to send address byte after CF
        if (cmds[c].cf == 0xEA) {
          txFrame[0] = 0xEA;
          txFrame[1] = 0x00;  // Address 0
          txFrame[2] = tmg_crc8(txFrame, 2);
          txLen = 3;
          EncoderSerial.write(txFrame[0]);
          EncoderSerial.flush();
          delayMicroseconds(10);
          EncoderSerial.write(txFrame + 1, 2);
        } else {
          txFrame[0] = cmds[c].cf;
          txLen = 1;
          EncoderSerial.write(cmds[c].cf);
        }
        EncoderSerial.flush();

        // Wait for response — encoder needs a few bit-times to respond
        delayMicroseconds(100);

        uint8_t rxBuf[32];
        int rxCount = 0;
        unsigned long deadline = millis() + 15;
        while (millis() < deadline && rxCount < 32) {
          if (EncoderSerial.available()) {
            rxBuf[rxCount++] = EncoderSerial.read();
            deadline = millis() + 3;  // extend deadline on each byte
          }
        }

        if (rxCount > 0) {
          // In HD mode, RX can include local TX echo. Strip one leading echoed frame.
          int dataStart = 0;
          if (configs[cfg].hdEcho && txLen > 0 && rxCount >= txLen) {
            bool echoed = true;
            for (int i = 0; i < txLen; i++) {
              if (rxBuf[i] != txFrame[i]) {
                echoed = false;
                break;
              }
            }
            if (echoed) dataStart = txLen;
          }

          int dataCount = rxCount - dataStart;

          if (!gotAnything) {
            Serial.printf("  Baud %s:\n", bn[b]);
            gotAnything = true;
          }
          Serial.printf("    %s: %d bytes", cmds[c].name, rxCount);
          if (dataStart > 0) Serial.printf(" (echo-stripped -> %d)", dataCount);
          Serial.printf(": ");
          for (int i = 0; i < rxCount; i++) Serial.printf("%02X ", rxBuf[i]);

          // Validate CRC if we got expected length
          int expLen = cmds[c].responseLen;
          uint8_t* data = rxBuf + dataStart;
          if (dataCount >= expLen && data[0] == cmds[c].cf) {
            uint8_t calcCrc = tmg_crc8(data, expLen - 1);
            if (calcCrc == data[expLen - 1]) {
              Serial.printf(" *** CRC OK! ***");
              baudSuccess++;
              totalSuccess++;
              if (bestConfig < 0) {
                bestConfig = cfg;
                bestBaud = b;
              }
              // Decode position data for 0x1A
              if (cmds[c].cf == 0x1A && expLen == 11) {
                uint8_t sf = data[1];
                uint32_t abs_pos = data[2] | (data[3] << 8) | (data[4] << 16);
                uint8_t enid = data[5];
                int16_t abm = data[6] | (data[7] << 8);
                uint8_t almc = data[9];
                Serial.printf("\n      SF=%02X ABS=%u ENID=%02X ABM=%d ALMC=%02X",
                              sf, abs_pos, enid, abm, almc);
              }
              // Decode for 0x02
              if (cmds[c].cf == 0x02 && expLen == 6) {
                uint8_t sf = data[1];
                uint32_t abs_pos = data[2] | (data[3] << 8) | (data[4] << 16);
                Serial.printf("\n      SF=%02X ABS=%u (0x%06X)", sf, abs_pos, abs_pos);
              }
            } else {
              Serial.printf(" CRC: calc=%02X got=%02X", calcCrc, data[expLen - 1]);
            }
          } else if (dataCount > 0) {
            // Got bytes but unexpected length or CF mismatch
            bool allSame = true;
            for (int i = 1; i < dataCount; i++) {
              if (data[i] != data[0]) { allSame = false; break; }
            }
            if (allSame && data[0] == 0x00) {
              Serial.printf("(all 0x00 = stuck LOW)");
            } else if (allSame && data[0] == 0xFF) {
              Serial.printf("(all 0xFF = stuck HIGH)");
            }
          }
          Serial.println();
        }
        delay(2);
      }

      if (!gotAnything) {
        // Print one-line summary for silent bauds
        if (b == 0) Serial.printf("  Baud %s: silent", bn[b]);
        else {
          // Check if ALL previous bauds were silent too — compress output
          Serial.printf(" | %s: silent", bn[b]);
        }
      } else if (baudSuccess > 0) {
        Serial.printf("    >>> %d valid CRC responses at %s!\n", baudSuccess, bn[b]);
      }

      // Disconnect inverted output before changing config
      if (configs[cfg].invPin >= 0) {
        gpio_reset_pin((gpio_num_t)configs[cfg].invPin);
        pinMode(configs[cfg].invPin, INPUT);
      }

      EncoderSerial.end();
      delay(5);
    }
    Serial.println();  // End of baud scan for this config
  }

  // Restore all pins to safe state
  pinMode(PIN_ENC_TXP, INPUT);
  pinMode(PIN_ENC_RXP, INPUT);
  pinMode(PIN_ENC_TXN, OUTPUT); digitalWrite(PIN_ENC_TXN, LOW);
  pinMode(PIN_ENC_RXN, OUTPUT); digitalWrite(PIN_ENC_RXN, LOW);
  batHigh = false;

  Serial.println("\n============================================");
  Serial.printf("  RESULTS: %d total CRC-valid responses\n", totalSuccess);

  if (totalSuccess > 0) {
    Serial.println("  *** ENCODER COMMUNICATION ESTABLISHED! ***");
    Serial.printf("  Best config: #%d at %s\n", bestConfig, bn[bestBaud]);
    oledShow("*** SUCCESS ***", "Encoder responded!", "Check serial log");

    // Auto-enter continuous position reading with best config
    Serial.println("\n  Starting continuous position read (press any key to stop)...\n");
    EncoderSerial.begin(bauds[bestBaud], SERIAL_8N1,
                        configs[bestConfig].rxPin, configs[bestConfig].txPin);
    if (configs[bestConfig].invPin >= 0) {
      pinMode(configs[bestConfig].invPin, OUTPUT);
      esp_rom_gpio_connect_out_signal(configs[bestConfig].invPin, U1TXD_OUT_IDX, true, false);
    }

    uint32_t lastPos = 0xFFFFFFFF;
    int reads = 0;
    while (!Serial.available()) {
      while (EncoderSerial.available()) EncoderSerial.read();
      EncoderSerial.write((uint8_t)0x02);  // Short position read
      EncoderSerial.flush();
      delayMicroseconds(100);

      uint8_t rxBuf[8];
      int rxCount = 0;
      unsigned long deadline = millis() + 10;
      while (millis() < deadline && rxCount < 8) {
        if (EncoderSerial.available()) {
          rxBuf[rxCount++] = EncoderSerial.read();
          deadline = millis() + 2;
        }
      }

      if (rxCount >= 6 && rxBuf[0] == 0x02) {
        uint8_t calcCrc = tmg_crc8(rxBuf, 5);
        if (calcCrc == rxBuf[5]) {
          uint32_t pos = rxBuf[2] | (rxBuf[3] << 8) | (rxBuf[4] << 16);
          if (pos != lastPos) {
            float deg = pos * 360.0f / 131072.0f;
            Serial.printf("[%d] ABS=%6u  %.3f deg\n", reads, pos, deg);
            lastPos = pos;
            if (oledOk) {
              char b1[22], b2[22], b3[22];
              snprintf(b1, sizeof(b1), "Pos: %u", pos);
              snprintf(b2, sizeof(b2), "Deg: %.2f", deg);
              snprintf(b3, sizeof(b3), "Reads: %d", reads);
              oledShow("ENCODER LIVE", b1, b2, b3);
            }
          }
          reads++;
        }
      }
      delay(10);
    }
    while (Serial.available()) Serial.read();
    Serial.printf("Stopped after %d reads\n", reads);

    if (configs[bestConfig].invPin >= 0) {
      gpio_reset_pin((gpio_num_t)configs[bestConfig].invPin);
    }
    EncoderSerial.end();

    // Restore pins
    pinMode(PIN_ENC_TXP, INPUT);
    pinMode(PIN_ENC_RXP, INPUT);
    pinMode(PIN_ENC_TXN, OUTPUT); digitalWrite(PIN_ENC_TXN, LOW);
    pinMode(PIN_ENC_RXN, OUTPUT); digitalWrite(PIN_ENC_RXN, LOW);
  } else {
    Serial.println("  No valid responses.");
    Serial.println("  Possible issues:");
    Serial.println("    1) Encoder may use non-standard baud rate");
    Serial.println("    2) Pin mapping might still be wrong");
    Serial.println("    3) 3.3V single-ended may not meet RS-422 threshold");
    Serial.println("    4) Try swapping DarkRed<->Blue and/or Purple<->White");
    Serial.println("    5) May still need MAX485 transceiver");
    oledShow("Mode D Done", "No valid resp", "Check serial log");
  }
  Serial.println("============================================");
}

// ============================================================
// Mode S: SSI / BiSS-C clock-synchronized serial read
// Clock-based protocols: master clocks, encoder shifts data out.
// Some encoders use this instead of Tamagawa-style UART.
//
// Wiring hypothesis:
//   CLK+ = GPIO5 (Blue)    — we drive clock here
//   CLK- = GPIO7 (White)   — we drive inverted clock here
//   DATA+= GPIO4 (DarkRed) — encoder shifts data out here
//   DATA-= GPIO6 (Purple)  — complementary data output
//
// SSI: CLK idles HIGH, data shifts on falling edge, sample on rising
// BiSS: CLK runs continuously, ACK+START handshake, then data+CRC
// ============================================================
void mode_ssi_biss() {
  Serial.println("\n============================================");
  Serial.println("  MODE S: SSI / BiSS-C CLOCK-SYNC READ");
  Serial.println("  Maybe encoder needs CLOCK, not UART!");
  Serial.println("============================================\n");

  Serial.println("Hypothesis: DarkRed+Purple = DATA pair (output)");
  Serial.println("            Blue+White = CLOCK pair (input)");
  Serial.println("            Encoder waits for clock pulses\n");

  oledShow("Mode S: SSI", "Clock sync read", "Bit-bang clock");

  // Configure pins
  pinMode(PIN_ENC_RXP, OUTPUT);  // GPIO5 = CLK+ (Blue)
  pinMode(PIN_ENC_RXN, OUTPUT);  // GPIO7 = CLK- (White)
  pinMode(PIN_ENC_TXP, INPUT);   // GPIO4 = DATA+ (DarkRed)
  pinMode(PIN_ENC_TXN, INPUT);   // GPIO6 = DATA- (Purple)

  // ---- Phase 1: SSI bit-bang at multiple clock speeds ----
  Serial.println(">>> Phase 1: SSI protocol (CLK idle HIGH)");

  int halfPeriodUs[] = {100, 50, 10, 5, 2, 1};
  const char* freqLabel[] = {"5kHz","10kHz","50kHz","100kHz","250kHz","500kHz"};

  for (int sp = 0; sp < 6; sp++) {
    int hp = halfPeriodUs[sp];
    Serial.printf("\n  SSI @ ~%s (hp=%dus):\n", freqLabel[sp], hp);

    // SSI idle: CLK HIGH
    digitalWrite(PIN_ENC_RXP, HIGH);  // CLK+ HIGH
    digitalWrite(PIN_ENC_RXN, LOW);   // CLK- LOW (inverted)
    delayMicroseconds(100);  // Let encoder load position data

    // Read 32 bits on DATA+ with clock pulses
    uint32_t dataP = 0;
    uint32_t dataN = 0;

    noInterrupts();
    for (int bit = 31; bit >= 0; bit--) {
      // Falling edge: CLK+ goes LOW, CLK- goes HIGH
      digitalWrite(PIN_ENC_RXP, LOW);
      digitalWrite(PIN_ENC_RXN, HIGH);
      if (hp > 1) delayMicroseconds(hp);
      else ets_delay_us(hp);

      // Rising edge: CLK+ goes HIGH, CLK- goes LOW → sample DATA
      digitalWrite(PIN_ENC_RXP, HIGH);
      digitalWrite(PIN_ENC_RXN, LOW);
      if (hp > 1) delayMicroseconds(hp > 1 ? hp - 1 : 0);

      // Sample DATA pins
      dataP |= ((uint32_t)digitalRead(PIN_ENC_TXP)) << bit;
      dataN |= ((uint32_t)digitalRead(PIN_ENC_TXN)) << bit;

      if (hp > 1) delayMicroseconds(1);
    }
    interrupts();

    // Return CLK to idle HIGH
    digitalWrite(PIN_ENC_RXP, HIGH);
    digitalWrite(PIN_ENC_RXN, LOW);

    // Print results
    Serial.printf("    DATA+(DarkRed): 0x%08X = ", dataP);
    for (int i = 31; i >= 0; i--) Serial.print((int)((dataP >> i) & 1));
    Serial.println();
    Serial.printf("    DATA-(Purple):  0x%08X = ", dataN);
    for (int i = 31; i >= 0; i--) Serial.print((int)((dataN >> i) & 1));
    Serial.println();

    // Check if data is meaningful
    uint32_t xorVal = dataP ^ dataN;
    int diffBits = 0;
    for (int i = 0; i < 32; i++) if ((xorVal >> i) & 1) diffBits++;

    if (dataP == 0 && dataN == 0) {
      Serial.println("    Both zero — no response");
    } else if (dataP == 0xFFFFFFFF && dataN == 0xFFFFFFFF) {
      Serial.println("    Both 0xFFFF — stuck HIGH");
    } else if (diffBits == 32) {
      Serial.println("    *** PERFECT DIFFERENTIAL! DATA+ and DATA- are complementary ***");
      // Extract position (assuming 17-bit, MSB first)
      uint32_t pos17 = (dataP >> 15) & 0x1FFFF;  // Top 17 bits
      float deg = pos17 * 360.0f / 131072.0f;
      Serial.printf("    Position (17-bit MSB): %u = %.3f deg\n", pos17, deg);
      // Try Gray code decode
      uint32_t gray = pos17;
      uint32_t bin = gray;
      for (uint32_t mask = bin >> 1; mask; mask >>= 1) bin ^= mask;
      float degGray = bin * 360.0f / 131072.0f;
      Serial.printf("    Gray decoded: %u = %.3f deg\n", bin, degGray);
    } else if (diffBits > 24) {
      Serial.println("    Mostly differential — likely valid data!");
      uint32_t pos17 = (dataP >> 15) & 0x1FFFF;
      Serial.printf("    Position (17-bit MSB): %u\n", pos17);
    } else if (dataP != 0 || dataN != 0) {
      Serial.printf("    Non-zero data! XOR diff bits: %d/32\n", diffBits);
    }

    delay(50);  // Monoflop reload time
  }

  // ---- Phase 2: SSI with CLK idle LOW (some encoders use this) ----
  Serial.println("\n>>> Phase 2: Inverted SSI (CLK idle LOW)");

  for (int sp = 2; sp < 5; sp++) {  // Just try 50k, 100k, 250k
    int hp = halfPeriodUs[sp];
    Serial.printf("  Inv SSI @ ~%s: ", freqLabel[sp]);

    // CLK idle: LOW
    digitalWrite(PIN_ENC_RXP, LOW);
    digitalWrite(PIN_ENC_RXN, HIGH);
    delayMicroseconds(100);

    uint32_t dataP = 0;
    noInterrupts();
    for (int bit = 31; bit >= 0; bit--) {
      // Rising edge
      digitalWrite(PIN_ENC_RXP, HIGH);
      digitalWrite(PIN_ENC_RXN, LOW);
      if (hp > 1) delayMicroseconds(hp);
      else ets_delay_us(hp);

      // Falling edge + sample
      digitalWrite(PIN_ENC_RXP, LOW);
      digitalWrite(PIN_ENC_RXN, HIGH);
      dataP |= ((uint32_t)digitalRead(PIN_ENC_TXP)) << bit;
      if (hp > 1) delayMicroseconds(hp);
      else ets_delay_us(hp);
    }
    interrupts();
    digitalWrite(PIN_ENC_RXP, LOW);
    digitalWrite(PIN_ENC_RXN, HIGH);

    if (dataP == 0) Serial.println("zero");
    else if (dataP == 0xFFFFFFFF) Serial.println("all 1s");
    else {
      Serial.printf("0x%08X !!!\n", dataP);
      uint32_t pos17 = (dataP >> 15) & 0x1FFFF;
      float deg = pos17 * 360.0f / 131072.0f;
      Serial.printf("    Position: %u = %.3f deg\n", pos17, deg);
    }
    delay(50);
  }

  // ---- Phase 3: BiSS-C protocol ----
  Serial.println("\n>>> Phase 3: BiSS-C protocol (continuous clock, wait for ACK)");
  oledShow("Mode S: BiSS", "ACK handshake", "Continuous clock");

  for (int sp = 2; sp < 5; sp++) {
    int hp = halfPeriodUs[sp];
    Serial.printf("\n  BiSS-C @ ~%s:\n", freqLabel[sp]);

    // Start with CLK running, look for ACK (DATA goes HIGH)
    uint8_t rawBits[64];
    int totalBits = 0;

    // CLK starts LOW
    digitalWrite(PIN_ENC_RXP, LOW);
    digitalWrite(PIN_ENC_RXN, HIGH);
    delay(1);

    noInterrupts();
    for (int bit = 0; bit < 64; bit++) {
      // Rising edge
      digitalWrite(PIN_ENC_RXP, HIGH);
      digitalWrite(PIN_ENC_RXN, LOW);
      if (hp > 1) delayMicroseconds(hp);
      else ets_delay_us(hp);

      // Sample on rising edge
      rawBits[bit] = digitalRead(PIN_ENC_TXP);

      // Falling edge
      digitalWrite(PIN_ENC_RXP, LOW);
      digitalWrite(PIN_ENC_RXN, HIGH);
      if (hp > 1) delayMicroseconds(hp);
      else ets_delay_us(hp);

      totalBits = bit + 1;
    }
    interrupts();

    // Print raw bit stream
    Serial.printf("    Raw %d bits: ", totalBits);
    for (int i = 0; i < totalBits; i++) Serial.print(rawBits[i]);
    Serial.println();

    // Look for BiSS-C pattern: ACK(1) then START(1) then data...
    int ackPos = -1;
    for (int i = 0; i < totalBits; i++) {
      if (rawBits[i] == 1) { ackPos = i; break; }
    }
    if (ackPos >= 0) {
      Serial.printf("    ACK found at bit %d!\n", ackPos);
      // Look for START (next 1 after possible 0s)
      int startPos = -1;
      for (int i = ackPos + 1; i < totalBits; i++) {
        if (rawBits[i] == 0) {
          // Found gap after ACK
          for (int j = i + 1; j < totalBits; j++) {
            if (rawBits[j] == 1) { startPos = j; break; }
          }
          break;
        }
      }
      if (startPos < 0) startPos = ackPos + 1;

      // Decode data after START
      uint32_t pos = 0;
      int dataBits = 0;
      for (int i = startPos; i < totalBits && dataBits < 17; i++) {
        pos = (pos << 1) | rawBits[i];
        dataBits++;
      }
      float deg = pos * 360.0f / 131072.0f;
      Serial.printf("    Data after START (bit %d): %d bits, pos=%u = %.3f deg\n",
                    startPos, dataBits, pos, deg);
    } else {
      Serial.println("    No ACK (all zeros)");
    }
    delay(50);
  }

  // ---- Phase 4: SPI hardware (precise clock) ----
  Serial.println("\n>>> Phase 4: ESP32-S3 Hardware SPI");
  oledShow("Mode S: SPI", "Hardware clock", "Precise timing");

  // Use HSPI (SPI2) with custom pins via GPIO matrix
  // SCLK = GPIO5 (Blue, CLK+), MISO = GPIO4 (DarkRed, DATA+)
  SPIClass encoderSPI(FSPI);

  long spiFreqs[] = {100000, 500000, 1000000, 2000000, 4000000};
  const char* spiLabel[] = {"100kHz","500kHz","1MHz","2MHz","4MHz"};

  for (int f = 0; f < 5; f++) {
    Serial.printf("  SPI @ %s (Mode 0,1,2,3):\n", spiLabel[f]);

    for (int mode = 0; mode < 4; mode++) {
      encoderSPI.begin(PIN_ENC_RXP, PIN_ENC_TXP, -1, -1);  // SCLK=5, MISO=4

      // Route inverted SPI clock to GPIO7 for pseudo-differential
      // HSPI CLK output signal index may vary; try HSPICLK_OUT_IDX
      pinMode(PIN_ENC_RXN, OUTPUT);
      esp_rom_gpio_connect_out_signal(PIN_ENC_RXN, FSPICLK_OUT_IDX, true, false);

      uint8_t spiMode = mode;
      encoderSPI.beginTransaction(SPISettings(spiFreqs[f], MSBFIRST, spiMode));

      uint8_t rx[4] = {0};
      encoderSPI.transferBytes(nullptr, rx, 4);  // 32 clock pulses, read 32 bits

      encoderSPI.endTransaction();
      encoderSPI.end();

      // Disconnect inverted clock
      gpio_reset_pin((gpio_num_t)PIN_ENC_RXN);
      pinMode(PIN_ENC_RXN, INPUT);

      uint32_t val = ((uint32_t)rx[0] << 24) | ((uint32_t)rx[1] << 16) |
                     ((uint32_t)rx[2] << 8) | rx[3];

      if (val == 0 || val == 0xFFFFFFFF) {
        Serial.printf("    Mode %d: %s\n", mode,
                      val == 0 ? "zero" : "all 1s");
      } else {
        Serial.printf("    Mode %d: 0x%08X *** NON-TRIVIAL! ***\n", mode, val);
        uint32_t pos17 = (val >> 15) & 0x1FFFF;
        float deg = pos17 * 360.0f / 131072.0f;
        Serial.printf("      17-bit MSB: %u = %.3f deg\n", pos17, deg);
      }
    }
    delay(50);
  }

  // ---- Phase 5: Single-ended clock (no differential) ----
  Serial.println("\n>>> Phase 5: Single-ended clock variations");
  oledShow("Mode S: Single", "No diff CLK");

  // Try clocking on JUST GPIO5, no inverted complement
  Serial.println("  Clock on GPIO5 only (no inverted on GPIO7):");
  for (int sp = 2; sp < 5; sp++) {
    int hp = halfPeriodUs[sp];
    Serial.printf("    %s: ", freqLabel[sp]);

    pinMode(PIN_ENC_RXN, INPUT);  // Don't drive GPIO7
    digitalWrite(PIN_ENC_RXP, HIGH);
    delayMicroseconds(100);

    uint32_t dataP = 0;
    noInterrupts();
    for (int bit = 31; bit >= 0; bit--) {
      digitalWrite(PIN_ENC_RXP, LOW);
      if (hp > 1) delayMicroseconds(hp); else ets_delay_us(hp);
      digitalWrite(PIN_ENC_RXP, HIGH);
      dataP |= ((uint32_t)digitalRead(PIN_ENC_TXP)) << bit;
      if (hp > 1) delayMicroseconds(hp); else ets_delay_us(hp);
    }
    interrupts();

    if (dataP == 0) Serial.println("zero");
    else if (dataP == 0xFFFFFFFF) Serial.println("all 1s");
    else Serial.printf("0x%08X *** DATA! ***\n", dataP);
    delay(50);
  }

  // Also try clocking on GPIO7
  Serial.println("  Clock on GPIO7 only:");
  pinMode(PIN_ENC_RXP, INPUT);
  pinMode(PIN_ENC_RXN, OUTPUT);
  for (int sp = 2; sp < 5; sp++) {
    int hp = halfPeriodUs[sp];
    Serial.printf("    %s: ", freqLabel[sp]);

    digitalWrite(PIN_ENC_RXN, HIGH);
    delayMicroseconds(100);

    uint32_t dataP = 0;
    noInterrupts();
    for (int bit = 31; bit >= 0; bit--) {
      digitalWrite(PIN_ENC_RXN, LOW);
      if (hp > 1) delayMicroseconds(hp); else ets_delay_us(hp);
      digitalWrite(PIN_ENC_RXN, HIGH);
      dataP |= ((uint32_t)digitalRead(PIN_ENC_TXP)) << bit;
      if (hp > 1) delayMicroseconds(hp); else ets_delay_us(hp);
    }
    interrupts();

    if (dataP == 0) Serial.println("zero");
    else if (dataP == 0xFFFFFFFF) Serial.println("all 1s");
    else Serial.printf("0x%08X *** DATA! ***\n", dataP);
    delay(50);
  }

  // Restore pins
  pinMode(PIN_ENC_TXP, INPUT);
  pinMode(PIN_ENC_RXP, INPUT);
  pinMode(PIN_ENC_TXN, OUTPUT); digitalWrite(PIN_ENC_TXN, LOW);
  pinMode(PIN_ENC_RXN, OUTPUT); digitalWrite(PIN_ENC_RXN, LOW);
  batHigh = false;

  Serial.println("\n============================================");
  Serial.println("  SSI/BiSS SCAN COMPLETE");
  Serial.println("  If any non-zero data found → encoder uses clock protocol!");
  Serial.println("============================================");
  oledShow("Mode S Done", "Check serial log");
}

// ============================================================
// Mode N: Nikon A-format CDF sweep (structured brute-force)
// Uses protocol field hints from TI headers:
//   sync(3) + frame(2) + cmd(5) + addr(3) + tx_crc(3) = 16 bits
// Framed on wire as start(0) + 16-bit field + stop(1) => 18 bits.
// ============================================================
static uint16_t rev16(uint16_t v) {
  uint16_t r = 0;
  for (int i = 0; i < 16; i++) {
    r <<= 1;
    r |= (v >> i) & 1U;
  }
  return r;
}

static inline uint32_t readCcount() {
  uint32_t c;
  asm volatile("rsr %0,ccount" : "=a"(c));
  return c;
}

static inline void waitUntilCycle(uint32_t target) {
  while ((int32_t)(readCcount() - target) < 0) {
  }
}

struct DiffPinsFast {
  int aPin;
  int bPin;
  uint32_t aMask;
  uint32_t bMask;
  bool fast;
};

static DiffPinsFast makeDiffPins(int aPin, int bPin) {
  DiffPinsFast p;
  p.aPin = aPin;
  p.bPin = bPin;
  p.fast = (aPin >= 0 && aPin <= 31 && bPin >= 0 && bPin <= 31);
  p.aMask = (1UL << aPin);
  p.bMask = (1UL << bPin);
  return p;
}

static inline void setDiffABFast(const DiffPinsFast& p, bool bit, bool invertLinePolarity) {
  // Default: mark(1) => A=1, B=0. If inverted, swap line polarity.
  bool a = invertLinePolarity ? !bit : bit;
  bool b = !a;
  if (p.fast) {
    if (a) GPIO.out_w1ts = p.aMask;
    else GPIO.out_w1tc = p.aMask;
    if (b) GPIO.out_w1ts = p.bMask;
    else GPIO.out_w1tc = p.bMask;
  } else {
    digitalWrite(p.aPin, a ? HIGH : LOW);
    digitalWrite(p.bPin, b ? HIGH : LOW);
  }
}

static inline void setDiffAB(int aPin, int bPin, bool bit) {
  // RS-485 logic convention here: A=1, B=0 for mark; A=0, B=1 for space.
  digitalWrite(aPin, bit ? HIGH : LOW);
  digitalWrite(bPin, bit ? LOW : HIGH);
}

static void sendAfmt18(int aPin, int bPin, uint16_t payload16, bool lsbFirst, int bitUs) {
  pinMode(aPin, OUTPUT);
  pinMode(bPin, OUTPUT);

  // Idle (mark)
  setDiffAB(aPin, bPin, true);
  delayMicroseconds(bitUs);

  // Start bit = 0
  setDiffAB(aPin, bPin, false);
  delayMicroseconds(bitUs);

  // 16 payload bits
  for (int i = 0; i < 16; i++) {
    int bi = lsbFirst ? i : (15 - i);
    bool bit = ((payload16 >> bi) & 1U) != 0;
    setDiffAB(aPin, bPin, bit);
    delayMicroseconds(bitUs);
  }

  // Stop bit = 1
  setDiffAB(aPin, bPin, true);
  delayMicroseconds(bitUs);

  // Release line so encoder can drive it
  pinMode(aPin, INPUT);
  pinMode(bPin, INPUT);
}

static void sendAfmt18Fast(const DiffPinsFast& pins,
                           uint16_t payload16,
                           bool lsbFirst,
                           bool invertLinePolarity,
                           uint32_t bitCycles) {
  pinMode(pins.aPin, OUTPUT);
  pinMode(pins.bPin, OUTPUT);

  uint32_t t = readCcount();

  // Idle mark
  setDiffABFast(pins, true, invertLinePolarity);
  t += bitCycles;
  waitUntilCycle(t);

  // Start bit = 0
  setDiffABFast(pins, false, invertLinePolarity);
  t += bitCycles;
  waitUntilCycle(t);

  // 16 payload bits
  for (int i = 0; i < 16; i++) {
    int bi = lsbFirst ? i : (15 - i);
    bool bit = ((payload16 >> bi) & 1U) != 0;
    setDiffABFast(pins, bit, invertLinePolarity);
    t += bitCycles;
    waitUntilCycle(t);
  }

  // Stop bit = 1
  setDiffABFast(pins, true, invertLinePolarity);
  t += bitCycles;
  waitUntilCycle(t);

  pinMode(pins.aPin, INPUT);
  pinMode(pins.bPin, INPUT);
}

static int captureAfmtBits(int aPin, int bPin, int bitUs, int maxBits, int timeoutUs, uint8_t* outBits) {
  unsigned long t0 = micros();

  // Wait for any non-idle differential state as response start hint
  while ((micros() - t0) < (unsigned long)timeoutUs) {
    int a = digitalRead(aPin);
    int b = digitalRead(bPin);
    if (!(a == 1 && b == 0)) break;
  }

  if ((micros() - t0) >= (unsigned long)timeoutUs) return 0;

  // Sample in the middle of each bit cell
  delayMicroseconds(bitUs / 2);

  int n = 0;
  for (; n < maxBits; n++) {
    int a = digitalRead(aPin);
    int b = digitalRead(bPin);

    // Strong differential decode first, fallback to A level if ambiguous
    uint8_t bit;
    if (a == 1 && b == 0) bit = 1;
    else if (a == 0 && b == 1) bit = 0;
    else bit = (uint8_t)a;

    outBits[n] = bit;
    delayMicroseconds(bitUs);
  }
  return n;
}

static int captureAfmtBitsFast(const DiffPinsFast& pins,
                               uint32_t bitCycles,
                               int maxBits,
                               uint32_t timeoutCycles,
                               bool invertLinePolarity,
                               uint8_t* outBits) {
  const int idleA = invertLinePolarity ? 0 : 1;
  const int idleB = invertLinePolarity ? 1 : 0;

  uint32_t t0 = readCcount();
  while ((uint32_t)(readCcount() - t0) < timeoutCycles) {
    int a = digitalRead(pins.aPin);
    int b = digitalRead(pins.bPin);
    if (!(a == idleA && b == idleB)) break;
  }
  if ((uint32_t)(readCcount() - t0) >= timeoutCycles) return 0;

  uint32_t t = readCcount() + (bitCycles / 2U);
  waitUntilCycle(t);

  int n = 0;
  for (; n < maxBits; n++) {
    int a = digitalRead(pins.aPin);
    int b = digitalRead(pins.bPin);

    uint8_t bit;
    if (!invertLinePolarity) {
      if (a == 1 && b == 0) bit = 1;
      else if (a == 0 && b == 1) bit = 0;
      else bit = (uint8_t)a;
    } else {
      if (a == 0 && b == 1) bit = 1;
      else if (a == 1 && b == 0) bit = 0;
      else bit = (uint8_t)(!a);
    }

    outBits[n] = bit;
    t += bitCycles;
    waitUntilCycle(t);
  }
  return n;
}

static int captureAfmtBitsPhaseFast(const DiffPinsFast& pins,
                                    uint32_t bitCycles,
                                    uint32_t phaseCycles,
                                    int maxBits,
                                    uint32_t timeoutCycles,
                                    bool invertLinePolarity,
                                    uint8_t* outBits) {
  const int idleA = invertLinePolarity ? 0 : 1;
  const int idleB = invertLinePolarity ? 1 : 0;

  uint32_t t0 = readCcount();
  while ((uint32_t)(readCcount() - t0) < timeoutCycles) {
    int a = digitalRead(pins.aPin);
    int b = digitalRead(pins.bPin);
    if (!(a == idleA && b == idleB)) break;
  }
  if ((uint32_t)(readCcount() - t0) >= timeoutCycles) return 0;

  uint32_t t = readCcount() + phaseCycles;
  waitUntilCycle(t);

  int n = 0;
  for (; n < maxBits; n++) {
    int a = digitalRead(pins.aPin);
    int b = digitalRead(pins.bPin);

    uint8_t bit;
    if (!invertLinePolarity) {
      if (a == 1 && b == 0) bit = 1;
      else if (a == 0 && b == 1) bit = 0;
      else bit = (uint8_t)a;
    } else {
      if (a == 0 && b == 1) bit = 1;
      else if (a == 1 && b == 0) bit = 0;
      else bit = (uint8_t)(!a);
    }

    outBits[n] = bit;
    t += bitCycles;
    waitUntilCycle(t);
  }
  return n;
}

static void printBitVector(const uint8_t* bits, int n) {
  for (int i = 0; i < n; i++) {
    Serial.print(bits[i] ? '1' : '0');
    if ((i + 1) % 18 == 0) Serial.print(' ');
  }
}

static int scoreBitVector(const uint8_t* bits, int n) {
  bool all0 = true;
  bool all1 = true;
  int trans = 0;
  int ones = 0;
  int trailingZeros = 0;
  for (int i = 0; i < n; i++) {
    if (bits[i]) {
      all0 = false;
      ones++;
      trailingZeros = 0;
    } else {
      all1 = false;
      trailingZeros++;
    }
    if (i > 0 && bits[i] != bits[i - 1]) trans++;
  }
  if (all0 || all1) return -1000;
  return (trans * 8) + ones - (trailingZeros / 2);
}

static void refineVp485Candidate(const DiffPinsFast& pins,
                                 uint16_t payload16,
                                 bool lsbFirst,
                                 bool invertLinePolarity,
                                 int profile,
                                 uint32_t baseBitCycles) {
  const int bitDelta[] = {-8, -4, 0, 4, 8};
  const int phaseNum[] = {1, 2, 3, 4, 5, 6, 7};
  uint8_t bits[54];
  int bestScore = -100000;
  int bestN = 0;
  uint8_t bestBits[54];
  uint32_t bestCycles = baseBitCycles;
  uint32_t bestPhase = baseBitCycles / 2U;
  uint32_t bestWord18 = 0;

  for (int bi = 0; bi < (int)(sizeof(bitDelta) / sizeof(bitDelta[0])); bi++) {
    uint32_t bitCycles = (uint32_t)((int32_t)baseBitCycles + bitDelta[bi]);
    if (bitCycles < 32) bitCycles = 32;
    uint32_t timeoutCycles = bitCycles * 12U;

    for (int pi = 0; pi < (int)(sizeof(phaseNum) / sizeof(phaseNum[0])); pi++) {
      uint32_t phaseCycles = (bitCycles * (uint32_t)phaseNum[pi]) / 8U;
      sendAfmt18Fast(pins, payload16, lsbFirst, invertLinePolarity, bitCycles);
      delayMicroseconds(3);
      sendAfmt18Fast(pins, payload16, lsbFirst, invertLinePolarity, bitCycles);

      int n = captureAfmtBitsPhaseFast(pins, bitCycles, phaseCycles, 54, timeoutCycles, invertLinePolarity, bits);
      if (n <= 0) continue;

      int score = scoreBitVector(bits, n);
      if (score > bestScore) {
        bestScore = score;
        bestN = n;
        bestCycles = bitCycles;
        bestPhase = phaseCycles;
        bestWord18 = 0;
        for (int i = 0; i < n && i < 18; i++) {
          bestWord18 = (bestWord18 << 1) | (uint32_t)(bits[i] ? 1U : 0U);
        }
        for (int i = 0; i < n; i++) bestBits[i] = bits[i];
      }
    }
  }

  if (bestN > 0 && bestScore > 15) {
    Serial.printf("      REFINE p=%d cyc=%u phase=%u score=%d word18=0x%05lX bits=",
                  profile, bestCycles, bestPhase, bestScore, (unsigned long)bestWord18);
    printBitVector(bestBits, bestN);
    Serial.println();

    if (bestScore >= 30) {
      if (bestWord18 == lastVpWord18) lastVpWordCount++;
      else {
        lastVpWord18 = bestWord18;
        lastVpWordCount = 1;
      }

      updateVpAngleFromWord(bestWord18, true);

      oledShowVp485Candidate(bestWord18, profile, bestScore);
      Serial.printf("      OLED candidate updated: word18=0x%05lX data12=0x%03lX seen=%d now=%.2f total=%.2f turns=%.3f spd=%.1f dir=%s upd=%lu\n",
                    (unsigned long)bestWord18,
                    (unsigned long)((bestWord18 >> 6) & 0x0FFFU),
                    lastVpWordCount,
                    vpLastDeg,
                    vpTotalDeg,
                    vpTotalDeg / 360.0f,
            vpFiltSpeedDps,
            vpDirectionTag(),
                    (unsigned long)vpAngleUpdates);
      Serial.printf("        quality=%d%% trusted=%lu rejected=%lu\n",
                    vpTrustedPercent(),
                    (unsigned long)vpTrustedSamples,
                    (unsigned long)vpRejectedSamples);
    }
  }
}

void mode_nikon_cdf_sweep() {
  Serial.println("\n============================================");
  Serial.println("  MODE N: NIKON A-FORMAT CDF SWEEP");
  Serial.println("  Structured 18-bit field brute-force");
  Serial.println("============================================\n");

  Serial.println("Using RS-485 pair assumption:");
  Serial.printf("  A/DATA+ = GPIO%d (Blue)\n", PIN_ENC_RXP);
  Serial.printf("  B/DATA- = GPIO%d (DarkRed)\n", PIN_ENC_TXP);
  Serial.println("  Frame: start(0) + payload16 + stop(1)");
  Serial.println("  payload16 = sync(3)|frame(2)|cmd(5)|addr(3)|crc3(3)\n");

  oledShow("Mode N: Nikon", "CDF sweep", "sync/frame/cmd", "addr/crc3 brute");

  const uint8_t cmdList[] = {0, 1, 3, 4, 5, 21, 22, 27, 28, 29, 30};
  const int cmdCount = sizeof(cmdList) / sizeof(cmdList[0]);
  const uint8_t addrList[] = {0, 1, 2, 3, 7};
  const int addrCount = sizeof(addrList) / sizeof(addrList[0]);
  const uint8_t syncList[] = {0, 7};
  const uint8_t frameList[] = {0, 3};

  const uint32_t baudList[] = {500000, 1000000, 2500000};
  const int speedCount = sizeof(baudList) / sizeof(baudList[0]);

  int cpuMHz = getCpuFrequencyMhz();
  DiffPinsFast pins = makeDiffPins(PIN_ENC_RXP, PIN_ENC_TXP);

  int hits = 0;
  uint8_t bits[72];  // up to 4 fields (4*18)

  for (int sp = 0; sp < speedCount; sp++) {
    uint32_t baud = baudList[sp];
    uint32_t bitCycles = (uint32_t)((cpuMHz * 1000000UL) / baud);
    if (bitCycles < 24) bitCycles = 24;  // keep minimum guard for software loop jitter
    uint32_t timeoutCycles = (uint32_t)(cpuMHz * 1200UL); // ~1200us wait window

    Serial.printf("\n>>> Sweep @ %lu bps (~%u cyc/bit, CPU=%dMHz)\n", baud, bitCycles, cpuMHz);

    for (int ci = 0; ci < cmdCount; ci++) {
      uint8_t cmd = cmdList[ci] & 0x1F;
      for (int ai = 0; ai < addrCount; ai++) {
        uint8_t addr = addrList[ai];
        for (uint8_t crc3 = 0; crc3 <= 7; crc3++) {
          for (int si = 0; si < 2; si++) {
            uint8_t sync = syncList[si];
            for (int fi = 0; fi < 2; fi++) {
              uint8_t frame = frameList[fi];

              uint16_t payload =
                ((uint16_t)(sync & 0x7U) << 13) |
                ((uint16_t)(frame & 0x3U) << 11) |
                ((uint16_t)cmd << 6) |
                ((uint16_t)addr << 3) |
                ((uint16_t)crc3);

              uint16_t payloadRev = rev16(payload);

              // Try LSB/MSB, payload/reversed payload, line polarity normal/inverted.
              uint16_t tries[4] = {payload, payload, payloadRev, payloadRev};
              bool lsb[4] = {true, false, true, false};

              for (int pol = 0; pol < 2; pol++) {
                bool invPol = (pol == 1);
                for (int ti = 0; ti < 4; ti++) {
                  sendAfmt18Fast(pins, tries[ti], lsb[ti], invPol, bitCycles);

                  int n = captureAfmtBitsFast(pins, bitCycles, 36, timeoutCycles, invPol, bits);
                  if (n <= 0) continue;

                  bool all0 = true;
                  bool all1 = true;
                  int trans = 0;
                  for (int i = 0; i < n; i++) {
                    if (bits[i]) all0 = false;
                    else all1 = false;
                    if (i > 0 && bits[i] != bits[i - 1]) trans++;
                  }
                  if (all0 || all1 || trans < 2) continue;

                  hits++;
                  Serial.printf("  HIT #%d b=%lu cmd=%u addr=%u sync=%u frame=%u crc3=%u ord=%s pack=%s pol=%s n=%d trans=%d bits=",
                                hits, baud, cmd, addr, sync, frame, crc3,
                                lsb[ti] ? "LSB" : "MSB",
                                (ti < 2) ? "norm" : "rev16",
                                invPol ? "inv" : "norm",
                                n, trans);
                  for (int i = 0; i < n; i++) {
                    Serial.print(bits[i] ? '1' : '0');
                    if ((i + 1) % 18 == 0) Serial.print(' ');
                  }
                  Serial.println();

                  if (hits >= 16) {
                    Serial.println("  Reached hit cap (16). Stopping early for analysis.");
                    goto done_nikon;
                  }
                }
              }
            }
          }
        }
      }
    }
  }

done_nikon:
  pinMode(PIN_ENC_RXP, INPUT);
  pinMode(PIN_ENC_TXP, INPUT);

  Serial.println("\n============================================");
  Serial.printf("  MODE N COMPLETE: %d candidate response hits\n", hits);
  if (hits == 0) {
    Serial.println("  No decoded activity up to 4Mbps software bit-bang sweep.");
    Serial.println("  Next tweak: expand command/sync hypotheses or capture with external host bus still connected.");
  }
  Serial.println("============================================");
  oledShow("Mode N Done", hits ? "Candidates found" : "No candidates", "Check serial log");
}

// ============================================================
// Mode V: VP485 link hunt (current wiring only)
// Repeats Nikon-like framed probing with larger listen windows
// and reports any non-trivial differential response candidates.
// ============================================================
void mode_vp485_link_hunt() {
  Serial.println("\n============================================");
  Serial.println("  MODE V: VP485 LINK HUNT (NO HW CHANGES)");
  Serial.println("  Current setup only: aggressive TX/RX timing scan");
  Serial.println("  Repeats forever until a serial key is pressed");
  Serial.println("============================================\n");

  // Reset integrated angle state for each new Mode V run.
  vpAngleInit = false;
  vpLastDeg = 0.0f;
  vpTotalDeg = 0.0f;
  vpAngleUpdates = 0;
  vpLastUpdateUs = 0;
  vpSpeedDps = 0.0f;
  vpFiltSpeedDps = 0.0f;
  vpLastDeltaDeg = 0.0f;
  vpTrustedSamples = 0;
  vpRejectedSamples = 0;
  lastVpWord18 = 0;
  lastVpWordCount = 0;

  oledShow("Mode V: VP485", "Looping forever", "Press key to stop");

  const uint32_t baudList[] = {500000, 1000000, 2500000, 4000000};
  const int speedCount = sizeof(baudList) / sizeof(baudList[0]);

  // Nikon A-format style command seeds (5-bit cmd field).
  const uint8_t cmdList[] = {0, 1, 3, 4, 5, 16, 21, 22, 27, 28, 29, 30, 31};
  const uint8_t addrList[] = {0, 1, 2, 3};

  // Try both common sync/frame hypotheses.
  const uint8_t syncList[] = {0, 7};
  const uint8_t frameList[] = {0, 3};

  const int cpuMHz = getCpuFrequencyMhz();
  DiffPinsFast pins = makeDiffPins(PIN_ENC_RXP, PIN_ENC_TXP);

  uint8_t bits[90];
  int hits = 0;

  struct WhiteProfile {
    const char* name;
    int mode;
  };
  // mode: 0=float, 1=pullup, 2=drive high, 3=pulse high then float
  WhiteProfile whiteProfiles[] = {
    {"WHITE float", 0},
    {"WHITE pullup", 1},
    {"WHITE drive HIGH", 2},
    {"WHITE pulse HIGH->float", 3},
  };
  const int profileCount = sizeof(whiteProfiles) / sizeof(whiteProfiles[0]);
  int pass = 0;

  if (pullUpsEnabled) {
    // Mode V manages line biasing explicitly.
    togglePullups();
  }

  while (!Serial.available()) {
    pass++;
    bool focusMode = (hits > 0);
    Serial.printf("\n>>> PASS %d START\n", pass);
    Serial.println(">>> Motor nudge: GPIO40/41/42 3-phase pre-pass");
    motorVerifyNudge(160, 220);
    if (focusMode) {
      Serial.println(">>> FOCUS MODE ACTIVE: restricting to proven hit region");
    }
    char passBuf[22];
    snprintf(passBuf, sizeof(passBuf), "Pass %d", pass);
    oledShow("Mode V: VP485", passBuf, "Looping forever", "Press key to stop");

    for (int prof = 0; prof < profileCount; prof++) {
      if (Serial.available()) goto done_vp485;
      Serial.printf("\n>>> Enable profile %d/%d: %s\n", prof + 1, profileCount, whiteProfiles[prof].name);

      // Purple is effectively ground return on this unit; hold low as reference.
      pinMode(PIN_ENC_TXN, OUTPUT);
      digitalWrite(PIN_ENC_TXN, LOW);

      // White/Orange may be backup/enable on some units; test several safe styles.
      if (whiteProfiles[prof].mode == 0) {
        pinMode(PIN_ENC_RXN, INPUT);
      } else if (whiteProfiles[prof].mode == 1) {
        pinMode(PIN_ENC_RXN, INPUT_PULLUP);
      } else if (whiteProfiles[prof].mode == 2) {
        pinMode(PIN_ENC_RXN, OUTPUT);
        digitalWrite(PIN_ENC_RXN, HIGH);
        delay(20);
      } else {
        pinMode(PIN_ENC_RXN, OUTPUT);
        digitalWrite(PIN_ENC_RXN, HIGH);
        delay(30);
        pinMode(PIN_ENC_RXN, INPUT);
        delay(5);
      }

      for (int sp = 0; sp < speedCount; sp++) {
        if (Serial.available()) goto done_vp485;
        const uint32_t baud = baudList[sp];
        if (focusMode && baud != 2500000) continue;
        uint32_t bitCycles = (uint32_t)((cpuMHz * 1000000UL) / baud);
        if (bitCycles < 24) bitCycles = 24;

        // Wider window for lower speeds.
        uint32_t timeoutCycles = (uint32_t)(cpuMHz * (baud <= 500000 ? 4000UL : 1600UL));

        Serial.printf("  Speed %lu bps (~%u cyc/bit)\n", baud, bitCycles);

        int txCount = 0;
        for (int si = 0; si < 2; si++) {
          const uint8_t sync = syncList[si];
          for (int fi = 0; fi < 2; fi++) {
            const uint8_t frame = frameList[fi];

            for (int ci = 0; ci < (int)(sizeof(cmdList) / sizeof(cmdList[0])); ci++) {
              const uint8_t cmd = cmdList[ci] & 0x1F;
              if (focusMode && !(cmd == 4 || cmd == 16)) continue;

              for (int ai = 0; ai < (int)(sizeof(addrList) / sizeof(addrList[0])); ai++) {
                const uint8_t addr = addrList[ai] & 0x07;
                if (focusMode && addr != 0) continue;

                for (uint8_t crc3i = 0; crc3i < 4; crc3i++) {
                  if (Serial.available()) goto done_vp485;
                  const uint8_t crc3 = (crc3i == 0) ? 0 : (crc3i == 1 ? 1 : (crc3i == 2 ? 3 : 7));
                  uint16_t payload =
                    ((uint16_t)(sync & 0x7U) << 13) |
                    ((uint16_t)(frame & 0x3U) << 11) |
                    ((uint16_t)cmd << 6) |
                    ((uint16_t)addr << 3) |
                    ((uint16_t)crc3);

                  uint16_t payloadRev = rev16(payload);
                  uint16_t tries[4] = {payload, payload, payloadRev, payloadRev};
                  bool lsb[4] = {true, false, true, false};

                  for (int pol = 0; pol < 2; pol++) {
                    bool invPol = (pol == 1);
                    if (focusMode && !invPol) continue;

                    for (int ti = 0; ti < 4; ti++) {
                      // Single frame + short gap + repeated frame can wake some receivers.
                      sendAfmt18Fast(pins, tries[ti], lsb[ti], invPol, bitCycles);
                      delayMicroseconds(4);
                      sendAfmt18Fast(pins, tries[ti], lsb[ti], invPol, bitCycles);
                      txCount++;
                      if ((txCount % 1500) == 0) {
                        Serial.printf("    ...progress pass=%d p=%d b=%lu tx=%d\n", pass, prof + 1, baud, txCount);
                      }

                      int n = captureAfmtBitsFast(pins, bitCycles, 72, timeoutCycles, invPol, bits);
                      if (n <= 0) continue;

                      bool all0 = true;
                      bool all1 = true;
                      int trans = 0;
                      for (int i = 0; i < n; i++) {
                        if (bits[i]) all0 = false;
                        else all1 = false;
                        if (i > 0 && bits[i] != bits[i - 1]) trans++;
                      }

                      // Require enough transitions to reject static/noise plateaus.
                      if (all0 || all1 || trans < 4) continue;

                      hits++;
                      Serial.printf("    HIT #%d pass=%d p=%d b=%lu cmd=%u addr=%u sync=%u frame=%u crc3=%u ord=%s pack=%s pol=%s n=%d trans=%d bits=",
                                    hits, pass, prof + 1, baud, cmd, addr, sync, frame, crc3,
                                    lsb[ti] ? "LSB" : "MSB",
                                    (ti < 2) ? "norm" : "rev16",
                                    invPol ? "inv" : "norm",
                                    n, trans);
                      printBitVector(bits, n);
                      Serial.println();

                      if (true) {
                        int hitScore = scoreBitVector(bits, n);
                        if (n >= 18 && hitScore > -999) {
                          uint32_t hitWord18 = 0;
                          for (int bi = 0; bi < 18; bi++) {
                            hitWord18 = (hitWord18 << 1) | (uint32_t)(bits[bi] ? 1U : 0U);
                          }
                          bool trustedMotion = (cmd == 16 && hitScore >= 10);
                          updateVpAngleFromWord(hitWord18, trustedMotion);
                          oledShowVp485Candidate(hitWord18, prof + 1, hitScore);
                          Serial.printf("      OLED fast update: word18=0x%05lX score=%d now=%.2f total=%.2f turns=%.3f spd=%.1f dir=%s upd=%lu\n",
                                        (unsigned long)hitWord18,
                                        hitScore,
                                        vpLastDeg,
                                        vpTotalDeg,
                                        vpTotalDeg / 360.0f,
                                        vpFiltSpeedDps,
                                        vpDirectionTag(),
                                        (unsigned long)vpAngleUpdates);
                          Serial.printf("        quality=%d%% trusted=%lu rejected=%lu\n",
                                        vpTrustedPercent(),
                                        (unsigned long)vpTrustedSamples,
                                        (unsigned long)vpRejectedSamples);
                        }

                        refineVp485Candidate(pins, tries[ti], lsb[ti], invPol, prof + 1, bitCycles);
                      }

                      if (hits >= 24) {
                        Serial.println("    Hit cap reached (24). Stopping for manual decode.");
                        goto done_vp485;
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }

    Serial.printf("\n>>> PASS %d COMPLETE, total hits=%d\n", pass, hits);
  }

done_vp485:
  while (Serial.available()) Serial.read();
  pinMode(PIN_ENC_RXP, INPUT);
  pinMode(PIN_ENC_TXP, INPUT);
  pinMode(PIN_ENC_TXN, OUTPUT);
  digitalWrite(PIN_ENC_TXN, LOW);
  pinMode(PIN_ENC_RXN, INPUT);

  Serial.println("\n============================================");
  Serial.printf("  MODE V COMPLETE: %d candidate response hits\n", hits);
  if (hits == 0) {
    Serial.println("  No candidate VP485 link detected in current-wiring hunt.");
    Serial.println("  Keep setup unchanged: rerun mode V after any power-cycle/shaft motion changes.");
  }
  Serial.println("============================================");
  oledShow("Mode V Done", hits ? "Candidates found" : "No candidates", "Check serial log");
}

// ============================================================
void setup() {
  Serial.begin(115200);
  delay(2000);

  Wire.begin(OLED_SDA, OLED_SCL);
  if (oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    oledOk = true;
    oledShow("Encoder v4", "Booting...");
  }

  pinMode(PIN_DATA_P, INPUT);
  pinMode(PIN_DATA_N, INPUT);
  pinMode(PIN_REQ, OUTPUT);
  pinMode(PIN_BAT, INPUT);
  digitalWrite(PIN_REQ, LOW);

  Serial.println();
  Serial.println("=== Absolute Encoder Probe v4a ===");
  Serial.println("=== Encoder on 5V, draws ~3.5mA ===");
  Serial.println("=== Blue LEDs clamp 5V to ~3V ===");
  Serial.printf("ENC_TX+=%d(DarkRed) ENC_RX+=%d(Blue) ENC_TX-=%d(Purple) ENC_RX-=%d(White)\n",
    PIN_ENC_TXP, PIN_ENC_RXP, PIN_ENC_TXN, PIN_ENC_RXN);
  Serial.printf("OLED: %s\n", oledOk ? "OK" : "not found");
  Serial.println();
  Serial.println("Commands:");
  Serial.println("  1=Passive  2=REQ pulse  3=Tamagawa");
  Serial.println("  4=Clock    5=UART       6=Diagnostics");
  Serial.println("  7=BRUTE FORCE");
  Serial.println("  d=PSEUDO-DIFF Tamagawa");
  Serial.println("  n=Nikon A-format CDF sweep");
  Serial.println("  v=VP485 link hunt (current setup)");
  Serial.println("  m=Motor nudge GPIO40/41/42 3-phase");
  Serial.println("  s=SSI/BiSS CLOCK-SYNC (try if d fails!)");
  Serial.println("  a=All(1-6) c=Continuous h=Toggle BAT+");
  Serial.println("  u=UART serial  p=Toggle pull-ups");
  Serial.println();

  int aP = analogRead(PIN_ENC_TXP);
  int aN = analogRead(PIN_ENC_RXP);
  int aR = analogRead(PIN_ENC_TXN);
  int aW = analogRead(PIN_ENC_RXN);
  Serial.printf("Idle: TXp=%.2fV RXp=%.2fV TXn=%.2fV RXn=%.2fV\n",
    aP*3.3/4095.0, aN*3.3/4095.0, aR*3.3/4095.0, aW*3.3/4095.0);

  char ib[32];
  snprintf(ib, sizeof(ib), "TX+:%.1fV RX+:%.1fV", aP*3.3/4095.0, aN*3.3/4095.0);
  oledShow("Encoder v4", "d=DIFF (try 1st!)", "1-7,a,u,p,c,h", ib);
}

// ============================================================
void loop() {
  if (Serial.available()) {
    char cmd = Serial.read();
    switch (cmd) {
      case '1': mode1_passive(); break;
      case '2': mode2_req_toggle(); break;
      case '3': mode3_tamagawa(); break;
      case '4': mode4_clock_read(); break;
      case '5': mode5_uart_probe(); break;
      case '6': mode6_diagnostics(); break;
      case '7': mode7_bruteforce(); break;
      case 'a':
        mode1_passive(); delay(200);
        mode2_req_toggle(); delay(200);
        mode3_tamagawa(); delay(200);
        mode4_clock_read(); delay(200);
        mode5_uart_probe(); delay(200);
        mode6_diagnostics();
        break;
      case 'c': continuous_read(); break;
      case 'h': toggleBat(); break;
      case 'u': mode_uart_serial(); break;
      case 'p': togglePullups(); break;
      case 'd': mode_diff_tamagawa(); break;
      case 'n': mode_nikon_cdf_sweep(); break;
      case 'v': mode_vp485_link_hunt(); break;
      case 'm':
        Serial.println("Motor nudge: GPIO40/41/42 3-phase");
        motorVerifyNudge(240, 220);
        break;
      case 's': mode_ssi_biss(); break;
      default: break;
    }
  }
}
