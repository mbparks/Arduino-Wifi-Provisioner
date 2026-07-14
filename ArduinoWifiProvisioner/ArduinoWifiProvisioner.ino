/*
 * WiFiProvisioner.ino  (save in a folder named WiFiProvisioner)
 * Version: 1.3.0
 * Target:  Arduino UNO R4 WiFi (Renesas RA4M1 + ESP32-S3, WiFiS3 library)
 *
 * Changes in 1.3.0:
 *   - FIX: after submitting credentials the board now REBOOTS instead of
 *     hot-switching from AP to station mode. The WiFiS3/ESP32-S3 stack
 *     does not survive that switch cleanly: it reports WL_CONNECTED
 *     before DHCP finishes (IP shows 0.0.0.0) and the web server never
 *     rebinds to the station interface (page unreachable until reset).
 *     A clean boot joins correctly every time.
 *   - FIX: connection is not declared successful until a non-zero DHCP
 *     lease is held (8 second wait), and the real IP is logged.
 *   - NEW: "Change WiFi network" on the dashboard reboots into setup
 *     mode while keeping old credentials and pin config until you save
 *     a new network. "Factory reset" wipes everything.
 *   - NEW: a failed join at boot drops into setup mode with an error
 *     message naming the SSID that failed.
 *   - NEW: timestamped serial logging of boot, join attempts, assigned
 *     IP, every HTTP connection (client IP + request line), pin actions,
 *     and link loss. Toggle from the dashboard checkbox.
 *   - NEW: serial commands (115200 baud, newline-terminated):
 *       status   print mode, SSID, IP, RSSI
 *       portal   reboot into setup mode (credentials kept)
 *       forget   factory reset (wipe credentials + pin config) and reboot
 *       log on / log off   toggle serial logging
 *       help     list commands
 *
 * Behavior:
 *   1. On boot, load saved pin configuration and apply it immediately,
 *      then load WiFi credentials and join (unless the force-portal
 *      flag is set, in which case go straight to setup mode).
 *   2. In connected mode, serve the GPIO dashboard: live state of
 *      D0-D13 and A0-A5, digital output toggling, PWM sliders on
 *      D3/D5/D6/D9/D10/D11, analog readings on A0-A5 inputs. All pin
 *      config persists in EEPROM across power cycles.
 *   3. With no/failed credentials or after "Change network": open setup
 *      AP "UNO-R4-Setup", browse to http://192.168.4.1, pick an SSID,
 *      enter the password. Board saves and reboots into the network.
 *
 * Notes:
 *   - D0/D1 are Serial1 on the header; leave them as inputs if wired.
 *   - D13 is LED_BUILTIN; the status blink yields while you own it.
 *   - WiFiS3 has no captive-portal DNS, browse to 192.168.4.1 manually.
 *   - WiFiS3 cannot report AP association events, so setup-mode logging
 *     shows HTTP hits, not the moment a device joins the AP.
 *   - The setup AP is open by design; provision at home.
 *   - No auth on the dashboard, and outputs restore after power cycles.
 *     Nothing safety-critical on these pins.
 *   - Will NOT compile for the UNO Q (WiFi lives on its Linux side).
 *
 * License: GPL-3.0
 */

#include <WiFiS3.h>
#include <EEPROM.h>

// ---------------------------------------------------------------- config

const char*    FW_VERSION        = "1.3.0";
const char*    AP_SSID           = "UNO-R4-Setup";
const uint32_t CRED_MAGIC        = 0x57F1C0DE;      // credentials marker
const uint32_t PINCFG_MAGIC      = 0x6F10C0DE;      // pin config marker
const uint16_t EEPROM_ADDR       = 0;               // credentials block
const uint16_t EEPROM_ADDR_FLAG  = 120;             // boot-behavior flag byte
const uint16_t EEPROM_ADDR_PINS  = 128;             // pin config block
const uint8_t  FLAG_FORCE_PORTAL = 0xA5;            // "boot into setup mode"
const uint32_t CONNECT_TIMEOUT   = 15000;           // ms waiting for WL_CONNECTED
const uint32_t DHCP_TIMEOUT      = 8000;            // ms waiting for a real IP
const uint8_t  MAX_SCAN_RESULTS  = 20;

// ---------------------------------------------------------------- logging

bool logEnabled = true;

void logMsg(const String& m) {
  if (!logEnabled) return;
  Serial.print('[');
  Serial.print(millis());
  Serial.print(F(" ms] "));
  Serial.println(m);
}

// ---------------------------------------------------------------- pins

const uint8_t NUM_PINS = 20;    // D0-D13 then A0-A5
const uint8_t MANAGED_PINS[NUM_PINS] = {
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
  A0, A1, A2, A3, A4, A5
};

const uint8_t PM_INPUT  = 0;
const uint8_t PM_OUTPUT = 1;
const uint8_t PM_PWM    = 2;

struct PinConfig {
  uint32_t magic;
  uint8_t  mode[NUM_PINS];   // PM_INPUT / PM_OUTPUT / PM_PWM
  uint8_t  val[NUM_PINS];    // 0/1 for outputs, 0-255 duty for PWM
};

PinConfig pins;
bool userOwnsLed = false;

String pinName(uint8_t idx) {
  if (idx < 14) return "D" + String(idx);
  return "A" + String(idx - 14);
}

bool pinIsAnalog(uint8_t idx) { return idx >= 14; }

bool pinIsPwmCapable(uint8_t idx) {
  return idx == 3 || idx == 5 || idx == 6 || idx == 9 || idx == 10 || idx == 11;
}

int pinIndexFromLabel(const String& label) {
  if (label.length() < 2) return -1;
  char kind = label[0];
  int num = label.substring(1).toInt();
  if (kind == 'D' && num >= 0 && num <= 13) return num;
  if (kind == 'A' && num >= 0 && num <= 5)  return 14 + num;
  return -1;
}

// ---------------------------------------------------------------- state

struct StoredCreds {
  uint32_t magic;
  char     ssid[33];
  char     pass[65];
};

enum RunMode { MODE_PORTAL, MODE_CONNECTED };

RunMode     mode = MODE_PORTAL;
WiFiServer  server(80);
StoredCreds creds;

String  scanSsid[MAX_SCAN_RESULTS];
int32_t scanRssi[MAX_SCAN_RESULTS];
bool    scanOpen[MAX_SCAN_RESULTS];
uint8_t scanCount = 0;

String lastError = "";

// ---------------------------------------------------------------- EEPROM

bool loadCreds() {
  EEPROM.get(EEPROM_ADDR, creds);
  if (creds.magic != CRED_MAGIC) return false;
  creds.ssid[32] = '\0';
  creds.pass[64] = '\0';
  return strlen(creds.ssid) > 0;
}

void saveCreds(const String& ssid, const String& pass) {
  memset(&creds, 0, sizeof(creds));
  creds.magic = CRED_MAGIC;
  ssid.toCharArray(creds.ssid, sizeof(creds.ssid));
  pass.toCharArray(creds.pass, sizeof(creds.pass));
  EEPROM.put(EEPROM_ADDR, creds);
}

void clearCreds() {
  memset(&creds, 0, sizeof(creds));
  EEPROM.put(EEPROM_ADDR, creds);
}

void setForcePortalFlag(bool on) {
  EEPROM.write(EEPROM_ADDR_FLAG, on ? FLAG_FORCE_PORTAL : 0x00);
}

bool forcePortalFlagSet() {
  return EEPROM.read(EEPROM_ADDR_FLAG) == FLAG_FORCE_PORTAL;
}

void savePinConfig() {
  pins.magic = PINCFG_MAGIC;
  EEPROM.put(EEPROM_ADDR_PINS, pins);
}

void clearPinConfig() {
  memset(&pins, 0, sizeof(pins));
  EEPROM.put(EEPROM_ADDR_PINS, pins);
}

void loadAndApplyPinConfig() {
  EEPROM.get(EEPROM_ADDR_PINS, pins);
  if (pins.magic != PINCFG_MAGIC) {
    memset(&pins, 0, sizeof(pins));
    pins.magic = PINCFG_MAGIC;
  }
  for (uint8_t i = 0; i < NUM_PINS; i++) {
    if (pins.mode[i] > PM_PWM) pins.mode[i] = PM_INPUT;
    if (pins.mode[i] == PM_PWM && !pinIsPwmCapable(i)) pins.mode[i] = PM_INPUT;
    if (pins.mode[i] == PM_OUTPUT && pins.val[i] > 1)  pins.val[i]  = 0;

    uint8_t hw = MANAGED_PINS[i];
    if (pins.mode[i] == PM_OUTPUT) {
      pinMode(hw, OUTPUT);
      digitalWrite(hw, pins.val[i] ? HIGH : LOW);
    } else if (pins.mode[i] == PM_PWM) {
      pinMode(hw, OUTPUT);
      analogWrite(hw, pins.val[i]);
    } else {
      pinMode(hw, INPUT);
    }
    if (hw == LED_BUILTIN && pins.mode[i] != PM_INPUT) userOwnsLed = true;
  }
}

// ---------------------------------------------------------------- helpers

String urlDecode(const String& in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < in.length()) {
      char hex[3] = { in[i + 1], in[i + 2], '\0' };
      out += (char) strtol(hex, nullptr, 16);
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

String formField(const String& body, const String& name) {
  String key = name + "=";
  int start = body.startsWith(key) ? 0 : body.indexOf("&" + key);
  if (start < 0) return "";
  start = body.indexOf('=', start) + 1;
  int end = body.indexOf('&', start);
  if (end < 0) end = body.length();
  return urlDecode(body.substring(start, end));
}

String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if      (c == '<')  out += "&lt;";
    else if (c == '>')  out += "&gt;";
    else if (c == '&')  out += "&amp;";
    else if (c == '"')  out += "&quot;";
    else                out += c;
  }
  return out;
}

void rebootBoard() {
  logMsg(F("Rebooting"));
  Serial.flush();
  delay(250);
  NVIC_SystemReset();
}

// ---------------------------------------------------------------- WiFi

void scanForNetworks() {
  logMsg(F("Scanning for networks"));
  int n = WiFi.scanNetworks();
  scanCount = 0;
  for (int i = 0; i < n && scanCount < MAX_SCAN_RESULTS; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    bool dup = false;
    for (uint8_t j = 0; j < scanCount; j++) {
      if (scanSsid[j] == s) { dup = true; break; }
    }
    if (dup) continue;
    scanSsid[scanCount] = s;
    scanRssi[scanCount] = WiFi.RSSI(i);
    scanOpen[scanCount] = (WiFi.encryptionType(i) == ENC_TYPE_NONE);
    scanCount++;
  }
  logMsg(String(scanCount) + " networks cached");
}

// Join and hold out for a real DHCP lease before declaring success.
bool tryConnect(const char* ssid, const char* pass) {
  logMsg("Joining '" + String(ssid) + "'");
  if (strlen(pass) > 0) WiFi.begin(ssid, pass);
  else                  WiFi.begin(ssid);

  uint32_t start = millis();
  while (millis() - start < CONNECT_TIMEOUT) {
    if (WiFi.status() == WL_CONNECTED) break;
    delay(250);
  }
  if (WiFi.status() != WL_CONNECTED) {
    logMsg(F("Join failed: association/authentication timed out"));
    return false;
  }

  // WL_CONNECTED can arrive before DHCP finishes; wait for a real address
  uint32_t ipStart = millis();
  while (WiFi.localIP() == IPAddress(0, 0, 0, 0) && millis() - ipStart < DHCP_TIMEOUT) {
    delay(200);
  }
  if (WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    logMsg(F("Join failed: associated but no DHCP lease"));
    return false;
  }

  logMsg("Connected. IP " + WiFi.localIP().toString() +
         ", RSSI " + String(WiFi.RSSI()) + " dBm");
  return true;
}

void startPortal() {
  WiFi.end();
  delay(500);
  scanForNetworks();
  delay(250);

  WiFi.config(IPAddress(192, 168, 4, 1));
  if (WiFi.beginAP(AP_SSID) != WL_AP_LISTENING) {
    logMsg(F("AP start failed"));
    rebootBoard();
  }
  delay(500);
  server.begin();
  mode = MODE_PORTAL;
  logMsg(F("Setup AP running. Join 'UNO-R4-Setup', browse to http://192.168.4.1"));
}

void startConnectedServer() {
  server.begin();
  mode = MODE_CONNECTED;
  logMsg("GPIO dashboard at http://" + WiFi.localIP().toString());
}

// ---------------------------------------------------------------- pages

void sendHeader(WiFiClient& c, int code, const char* status, const char* ctype = "text/html") {
  c.print(F("HTTP/1.1 "));
  c.print(code);
  c.print(' ');
  c.println(status);
  c.print(F("Content-Type: "));
  c.println(ctype);
  c.println(F("Connection: close"));
  c.println(F("Cache-Control: no-store"));
  c.println();
}

const char PAGE_HEAD[] =
  "<!DOCTYPE html><html><head><meta charset='utf-8'>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>UNO R4 WiFi</title><style>"
  "body{background:#12151a;color:#dfe6ee;font-family:system-ui,sans-serif;"
  "max-width:680px;margin:2rem auto;padding:0 1rem}"
  "h1{font-size:1.3rem;color:#7fd4a8}"
  "select,input,button{box-sizing:border-box;padding:.6rem;"
  "margin:.25rem 0;border-radius:6px;border:1px solid #3a4250;"
  "background:#1c212a;color:#dfe6ee;font-size:.95rem}"
  "select,input{width:100%}"
  "input[type=range]{width:130px;padding:0;vertical-align:middle}"
  "input[type=checkbox]{width:auto;min-height:0}"
  "button{background:#2b6e4f;border:none;cursor:pointer;min-height:44px;min-width:44px}"
  "button:hover{background:#358a63}"
  "button.alt{background:#3a4250}button.alt:hover{background:#4a5466}"
  "button:focus,a:focus,input:focus{outline:2px solid #7fb8d4;outline-offset:2px}"
  "table{width:100%;border-collapse:collapse;margin:1rem 0}"
  "th,td{text-align:left;padding:.45rem .5rem;border-bottom:1px solid #2a303c}"
  "th{color:#8b95a3;font-weight:600;font-size:.85rem}"
  ".hi{color:#7fd4a8;font-weight:700}.lo{color:#8b95a3}"
  ".badge{font-size:.75rem;padding:.15rem .45rem;border-radius:4px;background:#2a303c}"
  ".badge.out{background:#5a4a2b;color:#e8c987}"
  ".badge.pwm{background:#2b4a5a;color:#87c9e8}"
  ".err{background:#4a2328;border:1px solid #8c3a44;padding:.6rem;"
  "border-radius:6px;margin-bottom:1rem}"
  "a{color:#7fb8d4}small{color:#8b95a3}footer{margin:1.5rem 0;color:#8b95a3;font-size:.8rem}"
  "</style></head><body>";

void servePortalPage(WiFiClient& c) {
  sendHeader(c, 200, "OK");
  c.print(PAGE_HEAD);
  c.print(F("<h1>WiFi Setup</h1>"));

  if (lastError.length() > 0) {
    c.print(F("<div class='err'>"));
    c.print(htmlEscape(lastError));
    c.print(F("</div>"));
  }

  c.print(F("<form method='POST' action='/connect'>"
            "<label>Nearby networks</label><select name='ssid'>"));
  for (uint8_t i = 0; i < scanCount; i++) {
    c.print(F("<option value=\""));
    c.print(htmlEscape(scanSsid[i]));
    c.print(F("\">"));
    c.print(htmlEscape(scanSsid[i]));
    c.print(F(" ("));
    c.print(scanRssi[i]);
    c.print(F(" dBm"));
    if (scanOpen[i]) c.print(F(", open"));
    c.print(F(")</option>"));
  }
  c.print(F("</select>"
            "<label>Or type a hidden SSID <small>(overrides the list)</small></label>"
            "<input name='ssid_manual' maxlength='32' autocomplete='off'>"
            "<label>Password <small>(leave blank for open networks)</small></label>"
            "<input type='password' name='pass' maxlength='63'>"
            "<button type='submit'>Connect</button></form>"
            "<p><a href='/scan'>Rescan networks</a></p>"
            "</body></html>"));
}

void serveRebootingPage(WiFiClient& c, const String& ssid) {
  sendHeader(c, 200, "OK");
  c.print(PAGE_HEAD);
  c.print(F("<h1>Saved. Rebooting...</h1><p>The board is restarting and will join <b>"));
  c.print(htmlEscape(ssid));
  c.print(F("</b>.</p>"
            "<p>Its new IP address is printed on the serial monitor "
            "(115200 baud) and will appear in your router's client list. "
            "If the join fails (for example a wrong password), the "
            "<b>UNO-R4-Setup</b> network reappears in about 30 seconds "
            "with an error message.</p>"
            "</body></html>"));
}

void serveDashboardPage(WiFiClient& c) {
  sendHeader(c, 200, "OK");
  c.print(PAGE_HEAD);
  c.print(F("<h1>GPIO Dashboard</h1><p><b>Network:</b> "));
  c.print(htmlEscape(String(creds.ssid)));
  c.print(F(" &nbsp; <b>IP:</b> "));
  c.print(WiFi.localIP());
  c.print(F(" &nbsp; <b>Signal:</b> <span id='rssi'>...</span> dBm</p>"
            "<table><thead><tr><th>Pin</th><th>Mode</th><th>State</th>"
            "<th>Analog</th><th>Actions</th></tr></thead>"
            "<tbody id='rows'><tr><td colspan='5'>Loading...</td></tr></tbody></table>"
            "<p><small>Pin modes and states are saved to EEPROM and restored "
            "after a power cycle. D0/D1 are the Serial1 UART pins; leave them "
            "as inputs if anything is wired there. D13 is the built-in LED. "
            "PWM available on D3, D5, D6, D9, D10, D11 (duty 0-255, saved "
            "when you release the slider).</small></p>"
            "<p><label><input type='checkbox' id='logchk' "
            "onchange=\"act('/api/log?on='+(this.checked?1:0))\"> "
            "Serial logging</label></p>"
            "<p><a href='/change'>Change WiFi network</a> "
            "<small>(keeps pin config and old credentials until you save a "
            "new network)</small><br>"
            "<a href='/forget'>Factory reset</a> "
            "<small>(wipes credentials and pin config, reboots into setup "
            "mode)</small></p>"
            "<footer>WiFiProvisioner v"));
  c.print(FW_VERSION);
  c.print(F("</footer>"
            "<script>\n"
            "function act(url){fetch(url).then(function(){refresh();});}\n"
            "function refresh(){\n"
            "  fetch('/api/status').then(function(r){return r.json();}).then(function(d){\n"
            "    document.getElementById('rssi').textContent=d.rssi;\n"
            "    document.getElementById('logchk').checked=(d.log===1);\n"
            "    var ae=document.activeElement;\n"
            "    if(ae&&ae.type==='range'){return;}\n"
            "    var rows='';\n"
            "    for(var i=0;i<d.pins.length;i++){\n"
            "      var p=d.pins[i];\n"
            "      var modeB,state,btns='';\n"
            "      if(p.m===2){\n"
            "        modeB='<span class=\"badge pwm\">PWM</span>';\n"
            "        state='duty '+p.v;\n"
            "        btns='<input type=\"range\" min=\"0\" max=\"255\" value=\"'+p.v+'\" '\n"
            "            +'aria-label=\"PWM duty for '+p.n+'\" '\n"
            "            +'onchange=\"act(\\'/api/pwm?pin='+p.n+'&duty=\\'+this.value)\"> '\n"
            "            +'<button class=\"alt\" onclick=\"act(\\'/api/mode?pin='+p.n+'&m=1\\')\">Output</button> '\n"
            "            +'<button class=\"alt\" onclick=\"act(\\'/api/mode?pin='+p.n+'&m=0\\')\">Input</button>';\n"
            "      }else if(p.m===1){\n"
            "        modeB='<span class=\"badge out\">OUTPUT</span>';\n"
            "        state=p.v?'<span class=\"hi\">HIGH</span>':'<span class=\"lo\">LOW</span>';\n"
            "        btns='<button onclick=\"act(\\'/api/toggle?pin='+p.n+'\\')\">Toggle</button> ';\n"
            "        if(p.p){btns+='<button class=\"alt\" onclick=\"act(\\'/api/mode?pin='+p.n+'&m=2\\')\">PWM</button> ';}\n"
            "        btns+='<button class=\"alt\" onclick=\"act(\\'/api/mode?pin='+p.n+'&m=0\\')\">Input</button>';\n"
            "      }else{\n"
            "        modeB='<span class=\"badge\">INPUT</span>';\n"
            "        state=p.v?'<span class=\"hi\">HIGH</span>':'<span class=\"lo\">LOW</span>';\n"
            "        btns='<button class=\"alt\" onclick=\"act(\\'/api/mode?pin='+p.n+'&m=1\\')\">Output</button>';\n"
            "        if(p.p){btns+=' <button class=\"alt\" onclick=\"act(\\'/api/mode?pin='+p.n+'&m=2\\')\">PWM</button>';}\n"
            "      }\n"
            "      var analog=(p.a===undefined)?'':p.a;\n"
            "      rows+='<tr><td><b>'+p.n+'</b></td><td>'+modeB+'</td><td>'+state+'</td><td>'+analog+'</td><td>'+btns+'</td></tr>';\n"
            "    }\n"
            "    document.getElementById('rows').innerHTML=rows;\n"
            "  }).catch(function(e){});\n"
            "}\n"
            "refresh();\n"
            "setInterval(refresh,3000);\n"
            "</script></body></html>"));
}

void serveStatusJson(WiFiClient& c) {
  sendHeader(c, 200, "OK", "application/json");
  c.print(F("{\"rssi\":"));
  c.print(WiFi.RSSI());
  c.print(F(",\"log\":"));
  c.print(logEnabled ? 1 : 0);
  c.print(F(",\"pins\":["));
  for (uint8_t i = 0; i < NUM_PINS; i++) {
    if (i > 0) c.print(',');
    c.print(F("{\"n\":\""));
    c.print(pinName(i));
    c.print(F("\",\"m\":"));
    c.print(pins.mode[i]);
    c.print(F(",\"p\":"));
    c.print(pinIsPwmCapable(i) ? 1 : 0);
    c.print(F(",\"v\":"));
    if (pins.mode[i] == PM_INPUT) c.print(digitalRead(MANAGED_PINS[i]) == HIGH ? 1 : 0);
    else                          c.print(pins.val[i]);
    if (pinIsAnalog(i) && pins.mode[i] == PM_INPUT) {
      c.print(F(",\"a\":"));
      c.print(analogRead(MANAGED_PINS[i]));
    }
    c.print('}');
  }
  c.print(F("]}"));
}

void serveNotFound(WiFiClient& c) {
  sendHeader(c, 404, "Not Found");
  c.print(F("<html><body>Not found. Try <a href='/'>the main page</a>.</body></html>"));
}

// ---------------------------------------------------------------- GPIO API

void trackLedOwnership(uint8_t idx) {
  if (MANAGED_PINS[idx] == LED_BUILTIN) {
    userOwnsLed = (pins.mode[idx] != PM_INPUT);
  }
}

void handleModeRequest(WiFiClient& c, const String& query) {
  int idx = pinIndexFromLabel(formField(query, "pin"));
  String m = formField(query, "m");
  if (idx < 0 || (m != "0" && m != "1" && m != "2")) { serveNotFound(c); return; }
  uint8_t newMode = (uint8_t) m.toInt();
  if (newMode == PM_PWM && !pinIsPwmCapable(idx)) { serveNotFound(c); return; }

  uint8_t hw = MANAGED_PINS[idx];
  pins.mode[idx] = newMode;
  pins.val[idx]  = 0;
  if (newMode == PM_OUTPUT) {
    pinMode(hw, OUTPUT);
    digitalWrite(hw, LOW);
  } else if (newMode == PM_PWM) {
    pinMode(hw, OUTPUT);
    analogWrite(hw, 0);
  } else {
    pinMode(hw, INPUT);
  }
  trackLedOwnership(idx);
  savePinConfig();
  logMsg("Pin " + pinName(idx) + " set to " +
         (newMode == PM_OUTPUT ? "OUTPUT" : newMode == PM_PWM ? "PWM" : "INPUT"));
  serveStatusJson(c);
}

void handleToggleRequest(WiFiClient& c, const String& query) {
  int idx = pinIndexFromLabel(formField(query, "pin"));
  if (idx < 0 || pins.mode[idx] != PM_OUTPUT) { serveNotFound(c); return; }
  pins.val[idx] = pins.val[idx] ? 0 : 1;
  digitalWrite(MANAGED_PINS[idx], pins.val[idx] ? HIGH : LOW);
  savePinConfig();
  logMsg("Pin " + pinName(idx) + " toggled " + (pins.val[idx] ? "HIGH" : "LOW"));
  serveStatusJson(c);
}

void handlePwmRequest(WiFiClient& c, const String& query) {
  int idx = pinIndexFromLabel(formField(query, "pin"));
  String d = formField(query, "duty");
  if (idx < 0 || pins.mode[idx] != PM_PWM || d.length() == 0) { serveNotFound(c); return; }
  long duty = d.toInt();
  if (duty < 0) duty = 0;
  if (duty > 255) duty = 255;
  pins.val[idx] = (uint8_t) duty;
  analogWrite(MANAGED_PINS[idx], pins.val[idx]);
  savePinConfig();
  logMsg("Pin " + pinName(idx) + " PWM duty " + String(pins.val[idx]));
  serveStatusJson(c);
}

void handleLogRequest(WiFiClient& c, const String& query) {
  String on = formField(query, "on");
  if (on == "1") { logEnabled = true;  logMsg(F("Serial logging ON")); }
  if (on == "0") { logMsg(F("Serial logging OFF")); logEnabled = false; }
  serveStatusJson(c);
}

// ---------------------------------------------------------------- HTTP

void handleClient() {
  WiFiClient client = server.available();
  if (!client) return;

  String reqLine = client.readStringUntil('\n');
  reqLine.trim();
  if (reqLine.length() == 0) { client.stop(); return; }

  logMsg("HTTP " + client.remoteIP().toString() + " \"" + reqLine + "\"");

  int contentLength = 0;
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;
    if (line.startsWith("Content-Length:")) {
      contentLength = line.substring(15).toInt();
    }
  }

  String body = "";
  if (contentLength > 0 && contentLength < 512) {
    uint32_t start = millis();
    while ((int) body.length() < contentLength && millis() - start < 3000) {
      if (client.available()) body += (char) client.read();
    }
  }

  bool isPost = reqLine.startsWith("POST ");
  int sp1 = reqLine.indexOf(' ');
  int sp2 = reqLine.indexOf(' ', sp1 + 1);
  String fullPath = (sp1 > 0 && sp2 > sp1) ? reqLine.substring(sp1 + 1, sp2) : "/";

  String path = fullPath;
  String query = "";
  int qm = fullPath.indexOf('?');
  if (qm >= 0) {
    path  = fullPath.substring(0, qm);
    query = fullPath.substring(qm + 1);
  }

  if (mode == MODE_PORTAL) {
    if (isPost && path == "/connect") {
      String ssid = formField(body, "ssid_manual");
      if (ssid.length() == 0) ssid = formField(body, "ssid");
      String pass = formField(body, "pass");

      if (ssid.length() == 0) {
        lastError = "No SSID selected or entered.";
        servePortalPage(client);
        client.stop();
        return;
      }

      // save, tell the user, and reboot into a clean station-mode boot
      saveCreds(ssid, pass);
      setForcePortalFlag(false);
      logMsg("Credentials saved for '" + ssid + "'");
      serveRebootingPage(client, ssid);
      client.flush();
      delay(500);
      client.stop();
      rebootBoard();
      return;
    }
    if (path == "/scan") {
      // Rescan tears down and rebuilds the AP; the client must reconnect.
      client.stop();
      startPortal();
      return;
    }
    if (path == "/") servePortalPage(client);
    else             serveNotFound(client);

  } else {  // MODE_CONNECTED
    if (path == "/change") {
      sendHeader(client, 200, "OK");
      client.print(F("<html><body>Rebooting into setup mode. Join the "
                     "<b>UNO-R4-Setup</b> network and browse to "
                     "http://192.168.4.1 to pick a new network. Your pin "
                     "configuration is kept.</body></html>"));
      client.flush();
      delay(500);
      client.stop();
      setForcePortalFlag(true);
      rebootBoard();
      return;
    }
    if (path == "/forget") {
      sendHeader(client, 200, "OK");
      client.print(F("<html><body>Factory reset: credentials and pin "
                     "configuration wiped. Rebooting into setup mode. Look "
                     "for the <b>UNO-R4-Setup</b> network.</body></html>"));
      client.flush();
      delay(500);
      client.stop();
      clearCreds();
      clearPinConfig();
      setForcePortalFlag(false);
      rebootBoard();
      return;
    }
    if      (path == "/api/status") serveStatusJson(client);
    else if (path == "/api/mode")   handleModeRequest(client, query);
    else if (path == "/api/toggle") handleToggleRequest(client, query);
    else if (path == "/api/pwm")    handlePwmRequest(client, query);
    else if (path == "/api/log")    handleLogRequest(client, query);
    else if (path == "/")           serveDashboardPage(client);
    else                            serveNotFound(client);
  }

  client.stop();
}

// ---------------------------------------------------------------- serial

void printHelp() {
  Serial.println(F("Commands: status | portal | forget | log on | log off | help"));
}

void handleSerial() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toLowerCase();
  if (cmd.length() == 0) return;

  if (cmd == "help") {
    printHelp();
  } else if (cmd == "status") {
    Serial.print(F("Mode: "));
    Serial.println(mode == MODE_CONNECTED ? F("connected") : F("setup portal"));
    if (mode == MODE_CONNECTED) {
      Serial.print(F("SSID: ")); Serial.println(creds.ssid);
      Serial.print(F("IP:   ")); Serial.println(WiFi.localIP());
      Serial.print(F("RSSI: ")); Serial.print(WiFi.RSSI()); Serial.println(F(" dBm"));
    } else {
      Serial.println(F("AP 'UNO-R4-Setup' at http://192.168.4.1"));
    }
  } else if (cmd == "portal") {
    setForcePortalFlag(true);
    rebootBoard();
  } else if (cmd == "forget") {
    clearCreds();
    clearPinConfig();
    setForcePortalFlag(false);
    Serial.println(F("Wiped. Rebooting into setup mode."));
    rebootBoard();
  } else if (cmd == "log on") {
    logEnabled = true;
    logMsg(F("Serial logging ON"));
  } else if (cmd == "log off") {
    logMsg(F("Serial logging OFF"));
    logEnabled = false;
  } else {
    Serial.println(F("Unknown command."));
    printHelp();
  }
}

// ---------------------------------------------------------------- main

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(200);
  delay(1500);
  pinMode(LED_BUILTIN, OUTPUT);

  Serial.print(F("WiFiProvisioner v"));
  Serial.println(FW_VERSION);
  printHelp();

  // restore saved pin modes and states before anything else
  loadAndApplyPinConfig();

  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println(F("WiFi module not responding. Halting."));
    while (true) { digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN)); delay(100); }
  }

  if (forcePortalFlagSet()) {
    setForcePortalFlag(false);   // one-shot flag
    logMsg(F("Force-portal flag set, entering setup mode"));
    startPortal();
    return;
  }

  if (loadCreds()) {
    logMsg("Found saved credentials for '" + String(creds.ssid) + "'");
    if (tryConnect(creds.ssid, creds.pass)) {
      startConnectedServer();
      return;
    }
    lastError = "Could not join '" + String(creds.ssid) +
                "'. Check the password and try again.";
    logMsg(F("Saved network unavailable, falling back to setup mode"));
  } else {
    logMsg(F("No saved credentials, entering setup mode"));
  }
  startPortal();
}

void loop() {
  handleClient();
  handleSerial();

  // status LED on D13, unless the dashboard owns D13 as output or PWM
  if (!userOwnsLed) {
    if (mode == MODE_PORTAL) {
      digitalWrite(LED_BUILTIN, (millis() / 500) % 2);   // slow blink
    } else {
      digitalWrite(LED_BUILTIN, HIGH);                   // solid
    }
  }

  if (mode == MODE_CONNECTED) {
    // if the router reboots, fall back to setup mode after the link dies
    static uint32_t lastCheck = 0;
    if (millis() - lastCheck > 10000) {
      lastCheck = millis();
      if (WiFi.status() != WL_CONNECTED) {
        logMsg(F("Lost connection, retrying saved network"));
        if (!tryConnect(creds.ssid, creds.pass)) startPortal();
        else startConnectedServer();
      }
    }
  }
}