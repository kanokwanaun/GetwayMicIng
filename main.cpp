#include <Arduino.h>

/*
  ClapTuya — ตบมือ N ครั้ง เพื่อสั่งอุปกรณ์ Tuya / Smart Life ผ่าน Tuya Cloud
  -----------------------------------------------------------------------------
  บอร์ด: ESP32-WROOM-32E + CH340C (บอร์ด ClapLight ที่ตัดส่วนไฟบ้าน 220V ออก)
  PlatformIO: env esp32dev (ดู platformio.ini) ไม่ต้องลงไลบรารีเพิ่ม

  ขาที่ใช้:
    IO27 (U2 ขา 12) <- CLAP : ปกติ HIGH, ได้ยินเสียงดังเป็น LOW (มาจาก LM358 + Q1)
    IO26 (U2 ขา 11)         : ไม่ใช้แล้ว ตั้งเป็น LOW ไว้ไม่ให้วงจรรีเลย์เดิมทำงาน

  ใช้งานครั้งแรก:
    1) อัปโหลดโค้ด แล้วใช้มือถือต่อ Wi-Fi "ClapTuya-Setup" (รหัส 12345678)
    2) เปิด http://192.168.4.1 ใส่ชื่อ/รหัส Wi-Fi บ้าน กดบันทึก บอร์ดจะรีสตาร์ต
    3) ต่อมือถือกลับ Wi-Fi บ้าน เปิด http://claptuya.local (หรือ IP ที่ขึ้นใน Serial Monitor)
    4) ใส่ Access ID, Access Secret, UID จาก Tuya IoT Platform แล้วกด "ทดสอบเชื่อม Tuya"
    5) กด "ดึงรายการอุปกรณ์" > "เพิ่มเป็นกฎ" ตั้งจำนวนครั้งที่ตบ แล้วกดบันทึก

  ข้อมูลทั้งหมด (Wi-Fi, คีย์ Tuya, กฎ) เก็บในหน่วยความจำ ESP32 ไม่หายตอนถอดไฟ
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <sys/time.h>
#include <vector>
#include <algorithm>
#include "mbedtls/md.h"

// ---------------- ขาและค่าคงที่ ----------------
const int PIN_CLAP  = 27;
const int PIN_RELAY = 26;

const char* AP_SSID  = "ClapTuya-Setup";
const char* AP_PASS  = "12345678";
const char* HOSTNAME = "claptuya";

const uint32_t PULSE_GAP_MS   = 120;   // พัลส์ที่ห่างกันน้อยกว่านี้ = ตบครั้งเดียวกัน (เสียงก้อง)
const uint32_t MIN_BETWEEN_MS = 150;   // ระยะห่างต่ำสุดระหว่างการตบแต่ละครั้ง
const uint32_t STUCK_MS       = 400;   // CLAP เป็น LOW ค้างนานกว่านี้ = เสียงรบกวน / RV1 ไวเกิน
const int      MAX_RULES      = 8;
const int      MAX_CLAPS      = 6;

// ---------------- การตั้งค่า ----------------
struct Config {
  String ssid, pass;
  String host = "openapi-sg.iotbing.com";   // Singapore data center (ตรงกับโปรเจกต์ Tuya)
  String cid, csec, uid;
  uint16_t gapMs = 700;                 // ถ้าเงียบเกินเวลานี้ = นับจบ
  String rulesRaw;                      // แต่ละบรรทัด: ครั้ง|action|target|code|ชื่อ
} cfg;

struct Rule {
  uint8_t claps = 0;
  String action;   // toggle | on | off | scene
  String target;   // Device ID หรือ home_id/scene_id
  String code;     // DP code เช่น switch_1, switch_led
  String name;
};

Rule rules[MAX_RULES];
int ruleCount = 0;

Preferences prefs;
WebServer server(80);
bool apMode = false;

// ---------------- สถานะการตบ ----------------
volatile uint32_t isrLastPulse = 0;
volatile uint8_t  isrPending   = 0;
uint8_t  clapCount  = 0;
uint32_t lastClapAt = 0;
uint32_t lowSince   = 0;
uint8_t  lastClaps  = 0;

void IRAM_ATTR onClapEdge() {
  uint32_t now = millis();
  if (now - isrLastPulse >= PULSE_GAP_MS) {
    if (isrPending < 255) isrPending++;
  }
  isrLastPulse = now;
}

// ---------------- Log ----------------
const int LOG_N = 20;
String logs[LOG_N];
int logStart = 0, logLen = 0;

bool timeValid() { return time(nullptr) > 1700000000; }

void addLog(const String& msg) {
  String line;
  if (timeValid()) {
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char b[16];
    strftime(b, sizeof b, "%H:%M:%S ", &tmv);
    line = b;
  } else {
    line = String(millis() / 1000) + "s ";
  }
  line += msg;
  Serial.println(line);
  if (logLen < LOG_N) {
    logs[(logStart + logLen) % LOG_N] = line;
    logLen++;
  } else {
    logs[logStart] = line;
    logStart = (logStart + 1) % LOG_N;
  }
}

// ---------------- JSON ช่วยเล็ก ๆ ----------------
String jsonEsc(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

String jsonStr(const String& j, const char* key) {
  String k = String("\"") + key + "\":\"";
  int p = j.indexOf(k);
  if (p < 0) return "";
  p += k.length();
  int e = j.indexOf('"', p);
  return e < 0 ? "" : j.substring(p, e);
}

long jsonNum(const String& j, const char* key) {
  String k = String("\"") + key + "\":";
  int p = j.indexOf(k);
  if (p < 0) return 0;
  return j.substring(p + k.length(), p + k.length() + 12).toInt();
}

String shortMsg(const String& r) {
  String m = jsonStr(r, "msg");
  if (m.length()) {
    long c = jsonNum(r, "code");
    return c ? m + " (" + String(c) + ")" : m;
  }
  return r.substring(0, 80);
}

String errJson(const String& msg) {
  return "{\"success\":false,\"msg\":\"" + jsonEsc(msg) + "\"}";
}

// ---------------- เก็บ/โหลดการตั้งค่า ----------------
void parseRules() {
  ruleCount = 0;
  const String& raw = cfg.rulesRaw;
  int start = 0;
  while (start < (int)raw.length() && ruleCount < MAX_RULES) {
    int nl = raw.indexOf('\n', start);
    if (nl < 0) nl = raw.length();
    String line = raw.substring(start, nl);
    start = nl + 1;
    line.trim();
    if (!line.length()) continue;

    String f[5];
    int s = 0;
    for (int k = 0; k < 5; k++) {
      int bar = (k < 4) ? line.indexOf('|', s) : -1;
      if (bar < 0) { f[k] = line.substring(s); break; }
      f[k] = line.substring(s, bar);
      s = bar + 1;
    }
    Rule r;
    r.claps  = f[0].toInt();
    r.action = f[1];
    r.target = f[2];
    r.code   = f[3];
    r.name   = f[4];
    r.target.trim(); r.code.trim(); r.name.trim();
    bool okAction = r.action == "toggle" || r.action == "on" || r.action == "off" || r.action == "scene";
    if (r.claps >= 1 && r.claps <= MAX_CLAPS && okAction && r.target.length()) rules[ruleCount++] = r;
  }
}

void loadConfig() {
  prefs.begin("claptuya", true);
  cfg.ssid     = prefs.getString("ssid", "");
  cfg.pass     = prefs.getString("pass", "");
  cfg.host     = prefs.getString("host", "openapi-sg.iotbing.com");
  cfg.cid      = prefs.getString("cid", "");
  cfg.csec     = prefs.getString("csec", "");
  cfg.uid      = prefs.getString("uid", "");
  cfg.gapMs    = prefs.getUShort("gap", 700);
  cfg.rulesRaw = prefs.getString("rules", "");
  prefs.end();
}

void saveConfig() {
  prefs.begin("claptuya", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("host", cfg.host);
  prefs.putString("cid", cfg.cid);
  prefs.putString("csec", cfg.csec);
  prefs.putString("uid", cfg.uid);
  prefs.putUShort("gap", cfg.gapMs);
  prefs.putString("rules", cfg.rulesRaw);
  prefs.end();
}

// ---------------- Tuya Cloud (ลายเซ็น HMAC-SHA256) ----------------
String tuyaToken;
uint32_t tokenExpireAt = 0;
String lastTokenErr;

String toHex(const uint8_t* d, size_t n, bool upper) {
  const char* h = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  String o;
  o.reserve(n * 2);
  for (size_t i = 0; i < n; i++) { o += h[d[i] >> 4]; o += h[d[i] & 15]; }
  return o;
}

String sha256Hex(const String& s) {
  uint8_t out[32];
  mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
             (const unsigned char*)s.c_str(), s.length(), out);
  return toHex(out, 32, false);
}

String hmacUpper(const String& key, const String& msg) {
  uint8_t out[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                  (const unsigned char*)key.c_str(), key.length(),
                  (const unsigned char*)msg.c_str(), msg.length(), out);
  return toHex(out, 32, true);
}

String nowMsStr() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  unsigned long long ms = (unsigned long long)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
  char b[24];
  snprintf(b, sizeof b, "%llu", ms);
  return b;
}

// Tuya ให้เรียงพารามิเตอร์ใน query ตามตัวอักษรก่อนเซ็น
String canonicalPath(const String& p) {
  int q = p.indexOf('?');
  if (q < 0) return p;
  String qs = p.substring(q + 1);
  std::vector<String> parts;
  int s = 0;
  while (s <= (int)qs.length()) {
    int a = qs.indexOf('&', s);
    if (a < 0) a = qs.length();
    if (a > s) parts.push_back(qs.substring(s, a));
    s = a + 1;
  }
  std::sort(parts.begin(), parts.end(),
            [](const String& a, const String& b) { return strcmp(a.c_str(), b.c_str()) < 0; });
  String out = p.substring(0, q) + "?";
  for (size_t i = 0; i < parts.size(); i++) { if (i) out += "&"; out += parts[i]; }
  return out;
}

// คืนค่า HTTP code, resp เป็น JSON เสมอ
int tuyaRaw(const String& method, const String& path, const String& body, bool withToken, String& resp) {
  String cpath = canonicalPath(path);
  String t     = nowMsStr();
  String sts   = method + "\n" + sha256Hex(body) + "\n\n" + cpath;
  String sign  = hmacUpper(cfg.csec, cfg.cid + (withToken ? tuyaToken : String()) + t + sts);

  WiFiClientSecure client;
  client.setInsecure();   // ไม่ตรวจใบรับรอง (โปรเจกต์เรียน) ถ้าจะใช้จริงควรใส่ root CA
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, "https://" + cfg.host + cpath)) {
    resp = errJson("เชื่อมต่อ " + cfg.host + " ไม่ได้");
    return -1;
  }
  http.addHeader("client_id", cfg.cid);
  http.addHeader("sign", sign);
  http.addHeader("t", t);
  http.addHeader("sign_method", "HMAC-SHA256");
  http.addHeader("lang", "en");
  if (withToken) http.addHeader("access_token", tuyaToken);

  int code;
  if (method == "GET") {
    code = http.GET();
  } else {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(body);
  }
  if (code > 0) resp = http.getString();
  else          resp = errJson("HTTP " + http.errorToString(code));
  http.end();
  return code;
}

bool ensureToken() {
  if (tuyaToken.length() && (int32_t)(tokenExpireAt - millis()) > 60000) return true;
  tuyaToken = "";
  if (apMode || WiFi.status() != WL_CONNECTED) { lastTokenErr = "ยังไม่ได้ต่อ Wi-Fi บ้าน"; return false; }
  if (!cfg.cid.length() || !cfg.csec.length()) { lastTokenErr = "ยังไม่ได้ใส่ Access ID / Secret"; return false; }
  if (!timeValid()) { lastTokenErr = "ยังไม่ได้เวลาจากอินเทอร์เน็ต (NTP) รอสักครู่"; return false; }

  String r;
  int c = tuyaRaw("GET", "/v1.0/token?grant_type=1", "", false, r);
  if (c != 200 || r.indexOf("\"success\":true") < 0) {
    lastTokenErr = "ขอ token ไม่ได้: " + shortMsg(r);
    return false;
  }
  tuyaToken = jsonStr(r, "access_token");
  long exp = jsonNum(r, "expire_time");
  if (exp <= 0) exp = 3600;
  tokenExpireAt = millis() + (uint32_t)exp * 1000UL;
  if (!tuyaToken.length()) { lastTokenErr = "ไม่พบ access_token ในคำตอบ"; return false; }
  return true;
}

bool tuyaCall(const String& method, const String& path, const String& body, String& resp) {
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!ensureToken()) { resp = errJson(lastTokenErr); return false; }
    int c = tuyaRaw(method, path, body, true, resp);
    if (c == 200 && resp.indexOf("\"success\":true") >= 0) return true;
    // token หมดอายุ/ใช้ไม่ได้ -> ขอใหม่แล้วลองอีกครั้ง
    if (resp.indexOf("\"code\":1010") >= 0 || resp.indexOf("\"code\":1011") >= 0) { tuyaToken = ""; continue; }
    return false;
  }
  return false;
}

// อ่านค่า true/false ของ DP code จาก /status
bool getDpBool(const String& dev, const String& code, bool& val, String& err) {
  String r;
  if (!tuyaCall("GET", "/v1.0/devices/" + dev + "/status", "", r)) { err = "อ่านสถานะไม่ได้: " + shortMsg(r); return false; }
  String key = "\"code\":\"" + code + "\"";
  int p = r.indexOf(key);
  if (p < 0) { err = "อุปกรณ์นี้ไม่มี DP code " + code; return false; }
  int v = r.indexOf("\"value\":", p);
  int e = r.indexOf('}', p);
  if (v < 0 || (e >= 0 && v > e)) { err = "อ่านค่า " + code + " ไม่ได้"; return false; }
  v += 8;
  while (v < (int)r.length() && r[v] == ' ') v++;
  if (r.startsWith("true", v))  { val = true;  return true; }
  if (r.startsWith("false", v)) { val = false; return true; }
  err = code + " ไม่ใช่ค่าเปิด/ปิด";
  return false;
}

bool runRule(const Rule& r) {
  String label = r.name.length() ? r.name : r.target;
  String resp;

  if (r.action == "scene") {
    int sl = r.target.indexOf('/');
    if (sl < 1) { addLog("  " + label + ": ฉากต้องเขียนแบบ home_id/scene_id"); return false; }
    String path = "/v1.0/homes/" + r.target.substring(0, sl) + "/scenes/" + r.target.substring(sl + 1) + "/trigger";
    bool ok = tuyaCall("POST", path, "", resp);
    addLog("  " + label + (ok ? ": สั่งฉากแล้ว" : ": สั่งฉากไม่สำเร็จ " + shortMsg(resp)));
    return ok;
  }

  String code = r.code.length() ? r.code : String("switch_1");
  bool val;
  if (r.action == "toggle") {
    bool cur;
    String err;
    if (!getDpBool(r.target, code, cur, err)) { addLog("  " + label + ": " + err); return false; }
    val = !cur;
  } else {
    val = (r.action == "on");
  }
  String body = "{\"commands\":[{\"code\":\"" + code + "\",\"value\":" + (val ? "true" : "false") + "}]}";
  bool ok = tuyaCall("POST", "/v1.0/devices/" + r.target + "/commands", body, resp);
  addLog("  " + label + (ok ? (val ? " → เปิด" : " → ปิด") : ": สั่งไม่สำเร็จ " + shortMsg(resp)));
  return ok;
}

void handleClaps(uint8_t n) {
  lastClaps = n;
  bool any = false;
  for (int i = 0; i < ruleCount; i++) if (rules[i].claps == n) { any = true; break; }
  if (n == 1 && !any) return;   // ตบครั้งเดียวที่ไม่มีกฎ ไม่ต้องบันทึก (กันเสียงรบกวนรก log)

  addLog("ตบ " + String(n) + " ครั้ง");
  if (!any) addLog("  ไม่มีกฎสำหรับ " + String(n) + " ครั้ง");
  for (int i = 0; i < ruleCount; i++) if (rules[i].claps == n) runRule(rules[i]);

  // ระหว่างรอ Tuya ตอบ อาจมีพัลส์ค้าง ล้างทิ้ง
  noInterrupts();
  isrPending = 0;
  interrupts();
  clapCount = 0;
}

// ---------------- หน้าเว็บ ----------------
const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang="th"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>ClapTuya</title><style>
:root{--bg:#f2f3ef;--card:#fff;--ink:#1c2320;--mut:#67716c;--line:#d8dcd4;--acc:#d9572a;--ok:#2b8552;--bad:#c23d2c}
@media(prefers-color-scheme:dark){:root{--bg:#111413;--card:#1a1e1c;--ink:#e5e9e5;--mut:#9aa39e;--line:#2c3330;--acc:#ef8551;--ok:#5ac088;--bad:#ef7765}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.5 system-ui,-apple-system,"Segoe UI",Tahoma,sans-serif}
main{max-width:780px;margin:0 auto;padding:16px;display:grid;gap:14px}
h1{margin:0;font-size:26px}h2{margin:0 0 4px;font-size:17px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px;display:grid;gap:10px}
.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}.mut{color:var(--mut);font-size:13px}
input,select,button{font:inherit;padding:7px 9px;border:1px solid var(--line);border-radius:7px;background:var(--card);color:var(--ink);min-width:0;width:100%}
.row button{width:auto}button{cursor:pointer}button.p{background:var(--acc);border-color:var(--acc);color:#fff}
label{display:grid;gap:3px;font-size:13px;color:var(--mut)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:10px}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:10px}
.big{font-size:42px;font-weight:700;line-height:1}
#log{font:12.5px/1.5 ui-monospace,Menlo,Consolas,monospace;max-height:230px;overflow:auto;white-space:pre-wrap;background:var(--bg);border-radius:7px;padding:8px;margin:0}
.rule{display:grid;grid-template-columns:1.3fr 62px 128px 1.7fr 1fr auto auto;gap:6px;align-items:center;padding-top:8px;border-top:1px solid var(--line)}
.rule button{width:auto}
@media(max-width:680px){.rule{grid-template-columns:1fr 1fr}.rule .w{grid-column:1/-1}}
.dot{display:inline-block;width:12px;height:12px;border-radius:50%;background:var(--line);vertical-align:middle}.dot.on{background:var(--acc)}
.chip{display:inline-block;font-size:12px;border:1px solid var(--line);border-radius:5px;padding:0 6px;margin:2px 4px 0 0}
table{width:100%;border-collapse:collapse;font-size:14px}td{padding:7px 4px;border-top:1px solid var(--line);vertical-align:top}td:last-child{text-align:right}
td button{width:auto}.ok{color:var(--ok)}.bad{color:var(--bad)}
</style></head><body><main>
<header><div class="mut">ESP32 · Tuya Cloud</div><h1>ClapTuya</h1></header>

<section class="card">
<div class="stats">
<div><div class="mut">ตบล่าสุด (ครั้ง)</div><div class="big" id="last">–</div></div>
<div><div class="mut">ไมค์ได้ยินเสียง</div><span class="dot" id="mic"></span></div>
<div><div class="mut">Wi-Fi</div><div id="wifi">…</div></div>
<div><div class="mut">Tuya</div><div id="tuya">…</div></div>
</div>
<div class="row"><span class="mut">จำลองการตบ:</span>
<button onclick="sim(1)">1</button><button onclick="sim(2)">2</button><button onclick="sim(3)">3</button>
<button onclick="sim(4)">4</button><button onclick="sim(5)">5</button><button onclick="sim(6)">6</button></div>
<pre id="log"></pre>
</section>

<section class="card">
<div><h2>กฎการตบ</h2><div class="mut">ตบกี่ครั้ง → ทำอะไร ใส่จำนวนครั้งซ้ำกันได้ถ้าอยากสั่งหลายอย่างพร้อมกัน สูงสุด 8 กฎ</div></div>
<div id="rules"></div>
<div class="row"><button onclick="addRule()">+ เพิ่มกฎ</button><button class="p" onclick="save()">บันทึก</button><span id="saved" class="mut"></span></div>
</section>

<section class="card">
<div><h2>อุปกรณ์และฉากในบัญชี Tuya</h2><div class="mut">ต้องใส่ UID และบันทึกก่อน ฉาก Tap-to-Run ต้องเปิด API "Smart Home Scene Linkage" ในโปรเจกต์</div></div>
<div class="row"><button onclick="loadDevices()">ดึงรายการอุปกรณ์</button><button onclick="loadScenes()">ดึงฉาก Tap-to-Run</button></div>
<div id="found"></div>
</section>

<section class="card">
<h2>ตั้งค่า</h2>
<div class="grid">
<label>ชื่อ Wi-Fi บ้าน<input id="ssid" autocomplete="off"></label>
<label>รหัส Wi-Fi<input id="pass" type="password" placeholder="เว้นว่าง = ใช้รหัสเดิม"></label>
<label>Data center<select id="host">
<option value="openapi-sg.iotbing.com">Singapore</option>
<option value="openapi.tuyaus.com">Western America</option>
<option value="openapi-ueaz.tuyaus.com">Eastern America</option>
<option value="openapi.tuyaeu.com">Central Europe</option>
<option value="openapi-weaz.tuyaeu.com">Western Europe</option>
<option value="openapi.tuyain.com">India</option>
<option value="openapi.tuyacn.com">China</option></select></label>
<label>Access ID / Client ID<input id="cid" autocomplete="off"></label>
<label>Access Secret<input id="csec" type="password" autocomplete="off"></label>
<label>UID (จากเมนู Link App Account)<input id="uid" autocomplete="off"></label>
<label>รอการตบครั้งถัดไปนานสุด (ms)<input id="gap" type="number" min="300" max="2000" step="50"></label>
</div>
<div class="row"><button class="p" onclick="save()">บันทึก</button><button onclick="tok()">ทดสอบเชื่อม Tuya</button><span id="tokr" class="mut"></span></div>
</section>
</main><script>
const $=id=>document.getElementById(id);
const esc=s=>String(s??'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const ACT={toggle:'สลับ เปิด/ปิด',on:'เปิด',off:'ปิด',scene:'สั่งฉาก'};
const PREF=['switch_led','switch_1','switch','switch_2','switch_3','switch_4'];
let rules=[],devs=[],scenes=[];
function renderRules(){
 $('rules').innerHTML=rules.map((r,i)=>`<div class="rule">
<input class="w" value="${esc(r.name)}" placeholder="ชื่อ เช่น ไฟห้องนอน" oninput="rules[${i}].name=this.value">
<select onchange="rules[${i}].claps=+this.value">${[1,2,3,4,5,6].map(n=>`<option ${n==r.claps?'selected':''}>${n}</option>`).join('')}</select>
<select onchange="rules[${i}].action=this.value;renderRules()">${Object.keys(ACT).map(k=>`<option value="${k}" ${k==r.action?'selected':''}>${ACT[k]}</option>`).join('')}</select>
<input class="w" value="${esc(r.target)}" placeholder="${r.action=='scene'?'home_id/scene_id':'Device ID'}" oninput="rules[${i}].target=this.value.trim()">
<input value="${esc(r.code)}" placeholder="DP code" ${r.action=='scene'?'disabled':''} oninput="rules[${i}].code=this.value.trim()">
<button onclick="test(${i})">ทดสอบ</button><button onclick="rules.splice(${i},1);renderRules()">ลบ</button></div>`).join('')
 ||'<p class="mut">ยังไม่มีกฎ กด "ดึงรายการอุปกรณ์" แล้วเลือก "เพิ่มเป็นกฎ" หรือกด + เพิ่มกฎ</p>';}
function addRule(r){if(rules.length>=8){alert('ใส่ได้สูงสุด 8 กฎ');return}
 rules.push(Object.assign({name:'',claps:2,action:'toggle',target:'',code:'switch_1'},r||{}));renderRules();}
const clean=s=>String(s||'').replace(/[|\r\n]/g,' ');
const raw=()=>rules.filter(r=>r.target).map(r=>[r.claps,r.action,clean(r.target),clean(r.code),clean(r.name)].join('|')).join('\n');
async function save(){
 const f=new URLSearchParams({ssid:$('ssid').value,pass:$('pass').value,host:$('host').value,cid:$('cid').value.trim(),
  csec:$('csec').value.trim(),uid:$('uid').value.trim(),gap:$('gap').value,rules:raw()});
 try{const j=await(await fetch('/api/config',{method:'POST',body:f})).json();
  $('saved').textContent=j.restart?'บันทึกแล้ว บอร์ดกำลังรีสตาร์ตเพื่อต่อ Wi-Fi ใหม่':'บันทึกแล้ว';
  $('pass').value='';$('csec').value='';if(j.hasSecret)$('csec').placeholder='ตั้งค่าแล้ว (เว้นว่าง = ใช้ค่าเดิม)';
 }catch(e){$('saved').textContent='บันทึกไม่สำเร็จ'}}
async function test(i){const r=rules[i];
 await fetch('/api/run',{method:'POST',body:new URLSearchParams({action:r.action,target:r.target,code:r.code,name:r.name||'ทดสอบ'})});refresh();}
async function sim(n){await fetch('/api/clap?n='+n,{method:'POST'});refresh();}
async function tok(){$('tokr').textContent='กำลังทดสอบ…';
 const j=await(await fetch('/api/tokentest')).json();$('tokr').innerHTML=j.ok?'<span class="ok">เชื่อมต่อได้</span>':'<span class="bad">'+esc(j.msg)+'</span>';refresh();}
async function tuya(p){return(await fetch('/api/tuya?path='+encodeURIComponent(p))).json();}
function needUid(){const u=$('uid').value.trim();if(!u)$('found').innerHTML='<p class="bad">ใส่ UID ในส่วนตั้งค่า แล้วกดบันทึกก่อน</p>';return u;}
const bools=d=>(d.status||[]).filter(s=>typeof s.value==='boolean');
async function loadDevices(){const u=needUid();if(!u)return;$('found').innerHTML='<p class="mut">กำลังโหลด…</p>';
 try{const j=await tuya('/v1.0/users/'+u+'/devices');if(!j.success)throw new Error(j.msg||'ดึงข้อมูลไม่ได้');devs=j.result||[];
  $('found').innerHTML=devs.length?'<table>'+devs.map((d,i)=>`<tr><td><b>${esc(d.name)}</b>
  <div class="mut">${esc(d.id)} · ${d.online?'<span class="ok">ออนไลน์</span>':'<span class="bad">ออฟไลน์</span>'}</div>
  ${bools(d).map(s=>`<span class="chip">${esc(s.code)} = ${s.value}</span>`).join('')||'<span class="mut">ไม่มีสวิตช์เปิด/ปิด</span>'}</td>
  <td>${bools(d).length?`<button onclick="useDev(${i})">เพิ่มเป็นกฎ</button>`:''}</td></tr>`).join('')+'</table>'
  :'<p class="mut">ไม่พบอุปกรณ์ ตรวจว่าเชื่อมบัญชีแอปกับโปรเจกต์แล้ว และเลือก Data center ถูก</p>';
 }catch(e){$('found').innerHTML='<p class="bad">'+esc(e.message)+'</p>'}}
function useDev(i){const d=devs[i],c=bools(d).map(s=>s.code);addRule({name:d.name,target:d.id,code:PREF.find(x=>c.includes(x))||c[0]});}
async function loadScenes(){const u=needUid();if(!u)return;$('found').innerHTML='<p class="mut">กำลังโหลด…</p>';
 try{const h=await tuya('/v1.0/users/'+u+'/homes');if(!h.success)throw new Error(h.msg||'ดึงบ้านไม่ได้');scenes=[];
  for(const hm of h.result||[]){const s=await tuya('/v1.1/homes/'+hm.home_id+'/scenes');
   if(!s.success)throw new Error(s.msg||'ดึงฉากไม่ได้');for(const sc of s.result||[])scenes.push({h:hm.home_id,hn:hm.name,id:sc.scene_id,n:sc.name});}
  $('found').innerHTML=scenes.length?'<table>'+scenes.map((s,i)=>`<tr><td><b>${esc(s.n)}</b><div class="mut">${esc(s.hn)} · ${esc(s.h)}/${esc(s.id)}</div></td>
  <td><button onclick="useScene(${i})">เพิ่มเป็นกฎ</button></td></tr>`).join('')+'</table>':'<p class="mut">ไม่พบฉาก Tap-to-Run ในบัญชีนี้</p>';
 }catch(e){$('found').innerHTML='<p class="bad">'+esc(e.message)+'</p>'}}
function useScene(i){const s=scenes[i];addRule({name:s.n,action:'scene',target:s.h+'/'+s.id,code:''});}
async function refresh(){try{const s=await(await fetch('/api/status')).json();
 $('last').textContent=s.lastClaps||'–';$('mic').className='dot'+(s.mic?' on':'');
 $('wifi').innerHTML=s.mode=='ap'?'<span class="bad">โหมดตั้งค่า</span>':esc(s.ssid)+' <span class="mut">'+s.rssi+' dBm</span>';
 $('tuya').innerHTML=s.token?'<span class="ok">เชื่อมแล้ว</span>':s.configured?'<span class="mut">พร้อม (ยังไม่ได้ขอ token)</span>':'<span class="bad">ยังไม่ตั้งค่า</span>';
 const L=$('log'),end=L.scrollTop+L.clientHeight>=L.scrollHeight-4;L.textContent=s.log.join('\n');if(end)L.scrollTop=L.scrollHeight;}catch(e){}}
async function init(){try{const c=await(await fetch('/api/config')).json();
 $('ssid').value=c.ssid;$('host').value=c.host;$('cid').value=c.cid;$('uid').value=c.uid;$('gap').value=c.gap;
 if(c.hasSecret)$('csec').placeholder='ตั้งค่าแล้ว (เว้นว่าง = ใช้ค่าเดิม)';
 rules=(c.rules||'').split('\n').filter(Boolean).map(l=>{const[a,b,t,d,n]=l.split('|');return{claps:+a,action:b,target:t||'',code:d||'',name:n||''}});
}catch(e){}renderRules();refresh();setInterval(refresh,1500);}
init();
</script></body></html>)HTML";

void sendJson(const String& j) { server.send(200, "application/json; charset=utf-8", j); }

void handleStatus() {
  String j = "{";
  j += "\"mode\":\"" + String(apMode ? "ap" : "sta") + "\",";
  j += "\"ssid\":\"" + jsonEsc(apMode ? String(AP_SSID) : WiFi.SSID()) + "\",";
  j += "\"rssi\":" + String(apMode ? 0 : WiFi.RSSI()) + ",";
  j += "\"ip\":\"" + (apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\",";
  j += "\"token\":" + String(tuyaToken.length() ? "true" : "false") + ",";
  j += "\"configured\":" + String(cfg.cid.length() && cfg.csec.length() ? "true" : "false") + ",";
  j += "\"lastClaps\":" + String(lastClaps) + ",";
  j += "\"mic\":" + String(millis() - isrLastPulse < 1500 ? "true" : "false") + ",";
  j += "\"log\":[";
  for (int i = 0; i < logLen; i++) {
    if (i) j += ",";
    j += "\"" + jsonEsc(logs[(logStart + i) % LOG_N]) + "\"";
  }
  j += "]}";
  sendJson(j);
}

void handleConfigGet() {
  String j = "{";
  j += "\"ssid\":\"" + jsonEsc(cfg.ssid) + "\",";
  j += "\"host\":\"" + jsonEsc(cfg.host) + "\",";
  j += "\"cid\":\"" + jsonEsc(cfg.cid) + "\",";
  j += "\"hasSecret\":" + String(cfg.csec.length() ? "true" : "false") + ",";
  j += "\"uid\":\"" + jsonEsc(cfg.uid) + "\",";
  j += "\"gap\":" + String(cfg.gapMs) + ",";
  j += "\"rules\":\"" + jsonEsc(cfg.rulesRaw) + "\"}";
  sendJson(j);   // ไม่ส่งรหัส Wi-Fi และ Secret ออกไป
}

void handleConfigPost() {
  bool wifiChanged = false, tuyaChanged = false;
  if (server.hasArg("ssid")) {
    String s = server.arg("ssid");
    s.trim();
    if (s != cfg.ssid) { cfg.ssid = s; wifiChanged = true; }
  }
  if (server.arg("pass").length()) { cfg.pass = server.arg("pass"); wifiChanged = true; }
  if (server.hasArg("host")) {
    String h = server.arg("host");
    h.trim();
    if (h.length() && h != cfg.host) { cfg.host = h; tuyaChanged = true; }
  }
  if (server.hasArg("cid")) {
    String c = server.arg("cid");
    c.trim();
    if (c != cfg.cid) { cfg.cid = c; tuyaChanged = true; }
  }
  String sec = server.arg("csec");
  sec.trim();
  if (sec.length()) { cfg.csec = sec; tuyaChanged = true; }
  if (server.hasArg("uid")) { cfg.uid = server.arg("uid"); cfg.uid.trim(); }
  if (server.hasArg("gap")) {
    int g = server.arg("gap").toInt();
    if (g >= 300 && g <= 2000) cfg.gapMs = g;
  }
  if (server.hasArg("rules")) { cfg.rulesRaw = server.arg("rules"); parseRules(); }

  saveConfig();
  if (tuyaChanged) tuyaToken = "";
  addLog("บันทึกการตั้งค่าแล้ว (" + String(ruleCount) + " กฎ)");
  sendJson(String("{\"ok\":true,\"restart\":") + (wifiChanged ? "true" : "false") +
           ",\"hasSecret\":" + (cfg.csec.length() ? "true" : "false") + "}");
  if (wifiChanged) {
    delay(800);
    ESP.restart();
  }
}

bool allowedProxyPath(const String& p) {
  return p.startsWith("/v1.0/users/") || p.startsWith("/v1.1/homes/") ||
         p.startsWith("/v1.0/homes/") || p.startsWith("/v1.0/devices/");
}

void setupWeb() {
  server.on("/", HTTP_GET, []() { server.send(200, "text/html; charset=utf-8", PAGE); });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);

  server.on("/api/clap", HTTP_POST, []() {
    int n = server.arg("n").toInt();
    if (n >= 1 && n <= MAX_CLAPS) handleClaps(n);
    sendJson("{\"ok\":true}");
  });

  server.on("/api/run", HTTP_POST, []() {
    Rule r;
    r.action = server.arg("action");
    r.target = server.arg("target");
    r.code   = server.arg("code");
    r.name   = server.arg("name");
    r.target.trim();
    r.code.trim();
    addLog("ทดสอบกฎ");
    bool ok = r.target.length() ? runRule(r) : false;
    if (!r.target.length()) addLog("  ยังไม่ได้ใส่ Device ID");
    sendJson(String("{\"ok\":") + (ok ? "true" : "false") + "}");
  });

  server.on("/api/tokentest", HTTP_GET, []() {
    tuyaToken = "";
    bool ok = ensureToken();
    addLog(ok ? "เชื่อม Tuya สำเร็จ" : "เชื่อม Tuya ไม่สำเร็จ: " + lastTokenErr);
    sendJson(String("{\"ok\":") + (ok ? "true" : "false") + ",\"msg\":\"" + jsonEsc(ok ? String("") : lastTokenErr) + "\"}");
  });

  // ให้หน้าเว็บดึงรายการอุปกรณ์/ฉากจาก Tuya ผ่าน ESP32 (อ่านอย่างเดียว)
  server.on("/api/tuya", HTTP_GET, []() {
    String path = server.arg("path");
    if (!allowedProxyPath(path)) { sendJson(errJson("path นี้ไม่อนุญาต")); return; }
    String resp;
    tuyaCall("GET", path, "", resp);
    sendJson(resp);
  });

  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();
}

// ---------------- Wi-Fi ----------------
void startWiFi() {
  WiFi.setHostname(HOSTNAME);
  if (cfg.ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
    Serial.print("Connecting Wi-Fi");
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
      delay(300);
      Serial.print('.');
    }
    Serial.println();
  }
  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");
    addLog("ต่อ Wi-Fi แล้ว เปิด http://" + String(HOSTNAME) + ".local หรือ http://" + WiFi.localIP().toString());
  } else {
    apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    addLog("โหมดตั้งค่า: ต่อ Wi-Fi " + String(AP_SSID) + " แล้วเปิด http://" + WiFi.softAPIP().toString());
  }
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
}

// ---------------- setup / loop ----------------
void setup() {
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW);
  pinMode(PIN_CLAP, INPUT);     // มี R9 10k ดึงขึ้น 3V3 บนบอร์ดแล้ว
  Serial.begin(115200);
  delay(200);
  Serial.println("\nClapTuya start");

  loadConfig();
  parseRules();
  startWiFi();
  setupWeb();
  attachInterrupt(digitalPinToInterrupt(PIN_CLAP), onClapEdge, FALLING);
  addLog("พร้อมใช้งาน มี " + String(ruleCount) + " กฎ");
}

void loop() {
  server.handleClient();
  uint32_t now = millis();

  // CLAP ค้าง LOW = เสียงดังต่อเนื่อง หรือ RV1 ตั้งไวเกิน
  bool stuck = false;
  if (digitalRead(PIN_CLAP) == LOW) {
    if (lowSince == 0) lowSince = now ? now : 1;
    if (now - lowSince > STUCK_MS) {
      stuck = true;
      clapCount = 0;
      static uint32_t lastWarn = 0;
      if (now - lastWarn > 5000) { addLog("ไมค์ค้าง: หมุน RV1 ให้ไวน้อยลง"); lastWarn = now; }
    }
  } else {
    lowSince = 0;
  }

  noInterrupts();
  uint8_t pulses = isrPending;
  isrPending = 0;
  interrupts();

  while (pulses--) {
    if (stuck) continue;
    if (clapCount == 0 || now - lastClapAt >= MIN_BETWEEN_MS) {
      if (clapCount < 9) clapCount++;
      lastClapAt = now;
    }
  }

  // เงียบนานเกิน gapMs = ตบจบชุด แล้วไปทำตามกฎ
  if (clapCount > 0 && now - lastClapAt > cfg.gapMs) {
    uint8_t n = clapCount;
    clapCount = 0;
    if (n <= MAX_CLAPS) handleClaps(n);
    else addLog("ตบ " + String(n) + " ครั้ง (เกิน " + String(MAX_CLAPS) + " ไม่ทำอะไร)");
  }
}
