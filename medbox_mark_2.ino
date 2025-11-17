/*************************************************************
   MEDBOX REMINDER – ESP32 (NVS, UI, APIs, RTC, CLOUD SYNC)
   10 LEDs + BUZZER + DS3231 + MODERN UI + 12-HOUR TIME
   - Unique device ID
   - Cloud POST + Cloud GET polling (no port forwarding)
   - Short HTTP timeouts to avoid slowdowns
   - SoftAP config portal (WiFi + Scan)
   - mDNS: medbox.local & medboxconfig.local
   - NEW: /api/wifi JSON endpoint for app-based config
*************************************************************/

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <RTClib.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>     // for .local names

// ---------------------- HARD-CODED WIFI (DEFAULT) ----------------------
const char* WIFI_SSID_DEFAULT = "Coolkaru info solutions";
const char* WIFI_PASS_DEFAULT = "Coolnew@123";

// ---------------------- API KEY ------------------------------
#define API_KEY "MEDBOX2025"

// ---------------------- PIN DEFINITIONS -----------------------
#define SETUP_PIN   14
#define BUZZER_PIN  15

int LED_PINS[10] = {4, 16, 17, 18, 19, 23, 25, 26, 27, 32};

// ---------------------- OBJECTS ------------------------------
WebServer server(80);
Preferences prefs;
RTC_DS3231 rtc;

// ---------------------- WIFI CONFIG (NVS) --------------------
String wifiSsid;   // used for STA connection
String wifiPass;

// ---------------------- MEDICINES ----------------------------
const int MAX_MEDS = 10;

struct Medicine {
  String name;
  int qty;
  int hour;     // 24-hour stored
  int minute;
  int led;      // 1-10
  bool enabled;
};

Medicine meds[MAX_MEDS];
int medCount = 0;

int alarmDuration = 10;  // seconds

// ---------------------- CLOUD SYNC CONFIG ---------------------
String globalApiUrl = "";   // POST URL
String globalGetUrl = "";   // GET URL for commands
bool onlineSyncEnabled = false;

// ---------------------- DEVICE ID -----------------------------
String deviceId = "maxpark03";

// ---------------------- CLOUD POLLING -------------------------
unsigned long lastPoll = 0;
const unsigned long POLL_INTERVAL_MS = 120000UL;  // 2 minutes

/*************************************************************
                      API KEY HELPERS
*************************************************************/
bool checkAPIKey() {
  // 1) Header
  if (server.hasHeader("API_KEY")) {
    String key = server.header("API_KEY");
    if (key == API_KEY) return true;
  }
  // 2) Query ?api_key= or ?key=
  if (server.hasArg("api_key")) {
    if (server.arg("api_key") == API_KEY) return true;
  }
  if (server.hasArg("key")) {
    if (server.arg("key") == API_KEY) return true;
  }
  return false;
}

void rejectUnauthorized() {
  server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
}

/*************************************************************
                      DEVICE ID INIT
*************************************************************/
void initDeviceId() {
  prefs.putString("deviceId", deviceId);
  Serial.print("Device ID: ");
  Serial.println(deviceId);
}

/*************************************************************
                      WIFI CONFIG HELPERS
*************************************************************/
void loadWifiConfig() {
  // If nothing stored, fall back to hardcoded defaults
  wifiSsid = prefs.getString("wifi_ssid", WIFI_SSID_DEFAULT);
  wifiPass = prefs.getString("wifi_pass", WIFI_PASS_DEFAULT);

  Serial.print("Loaded WiFi SSID from NVS (or default): ");
  Serial.println(wifiSsid);
}

void saveWifiConfig(const String &s, const String &p) {
  prefs.putString("wifi_ssid", s);
  prefs.putString("wifi_pass", p);
  wifiSsid = s;
  wifiPass = p;

  Serial.println("WiFi credentials saved to NVS:");
  Serial.print("SSID: ");
  Serial.println(wifiSsid);
}

/*************************************************************
                  CLOUD SYNC (NO QUEUE, NO RETRY)
*************************************************************/
void maybeSyncToCloud() {
  if (!onlineSyncEnabled) {
    Serial.println("Cloud sync: disabled.");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Cloud sync skipped: WiFi not connected.");
    return;
  }
  if (globalApiUrl.length() == 0) {
    Serial.println("Cloud sync skipped: POST URL is empty.");
    return;
  }

  DynamicJsonDocument doc(4096);
  doc["deviceId"] = deviceId;
  doc["count"] = medCount;
  JsonArray arr = doc.createNestedArray("meds");

  for (int i = 0; i < medCount; i++) {
    JsonObject o = arr.createNestedObject();
    o["id"] = i;
    o["name"] = meds[i].name;
    o["qty"] = meds[i].qty;
    o["hour"] = meds[i].hour;
    o["minute"] = meds[i].minute;
    o["led"] = meds[i].led;
    o["enabled"] = meds[i].enabled;
  }

  String payload;
  serializeJson(doc, payload);

  Serial.print("Cloud sync: POST to ");
  Serial.println(globalApiUrl);
  Serial.print("Payload: ");
  Serial.println(payload);

  HTTPClient http;
  http.begin(globalApiUrl);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(2000);  // 2 seconds max

  int code = http.POST(payload);
  Serial.print("Cloud response code: ");
  Serial.println(code);

  http.end();
}

/*************************************************************
                      NVS HELPERS
*************************************************************/

// Save medicines to NVS as "name|qty|hour|min|led|enabled"
void saveMeds() {
  prefs.putInt("medCount", medCount);

  for (int i = 0; i < medCount; i++) {
    String key = "med" + String(i);
    String data = meds[i].name + "|" +
                  String(meds[i].qty) + "|" +
                  String(meds[i].hour) + "|" +
                  String(meds[i].minute) + "|" +
                  String(meds[i].led) + "|" +
                  String(meds[i].enabled ? 1 : 0);

    prefs.putString(key.c_str(), data);
  }

  // Clear old slots beyond medCount
  for (int i = medCount; i < MAX_MEDS; i++) {
    String key = "med" + String(i);
    prefs.remove(key.c_str());
  }

  Serial.println("Medicines saved to NVS.");
  maybeSyncToCloud();
}

void loadMeds() {
  medCount = prefs.getInt("medCount", 0);
  if (medCount < 0 || medCount > MAX_MEDS) medCount = 0;

  for (int i = 0; i < medCount; i++) {
    String key = "med" + String(i);
    String data = prefs.getString(key.c_str(), "");

    if (data == "") {
      medCount = i;
      break;
    }

    int pipeCount = 0;
    for (int k = 0; k < data.length(); k++) {
      if (data[k] == '|') pipeCount++;
    }

    if (pipeCount < 4) {  // corrupted
      medCount = i;
      break;
    }

    int p1 = data.indexOf('|');
    int p2 = data.indexOf('|', p1 + 1);
    int p3 = data.indexOf('|', p2 + 1);
    int p4 = data.indexOf('|', p3 + 1);
    int p5 = -1;

    meds[i].name   = data.substring(0, p1);
    meds[i].qty    = data.substring(p1 + 1, p2).toInt();
    meds[i].hour   = data.substring(p2 + 1, p3).toInt();
    meds[i].minute = data.substring(p3 + 1, p4).toInt();

    if (pipeCount == 4) {
      meds[i].led = data.substring(p4 + 1).toInt();
      meds[i].enabled = true;
    } else {
      p5 = data.indexOf('|', p4 + 1);
      if (p5 < 0) {
        meds[i].led = data.substring(p4 + 1).toInt();
        meds[i].enabled = true;
      } else {
        meds[i].led = data.substring(p4 + 1, p5).toInt();
        int en = data.substring(p5 + 1).toInt();
        meds[i].enabled = (en != 0);
      }
    }

    if (meds[i].led < 1 || meds[i].led > 10) meds[i].led = 1;
  }

  Serial.print("Loaded medicines from NVS: ");
  Serial.println(medCount);
}

/*************************************************************
                    NTP → RTC SYNC
*************************************************************/
void syncTimeNTP() {
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");

  struct tm t;
  if (!getLocalTime(&t)) {
    Serial.println("NTP Sync Failed!");
    return;
  }

  rtc.adjust(DateTime(
    t.tm_year + 1900,
    t.tm_mon + 1,
    t.tm_mday,
    t.tm_hour,
    t.tm_min,
    t.tm_sec
  ));

  Serial.println("RTC Time Synced Successfully from NTP!");
}

/*************************************************************
                HTML HEADER (TABS + LIVE CLOCK)
*************************************************************/
String headerHTML(String active = "home") {
  String h = R"====(
  <html>
  <head>
    <title>MedBox Reminder</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link href='https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css' rel='stylesheet'>
    <script>
      function updateClock(){
        var now=new Date();
        var h=now.getHours();
        var m=now.getMinutes();
        var s=now.getSeconds();
        var ap=h>=12?"PM":"AM";
        h=h%12; if(h==0) h=12;
        m=m<10?"0"+m:m;
        s=s<10?"0"+s:s;
        var c=document.getElementById("clock");
        if(c) c.innerHTML=h+":"+m+":"+s+" "+ap;
      }
      setInterval(updateClock,1000);
    </script>
  </head>

  <body onload='updateClock()'>
  <div class='container mt-3'>
    <center>
      <h2>MedBox Reminder</h2>
      <div id='clock' style='font-size:22px;font-weight:bold;'></div>
    </center>
    <hr>
    <ul class='nav nav-tabs'>
  )====";

  h += "<li class='nav-item'><a class='nav-link " + String(active=="home"?"active":"") + "' href='/'>Home</a></li>";
  h += "<li class='nav-item'><a class='nav-link " + String(active=="add"?"active":"") + "' href='/add'>Add Medicine</a></li>";
  h += "<li class='nav-item'><a class='nav-link " + String(active=="settings"?"active":"") + "' href='/settings'>Settings</a></li>";

  h += "</ul><br>";
  return h;
}

String footerHTML() { return "</div></body></html>"; }

/*************************************************************
                          HOME PAGE
*************************************************************/
void handleHome() {
  String page = headerHTML("home");

  if (medCount == 0) {
    page += "<div class='alert alert-info'>No medicines added yet.</div>";
  } else {
    for (int i = 0; i < medCount; i++) {
      int hr = meds[i].hour;
      String ap = (hr >= 12) ? "PM" : "AM";
      hr = hr % 12; if (hr == 0) hr = 12;

      String status = meds[i].enabled ? "Enabled" : "Disabled";
      String badgeClass = meds[i].enabled ? "badge bg-success" : "badge bg-secondary";

      page += "<div class='card mb-2'><div class='card-body'>";
      page += "<div class='d-flex justify-content-between align-items-center'>";
      page += "<h5><b>" + meds[i].name + "</b></h5>";
      page += "<span class='" + badgeClass + "'>" + status + "</span>";
      page += "</div>";

      page += "Qty: <b>" + String(meds[i].qty) + "</b><br>";
      page += "Time: <b>" + String(hr) + ":" +
              (meds[i].minute < 10 ? "0" : "") +
              String(meds[i].minute) + " " + ap + "</b><br>";
      page += "LED Slot: <b>" + String(meds[i].led) + "</b><br><br>";

      page += "<a href='/toggle?i=" + String(i) + "' class='btn btn-sm " +
              String(meds[i].enabled ? "btn-warning" : "btn-success") + "'>";
      page += meds[i].enabled ? "Disable" : "Enable";
      page += "</a> ";

      page += "<a href='/edit?i=" + String(i) + "' class='btn btn-sm btn-primary'>Edit</a> ";
      page += "<a href='/delete?i=" + String(i) + "' class='btn btn-sm btn-danger'>Delete</a>";

      page += "</div></div>";
    }
  }

  page += footerHTML();
  server.send(200, "text/html", page);
}

/*************************************************************
                       ADD MEDICINE
*************************************************************/
void handleAdd() {
  if (server.method() == HTTP_POST) {
    if (medCount >= MAX_MEDS) {
      String p = headerHTML("add");
      p += "<div class='alert alert-danger'>Maximum 10 medicines allowed (1 per LED slot).</div>";
      p += footerHTML();
      server.send(200, "text/html", p);
      return;
    }

    Medicine m;
    m.name = server.arg("name");
    m.qty  = server.arg("qty").toInt();

    int hr  = server.arg("hour").toInt();
    int min = server.arg("minute").toInt();
    String ap = server.arg("ampm");

    if (ap == "PM" && hr < 12) hr += 12;
    if (ap == "AM" && hr == 12) hr = 0;

    m.hour   = hr;
    m.minute = min;
    m.led    = server.arg("led").toInt();
    if (m.led < 1) m.led = 1;
    if (m.led > 10) m.led = 10;
    m.enabled = true;

    meds[medCount++] = m;
    saveMeds();

    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  String page = headerHTML("add");

  page += R"====(
  <div class='card'>
  <div class='card-header'>Add a Medicine</div>
  <div class='card-body'>
  <form method='POST'>

    <label>Name</label>
    <input class='form-control' name='name' required><br>

    <label>Quantity</label>
    <input type='number' class='form-control' name='qty' required><br>

    <label>Time (AM/PM)</label>
    <div class='row'>
      <div class='col-4'>
        <select name='hour' class='form-control'>
  )====";

  for (int h = 1; h <= 12; h++)
    page += "<option>" + String(h) + "</option>";

  page += R"====(
        </select>
      </div>

      <div class='col-4'>
        <select name='minute' class='form-control'>
  )====";

  for (int m = 0; m < 60; m++)
    page += "<option>" + String(m) + "</option>";

  page += R"====(
        </select>
      </div>

      <div class='col-4'>
        <select name='ampm' class='form-control'>
          <option>AM</option>
          <option>PM</option>
        </select>
      </div>
    </div><br>

    <label>LED Slot (1–10)</label>
    <input type='number' class='form-control' name='led' required><br>

    <button class='btn btn-success'>Save</button>
  </form>
  </div></div>
  )====";

  page += footerHTML();
  server.send(200, "text/html", page);
}

/*************************************************************
                       EDIT MEDICINE
*************************************************************/
void handleEdit() {
  // POST: apply edit
  if (server.method() == HTTP_POST) {
    int idx = server.arg("index").toInt();
    if (idx < 0 || idx >= medCount) {
      server.sendHeader("Location", "/");
      server.send(303);
      return;
    }

    Medicine &m = meds[idx];

    m.name = server.arg("name");
    m.qty  = server.arg("qty").toInt();

    int hr  = server.arg("hour").toInt();
    int min = server.arg("minute").toInt();
    String ap = server.arg("ampm");

    if (ap == "PM" && hr < 12) hr += 12;
    if (ap == "AM" && hr == 12) hr = 0;

    m.hour   = hr;
    m.minute = min;
    m.led    = server.arg("led").toInt();
    if (m.led < 1) m.led = 1;
    if (m.led > 10) m.led = 10;

    saveMeds();

    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  // GET: show edit form
  if (!server.hasArg("i")) {
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  int idx = server.arg("i").toInt();
  if (idx < 0 || idx >= medCount) {
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  Medicine &m = meds[idx];

  int hr24 = m.hour;
  String ap = (hr24 >= 12) ? "PM" : "AM";
  int hr12 = hr24 % 12;
  if (hr12 == 0) hr12 = 12;

  String page = headerHTML("home");

  page += "<div class='card'><div class='card-header'>Edit Medicine</div><div class='card-body'>";
  page += "<form method='POST'>";

  page += "<input type='hidden' name='index' value='" + String(idx) + "'>";

  page += "Name:<input class='form-control' name='name' value='" + m.name + "' required><br>";

  page += "Quantity:<input type='number' class='form-control' name='qty' value='" + String(m.qty) + "' required><br>";

  page += "Time (AM/PM):<div class='row'>";

  // Hour
  page += "<div class='col-4'><select class='form-control' name='hour'>";
  for (int h = 1; h <= 12; h++) {
    page += "<option";
    if (h == hr12) page += " selected";
    page += ">" + String(h) + "</option>";
  }
  page += "</select></div>";

  // Minute
  page += "<div class='col-4'><select class='form-control' name='minute'>";
  for (int mm = 0; mm < 60; mm++) {
    page += "<option";
    if (mm == m.minute) page += " selected";
    page += ">" + String(mm) + "</option>";
  }
  page += "</select></div>";

  // AM/PM
  page += "<div class='col-4'><select class='form-control' name='ampm'>";
  page += "<option";
  if (ap == "AM") page += " selected";
  page += ">AM</option>";
  page += "<option";
  if (ap == "PM") page += " selected";
  page += ">PM</option>";
  page += "</select></div>";

  page += "</div><br>";

  // LED
  page += "LED Slot (1–10):<input type='number' class='form-control' name='led' value='" + String(m.led) + "' required><br>";

  page += "<button class='btn btn-primary'>Update</button>";
  page += " <a href='/' class='btn btn-secondary'>Cancel</a>";

  page += "</form></div></div>";

  page += footerHTML();

  server.send(200, "text/html", page);
}

/*************************************************************
                      TOGGLE ENABLE/DISABLE
*************************************************************/
void handleToggle() {
  if (!server.hasArg("i")) {
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  int idx = server.arg("i").toInt();
  if (idx < 0 || idx >= medCount) {
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  meds[idx].enabled = !meds[idx].enabled;
  saveMeds();

  server.sendHeader("Location", "/");
  server.send(303);
}

/*************************************************************
                      DELETE MEDICINE
*************************************************************/
void handleDelete() {
  if (server.hasArg("i")) {
    int index = server.arg("i").toInt();
    if (index >= 0 && index < medCount) {
      for (int j = index; j < medCount - 1; j++) {
        meds[j] = meds[j + 1];
      }
      medCount--;
      saveMeds();
    }
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

/*************************************************************
                         TEST ALARM
*************************************************************/
void testAlarm() {
  Serial.println("Running Test Alarm...");
  // Turn ON all LEDs + buzzer
  for (int i = 0; i < 10; i++) {
    digitalWrite(LED_PINS[i], HIGH);
  }
  digitalWrite(BUZZER_PIN, HIGH);

  delay(alarmDuration * 1000);

  // Turn OFF all
  for (int i = 0; i < 10; i++) {
    digitalWrite(LED_PINS[i], LOW);
  }
  digitalWrite(BUZZER_PIN, LOW);

  Serial.println("Test Alarm finished.");
}

/*************************************************************
                         SETTINGS PAGE
*************************************************************/
void handleSettings() {
  if (server.method() == HTTP_POST) {
    String action = server.arg("action");

    if (action == "sync") {
      syncTimeNTP();
    } else if (action == "save") {
      alarmDuration = server.arg("duration").toInt();
      if (alarmDuration < 1) alarmDuration = 1;
      prefs.putInt("duration", alarmDuration);
      Serial.print("Saved alarmDuration = ");
      Serial.println(alarmDuration);
    } else if (action == "test") {
      testAlarm();
    } else if (action == "cloud") {
      globalApiUrl = server.arg("apiUrl");
      globalGetUrl = server.arg("getUrl");
      bool newSync = server.hasArg("sync");  // checkbox present if checked
      onlineSyncEnabled = newSync;
      prefs.putString("apiUrl", globalApiUrl);
      prefs.putString("getUrl", globalGetUrl);
      prefs.putBool("syncOn", onlineSyncEnabled);
      Serial.print("Saved Cloud POST URL: ");
      Serial.println(globalApiUrl);
      Serial.print("Saved Cloud GET URL: ");
      Serial.println(globalGetUrl);
      Serial.print("Online Sync: ");
      Serial.println(onlineSyncEnabled ? "ENABLED" : "DISABLED");

      maybeSyncToCloud();
    }

    server.sendHeader("Location", "/settings");
    server.send(303);
    return;
  }

  String page = headerHTML("settings");

  page += "<h4>Settings</h4>";

  // Internet status badge
  bool online = (WiFi.status() == WL_CONNECTED);
  String badgeClass = online ? "bg-success" : "bg-danger";
  String statusText = online ? "Online" : "Offline";
  page += "<p>Internet Status: <span class='badge " + badgeClass + "'>" + statusText + "</span></p>";

  // Show Device ID
  page += "<p>Device ID: <code>" + deviceId + "</code></p>";

  // Alarm duration
  page += "<div class='card mb-3'><div class='card-header'>Alarm Settings</div><div class='card-body'>";
  page += "<form method='POST'>";
  page += "<input type='hidden' name='action' value='save'>";
  page += "Alarm Duration (sec): <input class='form-control' name='duration' value='" + String(alarmDuration) + "'><br>";
  page += "<button class='btn btn-primary'>Save Duration</button>";
  page += "</form></div></div>";

  // RTC & Test
  page += "<div class='card mb-3'><div class='card-header'>RTC & Alarm Test</div><div class='card-body'>";
  page += "<form method='POST'>";
  page += "<input type='hidden' name='action' value='sync'>";
  page += "<button class='btn btn-warning mb-2'>Sync RTC with Internet (NTP)</button>";
  page += "</form>";
  page += "<form method='POST'>";
  page += "<input type='hidden' name='action' value='test'>";
  page += "<button class='btn btn-success'>Test Alarm (LEDs + Buzzer)</button>";
  page += "</form>";
  page += "</div></div>";

  // Cloud Integration
  page += "<div class='card mb-3'><div class='card-header'>Cloud Integration</div><div class='card-body'>";
  page += "<form method='POST'>";
  page += "<input type='hidden' name='action' value='cloud'>";
  page += "<label>Global API POST URL (ESP32 → Cloud)</label>";
  page += "<input class='form-control' name='apiUrl' value='" + globalApiUrl + "' placeholder='https://example.com/medbox/upload'><br>";

  page += "<label>Global API GET URL (Cloud → ESP32 commands)</label>";
  page += "<input class='form-control' name='getUrl' value='" + globalGetUrl + "' placeholder='https://example.com/medbox/changes'><br>";

  // Toggle switch
  page += "<div class='form-check form-switch'>";
  page += "<input class='form-check-input' type='checkbox' id='syncSwitch' name='sync' ";
  if (onlineSyncEnabled) page += "checked";
  page += ">";
  page += "<label class='form-check-label' for='syncSwitch'>Enable Online Sync (POST + GET every 2 min)</label>";
  page += "</div><br>";

  page += "<button class='btn btn-info'>Save Cloud Settings</button>";
  page += "</form>";
  page += "</div></div>";

  page += footerHTML();
  server.send(200, "text/html", page);
}

/*************************************************************
                    SOFTAP MODE CONFIG PORTAL
           + NEW JSON API FOR APP: POST /api/wifi
*************************************************************/

// Root page in SoftAP mode: Wi-Fi config + Scan (for browser)
void handleSoftAPRoot() {
  bool doScan = server.hasArg("scan");

  String page;
  page += F(
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>MedBox WiFi Setup</title>"
    "<link href='https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css' rel='stylesheet'>"
    "</head><body><div class='container mt-3'>"
    "<h3 class='mb-3'>MedBox WiFi Setup (SoftAP)</h3>"
    "<div class='alert alert-info'>"
    "You are connected to <b>MedBoxConfig</b>.<br>"
    "You can also try: <code>http://medboxconfig.local</code>."
    "</div>"
  );

  // Wi-Fi credentials form
  page += "<div class='card mb-3'><div class='card-header'>WiFi Credentials</div><div class='card-body'>";
  page += "<form method='POST' action='/savewifi'>";
  page += "<label class='form-label'>WiFi SSID</label>";
  page += "<input class='form-control' name='ssid' value='" + wifiSsid + "' placeholder='HomeWiFi' required><br>";
  page += "<label class='form-label'>WiFi Password</label>";
  page += "<input class='form-control' type='password' name='pass' placeholder='********'><br>";
  page += "<button class='btn btn-primary'>Save & Reboot</button>";
  page += "</form></div></div>";

  // Scan networks
  page += "<div class='card mb-3'><div class='card-header'>Scan Networks</div><div class='card-body'>";
  page += "<form method='GET' action='/'>";
  page += "<button class='btn btn-secondary' name='scan' value='1'>Scan WiFi Networks</button>";
  page += "</form><br>";

  if (doScan) {
    int n = WiFi.scanNetworks();
    if (n <= 0) {
      page += "<p>No networks found.</p>";
    } else {
      page += "<ul class='list-group'>";
      for (int i = 0; i < n; i++) {
        page += "<li class='list-group-item d-flex justify-content-between align-items-center'>";
        page += WiFi.SSID(i);
        page += "<span class='badge bg-light text-dark'>";
        page += String(WiFi.RSSI(i)) + " dBm";
        if (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) page += " 🔒";
        page += "</span></li>";
      }
      page += "</ul>";
    }
  } else {
    page += "<p>Click <b>Scan WiFi Networks</b> to see nearby SSIDs.</p>";
  }

  page += "</div></div>";

  page += "<div class='alert alert-warning'>After saving WiFi details, the ESP32 will reboot and try to connect to the configured WiFi. Then open <code>http://medbox.local</code> from the same WiFi.</div>";

  page += "</div></body></html>";

  server.send(200, "text/html", page);
}

// Save WiFi from browser form (HTML)
void handleSoftAPSaveWifi() {
  if (!server.hasArg("ssid") || !server.hasArg("pass")) {
    server.send(400, "text/plain", "Missing ssid or pass");
    return;
  }

  String ssid = server.arg("ssid");
  String pass = server.arg("pass");

  saveWifiConfig(ssid, pass);

  String page;
  page += F(
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>WiFi Saved</title></head><body><div class='container mt-3'>"
    "<h3>WiFi credentials saved.</h3>"
    "<p>Device will reboot now and attempt to connect to the configured WiFi.</p>"
    "<p>After it connects, open <code>http://medbox.local</code>.</p>"
    "</div></body></html>"
  );
  server.send(200, "text/html", page);

  delay(1500);
  ESP.restart();
}

// NEW: Save WiFi from app as JSON: POST /api/wifi
// Body: {"ssid":"YourSSID","pass":"YourPassword"}
void apiSetWifi() {
  DynamicJsonDocument doc(256);
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"error\":\"invalid json\"}");
    return;
  }

  String ssid = doc["ssid"] | "";
  String pass = doc["pass"] | "";

  if (ssid == "") {
    server.send(400, "application/json", "{\"error\":\"ssid missing\"}");
    return;
  }

  saveWifiConfig(ssid, pass);

  server.send(200, "application/json", "{\"status\":\"saved_rebooting\"}");

  Serial.println("WiFi credentials received via /api/wifi, rebooting...");
  delay(1500);
  ESP.restart();
}

void startSoftAP() {
  Serial.println("SOFTAP MODE ENABLED!");

  WiFi.mode(WIFI_AP);
  WiFi.softAP("MedBoxConfig", "medbox");   // AP SSID/Password

  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  // mDNS in AP mode
  if (!MDNS.begin("medboxconfig")) {
    Serial.println("mDNS (medboxconfig) failed in SoftAP");
  } else {
    Serial.println("mDNS for SoftAP: http://medboxconfig.local");
  }

  // Browser config
  server.on("/", handleSoftAPRoot);
  server.on("/savewifi", HTTP_POST, handleSoftAPSaveWifi);

  // App JSON config
  server.on("/api/wifi", HTTP_POST, apiSetWifi);

  server.begin();
  Serial.println("HTTP server started in SoftAP mode");
}

/*************************************************************
                         ALARM CHECK
*************************************************************/
void checkAlarms() {
  DateTime now = rtc.now();

  for (int i = 0; i < medCount; i++) {
    if (!meds[i].enabled) continue;

    if (now.hour() == meds[i].hour &&
        now.minute() == meds[i].minute &&
        now.second() == 0) {

      int ledPin = LED_PINS[meds[i].led - 1];

      digitalWrite(ledPin, HIGH);
      digitalWrite(BUZZER_PIN, HIGH);

      delay(alarmDuration * 1000);

      digitalWrite(ledPin, LOW);
      digitalWrite(BUZZER_PIN, LOW);
    }
  }
}

/*************************************************************
                  CLOUD GET POLLING (EVERY 2 MIN)
*************************************************************/
void pollCloud() {
  if (!onlineSyncEnabled) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (globalGetUrl.length() == 0) return;

  String url = globalGetUrl;
  if (url.indexOf("?") >= 0)
    url += "&deviceId=" + deviceId;
  else
    url += "?deviceId=" + deviceId;

  HTTPClient http;
  http.begin(url);
  http.setTimeout(2000);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("Cloud GET failed: %d\n", code);
    http.end();
    return;
  }

  String body = http.getString();
  http.end();

  Serial.println("Cloud GET response:");
  Serial.println(body);

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.print("Cloud JSON parse error: ");
    Serial.println(err.c_str());
    return;
  }

  if (!doc.containsKey("commands")) {
    Serial.println("Cloud JSON has no 'commands' field.");
    return;
  }

  JsonArray cmds = doc["commands"].as<JsonArray>();
  bool changed = false;

  for (JsonObject cmd : cmds) {
    String op = cmd["op"].as<String>();
    op.toLowerCase();

    if (op == "add") {
      if (medCount >= MAX_MEDS) {
        Serial.println("Cloud add skipped: max meds reached.");
        continue;
      }
      Medicine m;
      m.name = cmd["name"] | "Unnamed";
      m.qty = cmd["qty"] | 1;
      m.hour = cmd["hour"] | 0;
      m.minute = cmd["minute"] | 0;
      m.led = cmd["led"] | 1;
      m.enabled = cmd["enabled"] | true;

      if (m.led < 1) m.led = 1;
      if (m.led > 10) m.led = 10;

      meds[medCount++] = m;
      changed = true;
      Serial.println("Cloud: added medicine.");
    }
    else if (op == "edit") {
      if (!cmd.containsKey("id")) continue;
      int id = cmd["id"];
      if (id < 0 || id >= medCount) {
        Serial.println("Cloud edit: invalid id");
        continue;
      }
      if (cmd.containsKey("name"))   meds[id].name   = cmd["name"].as<String>();
      if (cmd.containsKey("qty"))    meds[id].qty    = cmd["qty"].as<int>();
      if (cmd.containsKey("hour"))   meds[id].hour   = cmd["hour"].as<int>();
      if (cmd.containsKey("minute")) meds[id].minute = cmd["minute"].as<int>();
      if (cmd.containsKey("led"))    meds[id].led    = cmd["led"].as<int>();
      if (cmd.containsKey("enabled"))meds[id].enabled= cmd["enabled"].as<bool>();

      if (meds[id].led < 1) meds[id].led = 1;
      if (meds[id].led > 10) meds[id].led = 10;

      changed = true;
      Serial.println("Cloud: edited medicine.");
    }
    else if (op == "delete") {
      if (!cmd.containsKey("id")) continue;
      int id = cmd["id"];
      if (id < 0 || id >= medCount) {
        Serial.println("Cloud delete: invalid id");
        continue;
      }
      for (int i = id; i < medCount - 1; i++) {
        meds[i] = meds[i + 1];
      }
      medCount--;
      changed = true;
      Serial.println("Cloud: deleted medicine.");
    }
  }

  if (changed) {
    saveMeds();
  }
}

/*************************************************************
                        API HANDLERS (NORMAL MODE)
*************************************************************/

// GET /api/meds
void apiGetMeds() {
  if (!checkAPIKey()) return rejectUnauthorized();

  DynamicJsonDocument doc(4096);
  doc["deviceId"] = deviceId;
  doc["count"] = medCount;
  JsonArray arr = doc.createNestedArray("meds");

  for (int i = 0; i < medCount; i++) {
    JsonObject o = arr.createNestedObject();
    o["id"] = i;
    o["name"] = meds[i].name;
    o["qty"] = meds[i].qty;
    o["hour"] = meds[i].hour;
    o["minute"] = meds[i].minute;
    o["led"] = meds[i].led;
    o["enabled"] = meds[i].enabled;
  }

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// POST /api/add
void apiAddMed() {
  if (!checkAPIKey()) return rejectUnauthorized();
  if (medCount >= MAX_MEDS) {
    server.send(400, "application/json", "{\"error\":\"max limit reached\"}");
    return;
  }

  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"error\":\"invalid json\"}");
    return;
  }

  Medicine m;
  m.name = doc["name"].as<String>();
  m.qty = doc["qty"] | 1;
  m.hour = doc["hour"] | 0;
  m.minute = doc["minute"] | 0;
  m.led = doc["led"] | 1;
  m.enabled = doc["enabled"] | true;

  if (m.led < 1) m.led = 1;
  if (m.led > 10) m.led = 10;

  meds[medCount++] = m;
  saveMeds();

  server.send(200, "application/json", "{\"status\":\"added\"}");
}

// POST /api/edit?id=
void apiEditMed() {
  if (!checkAPIKey()) return rejectUnauthorized();
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"error\":\"id missing\"}");
    return;
  }

  int id = server.arg("id").toInt();
  if (id < 0 || id >= medCount) {
    server.send(400, "application/json", "{\"error\":\"invalid id\"}");
    return;
  }

  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"error\":\"invalid json\"}");
    return;
  }

  meds[id].name = doc["name"] | meds[id].name;
  meds[id].qty = doc["qty"] | meds[id].qty;
  meds[id].hour = doc["hour"] | meds[id].hour;
  meds[id].minute = doc["minute"] | meds[id].minute;
  meds[id].led = doc["led"] | meds[id].led;
  meds[id].enabled = doc["enabled"] | meds[id].enabled;

  if (meds[id].led < 1) meds[id].led = 1;
  if (meds[id].led > 10) meds[id].led = 10;

  saveMeds();
  server.send(200, "application/json", "{\"status\":\"updated\"}");
}

// POST /api/delete?id=
void apiDeleteMed() {
  if (!checkAPIKey()) return rejectUnauthorized();
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"error\":\"id missing\"}");
    return;
  }

  int id = server.arg("id").toInt();
  if (id < 0 || id >= medCount) {
    server.send(400, "application/json", "{\"error\":\"invalid id\"}");
    return;
  }

  for (int i = id; i < medCount - 1; i++) {
    meds[i] = meds[i + 1];
  }
  medCount--;
  saveMeds();

  server.send(200, "application/json", "{\"status\":\"deleted\"}");
}

// POST /api/toggle?id=
void apiToggleMed() {
  if (!checkAPIKey()) return rejectUnauthorized();
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"error\":\"id missing\"}");
    return;
  }

  int id = server.arg("id").toInt();
  if (id < 0 || id >= medCount) {
    server.send(400, "application/json", "{\"error\":\"invalid id\"}");
    return;
  }

  meds[id].enabled = !meds[id].enabled;
  saveMeds();

  server.send(200, "application/json", "{\"status\":\"toggled\"}");
}

// GET /api/time
void apiGetTime() {
  if (!checkAPIKey()) return rejectUnauthorized();

  DateTime now = rtc.now();
  DynamicJsonDocument doc(256);
  doc["year"] = now.year();
  doc["month"] = now.month();
  doc["day"] = now.day();
  doc["hour"] = now.hour();
  doc["minute"] = now.minute();
  doc["second"] = now.second();

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// POST /api/test
void apiTestAlarm() {
  if (!checkAPIKey()) return rejectUnauthorized();

  testAlarm();
  server.send(200, "application/json", "{\"status\":\"alarm_test_started\"}");
}

// GET /api/settings
void apiGetSettings() {
  if (!checkAPIKey()) return rejectUnauthorized();

  DynamicJsonDocument doc(256);
  doc["deviceId"] = deviceId;
  doc["alarmDuration"] = alarmDuration;
  doc["cloudPostUrl"] = globalApiUrl;
  doc["cloudGetUrl"] = globalGetUrl;
  doc["onlineSync"] = onlineSyncEnabled;
  doc["internet"] = (WiFi.status() == WL_CONNECTED);

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// POST /api/settings
void apiUpdateSettings() {
  if (!checkAPIKey()) return rejectUnauthorized();

  DynamicJsonDocument doc(256);
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"error\":\"invalid json\"}");
    return;
  }

  if (doc.containsKey("alarmDuration")) {
    alarmDuration = doc["alarmDuration"] | alarmDuration;
    if (alarmDuration < 1) alarmDuration = 1;
    prefs.putInt("duration", alarmDuration);
  }

  if (doc.containsKey("cloudPostUrl")) {
    globalApiUrl = doc["cloudPostUrl"].as<String>();
    prefs.putString("apiUrl", globalApiUrl);
  }

  if (doc.containsKey("cloudGetUrl")) {
    globalGetUrl = doc["cloudGetUrl"].as<String>();
    prefs.putString("getUrl", globalGetUrl);
  }

  if (doc.containsKey("onlineSync")) {
    onlineSyncEnabled = doc["onlineSync"];
    prefs.putBool("syncOn", onlineSyncEnabled);
  }

  server.send(200, "application/json", "{\"status\":\"updated\"}");
}

// POST /api/sync_ntp
void apiSyncNTP() {
  if (!checkAPIKey()) return rejectUnauthorized();

  syncTimeNTP();
  server.send(200, "application/json", "{\"status\":\"synced_ntp\"}");
}

// POST /api/set_time
void apiSetTimeRTC() {
  if (!checkAPIKey()) return rejectUnauthorized();

  DynamicJsonDocument doc(256);
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"error\":\"invalid json\"}");
    return;
  }

  int year   = doc["year"]   | 2025;
  int month  = doc["month"]  | 1;
  int day    = doc["day"]    | 1;
  int hour   = doc["hour"]   | 0;
  int minute = doc["minute"] | 0;
  int second = doc["second"] | 0;

  rtc.adjust(DateTime(year, month, day, hour, minute, second));
  server.send(200, "application/json", "{\"status\":\"rtc_set\"}");
}

/*************************************************************
                            SETUP
*************************************************************/
void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin();
  prefs.begin("medbox");

  pinMode(SETUP_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  for (int i = 0; i < 10; i++) pinMode(LED_PINS[i], OUTPUT);

  initDeviceId();

  alarmDuration = prefs.getInt("duration", 10);
  globalApiUrl = prefs.getString("apiUrl", "");
  globalGetUrl = prefs.getString("getUrl", "");
  onlineSyncEnabled = prefs.getBool("syncOn", false);
  loadWifiConfig();

  Serial.print("Loaded alarmDuration = ");
  Serial.println(alarmDuration);
  Serial.print("Loaded Cloud POST URL = ");
  Serial.println(globalApiUrl);
  Serial.print("Loaded Cloud GET URL = ");
  Serial.println(globalGetUrl);
  Serial.print("Online Sync Enabled = ");
  Serial.println(onlineSyncEnabled ? "YES" : "NO");

  loadMeds();

  if (!rtc.begin()) {
    Serial.println("RTC NOT FOUND!");
  }

  delay(100);
  if (digitalRead(SETUP_PIN) == LOW) {
    startSoftAP();
    return;
  }

  // Normal WiFi Mode (STA)
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  Serial.print("Connecting to WiFi SSID: ");
  Serial.println(wifiSsid);

  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t < 40) {
    delay(250);
    Serial.print(".");
    t++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected! IP: ");
    Serial.println(WiFi.localIP());

    if (!MDNS.begin("medbox")) {
      Serial.println("mDNS start failed for medbox");
    } else {
      Serial.println("mDNS: http://medbox.local");
    }

  } else {
    Serial.println("WiFi Failed! Switching to SoftAP...");
    startSoftAP();
    return;
  }

  // UI routes
  server.on("/", handleHome);
  server.on("/add", handleAdd);
  server.on("/edit", handleEdit);
  server.on("/toggle", handleToggle);
  server.on("/settings", handleSettings);
  server.on("/delete", handleDelete);

  // API routes
  server.on("/api/meds", HTTP_GET, apiGetMeds);
  server.on("/api/add", HTTP_POST, apiAddMed);
  server.on("/api/edit", HTTP_POST, apiEditMed);
  server.on("/api/delete", HTTP_POST, apiDeleteMed);
  server.on("/api/toggle", HTTP_POST, apiToggleMed);
  server.on("/api/time", HTTP_GET, apiGetTime);
  server.on("/api/test", HTTP_POST, apiTestAlarm);
  server.on("/api/settings", HTTP_GET, apiGetSettings);
  server.on("/api/settings", HTTP_POST, apiUpdateSettings);
  server.on("/api/sync_ntp", HTTP_POST, apiSyncNTP);
  server.on("/api/set_time", HTTP_POST, apiSetTimeRTC);

  const char* headerKeys[] = {"API_KEY"};
  server.collectHeaders(headerKeys, 1);

  server.begin();
}

/*************************************************************
                            LOOP
*************************************************************/
void loop() {
  server.handleClient();
  checkAlarms();

  unsigned long now = millis();
  if (now - lastPoll > POLL_INTERVAL_MS) {
    lastPoll = now;
    pollCloud();
  }
}
