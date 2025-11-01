/********* ESP32: IR + DHT22 + RGB + WebUI (Auto/Manual, NO learn) *********
 * Features:
 *  - AUTO: RGB color follows room temperature (DHT22) using 5 bands: RED/ORANGE/YELLOW/GREEN/BLUE.
 *  - MANUAL: choose any captured color/effect on the web; ESP32 sets local RGB (approx) & transmits the mapped IR code.
 *  - Simple Web UI (http://<ip>/ or http://esp32-ir.local/ if mDNS resolves).
 *
 * Libraries:
 *  - IRremoteESP8266 by crankyoldgit
 *  - DHT sensor library (Adafruit) + Adafruit Unified Sensor
 *  - ESP32 board (Espressif) 2.x+
 ******************************************************************************/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

#include <IRremoteESP8266.h>
#include <IRsend.h>

#include <DHT.h>

/* ========== WIFI CONFIG ========= */
const char* WIFI_SSID = "YOUR_WIFI";
const char* WIFI_PASS = "YOUR_PASSWORD";
const char* MDNS_NAME = "esp32-ir"; // http://esp32-ir.local

/* ========== PIN MAP ========= */
#define PIN_IR_SEND 4
#define PIN_DHT     18
#define DHT_TYPE    DHT22
#define PIN_R       25
#define PIN_G       26
#define PIN_B       27

/* ========== LEDC PWM ========= */
#define CH_R 0
#define CH_G 1
#define CH_B 2
#define PWM_FREQ 5000
#define PWM_RES  8 // 0..255

/* ========== COLOR MAP =========
 * key: dùng trong API/UI
 * label: nhãn hiển thị
 * rgb: màu nội bộ để hiển thị trạng thái (xấp xỉ; riêng các hiệu ứng sẽ chọn màu đại diện)
 * code: NEC 32-bit để phát IR (0 = không phát)
 */
struct ColorEntry {
  const char* key;
  const char* label;
  uint8_t r, g, b;
  uint32_t code;
};

// --- Captured codes (from FULL COLOR.txt) ---
static ColorEntry COLORS[] = {
//  key           , label                           ,   R,   G,   B , code (NEC 32-bit)
  {"red"         , "Đỏ"                            , 255,   0,   0 , 0x00000000}, // pending
  {"orange1"     , "Cam 1"                         , 255,  80,   0 , 0xF710EF},
  {"orange2"     , "Cam 2"                         , 255, 100,  10 , 0xF730CF},
  {"orange3"     , "Cam 3"                         , 255, 120,  20 , 0xF708F7},
  {"yellow"      , "Vàng"                          , 255, 255,   0 , 0xF728D7},
  {"green1"      , "Xanh lá 1"                     ,   0, 255,   0 , 0xF7906F},
  {"lightblue"   , "Xanh nhạt"                     , 120, 200, 255, 0xF7B04F},
  {"lightdblue"  , "Xanh dương nhạt 1"             ,  90, 160, 255, 0xF78877},
  {"lightddblue" , "Xanh dương nhạt 2"             ,  60, 140, 255, 0xF7A857},
  {"bluel"       , "Xanh dương (BLUE L)"           ,   0,   0, 255, 0xF750AF},
  {"purple"      , "Tím"                           , 170,   0, 255, 0xF7708F},
  {"pink"        , "Hồng"                          , 255,  60, 160, 0xF748B7},
  {"pinkl"       , "Hồng nhạt"                     , 255, 120, 190, 0xF76897},
  // Effects (dùng màu đại diện để hiển thị trạng thái cục bộ)
  {"blink"       , "Nhấp nháy liên tục"            , 255, 255, 255, 0xF7E817},
  {"smooth"      , "Chuyển đổi từ từ"              ,  80, 200, 255, 0xF7C837},
  {"whitebreath" , "Trắng sáng & tối từ từ"        , 255, 255, 255, 0xF7F00F},
  {"autocycle"   , "Chuyển đổi liên tục (auto)"    ,  50, 180, 255, 0xF7D02F},
};
static const size_t COLOR_COUNT = sizeof(COLORS)/sizeof(COLORS[0]);

/* ========== AUTO bands (for TP.HCM) =========
   ≤ 24.0  -> BLUE (bluel)
   24.0–27.0 -> GREEN (green1)
   27.0–29.0 -> YELLOW (yellow)
   29.0–31.0 -> ORANGE (orange1)
   > 31.0    -> RED (red)
*/
int idxOfKey(const char* key) {
  for (size_t i=0;i<COLOR_COUNT;i++) if (strcmp(COLORS[i].key, key)==0) return i;
  return 0;
}
int bandIndexForTemp(float tC) {
  if (!isfinite(tC)) return idxOfKey("bluel");
  if (tC <= 24.0) return idxOfKey("bluel");
  if (tC <= 27.0) return idxOfKey("green1");
  if (tC <= 29.0) return idxOfKey("yellow");
  if (tC <= 31.0) return idxOfKey("orange1");
  return idxOfKey("red");
}

/* ========== GLOBALS ========= */
WebServer server(80);
DHT dht(PIN_DHT, DHT_TYPE);
IRsend irsend(PIN_IR_SEND);

enum Mode { AUTO=0, MANUAL=1 };
Mode currentMode = AUTO;
int currentIdxManual = 0;

/* ========== RGB OUTPUT ========= */
void rgbSet(uint8_t r, uint8_t g, uint8_t b) {
  ledcWrite(CH_R, r);
  ledcWrite(CH_G, g);
  ledcWrite(CH_B, b);
}
void applyIndex(int idx) {
  idx = constrain(idx, 0, (int)COLOR_COUNT-1);
  rgbSet(COLORS[idx].r, COLORS[idx].g, COLORS[idx].b);
}

/* ========== IR SEND ========= */
void sendIndexIR(int idx) {
  idx = constrain(idx, 0, (int)COLOR_COUNT-1);
  uint32_t code = COLORS[idx].code;
  if (code == 0) {
    Serial.printf("No IR code for %s (skip)\n", COLORS[idx].key);
    return;
  }
  irsend.sendNEC(code, 32); // adjust protocol if not NEC
  delay(40);
  Serial.printf("Sent IR for %s (%s): 0x%08lX\n", COLORS[idx].key, COLORS[idx].label, (unsigned long)code);
}

/* ========== HELPERS ========= */
int parseIndexFromKey(const String& s) {
  for (size_t i=0;i<COLOR_COUNT;i++)
    if (s.equalsIgnoreCase(COLORS[i].key)) return (int)i;
  return 0;
}

/* ========== WEB UI ========= */
String buildOptionsHtml() {
  String opt;
  for (size_t i=0;i<COLOR_COUNT;i++) {
    opt += "<option value=\"";
    opt += COLORS[i].key;
    opt += "\">";
    opt += COLORS[i].label;
    opt += "</option>";
  }
  return opt;
}

String buildCodesText() {
  String t;
  char line[128];
  for (size_t i=0;i<COLOR_COUNT;i++) {
    if (COLORS[i].code == 0)
      snprintf(line, sizeof(line), "%-14s : (0x00000000 / pending)\n", COLORS[i].key);
    else
      snprintf(line, sizeof(line), "%-14s : 0x%08lX\n", COLORS[i].key, (unsigned long)COLORS[i].code);
    t += line;
  }
  return t;
}

const char HTML_HEAD[] PROGMEM = R"HTML(
<!doctype html>
<html lang="vi">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>ESP32 IR RGB – Auto/Manual</title>
<style>
 body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;margin:16px}
 .card{border:1px solid #ddd;border-radius:12px;padding:16px;margin-bottom:16px}
 .row{display:flex;gap:12px;flex-wrap:wrap}
 button,select{font-size:16px;padding:8px;border-radius:8px;border:1px solid #ccc}
 .pill{display:inline-block;padding:6px 10px;border-radius:999px;background:#eee;margin-right:8px}
 .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px}
 .badge{padding:4px 8px;border-radius:6px;background:#eef}
 .muted{color:#666}
</style>
</head>
<body>
<h2>ESP32 – IR RGB Controller</h2>
)HTML";

String buildPage() {
  String html = HTML_HEAD;
  html += R"HTML(
<div class="card">
  <div class="row">
    <div>Chế độ hiện tại: <span id="mode" class="badge">...</span></div>
    <button onclick="setMode('auto')">Auto</button>
    <button onclick="setMode('manual')">Manual</button>
  </div>
  <div class="muted" style="margin-top:8px">Auto: theo nhiệt độ phòng. Manual: chọn màu/hiệu ứng để phát IR & đổi RGB.</div>
</div>

<div class="card">
  <h3>Trạng thái</h3>
  <div class="grid">
    <div>Nhiệt độ: <b id="temp">...</b> °C</div>
    <div>Độ ẩm: <b id="hum">...</b> %</div>
    <div>Mục tiêu: <span id="color" class="pill">...</span></div>
  </div>
</div>

<div class="card">
  <h3>Manual: chọn màu/hiệu ứng</h3>
  <div class="row">
    <select id="colorSelect">
)HTML";
  html += buildOptionsHtml();
  html += R"HTML(
    </select>
    <button onclick="setColor()">Đổi (phát IR)</button>
  </div>
</div>

<div class="card">
  <h3>Danh sách IR codes (NEC 32-bit)</h3>
  <pre id="codes" style="background:#111;color:#0f0;padding:8px;border-radius:8px;white-space:pre-wrap"></pre>
</div>

<script>
async function fetchStatus(){
  const r = await fetch('/api/status');
  const j = await r.json();
  document.getElementById('mode').textContent = j.mode;
  document.getElementById('temp').textContent = (j.temp ?? '--');
  document.getElementById('hum').textContent = (j.hum ?? '--');
  document.getElementById('color').textContent = j.target;
  document.getElementById('codes').textContent = j.codes_text;
}
async function setMode(m){
  await fetch('/api/mode?mode='+m);
  fetchStatus();
}
async function setColor(){
  const c = document.getElementById('colorSelect').value;
  await fetch('/api/setColor?key='+encodeURIComponent(c));
  fetchStatus();
}
setInterval(fetchStatus, 2000);
fetchStatus();
</script>
</body></html>
)HTML";
  return html;
}

/* ========== API HANDLERS ========= */
void handleRoot(){ server.send(200, "text/html; charset=utf-8", buildPage()); }

void handleStatus(){
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  int idxShow = currentIdxManual;
  if (currentMode == AUTO) {
    idxShow = bandIndexForTemp(t);
    applyIndex(idxShow);
  }

  String json = "{";
  json += "\"mode\":\"" + String(currentMode==AUTO?"auto":"manual") + "\",";
  if (isfinite(t)) json += "\"temp\":" + String(t,1) + ","; else json += "\"temp\":null,";
  if (isfinite(h)) json += "\"hum\":" + String(h,0) + ","; else json += "\"hum\":null,";
  json += "\"target\":\"" + String(COLORS[idxShow].label) + "\",";
  // codes text
  String codes = buildCodesText();
  json += "\"codes_text\":\"";
  for (size_t i=0;i<codes.length();++i) json += (codes[i]=='\n' ? "\\n" : String(codes[i]));
  json += "\"}";
  server.send(200, "application/json; charset=utf-8", json);
}

void handleMode(){
  String m = server.hasArg("mode") ? server.arg("mode") : "";
  if (m.equalsIgnoreCase("auto")) currentMode = AUTO;
  else if (m.equalsIgnoreCase("manual")) currentMode = MANUAL;
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleSetColor(){
  if (currentMode != MANUAL) currentMode = MANUAL;
  String key = server.hasArg("key") ? server.arg("key") : "bluel";
  int idx = parseIndexFromKey(key);
  currentIdxManual = idx;
  applyIndex(currentIdxManual);
  sendIndexIR(currentIdxManual);
  server.send(200, "text/plain; charset=utf-8", "OK");
}

/* ========== SETUP / LOOP ========= */
void setup() {
  Serial.begin(115200); delay(200);

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to %s", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.printf("\nWiFi OK: %s\n", WiFi.localIP().toString().c_str());
  if (MDNS.begin(MDNS_NAME)) {
    Serial.printf("mDNS: http://%s.local\n", MDNS_NAME);
  }

  // PWM
  ledcSetup(CH_R, PWM_FREQ, PWM_RES);
  ledcSetup(CH_G, PWM_FREQ, PWM_RES);
  ledcSetup(CH_B, PWM_FREQ, PWM_RES);
  ledcAttachPin(PIN_R, CH_R);
  ledcAttachPin(PIN_G, CH_G);
  ledcAttachPin(PIN_B, CH_B);
  rgbSet(0,0,0);

  // Sensors & IR
  dht.begin();
  irsend.begin();

  // Web routes
  server.on("/", handleRoot);
  server.on("/api/status", handleStatus);
  server.on("/api/mode", handleMode);
  server.on("/api/setColor", handleSetColor);
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();

  // AUTO cycle: update RGB by temperature, send IR only when band changes
  static uint32_t lastAuto = 0;
  static int lastBand = -1;
  if (currentMode == AUTO && millis() - lastAuto > 1500) {
    lastAuto = millis();
    float t = dht.readTemperature();
    if (isfinite(t)) {
      int band = bandIndexForTemp(t);
      applyIndex(band);
      if (band != lastBand) {
        sendIndexIR(band);
        lastBand = band;
      }
    } else {
      Serial.println("DHT read failed in AUTO");
    }
  }
}
