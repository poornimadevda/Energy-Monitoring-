
#define BLYNK_TEMPLATE_ID   "TMPL3nb-h5V6g"
#define BLYNK_TEMPLATE_NAME "Energy Monitoring"
#define BLYNK_AUTH_TOKEN    "DlXBosScNYbMmhHXr4POuWG5bMvykL-H"
#define BLYNK_PRINT         Serial

#include <Arduino.h>

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <PZEM004Tv30.h>
#include <BlynkSimpleEsp32.h>

// ── WiFi 
const char* WIFI_SSID     = "Poornima";     
const char* WIFI_PASSWORD = "Poornima@7";   

// ── Pins 
#define PZEM_RX_PIN   16    // PZEM TX → GPIO16
#define PZEM_TX_PIN   17    // PZEM RX → GPIO17
#define RELAY1_PIN    26    // Relay IN1
#define RELAY2_PIN    27    // Relay IN2
#define BUZZER_PIN    25    // Active buzzer +


// ── Thresholds 
#define OV_THRESHOLD   250.0f   // Overvoltage  (V)
#define UV_THRESHOLD   180.0f   // Undervoltage (V)
#define OC_THRESHOLD    90.0f   // Overcurrent  (A)
#define OP_THRESHOLD  5000.0f   // Overpower    (W)
#define POLL_MS        2000     // Sensor poll interval (ms)

// ── Objects 
PZEM004Tv30    pzem(Serial2, PZEM_RX_PIN, PZEM_TX_PIN);
AsyncWebServer server(80);
BlynkTimer     blynkTimer;

// ── State
bool relay1On    = false;
bool relay2On    = false;
bool buzzerOn    = false;
bool buzzerMuted = false;

struct Readings {
  float  voltage   = NAN;
  float  current   = NAN;
  float  power     = NAN;
  float  energy    = NAN;
  float  frequency = NAN;
  float  pf        = NAN;
  bool   fault     = false;
  bool   ovAlarm   = false;
  String faultMsg  = "";
} latest;

// Buzzer
void buzzerSet(bool on) {
  buzzerOn = on;
  digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
}

// Relay helpers — active LOW module

void setRelay(uint8_t pin, bool on) {
  digitalWrite(pin, on ? LOW : HIGH);
}

void relay1Set(bool on) {
  relay1On = on;
  setRelay(RELAY1_PIN, on);
  Blynk.virtualWrite(V7, on ? 1 : 0);
  Serial.printf("[RELAY1] %s\n", on ? "ON" : "OFF");
}

void relay2Set(bool on) {
  relay2On = on;
  setRelay(RELAY2_PIN, on);
  Blynk.virtualWrite(V8, on ? 1 : 0);
  Serial.printf("[RELAY2] %s\n", on ? "ON" : "OFF");
}


// Protection check — runs every poll

void checkProtection() {
  latest.fault    = false;
  latest.ovAlarm  = false;
  latest.faultMsg = "";

  if (!isnan(latest.voltage)) {
    if (latest.voltage > OV_THRESHOLD) {
      latest.fault    = true;
      latest.ovAlarm  = true;
      latest.faultMsg = "OVERVOLTAGE: " + String(latest.voltage, 1) + " V";
    } else if (latest.voltage < UV_THRESHOLD) {
      latest.fault    = true;
      latest.faultMsg = "UNDERVOLTAGE: " + String(latest.voltage, 1) + " V";
    }
  }
  if (!isnan(latest.current) && latest.current > OC_THRESHOLD) {
    latest.fault    = true;
    latest.faultMsg = "OVERCURRENT: " + String(latest.current, 2) + " A";
  }
  if (!isnan(latest.power) && latest.power > OP_THRESHOLD) {
    latest.fault    = true;
    latest.faultMsg = "OVERPOWER: " + String(latest.power, 1) + " W";
  }

  if (latest.fault) {
    relay1Set(false);
    relay2Set(false);
    Blynk.virtualWrite(V6, 255);
    Blynk.virtualWrite(V9, latest.faultMsg);
    Blynk.logEvent("fault_alert", latest.faultMsg);
    Serial.println("[FAULT] " + latest.faultMsg);
  } else {
    Blynk.virtualWrite(V6, 0);
    Blynk.virtualWrite(V9, "System Normal");
  }

  if (latest.ovAlarm && !buzzerMuted) {
    buzzerSet(true);
  } else {
    buzzerSet(false);
  }
}


// Send sensor data to Blynk every 2 seconds
void sendToBlynk() {
  if (!isnan(latest.voltage))   Blynk.virtualWrite(V0, latest.voltage);
  if (!isnan(latest.current))   Blynk.virtualWrite(V1, latest.current);
  if (!isnan(latest.power))     Blynk.virtualWrite(V2, latest.power);
  if (!isnan(latest.energy))    Blynk.virtualWrite(V3, latest.energy);
  if (!isnan(latest.frequency)) Blynk.virtualWrite(V4, latest.frequency);
  if (!isnan(latest.pf))        Blynk.virtualWrite(V5, latest.pf);
}


// Blynk write handlers


// Relay 1 toggle from Blynk app
BLYNK_WRITE(V7) {
  int val = param.asInt();
  if (!latest.fault) {
    relay1Set(val == 1);
  } else {
    Blynk.virtualWrite(V7, 0);
    Serial.println("[BLYNK] Relay1 blocked — fault active");
  }
}

// Relay 2 toggle from Blynk app
BLYNK_WRITE(V8) {
  int val = param.asInt();
  if (!latest.fault) {
    relay2Set(val == 1);
  } else {
    Blynk.virtualWrite(V8, 0);
    Serial.println("[BLYNK] Relay2 blocked — fault active");
  }
}

// Mute buzzer from Blynk app
BLYNK_WRITE(V10) {
  int val = param.asInt();
  if (val == 1) {
    buzzerMuted = true;
    buzzerSet(false);
    Serial.println("[BLYNK] Buzzer muted");
  } else {
    buzzerMuted = false;
    Serial.println("[BLYNK] Buzzer unmuted");
  }
}

// Reset kWh counter from Blynk app
BLYNK_WRITE(V11) {
  int val = param.asInt();
  if (val == 1) {
    bool ok = pzem.resetEnergy();
    Serial.println(ok ? "[PZEM] Energy reset OK"
                      : "[PZEM] Energy reset FAILED");
    Blynk.virtualWrite(V11, 0);  // auto-release button
  }
}

// HTML Web Dashboard

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Energy Monitor</title>
<style>
  :root{
    --bg:#0d1117;--card:#161b22;--border:#30363d;
    --text:#e6edf3;--muted:#8b949e;--accent:#58a6ff;
    --green:#3fb950;--red:#f85149;--amber:#d29922;
  }
  *{box-sizing:border-box;margin:0;padding:0;}
  body{background:var(--bg);color:var(--text);
       font-family:monospace;padding:24px;}
  h1{font-size:1.1rem;letter-spacing:2px;
     color:var(--accent);margin-bottom:22px;}
  .grid{display:grid;
        grid-template-columns:repeat(auto-fill,minmax(155px,1fr));
        gap:12px;margin-bottom:20px;}
  .card{background:var(--card);border:1px solid var(--border);
        border-radius:8px;padding:16px;}
  .card.alarm{border-color:var(--amber);
              animation:pulse 1s ease-in-out infinite;}
  @keyframes pulse{
    0%,100%{box-shadow:0 0 0 0 rgba(210,153,34,0)}
    50%{box-shadow:0 0 8px 2px rgba(210,153,34,.4)}
  }
  .label{font-size:0.65rem;color:var(--muted);
         letter-spacing:1px;margin-bottom:8px;}
  .val{font-size:1.5rem;font-weight:bold;}
  .unit{font-size:0.7rem;color:var(--muted);margin-left:4px;}
  .relays{display:flex;gap:12px;margin-bottom:16px;flex-wrap:wrap;}
  .rc{background:var(--card);border:1px solid var(--border);
      border-radius:8px;padding:16px;flex:1;min-width:190px;}
  .rl{font-size:0.65rem;color:var(--muted);
      letter-spacing:1px;margin-bottom:10px;}
  .dot{display:inline-block;width:9px;height:9px;
       border-radius:50%;margin-right:7px;}
  .on{background:var(--green);box-shadow:0 0 5px var(--green);}
  .off{background:var(--muted);}
  .btn{padding:7px 16px;border:none;border-radius:5px;
       font-family:monospace;font-size:0.8rem;
       cursor:pointer;margin-top:10px;}
  .btn-on{background:var(--green);color:#000;}
  .btn-off{background:var(--red);color:#fff;}
  .btn-mute{background:var(--amber);color:#000;}
  .fault{background:rgba(248,81,73,.1);
         border:1px solid var(--red);border-radius:8px;
         padding:12px 16px;margin-bottom:16px;
         color:var(--red);display:none;}
  .fault.show{display:block;}
  .alarm-bar{background:rgba(210,153,34,.1);
             border:1px solid var(--amber);border-radius:8px;
             padding:12px 16px;margin-bottom:16px;
             color:var(--amber);display:none;
             align-items:center;
             justify-content:space-between;gap:12px;}
  .alarm-bar.show{display:flex;}
  .note{font-size:0.7rem;color:var(--muted);margin-bottom:4px;}
  .ts{font-size:0.65rem;color:var(--muted);
      margin-top:12px;text-align:right;}
  .reset{padding:7px 16px;background:none;
         border:1px solid var(--border);color:var(--muted);
         border-radius:5px;font-family:monospace;
         font-size:0.75rem;cursor:pointer;margin-top:14px;}
  .reset:hover{border-color:var(--accent);color:var(--accent);}
  .status-bar{font-size:0.7rem;color:var(--muted);
              margin-bottom:16px;padding:8px 12px;
              background:var(--card);border-radius:6px;
              border:1px solid var(--border);}
  .status-bar span{color:var(--green);}
</style>
</head>
<body>
<h1>⚡ ENERGY MONITOR</h1>

<!-- Connection status bar -->
<div class="status-bar">
  Web dashboard active —
  <span>connected</span> |
  Blynk app running simultaneously
</div>

<!-- Overvoltage alarm bar -->
<div id="alarm-bar" class="alarm-bar">
  <span id="alarm-msg">⚠ OVERVOLTAGE ALARM</span>
  <button class="btn btn-mute" onclick="muteBuzzer()">
    MUTE BUZZER
  </button>
</div>

<!-- General fault bar -->
<div id="fault" class="fault"></div>

<!-- Sensor cards -->
<div class="grid">
  <div class="card" id="card-v">
    <div class="label">VOLTAGE</div>
    <span class="val" id="v">—</span>
    <span class="unit">V</span>
  </div>
  <div class="card">
    <div class="label">CURRENT</div>
    <span class="val" id="i">—</span>
    <span class="unit">A</span>
  </div>
  <div class="card">
    <div class="label">POWER</div>
    <span class="val" id="p">—</span>
    <span class="unit">W</span>
  </div>
  <div class="card">
    <div class="label">ENERGY</div>
    <span class="val" id="e">—</span>
    <span class="unit">kWh</span>
  </div>
  <div class="card">
    <div class="label">FREQUENCY</div>
    <span class="val" id="f">—</span>
    <span class="unit">Hz</span>
  </div>
  <div class="card">
    <div class="label">POWER FACTOR</div>
    <span class="val" id="pf">—</span>
  </div>
</div>

<!-- Relay controls -->
<div class="relays">
  <div class="rc">
    <div class="rl">RELAY 1 — MAIN LOAD</div>
    <span class="dot off" id="d1"></span>
    <span id="s1">OFF</span><br>
    <button class="btn btn-on"  onclick="rc(1,'on')">ON</button>
    &nbsp;
    <button class="btn btn-off" onclick="rc(1,'off')">OFF</button>
  </div>
  <div class="rc">
    <div class="rl">RELAY 2 — SPARE</div>
    <span class="dot off" id="d2"></span>
    <span id="s2">OFF</span><br>
    <button class="btn btn-on"  onclick="rc(2,'on')">ON</button>
    &nbsp;
    <button class="btn btn-off" onclick="rc(2,'off')">OFF</button>
  </div>
</div>

<div class="note">
  Protection thresholds —
  OV &gt; 250V · UV &lt; 180V · OC &gt; 90A · OP &gt; 5000W
</div>
<div class="note">
  Fault trips both relays automatically.
  Power-cycle ESP32 to reset after fixing fault.
</div>

<button class="reset" onclick="resetEnergy()">
  Reset kWh counter
</button>
<div class="ts" id="ts">—</div>

<script>
  function rc(n, c) {
    fetch('/relay?r=' + n + '&cmd=' + c)
      .then(r => r.json())
      .then(d => ru(d));
  }

  function ru(d) {
    document.getElementById('d1').className =
      'dot ' + (d.relay1 ? 'on' : 'off');
    document.getElementById('s1').textContent =
      d.relay1 ? 'ON' : 'OFF';
    document.getElementById('d2').className =
      'dot ' + (d.relay2 ? 'on' : 'off');
    document.getElementById('s2').textContent =
      d.relay2 ? 'ON' : 'OFF';
  }

  function muteBuzzer() {
    fetch('/mute-buzzer').then(() => {
      document.getElementById('alarm-bar')
              .classList.remove('show');
    });
  }

  function fmt(v, d) {
    return v == null ? '—' : Number(v).toFixed(d);
  }

  function poll() {
    fetch('/data')
      .then(r => r.json())
      .then(d => {
        document.getElementById('v').textContent  = fmt(d.voltage,   1);
        document.getElementById('i').textContent  = fmt(d.current,   3);
        document.getElementById('p').textContent  = fmt(d.power,     1);
        document.getElementById('e').textContent  = fmt(d.energy,    3);
        document.getElementById('f').textContent  = fmt(d.frequency, 1);
        document.getElementById('pf').textContent = fmt(d.pf,        2);
        document.getElementById('ts').textContent =
          'updated: ' + new Date().toLocaleTimeString();

        // Overvoltage alarm
        const cardV = document.getElementById('card-v');
        const ab    = document.getElementById('alarm-bar');
        if (d.ovAlarm) {
          cardV.classList.add('alarm');
          ab.classList.add('show');
          document.getElementById('alarm-msg').textContent =
            '⚠ OVERVOLTAGE — ' + fmt(d.voltage, 1) +
            ' V (limit: 250 V)';
        } else {
          cardV.classList.remove('alarm');
          ab.classList.remove('show');
        }

        // General fault
        const fb = document.getElementById('fault');
        if (d.fault && !d.ovAlarm) {
          fb.textContent = '⚠ FAULT — ' + d.faultMsg;
          fb.classList.add('show');
        } else {
          fb.classList.remove('show');
        }

        ru(d);
      })
      .catch(() => {});
  }

  function resetEnergy() {
    fetch('/reset-energy')
      .then(r => r.text())
      .then(t => alert(t));
  }

  poll();
  setInterval(poll, 2500);
</script>
</body>
</html>
)rawliteral";


// Setup

void setup() {
  Serial.begin(115200);
  delay(400);

  // Relay and buzzer pins
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  setRelay(RELAY1_PIN, false);  // relays OFF at boot
  setRelay(RELAY2_PIN, false);
  buzzerSet(false);

  // Connect via Blynk (handles WiFi internally)
  Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
  Blynk.begin(BLYNK_AUTH_TOKEN, WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connected! IP: %s\n",
                WiFi.localIP().toString().c_str());

  // ── Web server routes

  // Main dashboard page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", INDEX_HTML);
  });

  // JSON sensor data endpoint
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest* req) {
    StaticJsonDocument<320> doc;
    if (!isnan(latest.voltage))
      doc["voltage"]   = latest.voltage;
    else
      doc["voltage"]   = nullptr;
    if (!isnan(latest.current))
      doc["current"]   = latest.current;
    else
      doc["current"]   = nullptr;
    if (!isnan(latest.power))
      doc["power"]     = latest.power;
    else
      doc["power"]     = nullptr;
    if (!isnan(latest.energy))
      doc["energy"]    = latest.energy;
    else
      doc["energy"]    = nullptr;
    if (!isnan(latest.frequency))
      doc["frequency"] = latest.frequency;
    else
      doc["frequency"] = nullptr;
    if (!isnan(latest.pf))
      doc["pf"]        = latest.pf;
    else
      doc["pf"]        = nullptr;

    doc["relay1"]   = relay1On;
    doc["relay2"]   = relay2On;
    doc["fault"]    = latest.fault;
    doc["ovAlarm"]  = latest.ovAlarm;
    doc["faultMsg"] = latest.faultMsg;
    doc["buzzer"]   = buzzerOn;

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // Relay control: /relay?r=1&cmd=on
  server.on("/relay", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!latest.fault &&
        req->hasParam("r") &&
        req->hasParam("cmd")) {
      int  r  = req->getParam("r")->value().toInt();
      bool on = req->getParam("cmd")->value() == "on";
      if (r == 1) relay1Set(on);
      if (r == 2) relay2Set(on);
    }
    StaticJsonDocument<64> doc;
    doc["relay1"] = relay1On;
    doc["relay2"] = relay2On;
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // Mute buzzer from web dashboard
  server.on("/mute-buzzer", HTTP_GET,
    [](AsyncWebServerRequest* req) {
      buzzerMuted = true;
      buzzerSet(false);
      Blynk.virtualWrite(V10, 1);  // sync to Blynk app
      Serial.println("[BUZZER] Muted from web dashboard");
      req->send(200, "text/plain", "Muted");
    });

  // Reset energy counter from web dashboard
  server.on("/reset-energy", HTTP_GET,
    [](AsyncWebServerRequest* req) {
      bool ok = pzem.resetEnergy();
      Serial.println(ok ? "[PZEM] Energy reset OK"
                        : "[PZEM] Energy reset FAILED");
      req->send(200, "text/plain",
        ok ? "Energy reset OK" : "Reset failed");
    });

  server.begin();

  // Blynk timer — push data every 2 seconds
  blynkTimer.setInterval(2000L, sendToBlynk);

  Serial.println("=============================");
  Serial.println(" System ready");
  Serial.println("=============================");
  Serial.printf("Web dashboard : http://%s\n",
                WiFi.localIP().toString().c_str());
  Serial.println("Blynk app     : open Blynk on phone");
  Serial.println("Serial monitor: 115200 baud");
  Serial.println("=============================");
}


// Loop

void loop() {
  Blynk.run();       // keeps Blynk alive
  blynkTimer.run();  // fires sendToBlynk() every 2 sec

  static unsigned long lastPoll = 0;
  if (millis() - lastPoll < POLL_MS) return;
  lastPoll = millis();

  // Read PZEM sensor
  latest.voltage   = pzem.voltage();
  latest.current   = pzem.current();
  latest.power     = pzem.power();
  latest.energy    = pzem.energy();
  latest.frequency = pzem.frequency();
  latest.pf        = pzem.pf();

  // Print to Serial Monitor
  Serial.printf(
    "[PZEM] V=%.1f V  I=%.3f A  P=%.1f W  "
    "E=%.3f kWh  F=%.1f Hz  PF=%.2f\n",
    latest.voltage,   latest.current,
    latest.power,     latest.energy,
    latest.frequency, latest.pf);

  // Auto unmute buzzer when voltage returns to safe range
  if (!isnan(latest.voltage) &&
      latest.voltage <= OV_THRESHOLD) {
    buzzerMuted = false;
  }

  checkProtection();
}
