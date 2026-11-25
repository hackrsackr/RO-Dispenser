/*
  Rui Santos & Sara Santos - Random Nerd Tutorials
  https://RandomNerdTutorials.com/esp32-web-server-beginners-guide/
  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files.
  The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
*/

#include "Arduino.h"
#include "ArduinoJson.h"
#include <WiFi.h>
#include <WebServer.h>
#include <EspMQTTClient.h>

#include <vector>

#include <FlowMeter.hpp>
#include <flow_config.h>
#include <secrets.h>

std::vector<FlowMeter *> _FLOWMETERS;
std::vector<TDSSensor *> _TDSSENSORS;

// Replace with your network credentials
const char *ssid = SECRET_SSID;
const char *password = SECRET_PASS;

// Assign output variables to GPIO pins
const int RELAY_PIN_1 = 16;
const int LED_PIN = 23;

String relay_state = "off";
String ready_state = "Ready";
String flow_state = "0";

#ifndef TARGET_PULSES
#define TARGET_PULSES 6600 // fallback if an environment doesn't set it
#endif
int target_pulses = TARGET_PULSES; // still changeable at runtime via /config

// Fill safety limits (tune after a bench test)
constexpr uint32_t MAX_FILL_MS = 5UL * 60UL * 1000UL; // hard cap per fill
constexpr uint32_t STALL_TIMEOUT_MS = 10UL * 1000UL;  // stop if no pulses this long

String fill_fault = "";  // why the last fill stopped, if not the target
uint32_t fill_started_at = 0;
uint32_t last_pulse_at = 0;
uint32_t last_pulse_count = 0;

// Shared look for every page
const char *PAGE_STYLE = R"CSS(
:root {
  --bg: #f4f6f8; --card: #ffffff; --text: #1d2630; --muted: #5f6b76;
  --go: #2e7d32; --stop: #c62828; --accent: #1565c0;
}
@media (prefers-color-scheme: dark) {
  :root { --bg: #121417; --card: #1e2328; --text: #e8edf2; --muted: #9aa4af; }
}
* { box-sizing: border-box; }
body { margin: 0; font-family: system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
       background: var(--bg); color: var(--text); }
header { padding: 16px; text-align: center; }
h1 { margin: 0; font-size: 1.6rem; }
main { max-width: 720px; margin: 0 auto; padding: 0 16px 24px; }
h2 { font-size: 1.1rem; margin: 24px 0 8px; color: var(--muted); }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 12px; }
.card { background: var(--card); border-radius: 12px; padding: 16px;
        box-shadow: 0 1px 3px rgba(0, 0, 0, .12); margin-bottom: 12px; }
.label { font-size: .8rem; color: var(--muted); text-transform: uppercase; letter-spacing: .05em; }
.value { font-size: 2rem; font-weight: 700; font-variant-numeric: tabular-nums; }
.badge { display: inline-block; padding: 4px 12px; border-radius: 999px; font-weight: 600;
         color: #fff; background: var(--muted); }
.badge.on { background: var(--stop); }
.badge.off { background: var(--go); }
.fault { background: #fff3e0; color: #7a4100; border-radius: 8px; padding: 10px; margin: 12px 0; }
.btn { display: block; width: 100%; padding: 20px; font-size: 1.4rem; font-weight: 700;
       border: 0; border-radius: 12px; color: #fff; background: var(--go); cursor: pointer; }
.btn.stop { background: var(--stop); }
.btn.secondary { background: var(--accent); }
input { font-size: 1.2rem; padding: 10px; border-radius: 8px; width: 100%;
        border: 1px solid var(--muted); background: var(--card); color: var(--text); }
a { color: var(--accent); font-size: 1.1rem; }
)CSS";

String pageStart(const String &title)
{
  String s = "<!DOCTYPE html><html><head>"
             "<meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
             "<link rel=\"icon\" href=\"data:,\">"
             "<title>" + title + "</title><style>";
  s += PAGE_STYLE;
  s += "</style></head><body><header><h1>" + title + "</h1></header><main>";
  return s;
}

String pageEnd()
{
  return "</main></body></html>";
}

// Create a web server object
WebServer server(80);
EspMQTTClient client(ssid, password, _MQTTHOST, "", "", _CLIENTID, _MQTTPORT);

void onConnectionEstablished();
void handleRoot();
void handleData();
void handleConfig();
void handleSetTarget();

// Turn the fill relay on. No HTTP here, so it is safe to call from loop().
void setRelayOn()
{
  FlowMeter *f = _FLOWMETERS.empty() ? nullptr : _FLOWMETERS[0];
  last_pulse_count = f ? static_cast<uint32_t>(f->total_pulse_count) : 0;
  fill_started_at = millis();
  last_pulse_at = fill_started_at;
  fill_fault = "";

  relay_state = "on";
  digitalWrite(RELAY_PIN_1, HIGH);
}

// Turn the fill relay off. No HTTP here, so it is safe to call from loop().
void setRelayOff()
{
  relay_state = "off";
  digitalWrite(RELAY_PIN_1, LOW);
  ready_state = "Not Ready";
}

// Route handlers: change state, then redirect to "/" so the address bar never
// keeps /Relay/on or /Relay/off. A browser reload then re-fetches "/" and cannot
// start another fill.
void activateRelay1()
{
  setRelayOn();
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "");
}

void deactivateRelay1()
{
  setRelayOff();
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "");
}

// Function to handle turning GPIO 27 on
void lockDispenser()
{
  ready_state = "Not Ready";
  handleRoot();
}

// Function to handle turning GPIO 27 off
void resetDispenser()
{
  ready_state = "Ready";
  handleRoot();
}

void handleSetTarget()
{
  if (server.hasArg("target_pulses"))
  {
    int requested = server.arg("target_pulses").toInt();
    if (requested > 0)
    {
      target_pulses = requested;
    }
  }

  // Post/redirect/get: send the browser back to the config page
  server.sendHeader("Location", "/config");
  server.send(303, "text/plain", "");
}

// Function to handle the root URL and show the current states
void handleRoot()
{
  String html = pageStart("RO Dispenser");

  // Relay status and control
  html += "<div class=\"card\"><div class=\"label\">Relay</div>";
  html += (relay_state == "on") ? "<span class=\"badge on\">FILLING</span>"
                                : "<span class=\"badge off\">IDLE</span>";
  if (fill_fault.length() > 0)
  {
    html += "<div class=\"fault\">Last fill stopped: " + fill_fault + "</div>";
  }
  if (relay_state == "off")
  {
    html += "<form method=\"POST\" action=\"/Relay/on\" "
            "onsubmit=\"return confirm('Start a fill?');\">"
            "<button class=\"btn\" type=\"submit\">DO IT LADY!</button></form>";
  }
  else
  {
    html += "<form method=\"POST\" action=\"/Relay/off\">"
            "<button class=\"btn stop\" type=\"submit\">STOP</button></form>";
  }
  html += "</div>";

  // Live readings, filled in by /data every second
  html += R"HTML(
<h2>This fill</h2>
<div class="grid">
  <div class="card"><div class="label">Pulses</div><div class="value" id="pulses">--</div>
    <div class="label">target <span id="target">--</span></div></div>
  <div class="card"><div class="label">Dispensed</div><div class="value"><span id="ml">--</span> mL</div></div>
  <div class="card"><div class="label">Flow</div><div class="value"><span id="lpm">--</span></div>
    <div class="label">L/min</div></div>
</div>
<h2>TDS</h2>
<div class="grid" id="tds"><p class="label">waiting for sensors...</p></div>
<p class="label" id="status"></p>
<p><a href="/config">Config</a></p>
<script>
function poll() {
  fetch('/data', { cache: 'no-store' })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      document.getElementById('pulses').textContent = d.pulses;
      document.getElementById('target').textContent = d.target;
      document.getElementById('ml').textContent = d.ml;
      document.getElementById('lpm').textContent = Number(d.lpm).toFixed(2);
      var html = '';
      d.tds.forEach(function (t) {
        html += '<div class="card"><div class="label">' + t.id + '</div>'
              + '<div class="value">' + Number(t.tds).toFixed(0) + ' ppm</div>'
              + '<div class="label">EC ' + Number(t.ec).toFixed(0) + ' | '
              + Number(t.voltage).toFixed(3) + ' V</div></div>';
      });
      document.getElementById('tds').innerHTML = html;
      document.getElementById('status').textContent = 'updated ' + new Date().toLocaleTimeString();
    })
    .catch(function () {
      document.getElementById('status').textContent = 'lost connection to dispenser';
    });
}
poll();
setInterval(poll, 1000);
</script>
)HTML";

  html += pageEnd();
  server.send(200, "text/html", html);
}

// JSON snapshot of the current readings, polled by the status page
void handleData()
{
  DynamicJsonDocument doc(1024);
  doc["relay"] = relay_state;
  doc["target"] = target_pulses;

  if (!_FLOWMETERS.empty())
  {
    FlowMeter *f = _FLOWMETERS[0];
    doc["pulses"] = f->total_pulse_count;
    doc["ml"] = f->total_milliliters;
    doc["lpm"] = f->flow_rate;
  }

  JsonArray tds = doc.createNestedArray("tds");
  for (auto *t : _TDSSENSORS)
  {
    JsonObject o = tds.createNestedObject();
    o["id"] = t->id.c_str();
    o["tds"] = t->TDS;
    o["ec"] = t->EC;
    o["voltage"] = t->voltage;
    o["adc"] = t->adc;
  }

  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "application/json", out);
}

// Config page: settings that used to live on the home page
void handleConfig()
{
  String html = pageStart("RO Dispenser Config");
  html += "<form method=\"POST\" action=\"/settarget\">";
  html += "<div class=\"card\"><label class=\"label\">Target pulses</label>";
  html += "<input type=\"number\" name=\"target_pulses\" value=\"" + String(target_pulses) + "\" min=\"1\" step=\"1\">";
  html += "</div><p><button class=\"btn secondary\" type=\"submit\">Update</button></p></form>";
  html += "<p><a href=\"/\">Back to dashboard</a></p>";
  html += pageEnd();
  server.send(200, "text/html", html);
}

void setup()
{
  Serial.begin(115200);

  // client.enableDebuggingMessages();
  client.enableOTA();

  for (auto &cfg : FLOW_CFGS)
  {
    FlowMeter *f = new FlowMeter(cfg);
    _FLOWMETERS.push_back(f);
  }

  for (auto &cfg : TDS_CFGS)
  {
    TDSSensor *t = new TDSSensor(cfg);
    _TDSSENSORS.push_back(t);
  }

  // Initialize the output variables as outputs
  pinMode(RELAY_PIN_1, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  // Set outputs to LOW
  digitalWrite(RELAY_PIN_1, LOW);
  digitalWrite(LED_PIN, LOW);

  // Connect to Wi-Fi network
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected.");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  // Set up the web server to handle different routes
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/config", handleConfig);
  server.on("/settarget", HTTP_POST, handleSetTarget);
  server.on("/Relay/on", HTTP_POST, activateRelay1);
  server.on("/Relay/off", HTTP_POST, deactivateRelay1);
  server.on("/Ready", lockDispenser);
  server.on("/Not Ready", resetDispenser);
  // Start the web server
  server.begin();
  Serial.println("HTTP server started");
}

void loop()
{
  // Handle incoming client requests
  server.handleClient();
  client.loop();

  DynamicJsonDocument message(1024);
  message["key"] = _CLIENTID;

  for (auto &flowmeter : _FLOWMETERS)
  {
    flowmeter->run();

    message["data"][flowmeter->id]["Total_Pulses"] = flowmeter->total_pulse_count;
    // message["data"][flowmeter->id]["Flow_rate[LPM]"] = flowmeter->flow_rate;
    // message["data"][flowmeter->id]["Total[L]"] = flowmeter->total_liters;
    message["data"][flowmeter->id]["Total[mL]"] = flowmeter->total_milliliters;

    if (flowmeter->total_pulse_count >= target_pulses)
    {
      setRelayOff();
      flowmeter->total_pulse_count = 0;
    }

    flow_state = String(flowmeter->total_milliliters);
  }

  // Fill safety: stop on stall or on exceeding the max fill time
  if (relay_state == "on")
  {
    uint32_t now = millis();

    if (!_FLOWMETERS.empty())
    {
      uint32_t pulses = static_cast<uint32_t>(_FLOWMETERS[0]->total_pulse_count);
      if (pulses != last_pulse_count)
      {
        last_pulse_count = pulses;
        last_pulse_at = now;
      }
    }

    if (now - last_pulse_at > STALL_TIMEOUT_MS)
    {
      fill_fault = "no flow (stalled)";
      setRelayOff();
    }
    else if (now - fill_started_at > MAX_FILL_MS)
    {
      fill_fault = "max fill time reached";
      setRelayOff();
    }
  }

  for (auto &tdssensor : _TDSSENSORS)
  {
    tdssensor->readTDSSensor();
    message["data"][tdssensor->id]["ADC"] = tdssensor->adc;
    message["data"][tdssensor->id]["Voltage"] = tdssensor->voltage;
    message["data"][tdssensor->id]["EC"] = tdssensor->EC;
    message["data"][tdssensor->id]["TDS"] = tdssensor->TDS;
  }

  // message["data"]["memory"]["Output_memory_size"] = message.memoryUsage();

  serializeJsonPretty(message["data"], Serial);
  // serializeJson(message["data"], Serial);
  Serial.println("");

  static uint32_t lastPublish = 0;
  if (millis() - lastPublish >= _PUBLISH_INTERVAL_MS)
  {
    lastPublish = millis();
    String payload;
    serializeJson(message, payload);
    client.publish(_PUBTOPIC, payload); // returns false if MQTT is down; retried next interval
  }

  delay(1000);
}

// This function is called once everything is connected (Wifi and MQTT)
// WARNING : YOU MUST IMPLEMENT IT IF YOU USE EspMQTTClient
void onConnectionEstablished()
{
}