/* 

  OpenDosimeter Sketch
  Only works on the Raspberry Pi Pico with arduino-pico!

  Overview mode of operation:
  1) Triggers on newly detected X-ray photons (X-ray -> Scintillator -> SiPM)
  2) Saves pulse amplitude of detected events to an ADC histogram (12-bit)
  3) Once per second calculates the current dose rate (ADC histogram -> energy spectrum -> total dose)
    - Assumes device has been spectrally calibrated with Am-241 (e.g., from a smoke detector)
  4) Displays the current dose rate / accumulated dose

  2024, OpenDosimeter project:
  https://github.com/OpenDosimeter/OpenDosimeter

  Code built on existing Open Gamma Project (FW 4.2.1):
  https://github.com/OpenGammaProject/Open-Gamma-Detector

  ## Flash with default settings and
  ##   Flash Size: "2MB (Sketch: 1MB, FS: 1MB)"

  TODO: Clean up the code from unused OpenGammaDetector functions
  TODO: Add options for high-level user settings
  TODO: Make the Serial output compatible with the Gamma Spectrum visualizer (https://spectrum.nuclearphoenix.xyz/)
  TODO: Restructure files to make ino better readable 

*/

#define _TASK_SCHEDULING_OPTIONS
#define _TASK_TIMECRITICAL       // Enable monitoring scheduling overruns
#define _TASK_SLEEP_ON_IDLE_RUN  // Enable 1 ms SLEEP_IDLE powerdowns between tasks if no callback methods were invoked during the pass
#include <TaskScheduler.h>       // Periodically executes tasks

#include "hardware/vreg.h"    // Used for vreg_set_voltage
#include "Helper.h"           // Misc helper functions
//#include "FS.h"               // Functions for the LittleFS filesystem

#include <RunningAverage.h>   // Smoothing Am-241 spectrum before peak detection

#include "Scintillator.h"     // Functions and data on the scintillator used
#include "DoseConversion.h"   // Functions and data on the dose conversion data 

#include <SimpleShell_Enhanced.h>  // Serial Commands/Console
#include <ArduinoJson.h>           // Load and save the settings file
#include <LittleFS.h>              // Used for FS, stores the settings and debug files
#include <RunningMedian.h>         // Used to get running median and average with circular buffers

/*
    ===================
    BEGIN USER SETTINGS
    ===================
*/

// These are the default settings that can only be changed by reflashing the Pico
#define SCREEN_TYPE SCREEN_SSD1306  // Display type: Either SCREEN_SSD1306 or SCREEN_SH1106
#define SCREEN_WIDTH 128            // OLED display width, in pixels
#define SCREEN_HEIGHT 64            // OLED display height, in pixels
#define SCREEN_ADDRESS 0x3C         // See datasheet for Address; 0x3D for 128x64, 0x3C for 128x32
#define EVENT_BUFFER 50000          // Buffer this many events for Serial.print
#define TRNG_BITS 8                 // Number of bits for each random number, max 8
#define BASELINE_NUM 101            // Number of measurements taken to determine the DC baseline
#define CONFIG_FILE "/config.json"  // File to store the settings
#define DEBUG_FILE "/debug.json"    // File to store some misc debug info
#define DISPLAY_REFRESH 1000        // Milliseconds between display refreshs

#define BUZZER_FREQ 2700            // Resonance frequency of the buzzer
#define AUTOSAVE_TIME 900000        // 15 Minutes in ms, time between recording autosaves
// Display Types: Select only one! One of them must be true and the other one false
#define SCREEN_SH1106 false
#define SCREEN_SSD1306 true

struct Config {
  // These are the default settings that can also be changes via the serial commands
  // Do not touch the struct itself, but only the values of the variables!
  bool ser_output = false;         // Wheter data should be Serial.println'ed
  bool geiger_mode = false;       // Measure only cps, not energy
  bool print_spectrum = false;    // Print the finishes spectrum, not just chronological events
  size_t meas_avg = 1;            // Number of meas. averaged each event, higher=longer dead time
  bool enable_display = true;    // Enable I2C Display, see settings above
  bool enable_trng = false;       // Enable the True Random Number Generator
  bool subtract_baseline = true;  // Subtract the DC bias from each pulse
  bool cps_correction = false;    // Correct the cps for the DNL compensation
  bool enable_ticker = false;     // Enable the buzzer to be used as a ticker for pulses
  size_t tick_rate = 20;          // Buzzer ticks once every tick_rate pulses

  // Do NOT modify the following operator function
  bool operator==(const Config &other) const {
    return (tick_rate == other.tick_rate && enable_ticker == other.enable_ticker && cps_correction == other.cps_correction && ser_output == other.ser_output && geiger_mode == other.geiger_mode && print_spectrum == other.print_spectrum && meas_avg == other.meas_avg && enable_display == other.enable_display && enable_trng == other.enable_trng && subtract_baseline == other.subtract_baseline);
  }
};
/*
    =================
    END USER SETTINGS
    =================
*/

const String FW_VERSION = "1.0.1.L1";  // Firmware Version Code

const uint8_t GND_PIN = A2;    // GND meas pin
const uint8_t VSYS_MEAS = A3;  // VSYS/3
const uint8_t VBUS_MEAS = 24;  // VBUS Sense Pin
const uint8_t PS_PIN = 23;     // SMPS power save pin

const uint8_t AIN_PIN = 27;     // Analog input pin
const uint8_t SIG_PIN = 26;     // Signal (SiPM amplified) (baseline) meas pin
const uint8_t INT_PIN = 16;     // Signal interrupt pin
const uint8_t RST_PIN = 22;     // Peak detector MOSFET reset pin
const uint8_t LED = 25;         // Built-in LED on GP25
const uint8_t BUZZER_PIN = 7;   // Buzzer PWM pin for the ticker
const uint8_t BUTTON_PIN = 14;  // Misc button pin

const uint8_t THRESHOLD_PIN = 18; // PWM Pin for threshold 

// Battery variables (from the 'enhanced-geiger' project)
const uint8_t BAT_WARN = 20;               // % at which a warning icon appears for low battery
const uint8_t MIN_TEMP = 0;     // Max temperature for battery charging given by regulator
const uint8_t MAX_TEMP = 45;    // Min temperature for battery charging given by regulator
const uint8_t TEMP_OFFSET = 5;  // Temperature offset of the battery vs the processor when charging

const uint8_t BAT_PIN = 29;       // Battery voltage measurement pin
const uint8_t USB_PIN = 24;       // USB status pin
const uint8_t CHARGE_PIN = 21;    // Battery charger IC status pin (#CHG on the TI BQ21040DBVR)
const uint8_t BAT_REG_PIN = 20;   // Battery charger IC thermal shutdown pin (TS on the TI BQ21040DBVR)

const uint8_t BUZZER_TICK = 10;     // On-time of the buzzer for a single pulse in ms
const uint16_t EVT_RESET_C = 3000;  // Number of counts after which the OLED stats will be reset
const uint16_t OUT_REFRESH = 1000;  // Milliseconds between serial data outputs

const uint8_t LONG_PRESS = 10;  // Number of timesteps (100 ms) until considered long button press.

const uint16_t TIMEOUT_CALIBRATION_MENU = 4000;  // Milliseconds to exit the calibration mode with no user interaction. Needs to be shorter than the watchdog timer (5s currently)
const uint16_t TIMEOUT_CALIBRATION = 60000; // Milliseconds for recording Am-241 spectrum, after which the calibration exits without finishing
const uint8_t AVERAGING_WINDOW = 64;            // Size of the moving average window for spectral calibration (needs to be even number)
const uint8_t MAX_CALIBRATION_COUNTS = 50;      // Breakpoint for calibration, max collected counts for a certain bin

const uint8_t DOSE_DAILY_LIMIT = 100;    // [uSv] display daily limit for reference in the accumulated dose state
const uint8_t DOSE_RATE_WARNING = 5;  //[uSv/h] Real-time audible warning at set dose rate

// Struct for datalog measurements
struct LogEntry { // Maximum entry size is 41 bytes as saved in the datalog.csv file
  uint32_t currentDoseRate;  // in nSv/h
  uint32_t totalAccumulatedDose;  // in nSv
  uint8_t deadtime;  // 0-100%
  uint32_t countsPerSecond; // Rounded, up to 4,294,967,295 cps
  uint8_t battery;  // 0-100%
};

const int DATALOG_BUFFER_SIZE = 10; // Last 10 seconds of data stored in RAM, before written to flash memory.
const long DATALOG_FILE_SIZE = 750000; // Maximum size of the datalog.csv file in bytes (0.75 MB, to leave some margin for the 1 MB total flash size). Should be enough for saving >10 hours of logging (1 Hz)
const char* RESET_INDICATOR = "DEVICE_RESET"; // Will be printed in the datalog.csv each time the device is reset

LogEntry dataLogBuffer[DATALOG_BUFFER_SIZE];
int logIndex = 0;
int bat_lvl = 100;


// Structure for the energy calibration variable
struct EnergyCalibration {
  float E1 = 59.5;  // Am-241 Energy peak 1 (keV)
  //float E2 = 26.3;  // Am-241 Energy peak 2 (keV)
  // ** Change July 17 ** 
  // Unlike previously assumed (above), the low energy peak detected is 
  // likely an envelope average of a range of low-energy gamma peaks from Am-241 (9-36 keV)
  // By smoothing an Am241 gamma spectrum from a CdTe detector (Amptek X123),
  // we determined that the peak energy is probably around ~ 17-20 keV
  float E2 = 20;    // Am-241 Energy peak 2 (keV)
  uint16_t X1 = 0; // ADC bin corresponding to peak 1
  uint16_t X2 = 0; // ADC bin corresponding to peak 2
  float k = 0;      // ADC -> Energy linear slope (E = k*x + m)
  float m = 0;      // ADC -> Energy intercept (E = k*x + m) 
  float Emax = 0;   // Maximum detectable X-ray energy (dynamic range)
  float Emin = 0;   // Minimum detectable X-ray energy (dynamic range)
};

// Create energy calibration variable
EnergyCalibration ADC2E; 

// Also create a default calibration variable so that updated values of E1 and E2 will always be loaded
EnergyCalibration ADC2E_default;

// Define the file path where the calibration file is saved/loaded from flash memory
const char* energyCalibrationFile = "/calibration.bin";

RunningAverage spectrumAverage(AVERAGING_WINDOW);

const float VREF_VOLTAGE = 3.0;             // ADC reference voltage, default is 3.0 with reference
const uint8_t ADC_RES = 12;                 // Use 12-bit ADC resolution
const uint16_t ADC_BINS = 0x01 << ADC_RES;  // Number of ADC bins

volatile uint32_t spectrum[ADC_BINS];          // Holds the output histogram (spectrum)
volatile uint32_t display_spectrum[ADC_BINS];  // Holds the display histogram (spectrum)

volatile uint16_t events[EVENT_BUFFER];  // Buffer array for single events
volatile uint32_t event_position = 0;    // Target index in events array
volatile unsigned long start_time = 0;   // Time in ms when the spectrum collection has started
volatile unsigned long last_time = 0;    // Last time the display has been refreshed
volatile uint32_t last_total = 0;        // Last total pulse count for display

volatile unsigned long trng_stamps[3];     // Timestamps for True Random Number Generator
volatile uint8_t random_num = 0b00000000;  // Generated random bits that form a byte together
volatile uint8_t bit_index = 0;            // Bit index for the generated number
volatile uint32_t trng_nums[1000];         // TRNG number output array
volatile uint16_t number_index = 0;        // Amount of saved numbers to the TRNG array
volatile uint32_t total_events = 0;        // Total number of all registered pulses

RunningMedian baseline(BASELINE_NUM);  // Array of a number of baseline (DC bias) measurements at the SiPM input
uint16_t current_baseline = 0;         // Median value of the input baseline voltage

uint16_t current_threshold = 0;        // Threshold value, changing the PWM duty cycle (function of 'current_baseline')

uint32_t recordingDuration = 0;        // Duration of the spectrum recording in seconds
String recordingFile = "";             // Filename for the spectrum recording file
volatile bool isRecording = false;     // Currently running a recording
unsigned long recordingStartTime = 0;  // Start timestamp of the recording in ms

volatile bool adc_lock = false;  // Locks the ADC if it's currently in use

// Stores 5 * DISPLAY_REFRESH worth of "current" cps to calculate an average cps value in a ring buffer config
RunningMedian counts(1);

// Stores the last deadtime values
RunningMedian dead_time(100);

// ** Change July 17 ** 
// Add a constant few microseconds to the deadtime, taking into account overhead not measured in the 'eventInt' loop
const float dead_time_overhead = 6; // [us] To be measured accurately, currently estimated 

// Stores X * DISPLAY_REFRESH worth of "current" dose rate values to calculate an average dose rate value in a ring buffer config
RunningMedian dose_rate(30);

// Variable to track the accumulated dose and average dose rate (in micro sieverts), and counts per second and deadtime
float accumulated_dose = 0;
float current_dose_rate = 0;
float avg_dose_rate = 0;
float avg_cps_corrected = 0;
float deadtime_corr = 0;
// Track minimum and maximum dose rate
float max_dose_rate = -1;
float min_dose_rate = -1;
unsigned long startup_time; //Timer to indicate the startuo time

// Configuration struct with all user settings
Config conf;

// Set up serial shells
CShell shell(Serial);
CShell shell1(Serial2);

// Check for the right display type
#if (SCREEN_TYPE == SCREEN_SH1106)
#include <Adafruit_SH110X.h>
#define DISPLAY_WHITE SH110X_WHITE
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
#elif (SCREEN_TYPE == SCREEN_SSD1306)
#include <Adafruit_SSD1306.h>
#define DISPLAY_WHITE SSD1306_WHITE
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
#endif

// Forward declaration of callbacks
void writeDebugFileTime();
void queryButton();
void updateDisplay();
void dataOutput();
void updateBaseline();
void resetPHCircuit();
//void recordCycle();

// Tasks
Task writeDebugFileTimeTask(60 * 60 * 1000, TASK_FOREVER, &writeDebugFileTime);
Task queryButtonTask(100, TASK_FOREVER, &queryButton);
Task updateDisplayTask(DISPLAY_REFRESH, TASK_FOREVER, &updateDisplay);
Task dataOutputTask(OUT_REFRESH, TASK_FOREVER, &dataOutput);
Task updateBaselineTask(1, TASK_FOREVER, &updateBaseline);
Task resetPHCircuitTask(1, TASK_FOREVER, &resetPHCircuit);
//Task recordCycleTask(60000, 0, &recordCycle);

// Scheduler
Scheduler schedule;

// Declare CommandFunction type
using CommandFunction = void (*)(String *);

// Define a struct for command information
struct CommandInfo {
  CommandFunction function;
  const char *name;
  const char *description;
};


void clearSpectrum() {
  for (size_t i = 0; i < ADC_BINS; i++) {
    spectrum[i] = 0;
  }
}


void clearSpectrumDisplay() {
  for (size_t i = 0; i < ADC_BINS; i++) {
    display_spectrum[i] = 0;
  }
  start_time = millis();  // Spectrum pulse collection has started
  last_time = millis();
  last_total = 0;  // Remove old values
}


void resetSampleHold(uint8_t time = 2) {  // Reset sample and hold circuit
  digitalWriteFast(RST_PIN, HIGH);
  delayMicroseconds(time);  // Discharge for (default) 2 µs -> ~99% discharge time for 1 kOhm and 470 pF
  digitalWriteFast(RST_PIN, LOW);
}


void resetPHCircuit() {
  if (!adc_lock) {
    adc_lock = true;    // Disable interrupt ADC measurements while resetting
    resetSampleHold();  // Periodically reset the S&H/P&H circuit
    adc_lock = false;
  }
  // TODO: Check if adc was locked and decrease interval to compensate
}

// Modify behavior of button:
// - Short press: switch between 1) accumulated dose, and 2) dose rate
// - Long press: Followed by another short or long press
//      - Short press: Reset dose values (min rate, max rate, accumulated)
//      - Long press: go into spectral calibration

void queryButton() {
  static uint16_t pressed = 0;
  static bool press_lock = false;

  if (digitalRead(BUTTON_PIN) == LOW) {
    pressed++;

    if (pressed >= LONG_PRESS && !press_lock) {
      /*
        Long Press:
      */

      // Show display option: 
      // 1) Short press for reset
      // 2) Long for calibration

      // Ask user if they want to calibrate or reset
      calibrationMenu();

      // Once done, enable the query button and display tasks
      queryButtonTask.enable();
      updateDisplayTask.enableDelayed(DISPLAY_REFRESH);
      writeDebugFileTimeTask.enableDelayed(60 * 60 * 1000);

      updateDisplayTask.forceNextIteration();  // Update the display immediately
    
      saveSettings();  // Saved updated settings
      // Introduce some dead time after a button press
      // Fixes a bug that would make the buzzer go crazy
      // Idk what's going on, but it's related to the saveSettings() function
      // Maybe the flash is too slow or something
      delay(100);

      press_lock = true;
    }
        
  } else {
    if (pressed > 0) {
      if (pressed < LONG_PRESS) {
        /*
          Short Press: Switch Modes
        */
        conf.geiger_mode = !conf.geiger_mode;
        event_position = 0;
        clearSpectrum();
        clearSpectrumDisplay();
        resetSampleHold();

        println(conf.geiger_mode ? "Switched to accumulated dose." : "Switched to dose rate.");

        updateDisplayTask.forceNextIteration();  // Update the display immediately

        saveSettings();  // Saved updated settings
        // Introduce some dead time after a button press
        // Fixes a bug that would make the buzzer go crazy
        // Idk what's going on, but it's related to the saveSettings() function
        // Maybe the flash is too slow or something
        delay(100);
      }
    }

    pressed = 0;
    press_lock = false;
  }

  if (BOOTSEL) {
    rp2040.rebootToBootloader();
  }

  rp2040.wdt_reset();  // Reset watchdog, everything is fine
}

void dataOutput() {
  // MAYBE ONLY START TASK IF OUTPUT IS ENABLED?
  if (conf.ser_output) {
    if (Serial || Serial2) {
      if (conf.print_spectrum) {
        for (uint16_t index = 0; index < ADC_BINS; index++) {
          cleanPrint(String(spectrum[index]) + ";");
          //spectrum[index] = 0; // Uncomment for differential histogram
        }
        cleanPrintln();
      } else if (event_position > 0 && event_position <= EVENT_BUFFER) {
        for (uint16_t index = 0; index < event_position; index++) {
          cleanPrint(String(events[index]) + ";");
        }
        cleanPrintln();
      }
    }

    event_position = 0;
  }

  // MAYBE SEPARATE TRNG AND SERIAL OUTPUT TASKS?
  if (conf.enable_trng) {
    if (Serial || Serial2) {
      for (size_t i = 0; i < number_index; i++) {
        cleanPrint(trng_nums[i], DEC);
        cleanPrintln(";");
      }
      number_index = 0;
    }
  }
}

// Modified the function below to set the PWM threshold according to the baseline
void updateBaseline() {
  static uint8_t baseline_done = 0;

  // Compute the median DC baseline to subtract from each pulse
  if (conf.subtract_baseline) {
    if (!adc_lock) {
      adc_lock = true;  // Disable interrupt ADC measurements while resetting
      baseline.add(analogRead(AIN_PIN));
      adc_lock = false;

      baseline_done++;

      if (baseline_done >= BASELINE_NUM) {
        current_baseline = round(baseline.getMedian());  // Take the median value

        baseline_done = 0;
      }
    }
  }

  // Once baseline have been measured, set the threshold accordingly
  current_threshold = 1.1 * current_baseline * (3.3/0.5);
  analogWrite(THRESHOLD_PIN, current_threshold);
}


float readTemp() {
  adc_lock = true;                        // Flag this, so that nothing else uses the ADC in the mean time
  delayMicroseconds(conf.meas_avg * 20);  // Wait for an already-executing interrupt
  const float temp = analogReadTemp(VREF_VOLTAGE);
  adc_lock = false;
  return temp;
}


void serialEvent() {
  shell.handleEvent();  // Handle the serial input for the USB Serial
  //shell1.handleEvent();  // Handle the serial input for the Hardware Serial
}


void serialEvent2() {
  //shell.handleEvent();  // Handle the serial input for the USB Serial
  shell1.handleEvent();  // Handle the serial input for the Hardware Serial
}
/*
  END SERIAL COMMANDS
*/

/*
  BEGIN DEBUG AND SETTINGS
*/
void writeDebugFileTime() {
  // ALMOST THE SAME AS THE BOOT DEBUG FILE WRITE!
  File debugFile = LittleFS.open(DEBUG_FILE, "r");  // Open read and write

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, debugFile);

  if (!debugFile || error) {
    //println("Could not open debug file!", true);
    print("Could not load debug info from json file: ", true);
    cleanPrintln(error.f_str());

    doc["power_cycle_count"] = 0;
    doc["power_on_hours"] = 0;
  }

  debugFile.close();

  const uint32_t temp = doc.containsKey("power_on_hours") ? doc["power_on_hours"] : 0;
  doc["power_on_hours"] = temp + 1;

  debugFile = LittleFS.open(DEBUG_FILE, "w");  // Open read and write
  doc.shrinkToFit();                           // Optional
  serializeJson(doc, debugFile);
  debugFile.close();
}


void writeDebugFileBoot() {
  // ALMOST THE SAME AS THE TIME DEBUG FILE WRITE!
  File debugFile = LittleFS.open(DEBUG_FILE, "r");  // Open read and write

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, debugFile);

  if (!debugFile || error) {
    //println("Could not open debug file!", true);
    print("Could not load debug info from json file: ", true);
    cleanPrintln(error.f_str());

    doc["power_cycle_count"] = 0;
    doc["power_on_hours"] = 0;
  }

  debugFile.close();

  const uint32_t temp = doc.containsKey("power_cycle_count") ? doc["power_cycle_count"] : 0;
  doc["power_cycle_count"] = temp + 1;

  debugFile = LittleFS.open(DEBUG_FILE, "w");  // Open read and write
  doc.shrinkToFit();                           // Optional
  serializeJson(doc, debugFile);
  debugFile.close();
}


Config loadSettings(bool msg = true) {
  Config new_conf;
  File saveFile = LittleFS.open(CONFIG_FILE, "r");

  if (!saveFile) {
    println("Could not open save file! Creating a fresh file...", true);

    writeSettingsFile();  // Force creation of a new file

    return new_conf;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, saveFile);

  saveFile.close();

  if (error) {
    print("Could not load config from json file: ", true);
    cleanPrintln(error.f_str());
    return new_conf;
  }

  if (doc.containsKey("ser_output")) {
    new_conf.ser_output = doc["ser_output"];
  }
  if (doc.containsKey("geiger_mode")) {
    new_conf.geiger_mode = doc["geiger_mode"];
  }
  if (doc.containsKey("print_spectrum")) {
    new_conf.print_spectrum = doc["print_spectrum"];
  }
  if (doc.containsKey("meas_avg")) {
    new_conf.meas_avg = doc["meas_avg"];
  }
  if (doc.containsKey("enable_display")) {
    new_conf.enable_display = doc["enable_display"];
  }
  if (doc.containsKey("enable_trng")) {
    new_conf.enable_trng = doc["enable_trng"];
  }
  if (doc.containsKey("subtract_baseline")) {
    new_conf.subtract_baseline = doc["subtract_baseline"];
  }
  if (doc.containsKey("cps_correction")) {
    new_conf.cps_correction = doc["cps_correction"];
  }
  if (doc.containsKey("enable_ticker")) {
    new_conf.enable_ticker = doc["enable_ticker"];
  }
  if (doc.containsKey("tick_rate")) {
    new_conf.tick_rate = doc["tick_rate"];
  }

  if (msg) {
    println("Successfuly loaded settings from flash.");
  }
  return new_conf;
}


bool writeSettingsFile() {
  File saveFile = LittleFS.open(CONFIG_FILE, "w");

  if (!saveFile) {
    println("Could not open save file!", true);
    return false;
  }

  JsonDocument doc;

  doc["ser_output"] = conf.ser_output;
  doc["geiger_mode"] = conf.geiger_mode;
  doc["print_spectrum"] = conf.print_spectrum;
  doc["meas_avg"] = conf.meas_avg;
  doc["enable_display"] = conf.enable_display;
  doc["enable_trng"] = conf.enable_trng;
  doc["subtract_baseline"] = conf.subtract_baseline;
  doc["cps_correction"] = conf.cps_correction;
  doc["enable_ticker"] = conf.enable_ticker;
  doc["tick_rate"] = conf.tick_rate;

  doc.shrinkToFit();  // Optional
  serializeJson(doc, saveFile);

  saveFile.close();

  return true;
}


bool saveSettings() {
  const Config read_conf = loadSettings(false);

  if (read_conf == conf) {
    //println("Settings did not change... not writing to flash.");
    return false;
  }

  //println("Successfuly written config to flash.");
  return writeSettingsFile();
}
/*
  END DEBUG AND SETTINGS
*/

/*
  BEGIN DISPLAY FUNCTIONS
*/
void updateDisplay() {
  // Update display every DISPLAY_REFRESH ms
  if (conf.enable_display) {
    conf.geiger_mode ? drawAccumulatedDose() : drawDoseRate();
  }
}

// Function to draw the spectrum while calibrating
void drawSpectrumCalibration(unsigned long startTime) {
  const uint16_t BINSIZE = floor(ADC_BINS / SCREEN_WIDTH);
  uint32_t eventBins[SCREEN_WIDTH];
  uint16_t offset = 0;
  uint32_t max_num = MAX_CALIBRATION_COUNTS * BINSIZE; 
  uint32_t total = 0;

  for (uint16_t i = 0; i < SCREEN_WIDTH; i++) {
    uint32_t totalValue = 0;

    for (uint16_t j = offset; j < offset + BINSIZE; j++) {
      totalValue += display_spectrum[j];
    }

    offset += BINSIZE;
    eventBins[i] = totalValue;

    //if (totalValue > max_num) {
    //  max_num = totalValue;
    //}

    total += totalValue;
  }

  const unsigned long now_time = millis();

  // No events accumulated, catch divide by zero
  const float scale_factor = float(SCREEN_HEIGHT - 11) / float(max_num) * 1.3; // Peak heights reach the top horizontal line

  const uint32_t new_total = total - last_total;
  last_total = total;

  if (now_time < last_time) {  // Catch Millis() Rollover
    last_time = now_time;
    return;
  }

  unsigned long time_delta = now_time - last_time;
  last_time = now_time;

  if (time_delta == 0) {  // Catch divide by zero
    time_delta = 1000;
  }

  counts.add(new_total * 1000.0 / time_delta);

  const float avg_cps = counts.getAverage();
  // ** Change in July 17, 2024: **
  // Currently adding another few us (set by the constant 'dead_time_overhead') to the deadtime (to be accurately measured)
  // Expecting there to be some overhead of this magnitude, per detection event, that is missed in the 'eventInt' calculation
  // This would explain the inacuraccy seen at high dose rates, observed in the experiments against RaySafe i3 on July 16, 2024.
  // -> ~12 us per-event deadtime instead of ~8 us currently measured in 'eventInt'
  const float avg_dt = dead_time.getAverage() + dead_time_overhead;  //+ 5.0; <- Comment from the original OpenGammaDetector code (FW 4.2.1)

  float avg_cps_corrected = avg_cps;
  if (avg_dt > 0.) {
    avg_cps_corrected = avg_cps / (1.0 - avg_cps * avg_dt / 1.0e6);
  }

  display.clearDisplay();
  display.setCursor(0, 0);
  display.print("Recording Am-241");
  display.drawFastHLine(0, 10, SCREEN_WIDTH, DISPLAY_WHITE);

  // Display time left before timeout
  display.setCursor(108, 0);
  display.print((TIMEOUT_CALIBRATION-(millis()-startTime))/1000);
  display.drawFastVLine(102, 0, 10, DISPLAY_WHITE);
  display.print("s");

  for (uint16_t i = 0; i < SCREEN_WIDTH; i++) {
    const uint32_t val = round(eventBins[i] * scale_factor);
    display.drawFastVLine(i, SCREEN_HEIGHT - val - 1, val, DISPLAY_WHITE);
  }
  display.drawFastHLine(0, SCREEN_HEIGHT - 1, SCREEN_WIDTH, DISPLAY_WHITE);

  display.display();

}

// Instead of spectrum mode, show the total accumulated dose
// - Also display battery percentage
// - Use functions & data in 'Scintillator.h' and 'DoseConversion.h'
void drawAccumulatedDose() {

  calculateDoseRate();

  // -- Buzzer warning if dose rate is above set limit --
  if(current_dose_rate >= DOSE_RATE_WARNING) {
    buzzerDoseRateWarning();
  } 

  // Clear display for output
  display.clearDisplay();

  // Draw battery percentage and symbol
  bat_lvl = drawBattery(bat_lvl);

  // ** Draw rest of display **

  // Lines
  display.setCursor(0, 0);
  display.drawFastHLine(0, 19, SCREEN_WIDTH, DISPLAY_WHITE);
  display.drawFastHLine(0, 50, SCREEN_WIDTH, DISPLAY_WHITE);
  display.drawFastVLine(104, 0, 19, DISPLAY_WHITE);

  // Display text
  display.setCursor(0, 0);
  display.print("Accumulated");
  display.setCursor(0, 9);
  display.print("dose");

  // Total dose

  // If uncalibrated (no ADC2E values found)
  if(ADC2E.k == 0) {
    display.setTextSize(1);
    display.setCursor(0, 26);
    display.print("Need spectral");
    display.setCursor(0, 36);
    display.print("calibration");
  } else { // If calibration values found
    display.setTextSize(2);
    display.setCursor(0, 26);
    if (accumulated_dose > 1000) {
      display.print(accumulated_dose / 1000.0, 2);
      display.setTextSize(1);
      display.println(" mSv");
    } else {
      display.print(accumulated_dose, 2);
      display.setTextSize(1);
      display.println(" uSv");
    }
  }

  // Show daily accumulated dose limit for reference
  display.setCursor(0, 55);
  display.print("Daily limit: ");
  display.print(DOSE_DAILY_LIMIT);
  display.print(" uSv");

  // Draw display
  display.display();
}

// Instead of Geiger mode, show the current doserate (uSv/h)
// - Also display battery percentage
// - Use functions & data in 'Scintillator.h' and 'DoseConversion.h'
void drawDoseRate() {

  calculateDoseRate();

  // -- Buzzer warning if dose rate is above set limit --
  if(current_dose_rate >= DOSE_RATE_WARNING) {
    buzzerDoseRateWarning();
  } 

  // Clear display for output
  display.clearDisplay();

  // Draw battery percentage and symbol
  bat_lvl = drawBattery(bat_lvl);

  // ** Draw rest of display **
  // Lines
  display.setCursor(0, 0);
  display.drawFastHLine(0, 19, SCREEN_WIDTH, DISPLAY_WHITE);
  display.drawFastHLine(0, 50, SCREEN_WIDTH, DISPLAY_WHITE);
  display.drawFastVLine(104, 0, 19, DISPLAY_WHITE);
  //display.drawFastVLine(50, 50, 14, DISPLAY_WHITE);

  // Min & max recorded dose rates (only show if calibrated)
  display.setCursor(0, 0);
  display.print("Avg: ");
  if(ADC2E.k != 0) {
    if (avg_dose_rate > 1000) {
      display.print(avg_dose_rate / 1000.0, 2);
      display.println(" mSv/h");
    } else {
      display.print(avg_dose_rate, 2);
      display.println(" uSv/h");
    }
  }

  display.setCursor(0, 9);
  display.print("Max: ");
  if(ADC2E.k != 0) {
    if (max_dose_rate > 1000) {
      display.print(max_dose_rate / 1000.0, 2);
      display.println(" mSv/h");
    } else {
      display.print(max_dose_rate, 2);
      display.println(" uSv/h");
    }
  }

  // Current dose rate

  // If uncalibrated (no ADC2E values found)
  if(ADC2E.k == 0) {
    display.setTextSize(1);
    display.setCursor(0, 26);
    display.print("Need spectral");
    display.setCursor(0, 36);
    display.print("calibration");
  } else {
    display.setTextSize(2);
    display.setCursor(0, 26);
    if (current_dose_rate > 1000) {
      display.print(current_dose_rate / 1000.0, 2);
      display.setTextSize(1);
      display.println(" mSv/h");
    } else {
      display.print(current_dose_rate, 2);
      display.setTextSize(1);
      display.println(" uSv/h");
    }
  }

  // Current average counts per second (cps)
  display.setCursor(0, 55);
  display.print(avg_cps_corrected, 0);
  display.print(" cps");

  // ** Deadtime alternative 1
  display.drawFastVLine(50, 50, 14, DISPLAY_WHITE);
  display.setCursor(55, 55);
  display.print("Deadtime: ");
  //display.drawBitmap(54, 54, deadtime_symbol, 8, 8, DISPLAY_WHITE);

  //display.setCursor(55, 65);
  display.print(round((1-deadtime_corr)*100), 0);
  display.print("%");

  // Draw display
  display.display();
}


void calculateDoseRate(){
  
  //Calculate counts per second
  uint32_t total = 0;

  // Integrate all counts over spectrum
  for (uint16_t i = 0; i < ADC_BINS; i++) {
    total += display_spectrum[i];
  }

  const unsigned long now_time = millis();

  const uint32_t new_total = total - last_total;
  last_total = total;

  if (now_time <= last_time) {  // Catch Millis() Rollover
    last_time = now_time;
    return;
  }

  unsigned long time_delta = now_time - last_time;
  last_time = now_time;

  if (time_delta == 0) {  // Catch divide by zero
    time_delta = 1000;
  } else if (time_delta <500){
    return;
  }

  counts.add(new_total * 1000.0 / time_delta);

  const float avg_cps = counts.getAverage();

  //Calculate Deadtime
  const float avg_dt = dead_time.getAverage() + dead_time_overhead;  //+ 5.0; <- Comment from the original OpenGammaDetector code (FW 4.2.1)

  // Debug: 
  //Serial.print("Average deadtime: "); Serial.println(avg_dt);

  // Deadtime correction
  deadtime_corr = (1.0 - avg_cps * avg_dt / 1.0e6);

  // Calculate corrected counts
  // ** Change in July 17, 2024: **
  // Currently adding another few us (set by the constant 'dead_time_overhead') to the deadtime (to be accurately measured)
  // Expecting there to be some overhead of this magnitude, per detection event, that is missed in the 'eventInt' calculation
  // This would explain the inacuraccy seen at high dose rates, observed in the experiments against RaySafe i3 on July 16, 2024.
  // -> ~12 us per-event deadtime instead of ~8 us currently measured in 'eventInt'

  if (avg_dt > 0.) {
    avg_cps_corrected = avg_cps / deadtime_corr;
  } else{
    avg_cps_corrected = avg_cps;
  }

  static float max_cps = -1;
  static float min_cps = -1;

  if (max_cps == -1 || avg_cps_corrected > max_cps) {
    max_cps = avg_cps_corrected;
  }
  if (min_cps <= 0 || avg_cps_corrected < min_cps) {
    min_cps = avg_cps_corrected;
  }

  // ** Calculate dose rate **
  // Loop over 'display_spectrum' and calculate the current dose rate and total dose
  uint16_t energy; // Variable to convert ADC_BIN to keV energy
  float dose_delta = 0; // Variable to hold the dose since the last 'display_spectrum' reset

  int subtraction_counts = 10;

  // New addition, FW 1.0.1., Sept 4, trying to remove some background counts some predefined energy limit (LYSO, 300, 200, 88 keV emissions)
  for (uint16_t i = 0; i < ADC_BINS; i++) {
    energy = round(ADC2E.k * i + ADC2E.m); // Round to nearest keV
    if(energy >= 10) { // Only calculate dose if above 10 keV (no scintillator efficiency or dose conversion data below 10 keV)
      uint8_t counts = display_spectrum[i];
      if(energy >= 280){ // Subtract only counts that are higher than 180 keV
        subtraction_counts -= counts;
        if(subtraction_counts <= 0){
          dose_delta += counts * getDoseConversion(energy) / getScintEfficiency(energy); // Calculate dose per ADC bin (in units of pSv)
        }
      } else { 
        dose_delta += counts * getDoseConversion(energy) / getScintEfficiency(energy); // Calculate dose per ADC bin (in units of pSv)
      }
    }
  }

  // Perform deadtime correction
  float dose_delta_corr = dose_delta;
  if (avg_dt > 0.) {
    dose_delta_corr = dose_delta / deadtime_corr;
  }

  // Scale delta dose with the time integration & pSv/s -> uSv / h
  current_dose_rate = dose_delta_corr * 1000.0 / time_delta / 1.0e6 * 3600;
  dose_rate.add(current_dose_rate);

  // Calculate running average dose rate
  avg_dose_rate = dose_rate.getAverage();

  // Add the delta dose to the total dose tracking variable
  // ** BUG FOUND ** July 16 2024: Original line was: accumulated_dose += dose_delta / 1.0e6; // Convert pSv -> uSv
  // -> Did not add the deadtime corrected dose_delta to 'accumulated_dose'
  accumulated_dose += dose_delta_corr / 1.0e6; // Convert pSv -> uSv


  //Calculate Min and Max Dose Rate
  unsigned long current_time = millis();
  if (max_dose_rate == -1 || current_dose_rate > max_dose_rate) {
    if(current_time - startup_time>5000){
      max_dose_rate = current_dose_rate;
    }
    if (isinf(current_dose_rate)){
      max_dose_rate = 0;
    }
  }

  if (min_dose_rate <= 0 || current_dose_rate < min_dose_rate) {
    min_dose_rate = current_dose_rate;
    if (isinf(current_dose_rate)){
      min_dose_rate = 0;
    }
  }

  // At the end of the dose calculation, clear 'display_spectrum' for next call
  clearSpectrumDisplay();

  // Log the data
  logData(current_dose_rate, accumulated_dose, deadtime_corr, avg_cps, bat_lvl);

  // Save dose data every 5 seconds
  static unsigned long lastSaveTime = 0;
  if (millis() - lastSaveTime > 5000) {
    saveDoseData();
    lastSaveTime = millis();
  }
}

/*
  END DISPLAY FUNCTIONS
*/

// ** New in FW 2.5, pushing deadtime as low as possible (1 sample, no averaging)
void eventInt() {

  // Time deadtime
  const unsigned long start = micros();

  // Disable interrupt generation for this pin ASAP.
  // Directly uses Core0 IRQ Ctrl (core1 does not set the interrupt).
  // Thanks a lot to all the replies at
  // https://github.com/earlephilhower/arduino-pico/discussions/1397!
  static io_rw_32 *addr = &(iobank0_hw->proc0_irq_ctrl.inte[INT_PIN / 8]);
  static uint32_t mask1 = 0b1111 << (INT_PIN % 8) * 4u;
  hw_clear_bits(addr, mask1);

  // // Time deadtime
  // const unsigned long start = micros();

  if (!adc_lock) {
    const uint16_t m = analogRead(AIN_PIN);
    uint16_t adc_bin;
    resetSampleHold();

    // Pico-ADC DNL issues, see https://pico-adc.markomo.me/INL-DNL/#dnl
    // Discard channels 512, 1536, 2560, and 3584. For now.
    // See RP2040 datasheet Appendix B: Errata
    if (m != 511 && m != 1535 && m != 2559 && m != 3583) {
      // Subtract baseline from the reading
      if (current_baseline <= m) {  // Catch negative numbers
        // Subtract DC bias from pulse avg and then convert float --> uint16_t ADC channel
        adc_bin = round(m - current_baseline);
      }
    
      // Add amplitude reading to the ADC spectrum
      display_spectrum[adc_bin] += 1;
      spectrum[adc_bin] += 1; // Only used for the Gamma Spectrum MCA (https://spectrum.nuclearphoenix.xyz/)
    }
  }

  // Re-enable interrupts
  static uint32_t mask2 = 0b0100 << (INT_PIN % 8) * 4u;
  hw_set_bits(addr, mask2);

  // Clear all interrupts on the executing core
  irq_clear(15);  // IRQ 15 = SIO_IRQ_PROC0

  // End timing of deadtime
  const unsigned long end = micros();

  if (end >= start) {  // Catch micros() overflow
    // Compute Detector Dead Time
    total_events++;
    dead_time.add(end - start);
  }

}


/*
  BEGIN SETUP FUNCTIONS
*/
void setup() {
  pinMode(SIG_PIN, INPUT);
  pinMode(INT_PIN, INPUT);
  pinMode(AIN_PIN, INPUT);
  pinMode(RST_PIN, OUTPUT_12MA);
  pinMode(LED, OUTPUT);

  // Set PWM output for threshold
  pinMode(THRESHOLD_PIN, OUTPUT);
  analogWriteFreq(1000);

  // 12-bit
  analogWriteRange(4096);
  analogWriteResolution(12);
  analogWrite(THRESHOLD_PIN, 2000); // Set very high in the beginning, then change dynamically inside 'updateBaseline'

  //gpio_set_slew_rate(RST_PIN, GPIO_SLEW_RATE_SLOW);  // Slow slew rate to reduce EMI
  gpio_set_slew_rate(LED, GPIO_SLEW_RATE_SLOW);  // Slow slew rate to reduce EMI

  analogReadResolution(ADC_RES);

  resetSampleHold(5);  // Reset before enabling the interrupts to avoid jamming

  attachInterrupt(digitalPinToInterrupt(INT_PIN), eventInt, FALLING);

  start_time = millis();  // Spectrum pulse collection has started
}


void setup1() {
  rp2040.wdt_begin(5000);  // Enable hardware watchdog to check every 5s

  // Undervolt a bit to save power, pretty conservative value could be even lower probably
  vreg_set_voltage(VREG_VOLTAGE_1_00);

  // Disable "Power-Saving" power supply option.
  // -> does not actually significantly save power, but output is much less noisy in HIGH!
  // -> Also with PS_PIN LOW I have experiences high-pitched (~ 15 kHz range) coil whine!
  pinMode(PS_PIN, OUTPUT_4MA);
  gpio_set_slew_rate(PS_PIN, GPIO_SLEW_RATE_SLOW);  // Slow slew rate to reduce EMI
  // Not really sure what results in better spectra...
  // It doesn't change a whole lot, so might as well keep the energy saving mode?
  digitalWrite(PS_PIN, LOW);
  //digitalWrite(PS_PIN, HIGH);

  pinMode(GND_PIN, INPUT);
  pinMode(VSYS_MEAS, INPUT);
  pinMode(VBUS_MEAS, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Set battery pins (from 'enhanced-geiger')
  // Set as input for high impedance so that battery charge works as normal
  // Set to OUTPUT and LOW to disable charging!
  pinMode(USB_PIN, INPUT);
  pinMode(BAT_PIN, INPUT);
  pinMode(BAT_REG_PIN, INPUT);
  pinMode(CHARGE_PIN, INPUT_PULLUP);

  // Define an array of CommandInfo objects with function pointers and descriptions
  CommandInfo allCommands[] = {
    { printDataLogToSerial, "P", "Print the dose data log" },
    { printSpectrumLogToSerial, "S", "Print the current spectrum to serial" }
  };

  // Get the number of allCommands
  const size_t numCommands = sizeof(allCommands) / sizeof(allCommands[0]);

  // Loop through allCommands and register commands with both shell instances
  for (size_t i = 0; i < numCommands; ++i) {
    shell.registerCommand(new ShellCommand(allCommands[i].function, allCommands[i].name, allCommands[i].description));
    shell1.registerCommand(new ShellCommand(allCommands[i].function, allCommands[i].name, allCommands[i].description));
  }

  // Starts FileSystem, autoformats if no FS is detected

  //LittleFS.begin();
  
  conf = loadSettings();  // Read all the detector settings from flash

  saveSettings();        // Create settings file if none is present
  writeDebugFileBoot();  // Update power cycle count

  // Set the correct SPI pins
  SPI.setRX(4);
  SPI.setTX(3);
  SPI.setSCK(2);
  SPI.setCS(5);
  // Set the correct I2C pins
  Wire.setSDA(0);
  Wire.setSCL(1);
  // Set the correct UART pins
  Serial2.setRX(9);
  Serial2.setTX(8);

  // Set up buzzer
  pinMode(BUZZER_PIN, OUTPUT_12MA);
  gpio_set_slew_rate(BUZZER_PIN, GPIO_SLEW_RATE_SLOW);  // Slow slew rate to reduce EMI
  digitalWrite(BUZZER_PIN, LOW);

  // Play start-up sound
  buzzerStartUp();

  shell.begin(2000000);
  shell1.begin(2000000);

  println("Welcome to the OpenDosimeter!");
  println("Firmware Version " + FW_VERSION);

  // Initialize LittleFS
  Serial.print(LittleFS.begin());
  if (!LittleFS.begin()) {
    LittleFS.format();
    if (!LittleFS.begin()) {
      Serial.println("Failed to mount LittleFS");
    }
  } else {
    loadDoseData();  // Load saved dose data      
    loadEnergyCalibration(); // Load ADC to energy calibration variable 'ADC2E' if there is one stored in flash memory
  }

  // Check if the data logging file exists
  if (!LittleFS.exists("/datalog.csv")) {
      File dataFile = LittleFS.open("/datalog.csv", "w");
      if (dataFile) {
          dataFile.println("Time,CurrentDoseRate,TotalAccumulatedDose,Deadtime,CountsPerSecond,Battery");
          dataFile.close();
      } else {
          Serial.println("Error creating datalog.csv");
          return;  // Exit the function if we couldn't create the file
      }
  }

  // Add reset indicator to the end of the current datalog file
  File dataFile = LittleFS.open("/datalog.csv", "a");
  if (dataFile) {
      dataFile.println(RESET_INDICATOR);
      dataFile.close();
  } 
    
  if (conf.enable_display) {
    bool begin = false;

#if (SCREEN_TYPE == SCREEN_SSD1306)
    begin = display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
#elif (SCREEN_TYPE == SCREEN_SH1106)
    begin = display.begin(SCREEN_ADDRESS, true);
#endif

    if (!begin) {
      println("Failed communication with the display. Maybe the I2C address is incorrect?", true);
      conf.enable_display = false;
    } else {
      display.setRotation(0);
      display.setTextSize(2);  // Draw 1X-scale text
      display.setTextColor(DISPLAY_WHITE);

      display.clearDisplay();

      display.println("Open");
      display.println("Dosimeter");
      display.println();
      display.setTextSize(1);
      display.print("FW ");
      display.println(FW_VERSION);
      display.drawBitmap(128 - 34, 64 - 34, radiation_symbol, 32, 32, DISPLAY_WHITE);

      display.display();
      delay(2000);

      startup_time = millis();
    }
  }

  // Set up task scheduler and enable tasks
  updateDisplayTask.setSchedulingOption(TASK_INTERVAL);  // TASK_SCHEDULE, TASK_SCHEDULE_NC, TASK_INTERVAL
  dataOutputTask.setSchedulingOption(TASK_INTERVAL);
  queryButtonTask.setSchedulingOption(TASK_INTERVAL);
  //resetPHCircuitTask.setSchedulingOption(TASK_INTERVAL);
  //updateBaselineTask.setSchedulingOption(TASK_INTERVAL);

  schedule.init();
  schedule.allowSleep(true);

  schedule.addTask(writeDebugFileTimeTask);
  schedule.addTask(queryButtonTask);
  schedule.addTask(updateDisplayTask);
  schedule.addTask(dataOutputTask);
  schedule.addTask(updateBaselineTask);
  schedule.addTask(resetPHCircuitTask);
  //schedule.addTask(recordCycleTask);

  queryButtonTask.enable();
  resetPHCircuitTask.enable();
  updateBaselineTask.enable();
  writeDebugFileTimeTask.enableDelayed(60 * 60 * 1000);
  dataOutputTask.enableDelayed(OUT_REFRESH);

  if (conf.enable_display) {
    // Only enable display task if the display function is enabled
    updateDisplayTask.enableDelayed(DISPLAY_REFRESH);
  }
}
/*
  END SETUP FUNCTIONS
*/

/*
  BEGIN LOOP FUNCTIONS
*/
void loop() {
  // X-ray detection loop, only wait for interrupts
  __wfi();
}


void loop1() {
  // Scheduling loop, handles rest of the functionality
  schedule.execute();
  delay(1);  // Wait for 1 ms, slightly reduces power consumption
}
/*
  END LOOP FUNCTIONS
*/

// --  Buzzer sound effects  -- 
void buzzerStartUp() {
 
  tone(BUZZER_PIN, 1000, 75); delay(75);
  tone(BUZZER_PIN, 1500, 75); delay(75);
  tone(BUZZER_PIN, 2000, 75);
}

void buzzerCalibrationRecording() { 
  tone(BUZZER_PIN, 660, 10); 
}

void buzzerCalibrationSuccess() { 
  tone(BUZZER_PIN, 880, 200); delay(75);
  tone(BUZZER_PIN, 1109, 200); delay(75);
}

void buzzerCalibrationFail() { 
  tone(BUZZER_PIN, 880, 200); delay(75);
  tone(BUZZER_PIN, 1046 , 200); delay(75);
}

void buzzerDoseRateWarning() {
   tone(BUZZER_PIN, 1000, 200);
}

// --  Battery percentage calculation -- 
uint8_t battery_percent(uint16_t adc_reading) {
  double L = 184.78;
  double k = 0.01;
  double x0 = 1718.64;
  double a = 0.25;
  double result = a + L / (1 + pow(2.7182818284, -k * (double(adc_reading) - x0)));

  if (result < 0.0) {
    return 0;
  } else if (result > 100.0) {
    return 100;
  } else {
    return round(result);
  }
}


// --  Calibration menu state (long press activated) -- 
void calibrationMenu(){

  Serial.println("Entered 'calibration menu'");

  // Temporarily disable display & query button tasks
  updateDisplayTask.disable();
  queryButtonTask.disable();
  writeDebugFileTimeTask.disable();

  // Then show options
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.println("Reset or Calibrate?");
  display.drawFastHLine(0, 10, SCREEN_WIDTH, DISPLAY_WHITE);

  display.setCursor(0, 15);
  display.print("Short press:");
  display.setCursor(0, 27);
  display.print(" -> Reset");
  display.setCursor(0, 42);
  display.print("Long press:");
  display.setCursor(0, 54);
  display.print(" -> Calibrate");
  display.display();

  // When entering this mode, the button is still being held down from before
  Serial.println("Waiting for button to be released...");
  
  // Timer variable to exit function if no action is taken with 4 seconds ('TIMEOUT_CALIBRATION_MENU')
  unsigned long startTime = millis();
  while(digitalRead(BUTTON_PIN) == LOW) {
    // Looping as long as button is still held downs
    if(millis() - startTime > TIMEOUT_CALIBRATION_MENU){
      buzzerCalibrationFail();
      return;
    }
  }

  Serial.println("Button released! Waiting for another press...");
  rp2040.wdt_reset();  // Reset watchdog, everything is fine
  delay(200);
  
  // Now the initial long-press has been released
  // Wait for the button to be pressed again
  startTime = millis();
  while (digitalRead(BUTTON_PIN) == HIGH) {
    // Looping until button is pressed again
    if(millis() - startTime > TIMEOUT_CALIBRATION_MENU){
      return;
    }
  }

  Serial.println("Button pressed! Now check if it's a long or short press...");

  // Reset watchdog - user interacted with the button
  rp2040.wdt_reset();

  static uint16_t pressed = 0;

  while(digitalRead(BUTTON_PIN) == LOW && pressed <= LONG_PRESS) {
    pressed++;
    delay(100);
    Serial.print("Pressed inside 'calibrate or reset': ");
    Serial.println(pressed);
  }

  if (pressed >= LONG_PRESS) {
      /*
        Long Press: Calibrate spectrum with Am-241
      */
      pressed = 0;
      calibrateSpectrum();
      delay(100);

  } else {
        /*
          Short Press: Reset dose values
        */
        pressed = 0;
        resetDoseValues();
        delay(100);
  }

  // Reset internal variable for next time this function is called
  pressed = 0;

}

// --  Record spectrum for calibration w/ Am-241  -- 
void calibrateSpectrum(){

  // Start timer to exit within a predefined time if not enough counts are recorded
  unsigned long startTime = millis();

  clearSpectrumDisplay();

  // Here, assume user is holding the Am-241 close to the dosimeter now
  // Record spectrum until the maximum peak (59.5 keV) has reached a predefined number of counts
  uint32_t max_value = 0;
  uint32_t am241_spectrum[ADC_BINS]; // Spectrum to be recorded of Am-241 sample

  bool calibration_success = false;

  while(max_value < MAX_CALIBRATION_COUNTS){
    buzzerCalibrationRecording();

    drawSpectrumCalibration(startTime);\

    rp2040.wdt_reset();
    max_value = maxSpectrumValue(display_spectrum);
    Serial.print("Maximum value: ");
    Serial.println(max_value);
    delay(500); // Update every 0.5 s

    if(millis() - startTime > TIMEOUT_CALIBRATION){
      buzzerCalibrationFail();

      // Display that calibration has not finished
      display.clearDisplay();
      display.setCursor(0, 15);
      display.print("Timeout (");
      display.print(TIMEOUT_CALIBRATION/1000);
      display.print(" s)");
      display.setCursor(0, 30);
      display.print("-> Exit calibration");
      display.display();
      delay(2000);
      return;
    }
  }

  // Spectrum recorded
  Serial.println("Maximum value reached. Spectrum recorded");

  // Now perform calibration algorithm
  display.clearDisplay();
  display.setCursor(0, 0);
  display.print("Calibrating...");
  display.drawFastHLine(0, 10, SCREEN_WIDTH, DISPLAY_WHITE);
  display.display();

  // Copy over to a local variable for further processing
  char temp_buf[20];
  Serial.println("========Recorded spectrum:");
  for (uint16_t i = 0; i < ADC_BINS; i++) {
      am241_spectrum[i] = display_spectrum[i];
      // Print the whole ADC spectrum on Serial (for debugging)
      sprintf(temp_buf, "%04d:%03d, ", i, am241_spectrum[i]); // It will be a large blob, 4096 values of "xxxx:xxx, "
      Serial.print(temp_buf);
  }
  Serial.println("");
  Serial.println("=========End of spectrum");

  // Now detect the Am-241 peaks in the spectrum and perform energy calibration
  calibration_success = detectPeaksAndCalibrate(am241_spectrum);

  // Calibration performed!
  if (calibration_success == true){
    buzzerCalibrationSuccess();

    // Save new calibration to flash memory
    saveEnergyCalibration();

    display.clearDisplay();
    display.setCursor(0, 0);
    display.print("Calibrating... Done!");
    display.drawFastHLine(0, 10, SCREEN_WIDTH, DISPLAY_WHITE);

    display.setCursor(0, 20);
    display.print("Bin ");
    display.print(ADC2E.X1);
    display.print(" <-> ");
    display.print(ADC2E.E1, 1);
    display.print(" keV ");

    display.setCursor(0, 32);
    display.print("Bin ");
    display.print(ADC2E.X2);
    display.print(" <-> ");
    display.print(ADC2E.E2, 1);
    display.print(" keV ");

    display.setCursor(0, 48);
    display.print("Range: ");
    display.print(round(ADC2E.Emin), 0);
    display.print(" - ");
    display.print(round(ADC2E.Emax), 0);
    display.print(" keV");

    display.display();

  } else {
    buzzerCalibrationFail();

    display.clearDisplay();
    display.setCursor(0, 0);
    display.print("Calibrating... :(");
    display.drawFastHLine(0, 10, SCREEN_WIDTH, DISPLAY_WHITE);

    display.setCursor(0, 25);
    display.print("Unsuccessful...");

    display.setCursor(0, 40);
    display.print("Peaks not found");

    display.display();
  }
 
  delay(3500); // Show results for 3 seconds before returning to normal mode
}

// --  Reset dose values (short press in calibration menu) --
// ->  Called within 'calibrationMenu' function
void resetDoseValues(){

  display.clearDisplay();
  display.setCursor(0, 25);
  display.setTextSize(1);
  display.println("Resetting dose...");
  display.display();

  // Reset dose variables
  accumulated_dose = 0;
  min_dose_rate = 0;
  max_dose_rate = 0;

  // Clear the data log
  clearDataLog();

  delay(2000);
}

// -- Clear datalog output (when dose values are reset) -- 
// -> Called within 'resetDoseValues()'
void clearDataLog() {
  // Clear the buffer
  for (int i = 0; i < DATALOG_BUFFER_SIZE; i++) {
    dataLogBuffer[i] = {0, 0, 0, 0, 0};
  }

  // Reset index
  logIndex = 0;

  // Reset the datalog.csv file
  File dataFile = LittleFS.open("/datalog.csv", "w");
  if (dataFile) {
    dataFile.println("CurrentDoseRate,TotalAccumulatedDose,Deadtime,CountsPerSecond,Battery");
    dataFile.close();
    Serial.println("Data log file cleared and reset.");
  } else {
    Serial.println("Error opening datalog.csv for clearing");
  }

  Serial.println("Log buffer cleared.");
}

// -- Reset dose values (short press in calibration menu) -- 
// -> Called within the 'calibrateSpectrum' function
uint32_t maxSpectrumValue(volatile uint32_t* array) {
  uint32_t max_value = 0;

  for (size_t i = 0; i < ADC_BINS; i++) {
    // Ignore channels 512, 1536, 2560, and 3584 (Pico-ADC DNL issues, see https://pico-adc.markomo.me/INL-DNL/#dnl)
    if (i == 511 || i == 1535 || i == 2559 || i == 3583) {
      continue; 
    }
    // Update maximum value
    if (array[i] > max_value) {
      max_value = array[i];
    }
  }

  return max_value;
}
// -- Detect peaks in calibration spectrum -- 
// -> Called within the 'calibrateSpectrum' function
bool detectPeaksAndCalibrate(uint32_t* input) {

  // Create a new variable for the smoothed spectrum
  float smoothed_spectrum[ADC_BINS];

  // Apply moving average filter to the spectrum
  spectrumSmoothing(input, smoothed_spectrum);

  // Print the whole ADC spectrum on Serial (for debugging)
  for (uint16_t i = 0; i < ADC_BINS; i++) {
      Serial.print(input[i]);
      Serial.print(" ");
  }
  Serial.println("");

  // Print the whole smoothed ADC spectrum on Serial (for debugging)
  for (uint16_t i = 0; i < ADC_BINS; i++) {
      Serial.print(smoothed_spectrum[i]);
      Serial.print(" ");
  }
  Serial.println("");
  
  rp2040.wdt_reset(); // Reset watchdog to prevent reset 

   // Detect peaks
  uint16_t p1 = 0, p2 = 0;
  uint32_t p1_value = 0, p2_value = 0;
  uint16_t peaks[ADC_BINS] = {0};
  uint16_t peak_count = 0;

  // Find all local maxima
  for (size_t i = 1; i < ADC_BINS - 1; i++) {
     if (smoothed_spectrum[i] > smoothed_spectrum[i - 1] && smoothed_spectrum[i] > smoothed_spectrum[i + 1]) {
      peaks[peak_count++] = i;
    }
  }

  Serial.print("Peaks found: ");
  Serial.println(peak_count++);

  // Calculate prominence of each peak
  for (size_t i = 0; i < peak_count; i++) {
    uint16_t peak = peaks[i];
    uint32_t left_min = smoothed_spectrum[peak];
    uint32_t right_min = smoothed_spectrum[peak];

    // Find minimum to the left
    for (int j = peak; j >= 0 && smoothed_spectrum[j] <= smoothed_spectrum[peak]; j--) {
      left_min = min(left_min, smoothed_spectrum[j]);
    }

    // Find minimum to the right
    for (size_t j = peak; j < ADC_BINS && smoothed_spectrum[j] <= smoothed_spectrum[peak]; j++) {
      right_min = min(right_min, smoothed_spectrum[j]);
    }

    uint32_t prominence = smoothed_spectrum[peak] - max(left_min, right_min);

    if (prominence > round(MAX_CALIBRATION_COUNTS/5)) {
      if (smoothed_spectrum[peak] > p1_value) {
        p2 = p1;
        p2_value = p1_value;
        p1 = peak;
        p1_value = smoothed_spectrum[peak];
      } else if (smoothed_spectrum[peak] > p2_value) {
        p2 = peak;
        p2_value = smoothed_spectrum[peak];
      }
    }
  }

  rp2040.wdt_reset(); // Reset watchdog to prevent reset 
  
  // Store the peak ADC positions for each peak
  ADC2E.X1 = p1;
  ADC2E.X2 = p2;  

  // If two peaks were found
  if (p1_value > 0 && p2_value > 0) {

      // Calculate the linear fit constants k and m
      ADC2E.k = (ADC2E.E1 - ADC2E.E2) / float(p1 - p2);
      ADC2E.m = ADC2E.E1 - ADC2E.k  * float(p1);

      // Calculate energy dynamic range with this calibration (keV)
      ADC2E.Emax = ADC2E.k * ADC_BINS + ADC2E.m;
      ADC2E.Emin = ADC2E.m;

      // Print the peak positions for debugging
      Serial.print("Peak 1 (");
      Serial.print(ADC2E.E1);
      Serial.print(" keV) at bin: ");
      Serial.println(p1);
      Serial.print("Peak 2 (");
      Serial.print(ADC2E.E2);
      Serial.print(" keV) at bin: ");
      Serial.println(p2);

      // Print the calibration constants for debugging
      Serial.print("k: ");
      Serial.println(ADC2E.k, 6); // Print with 6 decimal places
      Serial.print("m: ");
      Serial.println(ADC2E.m, 6); // Print with 6 decimal places

      return true; // Calibration successful
  } else {
      // Here two peaks were not found
      Serial.println("Not enough peaks detected for calibration.");

      return false; // Calibration unsuccessful
  }
}

void spectrumSmoothing(uint32_t* input, float* output) {

  for (size_t i = 0; i < ADC_BINS; i++) {
    spectrumAverage.clear();
    
    for (int j = -AVERAGING_WINDOW / 2; j <= AVERAGING_WINDOW  / 2; j++) {
      if (i + j >= 0 && i + j < ADC_BINS) {
        spectrumAverage.addValue(input[i + j]);
      }
    }
    output[i] = spectrumAverage.getAverage();
  }
}

// Function to load the energy calibration variable 'ADC2E' if stored in flash memory
void loadEnergyCalibration() {

  if (!LittleFS.begin()) {
    Serial.println("An error has occurred while mounting LittleFS");
    return;
  }

  File file = LittleFS.open(energyCalibrationFile, "r");
  if (!file) {
    Serial.println("** Failed to open calibration file for reading");
    return;
  }

  if (file.read((uint8_t*)&ADC2E, sizeof(ADC2E)) != sizeof(ADC2E)) {
    Serial.println("**  Failed to read the entire structure");
  } else {
    Serial.println("**  Calibration data loaded from flash memory");
    // Print loaded values for debugging
    Serial.print("**  Loaded k: "); Serial.println(ADC2E.k);
  }

  // Overwrite the E1 and E2 variables with what is in 'ADC2E_default'
  // This ensures these values can be changed when modifying the code, without rewriting the calibration values
  ADC2E.E1 = ADC2E_default.E1;
  ADC2E.E2 = ADC2E_default.E2;

  file.close();
}

// Function to override existing saved energy calibration 'ADC2E' if a new calibration is performed
void saveEnergyCalibration() {
  File file = LittleFS.open(energyCalibrationFile, "w");
  if (!file) {
    Serial.println("** Failed to open file for writing");
    return;
  }

  if (file.write((uint8_t*)&ADC2E, sizeof(ADC2E)) != sizeof(ADC2E)) {
    Serial.println("** Failed to write the entire structure");
  } else {
    Serial.println("** Calibration data saved to flash memory");
    // Print saved values for debugging
    Serial.print("** Saved k: "); Serial.println(ADC2E.k);
  }
  file.close();
}

// Function to read the battery voltage and draw the battery percentage and symbol
int drawBattery(int old_lvl) {
  // Battery percentage calculation
  // Updated to take more samples to stop
  const uint16_t ADC_OFFSET = round(0.032 / 3.3 * 4096);  // Remove ADC offset of ~30 - 34 mV
  const uint8_t NUM_SAMPLES = 5;
  double sum_batv = 0;
  for (uint8_t i = 0; i < NUM_SAMPLES; i++) {
      sum_batv += analogRead(BAT_PIN);
      delay(2);  // Allow the ADC to stabilize
  }
  uint16_t avg_batv = round(sum_batv / NUM_SAMPLES) - ADC_OFFSET;

  uint8_t bat_lvl = battery_percent(avg_batv);
  bool charging = (digitalRead(CHARGE_PIN) == LOW);

  if(!charging && bat_lvl > old_lvl){
    bat_lvl = old_lvl;
  }


  // Check if charging battery
  if (charging) {

    // Display charging symbol & full battery symbol
    display.drawBitmap(110, 0, battery_full, 16, 6, SSD1306_WHITE);
    display.drawBitmap(114, 8, bolt, 9, 10, SSD1306_WHITE);

    // Over- or under-temperature check for battery    
    static int16_t temp = round(readTemp());
    if (!adc_lock) {  // Only update if ADC is free atm
      temp = round(readTemp());
    }

    if (temp - TEMP_OFFSET < MIN_TEMP || temp - TEMP_OFFSET > MAX_TEMP) {

      // Set to OUTPUT and LOW to disable charging!
      pinMode(BAT_REG_PIN, OUTPUT);
      gpio_set_slew_rate(BAT_REG_PIN, GPIO_SLEW_RATE_SLOW);
      digitalWrite(BAT_REG_PIN, LOW);

    } else {
      // Set as input for high impedance so that battery charge works as normal
      pinMode(BAT_REG_PIN, INPUT);
    }
    
  } else { // Not charging
      // Display battery symbol
      if (bat_lvl > 80) {
        display.drawBitmap(110, 0, battery_full, 16, 6, SSD1306_WHITE);
      } else if (bat_lvl > 60) {
        display.drawBitmap(110, 0, battery_three_quarters, 16, 6, SSD1306_WHITE);
      } else if (bat_lvl > 40) {
        display.drawBitmap(110, 0, battery_half, 16, 6, SSD1306_WHITE);
      } else if (bat_lvl > 20) {
        display.drawBitmap(110, 0, battery_quarter, 16, 6, SSD1306_WHITE);
      } else {
        display.drawBitmap(110, 0, battery_empty, 16, 6, SSD1306_WHITE);
      }

      // Also show current battery percentage
      if(bat_lvl < 100){
        display.setCursor(110, 8); 
        display.print(bat_lvl); 
        display.print("%");
      } else if(bat_lvl < 10){
        display.setCursor(110, 8);
        display.print(bat_lvl);
        display.print(" %");
      } else { // Here battery is 100%
        display.setCursor(109, 8);
        display.print(bat_lvl);
      }
  }

  return bat_lvl;

}

void logData(float dose_rate, float accumulated_dose, float deadtime, float cps, int battery) {
  uint32_t current_dose_rate = round(dose_rate * 1000);  // Convert µSv/h to nSv/h
  uint32_t total_accumulated_dose = round(accumulated_dose * 1000);  // Convert µSv to nSv
  uint8_t deadtime_percent = 100-round(deadtime * 100);
  uint32_t counts_per_second = round(cps);
  
  dataLogBuffer[logIndex] = {
    current_dose_rate,
    total_accumulated_dose,
    deadtime_percent,
    counts_per_second,
    (uint8_t)battery
  };

  // Update the index
  logIndex = (logIndex + 1) % DATALOG_BUFFER_SIZE;

  // Write the buffer to the file every 10 seconds
  if (logIndex == 0) {
    writeDataLogBufferToFile();
  }
}

void writeDataLogBufferToFile() {
  File dataFile = LittleFS.open("/datalog.csv", "a");
  if (dataFile) {
    for (int i = 0; i < DATALOG_BUFFER_SIZE; i++) {
      dataFile.print(dataLogBuffer[i].currentDoseRate);
      dataFile.print(",");
      dataFile.print(dataLogBuffer[i].totalAccumulatedDose);
      dataFile.print(",");
      dataFile.print(dataLogBuffer[i].deadtime);
      dataFile.print(",");
      dataFile.print(dataLogBuffer[i].countsPerSecond);
      dataFile.print(",");
      dataFile.println(dataLogBuffer[i].battery);
    }
    dataFile.close();
  } else {
    Serial.println("Error opening datalog.csv");
  }

  // Check file size and truncate if necessary
  truncateDataLogFile();
}

void truncateDataLogFile() {
  File dataFile = LittleFS.open("/datalog.csv", "r");
  if (dataFile) {
    long fileSize = dataFile.size();

    //Serial.print("Datalog.csv file size: "); Serial.println(fileSize);

    if (fileSize > DATALOG_FILE_SIZE) {
      // Read the last DATALOG_FILE_SIZE bytes
      //Serial.print("Truncating the data");;
      dataFile.seek(fileSize - DATALOG_FILE_SIZE);
      String remainingData = dataFile.readString();
      dataFile.close();

      // Rewrite the file with only the last part
      dataFile = LittleFS.open("/datalog.csv", "w");
      if (dataFile) {
        dataFile.println("CurrentDoseRate,TotalAccumulatedDose,Deadtime,CountsPerSecond,Battery");
        dataFile.print(remainingData);
        dataFile.close();
      }
    } else {
      dataFile.close();
    }
  }
}

void printSpectrumLogToSerial([[maybe_unused]] String *args) {
  // Messsage to indicate the start of the spectrum log
  Serial.println("---Spectrum Log Start");
  // Print the current spectrum from the global variable 'spectrum'
  Serial.println("ADC_BIN,COUNT");
  for (uint16_t i = 0; i < ADC_BINS; ++i) {
    Serial.print(i);
    Serial.print(",");
    Serial.println(spectrum[i]);
  }
  Serial.println("---Spectrum Log End");
}

void printDataLogToSerial([[maybe_unused]] String *args) {
  File dataFile = LittleFS.open("/datalog.csv", "r");
  if (dataFile) {
    // Debug:
    //long i = 0;
    while (dataFile.available()) {
      String line = dataFile.readStringUntil('\n');
      // Debug:
      //Serial.print(i);Serial.print(" ");Serial.println(line);
      //i++;
      Serial.println(line);
      
    }
    //Serial.print("Number of entries in datalog.csv: "); Serial.println(i); 
    dataFile.close();
    Serial.println("END_OF_LOG"); // Indicate end of log
  } else {
    Serial.println("Error opening datalog.csv");
  }
}

void saveDoseData() {
  File doseFile = LittleFS.open("/dose_data.bin", "w");
  if (doseFile) {
    doseFile.write((uint8_t*)&accumulated_dose, sizeof(accumulated_dose));
    doseFile.write((uint8_t*)&max_dose_rate, sizeof(max_dose_rate));
    doseFile.write((uint8_t*)&min_dose_rate, sizeof(min_dose_rate));
    doseFile.close();
  } else {
    Serial.println("Error opening dose_data.bin for writing");
  }
}

void loadDoseData() {
  File doseFile = LittleFS.open("/dose_data.bin", "r");
  if (doseFile) {
    doseFile.read((uint8_t*)&accumulated_dose, sizeof(accumulated_dose));
    doseFile.read((uint8_t*)&max_dose_rate, sizeof(max_dose_rate));
    doseFile.read((uint8_t*)&min_dose_rate, sizeof(min_dose_rate));
    doseFile.close();
  } else {
    Serial.println("Error opening dose_data.bin for reading");
    // Initialize with default values if file doesn't exist
    accumulated_dose = 0;
    max_dose_rate = -1;
    min_dose_rate = -1;
  }
}
