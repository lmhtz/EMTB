/*
 // color definitions
 TFT_BLACK       0x0000
 TFT_NAVY        0x000F
 TFT_DARKGREEN   0x03E0
 TFT_DARKCYAN    0x03EF
 TFT_MAROON      0x7800
 TFT_PURPLE      0x780F
 TFT_OLIVE       0x7BE0
 TFT_LIGHTGREY   0xC618
 TFT_DARKGREY    0x7BEF
 TFT_BLUE        0x001F
 TFT_GREEN       0x07E0
 TFT_CYAN        0x07FF
 TFT_RED         0xF800
 TFT_MAGENTA     0xF81F
 TFT_YELLOW      0xFFE0
 TFT_WHITE       0xFFFF
 TFT_ORANGE      0xFD20
 TFT_GREENYELLOW 0xAFE5
 TFT_PINK        0xF81F
*/

#include <Bounce2.h>
#include <EEPROM.h>
#include <RF24.h>
#include <RF24_config.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_ST7735.h>
#include <buffer.h>          //VESC
#include <crc.h>             //VESC
#include <datatypes.h>       //VESC
#include <local_datatypes.h> //VESC
#include <nRF24L01.h>
#include <printf.h>

/* PIN Definitions -> ProMini
                DTR|TX0|RXI|VCC|GND|GND --> FTDI programmer

                                VESC RX <-- TX		RAW
                                VESC TX <-- RX		GND --> Step-Down GND
                                                                                RST		RST
                                                                                GND		VCC --> Step-Down +3.3V
                                                                                                        A5
        Button Cruise <-- 2			A3 --> Poti(Throttle)
                                                                                                        A4
Button Settings <-- 3			A2
                CE nRF24L01 <-- 4			A1
                CS nRF24L01 <--	5			A0
                        CS SDcard <-- 6			13 --> nRF24L01 SCK  | SDcard SCK	 | TFT SCK
                                 CS TFT <-- 7			12 --> nRF24L01 MISO | SDcard MISO
                  Reset TFT <-- 8			11 --> nRF24L01 MOSI | SDcard MOSI | TFT SDA
                                 A0 TFT <--	9			10 --> TFT LED
*/

// Idea: Second/Third Poti for max AMPS

#define PIN_BTN_CRUISE 2
#define PIN_BTN_SETTINGS 3
#define PIN_RADIO_CS 4
#define PIN_RADIO_CE 5
#define PIN_SDCARD_CS 6
#define PIN_TFT_CS 7
#define PIN_TFT_RESET 8
#define PIN_TFT_A0 9
#define PIN_TFT_LED 10
#define PIN_POTI_THR A3
#define PIN_POTI_FWD A2
#define PIN_POTI_BREAK A1

// constants
const uint8_t channel = 77;
const uint64_t pipe = 0x52582d5458;                                                                         // 'RX-TX' pipe
const uint32_t time_settings = 4000;                                                                        // [ms]
const uint32_t SdAbortOk = 4000;                                                                            // [ms]
const uint8_t eeDeadband = 0;                                                                               // EEPROM Address
const uint8_t eeFwdMax = 1;                                                                                 // EEPROM Address
const uint8_t eeBreakMax = 2;                                                                               // EEPROM Address
const uint8_t eeFwdMin = 3;                                                                                 // EEPROM Address
const uint8_t eeBreakMin = 4;                                                                               // EEPROM Address
const uint8_t eeCellcount = 5;                                                                              // EEPROM Address
const uint16_t TFTrefresh = 500;                                                                            // [ms]
const uint16_t SDrefresh = 500;                                                                             // [ms]
const uint8_t wheelsize = 200;                                                                              // [mm]
const uint8_t gearratio = 3;                                                                                // [1:X]
const uint8_t pulse_rpm = 42;                                                                               // Number of poles * 3
const uint8_t erpm_rpm = 7;                                                                                 // Number of polse / 2
const float dist_corr_factor = 0.8;                                                                         // Number of polse / 2
const float ratio_RpmSpeed = (wheelsize * 3.141 * 60) / (erpm_rpm * gearratio * 1000000);                   // ERPM to km/h
const float ratio_TachoDist = ((wheelsize * 3.141) / (pulse_rpm * gearratio * 1000000)) * dist_corr_factor; // pulses to km
const uint16_t waitBeforeSend = 5000;                                                                       //[ms]

// globals
uint32_t TFTlastPaint;
uint32_t SDlastPrint;
uint32_t SettingsBtnPushTime;
uint32_t ridetime;
bool SendEnabled;
bool hasSDcard;
uint8_t amp_fwd_max;
uint8_t amp_break_max;
uint8_t amp_fwd_min;
uint8_t amp_break_min;
uint8_t cellcount;

// average
uint16_t avgSum = 0;
const uint8_t avgCnt = 10;
uint8_t avg[avgCnt];
uint8_t avgIdx = 0;

File logfile;

struct RemoteData RemoteDataStruct;

struct bldcMeasure VescMeasuredValues;

// functions
void WaitForSdAbort();
void drawLabels();
void drawValues();
void drawValuesNONE();
uint16_t gradientRYG(uint8_t value);
void drawBattery(uint16_t color);
void fillBattery(uint8_t value);
void settingsMenu();
void changeSettings(bool up, uint16_t currentS);
void drawSettings();
void drawSettingValues(uint16_t currentS);
void saveSettings();

// objects
RF24 radio(PIN_RADIO_CS, PIN_RADIO_CE); // Set up nRF24L01 radio on SPI bus
TFT_ST7735 tft = TFT_ST7735();          // pins defined in User_Setup.h // ToDo: Move pin definition to this file
Bounce DEB_cruise = Bounce();

void setup() {

  pinMode(PIN_BTN_CRUISE, INPUT_PULLUP);
  pinMode(PIN_BTN_SETTINGS, INPUT_PULLUP);
  pinMode(PIN_POTI_THR, INPUT);
  pinMode(PIN_POTI_FWD, INPUT);
  pinMode(PIN_POTI_BREAK, INPUT);

  DEB_cruise.attach(PIN_BTN_CRUISE); // standard-interval 10 ms

  EEPROM.get(eeDeadband, RemoteData._deadband);
  EEPROM.get(eeFwdMax, amp_fwd_max);
  EEPROM.get(eeBreakMax, amp_break_max);
  EEPROM.get(eeFwdMin, amp_fwd_min);
  EEPROM.get(eeBreakMin, amp_break_min);
  EEPROM.get(eeCellcount, cellcount);

  tft.init();
  tft.setRotation(0); // portrait
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("STARTUP", 64, 40, 4);

  // Check if the button is pressed at startup.
  // Holding it down longer then "time_settings" will enter the settingsMenu and abort startup
  while (digitalRead(PIN_BTN_SETTINGS)) {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawCentreString("Hold for Settings", 64, 100, 2);
    if (millis() > time_settings) {
      settingsMenu();
    }
  }
  tft.fillRect(10, 100, 109, 16, TFT_WHITE); // Overwrite "Settings"

  if (!SD.begin(PIN_SDCARD_CS)) {
    WaitForSdAbort();
  } else {
    hasSDcard = true;
    logfile = SD.open("logfile.txt", FILE_WRITE);
    if (!logfile)
      WaitForSdAbort();
  }

  Serial.begin(115200);
  // while (!Serial()) {
  // } // Wait for Serial

  // Setup and configure rf radio
  radio.begin();
  radio.setChannel(channel);
  radio.setPALevel(RF24_PA_MIN); // RF24_PA_MIN=-18dBm, RF24_PA_LOW=-12dBm, RF24_PA_MED=-6dBM, and RF24_PA_HIGH=0dBm.
  radio.setDataRate(RF24_2MBPS); // RF24_250KBPS for 250kbs, RF24_1MBPS for 1Mbps, or RF24_2MBPS for 2Mbps
  radio.enableDynamicPayloads(); // enabled for 'enableAckPayload()
  radio.enableAckPayload();
  radio.setRetries(1, 15);         // delay (n-1)x250µs // #retries max 15
  radio.setCRCLength(RF24_CRC_16); // Use 16-bit CRC for safety

  radio.openWritingPipe(pipe);

  radio.powerUp(); // Leave low-power mode - making radio more responsive. // powerDown() for low-power

  tft.fillScreen(TFT_BLACK);
  drawBattery(TFT_WHITE);
  fillBattery(0);
}

void WaitForSdAbort() {
  uint16_t waitStart = millis();
  bool pressStart;
  tft.drawCentreString("FAILED ...", 64, 100, 4);
  tft.drawString("Continue with [CRUISE] ...", 0, 100, 2);
  while (1) {
    pressStart = false;
    while (digitalRead(PIN_BTN_CRUISE)) {
      if (!pressStart) {
        pressStart = true;
        waitStart = millis();
      }
      tft.setTextColor(TFT_YELLOW, TFT_BLACK);
      tft.drawCentreString("Settings...", 64, 40, 2);
      if (millis() - waitStart > SdAbortOk) {
        hasSDcard = false;
        goto bailout;
      }
    }
  }
bailout:
  delay(1);
}

void loop() {
  // read POTI_THR and build average (we don't want a spiking throttle)
  avgSum -= avg[avgIdx];
  avg[avgIdx] = analogRead(PIN_POTI_THR);
  ;
  avgSum += avg[avgIdx];
  avgIdx++;
  if (avgIdx == avgCnt)
    avgIdx = 0;
  RemoteData.thr = avgSum / avgCnt;

  RemoteData._amp_fwd = map(analogRead(PIN_POTI_FWD), 0, 1023, amp_fwd_min, amp_fwd_max) RemoteData._amp_break = map(analogRead(PIN_POTI_BREAK), 0, 1023, amp_break_min, amp_break_max)

                                                                                                                 // readButtons
                                                                                                                 DEB_cruise.update();
  RemoteData.cruise = DEB_cruise.read();

  if (SendEnabled) {
    // send values to RX
    bool sendOK = radio.write(&RemoteData, sizeof(RemoteData));

    // recieve AckPayload
    while (radio.isAckPayloadAvailable()) {
      radio.read(&VescMeasuredValues, sizeof(VescMeasuredValues));
      bool recOK = true;
    }
  } else {
    if (millis() > waitBeforeSend)
      SendEnabled = true;
  }

  uint32_t _millis; // buffer 1x instead of 5x exec
  // Write Readings and AckPayload into a Logfile on the SD
  if (hasSDcard) {
    _millis = millis();
    if (_millis > SDlastPrint + SDrefresh) {
      String logString;
      logString += String(RemoteData.thr) + ";" + String(RemoteData.cruise) + ";" + String(RemoteData._deadband) + ";" + String(RemoteData._amp_fwd) + ";" + String(RemoteData._amp_break) + ";" + String(VescMeasuredValues.current_motor) +
                   ";" + String(VescMeasuredValues.current_in) + ";" + String(VescMeasuredValues.duty_now) + ";" + String(VescMeasuredValues.rpm) + ";" + String(VescMeasuredValues.v_in) + ";" + String(VescMeasuredValues.ampHours) + ";" +
                   String(VescMeasuredValues.ampHoursCharged) + ";" + String(VescMeasuredValues.tachometerAbs);
      logfile.println(logString);
      SDlastPrint = _millis;
    }
  }
  // Calculate Averages

  // Write Average-Values to screen (if changed)
  _millis = millis(); // buffer 1x instead of 5x exec
  if (_millis > TFTlastPaint + TFTrefresh) {
    if (digitalRead(PIN_BTN_SETTINGS)) {
      if (SettingsBtnPushTime == 0)
        SettingsBtnPushTime = _millis;
      if (_millis > SettingsBtnPushTime + 2000) {
        SettingsBtnPushTime = 0;
        ridetime = _millis; // Reset Ridetime. This is inside the TFT Loop so it doesn't get called every loop.
      }
    }
    drawValues();
    fillBattery(VescMeasuredValues.v_in * 255 / cellcount * 4.2);
    TFTlastPaint = _millis;
  }
}

void drawLabels() {
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("km/h", 65, 1, 2);
  tft.drawString("Motor", 2, 74, 2);
  tft.drawRightString("Duty", 76, 74, 2);
  tft.drawRightString("Dist", 78, 96, 2);
  tft.drawString(":", 23, 115, 2);
  tft.drawString(":", 47, 115, 2);
}

void drawValues() {
  drawValuesNONE();
  tft.setTextSize(2);
  tft.drawNumber(VescMeasuredValues.rpm * ratio_RpmSpeed, 7, 0, 4);
  tft.setTextSize(1);
  tft.drawNumber(RemoteData._amp_fwd / 2, 90, 20, 2);
  tft.drawNumber(RemoteData._amp_break / 10, 110, 20, 2);

  tft.drawNumber(VescMeasuredValues.v_in, 108 + 3, 100, 4);
  tft.drawNumber(VescMeasuredValues.avgMotorCurrent, 2, 51, 4);
  tft.drawNumber(VescMeasuredValues.dutyCycleNow, 49, 51, 4);
  tft.drawNumber(VescMeasuredValues.tachometer * ratio_TachoDist, 2, 96, 2);
  tft.drawNumber(millis() / (1000 * 60), 12, 115, 2);          // h
  tft.drawNumber((millis() % (1000 * 60)) / 1000, 53, 115, 2); // s
  tft.drawNumber(VescMeasuredValues.avgInputCurrent, 6, 134, 4);
}

void drawValuesNONE() {
  tft.fillRect(7, 0, 56, 38, TFT_BLACK);            // KMH
  tft.fillRect(90, 29, 28, 18, TFT_BLACK);          // ampSettings
  tft.fillRect(2, 52, 28, 18, TFT_BLACK);           // Motor
  tft.fillRect(50, 52, 28, 18, TFT_BLACK);          // Duty
  tft.fillRect(2, 96, 50, 16, TFT_BLACK);           // Dist
  tft.fillRect(7, 115, 70, 39, TFT_BLACK);          // Time&&mAh
  tft.fillRect(108, 100, 128 - 108, 16, TFT_WHITE); // bat
}

// Return is an RGB value.
uint16_t gradientRYG(uint8_t value) {
  // From green to yellow G stays at 0xFF and R goes from 0x00 to 0xFF
  // Everything over yellow has R = 0xFF
  if (value < 128) {
    return 0xF800 + ((value * 2) << 3);
  } else {
    return 0x7E0 + (((255 - value) >> 2) << 11);
  }
}

void drawBattery(uint16_t color) {
  // fillRect(x, y, w, h, color);
  // Filling overlapping rectangles saves 86byte but is 9 times slower
  tft.fillRect(108 + 2, 50, 16, 2, color);       // Top
  tft.fillRect(108 + 2, 50 + 100, 16, 2, color); // Bot
  tft.fillRect(108, 50, 2, 100, color);          // Left
  tft.fillRect(108 + 20, 50, 2, 100, color);     // Right
}

void fillBattery(uint8_t value) {
  uint8_t line = 107 + 50 - (value / 3);            // 85 lines @ 255
  tft.fillRect(108 + 2, 50 + 2, 16, 96, TFT_BLACK); // Overwrite all previous
  if (value > 0) {
    while (line <= 157) { // last line
      int16_t color_input;
      if (line >= 152) {
        color_input = 0;
      } else {
        color_input = map(line, 60, 153, 255, 0);
      }
      // drawFastHLine(x, y, w, color)
      tft.drawFastHLine(108 + 2, line, 18, gradientRYG(color_input));
      line++;
    }
  } else { // draw a red X
    // drawLine(x0, y0, x1, y1, color)
    tft.drawLine(108 + 14, 50 + 30, 108 + 6, 50 + 80, TFT_RED);
    tft.drawLine(108 + 13, 50 + 30, 108 + 5, 50 + 80, TFT_RED);
    tft.drawLine(108 + 12, 50 + 30, 108 + 4, 50 + 80, TFT_RED);
  }
}

// Enter Settings Mode. Exit only over reset.
void settingsMenu() {
  drawSettings();
  drawSettingValues(0);
  int8_t currentSetting = 0; // 0=save // 1=deadband // 2=amp_fwd_max // 3=amp_break_max // 4=amp_fwd_min // 5=amp_break_min // 6=cellcount
  bool ok;
  bool triggerStick;
  uint16_t stick;
  while (1) {
    DEB_cruise.update();
    ok = DEB_cruise.read();
    stick = analogRead(PIN_POTI_THR);
    // some movement has to be done to triggerStick
    // when button is pressed and stick moved change value.
    // without button change menu
    if (stick > 712 && !triggerStick) { // mid 512
      if (ok) {
        changeSettings(true, currentSetting);
      } else {
        currentSetting++;
        triggerStick = true;
      }
    } else if (stick < 312 && !triggerStick) {
      if (ok) {
        changeSettings(false, currentSetting);
      } else {
        currentSetting--;
        triggerStick = true;
      }
    } else if (stick < 612 && stick > 412 && triggerStick) { // 100 difference so it won't jitter
      if (ok)
        changeSettings(false, currentSetting);
      triggerStick = false;
    }
    if (currentSetting > 6)
      currentSetting = 6;
    if (currentSetting < 0)
      currentSetting = 0;
    drawSettingValues(currentSetting);
  }
}

void changeSettings(bool up, uint16_t currentS) {
  switch (currentS) {
  case 0:
    saveSettings();
    break;
  case 1:
    up ? RemoteData._deadband++ : RemoteData._deadband--;
    break;
  case 2:
    up ? amp_fwd_max++ : amp_fwd_max--;
    break;
  case 3:
    up ? amp_break_max++ : amp_break_max--;
    break;
  case 4:
    up ? amp_fwd_min++ : amp_fwd_min--;
    break;
  case 5:
    up ? amp_break_min++ : amp_break_min--;
    break;
  case 6:
    up ? cellcount++ : cellcount--;
    break;
  }
}

void drawSettings() {
  tft.setTextSize(1); // no scaling
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawCentreString("SETTINGS", 64, 5, 4); // Font 4

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Deadband", 5, 35, 2);
  tft.drawString("FWD max", 5, 55, 2);
  tft.drawRightString("A", 120, 55, 2);
  tft.drawString("Break max", 5, 75, 2);
  tft.drawRightString("A", 120, 75, 2);
  tft.drawString("FWD min", 5, 95, 2);
  tft.drawRightString("A", 120, 95, 2);
  tft.drawString("Break min", 5, 115, 2);
  tft.drawRightString("A", 120, 115, 2);
  tft.drawString("LiPo Cells", 5, 135, 2);
}

// Write Values, green when saved, red when new, current marked with white background
void drawSettingValues(uint16_t currentS) {
  tft.setTextColor(RemoteData._deadband == EEPROM.get(eeDeadband, RemoteData._deadband) ? TFT_GREEN : TFT_RED, currentS == 1 ? TFT_WHITE : TFT_BLACK);
  tft.drawNumber(RemoteData._deadband, 85, 35, 2);

  tft.setTextColor(amp_fwd_max == EEPROM.get(eeFwdMax, amp_fwd_max) ? TFT_GREEN : TFT_RED, currentS == 2 ? TFT_WHITE : TFT_BLACK);
  tft.drawNumber(amp_fwd_max, 85, 55, 2);

  tft.setTextColor(amp_break_max == EEPROM.get(eeBreakMax, amp_break_max) ? TFT_GREEN : TFT_RED, currentS == 3 ? TFT_WHITE : TFT_BLACK);
  tft.drawNumber(amp_break_max, 85, 75, 2);

  tft.setTextColor(amp_fwd_min == EEPROM.get(eeFwdMin, amp_fwd_min) ? TFT_GREEN : TFT_RED, currentS == 4 ? TFT_WHITE : TFT_BLACK);
  tft.drawNumber(amp_fwd_min, 85, 95, 2);

  tft.setTextColor(amp_break_min == EEPROM.get(eeBreakMin, amp_break_min) ? TFT_GREEN : TFT_RED, currentS == 5 ? TFT_WHITE : TFT_BLACK);
  tft.drawNumber(amp_break_min, 85, 115, 2);

  tft.setTextColor(cellcount == EEPROM.get(eeCellcount, cellcount) ? TFT_GREEN : TFT_RED, currentS == 6 ? TFT_WHITE : TFT_BLACK);
  tft.drawNumber(cellcount, 85, 135, 2);
}

void saveSettings() {
  EEPROM.update(eeDeadband, RemoteData.deadband);
  EEPROM.update(eeFwdMax, amp_fwd_max);
  EEPROM.update(eeBreakMax, amp_break_max);
  EEPROM.update(eeFwdMin, amp_fwd_min);
  EEPROM.update(eeBreakMin, amp_break_min);
  EEPROM.update(eeCellcount, cellcount);
}

voide test() {
  EEPROM.update();
  tft.drawNumber();
  drawSettingValues();
  VescMeasuerdValues();
  RemoteData.deadband = 0;
}