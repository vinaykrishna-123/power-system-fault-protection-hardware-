/*
 * ===================================================================
 * ESP32 SMART GRID MONITOR - (LOCAL WEB SERVER EDITION)
 * ===================================================================
 *
 * This code replaces the Firebase method. It turns your ESP32
 * into a mini web server.
 *
 * HOW TO USE:
 * 1. Fill in your WiFi credentials below.
 * 2. Check all your hardware pin numbers.
 * 3. Upload the code to your ESP32.
 * 4. Open the Arduino Serial Monitor (Tools > Serial Monitor).
 * 5. Wait for it to connect to WiFi and print an IP address.
 * 6. Type that IP address (e.g., "192.168.1.123") into any
 * web browser on the same WiFi network.
 * 7. Your dashboard will appear!
 *
 */

// ===================================================================
// LIBRARIES
// ===================================================================

// --- Core ESP32 & Web Server Libraries ---
#include <WiFi.h>
#include <WebServer.h> // The web server library

// --- Your Project Libraries ---
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "EmonLib.h"

// ===================================================================
// 1. FILL IN YOUR WIFI CREDENTIALS
// ===================================================================
#define WIFI_SSID "Vinay" // <--- PASTE YOUR WIFI SSID HERE
#define WIFI_PASSWORD "12345678" // <--- PASTE YOUR WIFI PASSWORD HERE

// ===================================================================
// 2. CHECK YOUR HARDWARE PINS
// ===================================================================
// (These are the pins from your last file)
const int CURRENT_SENSOR_PIN = 34;
const int VOLTAGE_SENSOR_PIN = 35;
const int SDA_PIN = 21;
const int SCL_PIN = 22;
const int LOAD_RELAY_PIN = 5;
const int CAP_BANK_1_RELAY_PIN = 4;
const int CAP_BANK_2_RELAY_PIN = 18; // <-- CHECK THIS PIN!
const int CAP_BANK_3_RELAY_PIN = 19; // <-- CHECK THIS PIN!
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ===================================================================
// 3. CHECK CALIBRATION & LOGIC
// ===================================================================
// (Fixed names to prevent compile errors)
const float VOLTAGE_CALIBRATION = 48.0; // RECALIBRATE THIS!
const float CURRENT_CALIBRATION = 5.17; // RECALIBRATE THIS!

// --- Protection & PFC Logic ---
const float SHORT_CIRCUIT_THRESHOLD = 7.0;
#define OVER_VOLTAGE_THRESHOLD 245.0
#define UNDER_VOLTAGE_THRESHOLD 2.0
#define PF_STAGE_1_TRIGGER 0.95
#define PF_STAGE_2_TRIGGER 0.85
#define PF_STAGE_3_TRIGGER 0.75

// ===================================================================
// GLOBAL OBJECTS & VARIABLES
// ===================================================================
EnergyMonitor emon;
WebServer server(80); // Create the web server object on port 80

// Global struct to hold all data
struct SensorData {
    float voltage = 0.0;
    float current = 0.0;
    float realPower = 0.0;
    float reactivePower = 0.0;
    float powerFactor = 1.0;
    String mainLoadStatus = "OFFLINE";
    bool capBank1 = false;
    bool capBank2 = false;
    bool capBank3 = false;
    String systemStatus = "Initializing...";
};
SensorData gridData;

// Non-Blocking Timers
unsigned long lastSensorRead = 0;
const int SENSOR_READ_INTERVAL = 2000; // Read sensors every 2 seconds

// ===================================================================
// 4. FUNCTION PROTOTYPES (Forward Declarations)
// ===================================================================
// FIX: This new section fixes all "was not declared in this scope" errors.
// It tells the compiler that these functions exist before they are defined.
void handleRoot();
void handleData();
void handleNotFound();
void readEnergySensors();
void checkVoltageProtection();
void checkShortCircuit(double currentReading);
void tripMainLoad();
void runPowerFactorCorrection();
void setCapBank(int bank, bool state);
void updateDisplay(); // This is your function for the LCD

// ===================================================================
// S E T U P
// ===================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\nESP32 Local Web Monitor Initializing...");

    // --- Initialize Relays ---
    pinMode(LOAD_RELAY_PIN, OUTPUT);
    pinMode(CAP_BANK_1_RELAY_PIN, OUTPUT);
    pinMode(CAP_BANK_2_RELAY_PIN, OUTPUT);
    pinMode(CAP_BANK_3_RELAY_PIN, OUTPUT);
    digitalWrite(LOAD_RELAY_PIN, HIGH);
    digitalWrite(CAP_BANK_1_RELAY_PIN, HIGH);
    digitalWrite(CAP_BANK_2_RELAY_PIN, HIGH);
    digitalWrite(CAP_BANK_3_RELAY_PIN, HIGH);

    // --- Initialize I2C and LCD ---
    Wire.begin(SDA_PIN, SCL_PIN);
    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0);
    lcd.print("Connecting to");
    lcd.setCursor(0, 1);
    lcd.print("WiFi...");

    // --- Configure EmonLib ---
    emon.voltage(VOLTAGE_SENSOR_PIN, VOLTAGE_CALIBRATION, 1.7);
    emon.current(CURRENT_SENSOR_PIN, CURRENT_CALIBRATION);

    // --- Connect to WiFi ---
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        delay(500);
    }
    Serial.println("\nWiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    // --- Display IP on LCD ---
    lcd.clear();
    lcd.print("IP Address:");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP());

    // --- CONFIGURE WEB SERVER ---
    // This is the main web page
    server.on("/", handleRoot);
    
    // This is the endpoint the webpage will call to get new data
    server.on("/data", handleData);
    
    // This is for all other requests (sends a 404 error)
    server.onNotFound(handleNotFound);

    // Start the server
    server.begin();
    Serial.println("Web Server Started. Open IP address in browser.");

    // --- Turn on Main Load ---
    digitalWrite(LOAD_RELAY_PIN, LOW); // Load ON (LOW = ON)
    gridData.mainLoadStatus = "ONLINE";
    gridData.systemStatus = "OK";
}

// ===================================================================
// M A I N   L O O P
// ===================================================================
void loop() {
    // This is the most important line for the web server!
    // It checks if any browsers are trying to connect.
    server.handleClient();

    // This is your non-blocking sensor logic
    unsigned long currentMillis = millis();
    if (currentMillis - lastSensorRead >= SENSOR_READ_INTERVAL) {
        lastSensorRead = currentMillis;

        readEnergySensors();
        
        if (gridData.mainLoadStatus == "ONLINE") {
            checkShortCircuit(gridData.current);
            checkVoltageProtection();
            if (gridData.systemStatus == "OK") {
                 runPowerFactorCorrection();
            }
        }
        // FIX: Changed 'updateLCD()' to 'updateDisplay()' to match your function definition
        updateDisplay();
    }
}

// ===================================================================
// WEB SERVER HANDLER FUNCTIONS
// ===================================================================

/*
 * This function sends the main HTML web page to the browser
 */
void handleRoot() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 Smart Grid Monitor</title>
    <!-- 1. Load Google Charts Library -->
    <script type="text/javascript" src="https://www.gstatic.com/charts/loader.js"></script>
    <style>
        body {
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
            background-color: #f0f2f5;
            color: #1a1a1a;
            margin: 0;
            padding: 24px;
            display: grid;
            gap: 24px;
            justify-content: center;
        }
        h1 {
            text-align: center;
            margin: 0;
            color: #0052cc;
        }
        .header { text-align: center; margin-bottom: 12px; }
        .grid-container {
            display: grid;
            grid-template-columns: repeat(3, 1fr);
            gap: 24px;
            max-width: 1000px;
        }
        .card, .card-full {
            background-color: #ffffff;
            border-radius: 16px;
            padding: 24px;
            box-shadow: 0 4px 12px rgba(0,0,0,0.05);
        }
        .card-full { grid-column: 1 / -1; }
        .gauge-chart { width: 100%; height: 200px; }
        h2 {
            text-align: center;
            margin-top: 0;
            margin-bottom: 16px;
            color: #333;
        }
        .status-grid, .power-grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 16px;
        }
        .power-item {
            display: flex;
            justify-content: space-between;
            align-items: center;
            font-size: 1.2rem;
            padding: 16px;
            background: #f0f2f5;
            border-radius: 12px;
        }
        .power-item span:first-child { font-weight: 500; color: #555; }
        .power-item span:last-child { font-weight: 600; color: #000; }
        .status-item {
            padding: 16px;
            border-radius: 12px;
            text-align: center;
            font-size: 1.1rem;
            font-weight: 600;
            transition: all 0.3s ease;
        }
        .status-on, .status-ok { background-color: #d9f7e6; color: #008a3e; }
        .status-off, .status-fault { background-color: #ffeded; color: #d92d2d; }
        
        /* Responsive */
        @media (max-width: 900px) {
            .grid-container { grid-template-columns: 1fr 1fr; }
        }
        @media (max-width: 600px) {
            body { padding: 16px; }
            .grid-container { grid-template-columns: 1fr; }
            .status-grid { grid-template-columns: 1fr; }
        }
    </style>
</head>
<body>
    <div class="header">
        <h1>Smart Grid Monitor</h1>
        <ul>
          <li>Abhinav</li>
          <li>Vinay</li>
        </ul>
        <p>Real-time data from your ESP32</p>
    </div>

    <!-- 2. The DIVs where the charts will be drawn -->
    <div class="grid-container">
        <div class="card">
            <h2>Voltage (V)</h2>
            <div id="gauge_voltage" class="gauge-chart"></div>
        </div>
        <div class="card">
            <h2>Current (A)</h2>
            <div id="gauge_current" class="gauge-chart"></div>
        </div>
        <div class="card">
            <h2>Power Factor</h2>
            <div id="gauge_pf" class="gauge-chart"></div>
        </div>
    </div>

    <div class="card-full">
        <h2>Power Analysis</h2>
        <div class="power-grid">
            <div class="power-item">
                <span>Real Power (P)</span>
                <span id="val_real_power">... W</span>
            </div>
            <div class="power-item">
                <span>Reactive Power (Q)</span>
                <span id="val_reactive_power">... VAR</span>
            </div>
        </div>
    </div>

    <div class="card-full">
        <h2>System & Relay Status</h2>
        <div class="status-grid">
            <div class="status-item" id="status_system">Initializing...</div>
            <div class="status-item" id="status_main_load">...</div>
            <div class="status-item" id="status_cap1">Cap Bank 1: ...</div>
            <div class="status-item" id="status_cap2">Cap Bank 2: ...</div>
            <div class="status-item" id="status_cap3">Cap Bank 3: ...</div>
        </div>
    </div>

    <!-- 3. The JavaScript to draw charts and get data -->
    <script type="text/javascript">
        // Load the Google Charts visualization library
        google.charts.load('current', {'packages':['gauge']});
        google.charts.setOnLoadCallback(drawGauges);

        var gaugeData, gaugeVoltage, gaugeCurrent, gaugePF;
        var gaugeOptions = {
            width: 400, height: 200,
            redFrom: 90, redTo: 100,
            yellowFrom:75, yellowTo: 90,
            minorTicks: 5,
            animation: { duration: 500, easing: 'inAndOut' }
        };

        // Function to draw the gauges
        function drawGauges() {
            // Voltage
            gaugeData = google.visualization.arrayToDataTable([
                ['Label', 'Value'], ['V', 0]
            ]);
            gaugeOptions.min = 0; gaugeOptions.max = 300;
            gaugeOptions.yellowFrom = 245; gaugeOptions.yellowTo = 300;
            gaugeOptions.redFrom = 250; gaugeOptions.redTo = 300;
            gaugeVoltage = new google.visualization.Gauge(document.getElementById('gauge_voltage'));
            gaugeVoltage.draw(gaugeData, gaugeOptions);

            // Current
            gaugeData = google.visualization.arrayToDataTable([
                ['Label', 'Value'], ['A', 0]
            ]);
            gaugeOptions.min = 0; gaugeOptions.max = 20; // Set to your max expected current
            gaugeOptions.yellowFrom = 15; gaugeOptions.yellowTo = 20;
            gaugeOptions.redFrom = 17; gaugeOptions.redTo = 20;
            gaugeCurrent = new google.visualization.Gauge(document.getElementById('gauge_current'));
            gaugeCurrent.draw(gaugeData, gaugeOptions);

            // Power Factor
            gaugeData = google.visualization.arrayToDataTable([
                ['Label', 'Value'], ['PF', 0]
            ]);
            gaugeOptions.min = 0; gaugeOptions.max = 1;
            gaugeOptions.yellowFrom = 0.7; gaugeOptions.yellowTo = 0.85;
            gaugeOptions.redFrom = 0; gaugeOptions.redTo = 0.7;
            gaugePF = new google.visualization.Gauge(document.getElementById('gauge_pf'));
            gaugePF.draw(gaugeData, gaugeOptions);
            
            // Get the first set of data
            fetchData();
        }

        // Function to fetch new data from the ESP32
        function fetchData() {
            // Fetch data from the /data endpoint on the ESP32
            fetch('/data')
                .then(response => response.json())
                .then(data => {
                    console.log(data);
                    
                    // Update Gauges
                    gaugeData = google.visualization.arrayToDataTable([['Label', 'Value'], ['V', data.voltage]]);
                    gaugeVoltage.draw(gaugeData, gaugeOptions);
                    
                    gaugeData = google.visualization.arrayToDataTable([['Label', 'Value'], ['A', data.current]]);
                    gaugeCurrent.draw(gaugeData, gaugeOptions);
                    
                    gaugeData = google.visualization.arrayToDataTable([['Label', 'Value'], ['PF', data.powerFactor]]);
                    gaugePF.draw(gaugeData, gaugeOptions);

                    // Update Power Analysis
                    document.getElementById('val_real_power').innerText = data.realPower + " W";
                    document.getElementById('val_reactive_power').innerText = data.reactivePower + " VAR";

                    // Update Statuses
                    updateStatus('status_system', data.systemStatus, ['OK']);
                    updateStatus('status_main_load', 'Main Load: ' + data.mainLoadStatus, ['ONLINE']);
                    updateStatus('status_cap1', 'Cap Bank 1: ' + (data.capBank1 ? 'ON' : 'OFF'), ['ON']);
                    updateStatus('status_cap2', 'Cap Bank 2: ' + (data.capBank2 ? 'ON' : 'OFF'), ['ON']);
                    updateStatus('status_cap3', 'Cap Bank 3: ' + (data.capBank3 ? 'ON' : 'OFF'), ['ON']);

                    // Fetch new data again after 2.5 seconds
                    setTimeout(fetchData, 1000);
                })
                .catch(error => {
                    console.error('Error fetching data:', error);
                    // If it fails, try again
                    setTimeout(fetchData, 5000);
                });
        }
        
        // Helper function to update status colors
        function updateStatus(elementId, text, goodStates) {
            let el = document.getElementById(elementId);
            el.innerText = text;
            
            let isGood = false;
            if (goodStates.includes(text)) isGood = true;
            if (goodStates.includes(text.split(': ')[1])) isGood = true;

            if (isGood) {
                el.classList.add('status-ok');
                el.classList.remove('status-fault');
            } else {
                el.classList.add('status-fault');
                el.classList.remove('status-ok');
            }
        }
    </script>
</body>
</html>
)rawliteral";

    server.send(200, "text/html", html);
}

/*
 * This function is called by the JavaScript.
 * It sends all your sensor data as a JSON object.
 */
void handleData() {
    // Create a JSON string
    String json = "{";
    json += "\"voltage\":" + String(gridData.voltage, 2) + ",";
    json += "\"current\":" + String(gridData.current, 2) + ",";
    json += "\"realPower\":" + String(gridData.realPower, 0) + ",";
    json += "\"reactivePower\":" + String(gridData.reactivePower, 0) + ",";
    json += "\"powerFactor\":" + String(gridData.powerFactor, 2) + ",";
    json += "\"mainLoadStatus\":\"" + gridData.mainLoadStatus + "\",";
    json += "\"systemStatus\":\"" + gridData.systemStatus + "\",";
    json += "\"capBank1\":" + String(gridData.capBank1 ? "true" : "false") + ",";
    json += "\"capBank2\":" + String(gridData.capBank2 ? "true" : "false") + ",";
    json += "\"capBank3\":" + String(gridData.capBank3 ? "true" : "false");
    json += "}";

    // Send the JSON to the browser
    server.send(200, "application/json", json);
}

/*
 * This function is called if the browser requests a page that doesn't exist
 */
void handleNotFound() {
    server.send(404, "text/plain", "404: Not found");
}


// ===================================================================
// YOUR ORIGINAL HELPER FUNCTIONS (UNCHANGED, BUT UPDATED)
// ===================================================================

/*
 * Reads EmonLib and stores all data in the global struct
 */
void readEnergySensors() {
    emon.calcVI(20, 2000);
    gridData.voltage = emon.Vrms;
    gridData.current = emon.Irms;
    gridData.realPower = emon.realPower;
    gridData.reactivePower = emon.apparentPower * sin(acos(emon.powerFactor));
    float pf = emon.powerFactor;
    if (gridData.current < 0.1) {
        pf = 1.00;
        gridData.realPower = 0.0;
        gridData.reactivePower = 0.0;
    }
    if (pf > 1.0 || pf < 0.0 || isnan(pf)) {
        pf = 1.0;
    }
    gridData.powerFactor = pf;
    
    Serial.printf("V: %.1fV, I: %.2fA, P: %.0fW, Q: %.0fVAR, PF: %.2f\n",
                  gridData.voltage, gridData.current, gridData.realPower,
                  gridData.reactivePower, gridData.powerFactor);
}

/*
 * Voltage protection logic
 */
void checkVoltageProtection() {
    if (gridData.voltage < 50.0) return;
    if (gridData.voltage > OVER_VOLTAGE_THRESHOLD) {
        gridData.systemStatus = "OVER VOLTAGE";
        Serial.println(gridData.systemStatus);
        tripMainLoad();
    } else if (gridData.voltage < UNDER_VOLTAGE_THRESHOLD) {
        gridData.systemStatus = "UNDER VOLTAGE";
        Serial.println(gridData.systemStatus);
        tripMainLoad();
    } else {
        gridData.systemStatus = "OK";
    }
}

/*
 * Short circuit protection logic
 */
void checkShortCircuit(double currentReading) {
    if (currentReading > SHORT_CIRCUIT_THRESHOLD) {
        Serial.println(" !!! SHORT CIRCUIT DETECTED !!!");
        gridData.systemStatus = "SHORT CIRCUIT";
        tripMainLoad();
        while (true) {
            updateDisplay(); // This was correct
            delay(1000);
        }
    }
}

/*
 * Trip all relays
 */
void tripMainLoad() {
    digitalWrite(LOAD_RELAY_PIN, HIGH);
    gridData.mainLoadStatus = "TRIPPED";
    setCapBank(1, false);
    setCapBank(2, false);
    setCapBank(3, false);
}

/*
 * 3-Stage PFC logic
 */
void runPowerFactorCorrection() {
    float pf = gridData.powerFactor;
    if (pf < PF_STAGE_3_TRIGGER) {
        setCapBank(1, true);
        setCapBank(2, true);
        setCapBank(3, true);
    } else if (pf < PF_STAGE_2_TRIGGER) {
        setCapBank(1, true);
        setCapBank(2, true);
        setCapBank(3, false);
    } else if (pf < PF_STAGE_1_TRIGGER) {
        setCapBank(1, true);
        setCapBank(2, false);
        setCapBank(3, false);
    } else {
        setCapBank(1, false);
        setCapBank(2, false);
        setCapBank(3, false);
    }
}

/*
 * Helper function to control cap banks
 */
void setCapBank(int bank, bool state) {
    int pin;
    bool* globalState;
    if (bank == 1) {
        pin = CAP_BANK_1_RELAY_PIN; globalState = &gridData.capBank1;
    } else if (bank == 2) {
        pin = CAP_BANK_2_RELAY_PIN; globalState = &gridData.capBank2;
    } else {
        pin = CAP_BANK_3_RELAY_PIN; globalState = &gridData.capBank3;
    }
    if (*globalState != state) {
        *globalState = state;
        digitalWrite(pin, state ? LOW : HIGH);
        Serial.printf("Capacitor Bank %d -> %s\n", bank, state ? "ON" : "OFF");
    }
}

/*
 * Update the local LCD display
 */
void updateDisplay() {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("V:");
    lcd.print(gridData.voltage, 1);
    lcd.setCursor(9, 0);
    lcd.print("I:");
    lcd.print(gridData.current, 2);
    lcd.setCursor(0, 1);
    lcd.print("PF:");
    lcd.print(gridData.powerFactor, 2);
    lcd.setCursor(9, 1);
    lcd.print(gridData.systemStatus);
}