#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>

#include <RTClib.h>
#include <ESP32Servo.h>
#include <EEPROM.h>
#include "HX711.h"

// WiFi credentials
const char* ssid = "PillDispenser_AP";
const char* password = "12345678";

// Pin definitions
const int servoPin = 4;
const int speakerPin = 2;
const int ledPin = 32;
const int DOUT = 14;  // HX711 DT
const int CLK = 12;   // HX711 SCK

// Objects
WebServer server(80);
RTC_DS3231 rtc;
Servo myServo;
HX711 scale;

// Constants
const int EEPROM_SIZE = 128;
const float CALIBRATION_FACTOR = 2280.0f; // Adjust this after calibration
const float PILL_WEIGHT_THRESHOLD = 0.5; // Minimum weight to consider pill dispensed (grams)

// Variables
String schedule[7];
bool triggeredToday[7] = {false, false, false, false, false, false, false};
String lastDispenseTime = "None";
String dayNames[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// Weight monitoring variables
bool pillDispensed = false;
float baselineWeight = 0.0;
unsigned long pillDispenseTime = 0;
bool ledState = false;
unsigned long lastLedToggle = 0;
unsigned long baselineReturnTime = 0;  // Time when weight first returned to baseline
bool waitingForConfirmation = false;   // Flag to indicate we're in the 500 millisecond confirmation period
const unsigned long CONFIRMATION_DELAY = 500; // 500 milliseconds, .5 seconds

// EEPROM Functions
void saveScheduleToEEPROM() {
  for (int i = 0; i < 7; i++) {
    int addr = i * 10;
    for (int j = 0; j < schedule[i].length(); j++) {
      EEPROM.write(addr + j, schedule[i][j]);
    }
    EEPROM.write(addr + schedule[i].length(), '\0');
  }
  EEPROM.commit();
}

void loadScheduleFromEEPROM() {
  for (int i = 0; i < 7; i++) {
    int addr = i * 10;
    char buf[10];
    for (int j = 0; j < 9; j++) {
      buf[j] = EEPROM.read(addr + j);
    }
    buf[9] = '\0';
    schedule[i] = String(buf);
  }
}

// Sound Functions
void toneManual(int pin, int frequency, int duration) {
  long period = 1000000L / frequency;
  long halfPeriod = period / 2;
  long cycles = (long)frequency * duration / 1000;
  for (long i = 0; i < cycles; i++) {
    digitalWrite(pin, HIGH);
    delayMicroseconds(halfPeriod);
    digitalWrite(pin, LOW);
    delayMicroseconds(halfPeriod);
  }
}

// Fast 3-second alert pattern
void playJingle() {
  int melody[] = {523, 659, 784};
  int durations[] = {150, 150, 200};
  int pauseBetweenNotes = 30;
  int pauseBetweenRepeats = 200;
  
  // Calculate how many times the pattern fits in 3 seconds (3000ms)
  int patternTime = durations[0] + pauseBetweenNotes + durations[1] + pauseBetweenNotes + durations[2] + pauseBetweenRepeats;
  int repeats = 3000 / patternTime;
  
  for (int repeat = 0; repeat < repeats; repeat++) {
    for (int i = 0; i < 3; i++) {
      toneManual(speakerPin, melody[i], durations[i]);
      delay(pauseBetweenNotes);
    }
    delay(pauseBetweenRepeats - pauseBetweenNotes);
  }
}

// Time formatting
String formatTime(DateTime now) {
  String h = now.hour() < 10 ? "0" + String(now.hour()) : String(now.hour());
  String m = now.minute() < 10 ? "0" + String(now.minute()) : String(now.minute());
  String s = now.second() < 10 ? "0" + String(now.second()) : String(now.second());
  return dayNames[now.dayOfTheWeek()] + " " + h + ":" + m + ":" + s;
}

// Weight monitoring functions
void calibrateScale() {
  Serial.println("Calibrating scale...");
  scale.set_scale(CALIBRATION_FACTOR);
  scale.tare(); // Reset to zero
  baselineWeight = scale.get_units(5); // Get average of 5 readings
  Serial.print("Baseline weight set to: ");
  Serial.print(baselineWeight);
  Serial.println(" g");
}

void monitorWeight() {
  // Only monitor if a pill has been dispensed
  if (!pillDispensed) {
    return;
  }
  
  float currentWeight = scale.get_units(3); // Average of 3 readings for stability
  bool weightAtBaseline = (currentWeight <= 5.0); // Simple: if weight is 5g or less, pill is gone
  
  if (weightAtBaseline) {
    // Weight is at baseline
    if (!waitingForConfirmation) {
      // First time back to baseline - start confirmation timer
      waitingForConfirmation = true;
      baselineReturnTime = millis();
      digitalWrite(ledPin, LOW); // Turn off LED immediately
      Serial.println("Weight returned to baseline - confirming for 1.5 seconds...");
    } else {
      // We're already in confirmation period - check if enough time has passed
      if (millis() - baselineReturnTime >= CONFIRMATION_DELAY) {
        // Pill has been taken and confirmed
        pillDispensed = false;
        waitingForConfirmation = false;
        Serial.println("Pill taken successfully and confirmed!");
        Serial.print("Final weight: ");
        Serial.print(currentWeight);
        Serial.println(" g");
      }
    }
  } else {
    // Weight is above baseline (pill or pressure detected)
    if (waitingForConfirmation) {
      // Weight went back up during confirmation period - reset confirmation
      waitingForConfirmation = false;
      Serial.println("Weight changed during confirmation - resetting timer");
    }
    
    // Flash LED to indicate pill needs to be taken
    unsigned long currentTime = millis();
    if (currentTime - lastLedToggle > 500) { // Flash every 500ms
      ledState = !ledState;
      digitalWrite(ledPin, ledState ? HIGH : LOW);
      lastLedToggle = currentTime;
    }
    
    // Debug output (can be removed in final version)
    static unsigned long lastDebugTime = 0;
    if (currentTime - lastDebugTime > 2000) { // Print every 2 seconds
      Serial.print("Pill detected on scale. Weight: ");
      Serial.print(currentWeight);
      Serial.println(" g - Please take your pill!");
      lastDebugTime = currentTime;
    }
  }
}

// Web interface
String getHTMLPage(String message = "") {
  float currentWeight = scale.get_units(3);
  String weightStatus = pillDispensed ? "PILL WAITING TO BE TAKEN" : "Ready";
  
  String html = R"rawliteral(
  <!DOCTYPE html><html><head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body {
      font-family: 'Segoe UI', sans-serif;
      background: #181818;
      color: #eee;
      padding: 20px;
      margin: 0;
    }
    h2 {
      color: #00d9ff;
      text-shadow: 1px 1px 5px #00d9ff88;
    }
    .card {
      background: #222;
      padding: 20px;
      border-radius: 12px;
      box-shadow: 0 0 15px #000;
      margin-bottom: 20px;
    }
    input[type=time] {
      padding: 8px;
      border-radius: 6px;
      border: none;
      background: #333;
      color: #fff;
    }
    input[type=submit], button {
      padding: 10px 20px;
      border: none;
      border-radius: 8px;
      background: #00d9ff;
      color: #000;
      font-weight: bold;
      cursor: pointer;
      box-shadow: 0 0 10px #00d9ff88;
      transition: 0.2s;
    }
    input[type=submit]:hover, button:hover {
      background: #0ff;
    }
    .success {
      color: lightgreen;
      margin-top: 10px;
    }
    .warning {
      color: #ff6b6b;
      font-weight: bold;
    }
    .status {
      padding: 10px;
      border-radius: 8px;
      margin: 10px 0;
    }
    .ready { background: #2d5a27; }
    .waiting { background: #5a2727; }
  </style></head><body>
  <h2>Pill Dispenser Scheduler</h2>
  <div class="card">
  <form id="form">
  )rawliteral";

  for (int i = 0; i < 7; i++) {
    html += dayNames[i] + ": <input type='time' name='d" + String(i) + "' value='" + schedule[i] + "'><br><br>";
  }

  html += R"rawliteral(
    <input type="submit" value="Save Schedule">
  </form>
  <div class="success" id="msg">)rawliteral" + message + R"rawliteral(</div>
  </div>
  <div class="card">
    <p><strong>Current Time:</strong> <span id="clock">Loading...</span></p>
    <p><strong>Last Dispensed:</strong> )rawliteral" + lastDispenseTime + R"rawliteral(</p>
    <p><strong>Current Weight:</strong> <span id="weight">)rawliteral" + String(currentWeight, 2) + R"rawliteral( g</span></p>
    <div class="status )rawliteral" + (pillDispensed ? "waiting" : "ready") + R"rawliteral(">
      <strong>Status:</strong> )rawliteral" + weightStatus + R"rawliteral(
    </div>
    <button onclick="syncTime()">Sync with my time</button>
    <button onclick="testDispense()">Test Dispense</button>
    <button onclick="calibrateScale()">Calibrate Scale</button>
  </div>
  <script>
    document.getElementById('form').addEventListener('submit', function(e) {
      e.preventDefault();
      const formData = new FormData(this);
      const params = new URLSearchParams(formData).toString();
      fetch('/set?' + params).then(() => {
        document.getElementById('msg').innerText = "✔ Schedule saved successfully.";
      });
    });

    function syncTime() {
      const now = new Date();
      const hour = now.getHours();
      const minute = now.getMinutes();
      const year = now.getFullYear();
      const month = now.getMonth() + 1; // JavaScript months are 0-based
      const day = now.getDate(); // Day of month (1-31)
      
      fetch(`/sync?hour=${hour}&minute=${minute}&year=${year}&month=${month}&day=${day}`)
        .then(() => alert("Date and time synced!"));
    }

    function testDispense() {
      fetch('/dispense').then(() => alert("Test dispense activated!"));
    }

    function calibrateScale() {
      if(confirm("Remove all items from scale, then click OK to calibrate.")) {
        fetch('/calibrate').then(() => alert("Scale calibrated!"));
      }
    }

    function updateStatus() {
      fetch("/status")
        .then(res => res.json())
        .then(data => {
          document.getElementById("clock").innerText = data.time;
          document.getElementById("weight").innerText = data.weight + " g";
        });
    }
    setInterval(updateStatus, 2000);
    updateStatus();
  </script>
  </body></html>
  )rawliteral";
  return html;
}

// Dispense function
void dispense() {
  Serial.println("Dispensing pill...");
  
  // Set baseline weight BEFORE dispensing (empty scale)
  baselineWeight = scale.get_units(5);
  Serial.print("Baseline weight set to: ");
  Serial.print(baselineWeight);
  Serial.println(" g");
  
  myServo.write(180);
  delay(150);
  myServo.write(90);
  playJingle();  // Now uses the fast 3-second alert
  
  DateTime now = rtc.now();
  lastDispenseTime = dayNames[now.dayOfTheWeek()] + " " +
    (now.hour() < 10 ? "0" : "") + String(now.hour()) + ":" +
    (now.minute() < 10 ? "0" : "") + String(now.minute());
  
  // Reset all pill monitoring variables
  pillDispensed = true;
  waitingForConfirmation = false;
  baselineReturnTime = 0;
  pillDispenseTime = millis();
  digitalWrite(ledPin, LOW); // Make sure LED starts off
  
  Serial.println("Pill dispensed. Monitoring for pickup...");
}

// Schedule checking
void checkSchedule() {
  DateTime now = rtc.now();
  int day = now.dayOfTheWeek();
  String currentTime = (now.hour() < 10 ? "0" : "") + String(now.hour()) + ":" +
                       (now.minute() < 10 ? "0" : "") + String(now.minute());

  if (schedule[day] == currentTime && !triggeredToday[day]) {
    dispense();
    triggeredToday[day] = true;
  }

  // Reset triggers at midnight
  if (now.hour() == 0 && now.minute() == 0 && now.second() == 0) {
    for (int i = 0; i < 7; i++) triggeredToday[i] = false;
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  
  // Initialize RTC
  rtc.begin();
  if (rtc.lostPower()) {
    Serial.println("RTC lost power, setting time to compile time.");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // Initialize EEPROM and load schedule
  EEPROM.begin(EEPROM_SIZE);
  loadScheduleFromEEPROM();

  // Initialize servo
  myServo.setPeriodHertz(50);
  myServo.attach(servoPin);
  myServo.write(90);

  // Initialize pins
  pinMode(speakerPin, OUTPUT);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  // Initialize HX711 scale
  scale.begin(DOUT, CLK);
  Serial.println("Initializing scale...");
  delay(1000);

  if (!scale.is_ready()) {
    Serial.println("HX711 not found. Check wiring!");
  } else {
    calibrateScale();
  }

  // Start WiFi AP
  WiFi.softAP(ssid, password);
  Serial.println("AP ready at http://192.168.4.1");

  // Web server routes
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", getHTMLPage());
  });

  server.on("/status", HTTP_GET, []() {
    String json = "{";
    json += "\"time\":\"" + formatTime(rtc.now()) + "\",";
    json += "\"weight\":\"" + String(scale.get_units(3), 2) + "\",";
    json += "\"pillWaiting\":" + String(pillDispensed ? "true" : "false");
    json += "}";
    server.send(200, "application/json", json);
  });

  server.on("/set", HTTP_GET, []() {
    for (int i = 0; i < 7; i++) {
      if (server.hasArg("d" + String(i))) {
        schedule[i] = server.arg("d" + String(i));
      }
    }
    saveScheduleToEEPROM();
    server.send(200, "text/html", getHTMLPage("✔ Schedule saved successfully."));
  });

  server.on("/sync", HTTP_GET, []() {
    if (server.hasArg("hour") && server.hasArg("minute") && server.hasArg("year") && server.hasArg("month") && server.hasArg("day")) {
      int h = server.arg("hour").toInt();
      int m = server.arg("minute").toInt();
      int year = server.arg("year").toInt();
      int month = server.arg("month").toInt();
      int day = server.arg("day").toInt();
      
      // Set the complete date and time
      rtc.adjust(DateTime(year, month, day, h, m, 0));
      Serial.println("Full date and time synced!");
    }
    server.send(200, "text/plain", "Date and time synced");
  });

  server.on("/dispense", HTTP_GET, []() {
    dispense();
    server.send(200, "text/plain", "Pill dispensed");
  });

  server.on("/calibrate", HTTP_GET, []() {
    calibrateScale();
    server.send(200, "text/plain", "Scale calibrated");
  });

  server.begin();
  Serial.println("Pill dispenser system ready!");
}

void loop() {
  server.handleClient();
  checkSchedule();
  monitorWeight();
  delay(100); // Faster loop for better weight monitoring
}
