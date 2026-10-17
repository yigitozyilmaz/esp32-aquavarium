#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>

// ESP32 Dev Module / Arduino-ESP32 core 3.x
const char* WIFI_SSID = "HUAWEI-1GH9J9";
const char* WIFI_PASSWORD = "Nilufer17";
constexpr uint8_t PIN_WHITE = 25;
constexpr uint8_t PIN_PINK = 26;
constexpr uint8_t PIN_TEMP = 33; // DS18B20 DQ, 4.7k pull-up to 3V3
constexpr uint32_t PWM_FREQ = 1000;
constexpr uint8_t PWM_BITS = 8;
// BC547 common-emitter stage: HIGH on ESP32 turns LED OFF.
constexpr bool INVERTED_DRIVER = true;

// One three-wire DS18B20, externally powered from ESP32 3V3.
OneWire oneWire(PIN_TEMP);
DallasTemperature sensors(&oneWire);
float waterTemp = NAN;
unsigned long lastTempCycle = 0;
unsigned long lastGoodTemp = 0;
uint32_t temperatureReadings = 0;
uint32_t temperatureFailures = 0;
String tempState = "baslatiliyor";
constexpr unsigned long TEMP_PERIOD_MS = 5000;
constexpr unsigned long TEMP_STALE_MS = 90000;
WebServer server(80);
Preferences prefs;
struct Channel { bool manualOn; uint8_t target; };
Channel white = {false, 80};
Channel pink  = {false, 50};
bool autoMode = true;
int sunriseStart = 8 * 60, sunriseEnd = 10 * 60;
int sunsetStart = 20 * 60, sunsetEnd = 22 * 60;
unsigned long lastTick = 0;
unsigned long lastReconnect = 0;

void output(uint8_t pin, uint8_t brightness) {
  uint32_t duty = map(brightness, 0, 100, 0, 255);
  if (INVERTED_DRIVER) duty = 255 - duty;
  ledcWrite(pin, duty);
}

bool clockNow(struct tm &now) {
  if (!getLocalTime(&now, 5)) return false;
  return (now.tm_year + 1900) >= 2025;
}

int parseTime(String s, int fallback) {
  if (s.length() != 5 || s.charAt(2) != ':') return fallback;
  for (int i : {0, 1, 3, 4}) if (!isDigit(s[i])) return fallback;
  int h = s.substring(0,2).toInt(), m = s.substring(3,5).toInt();
  if (h > 23 || m > 59) return fallback;
  return h*60+m;
}
String fmtTime(int mins) {
  char b[6]; snprintf(b, sizeof(b), "%02d:%02d", mins/60, mins%60);
  return String(b);
}

int factorPercent(int minute) {
  if (minute < sunriseStart) return 0;
  if (minute < sunriseEnd) return 100L * (minute - sunriseStart) / (sunriseEnd - sunriseStart);
  if (minute < sunsetStart) return 100;
  if (minute < sunsetEnd) return 100L * (sunsetEnd - minute) / (sunsetEnd - sunsetStart);
  return 0;
}

void updateLights() {
  int factor = 0;
  struct tm t;
  bool valid = clockNow(t);
  if (valid) factor = factorPercent(t.tm_hour * 60 + t.tm_min);
  // Fail OFF if network time hasn't synchronized in automatic mode.
  uint8_t w = autoMode ? white.target * factor / 100 : (white.manualOn ? white.target : 0);
  uint8_t p = autoMode ? pink.target * factor / 100 : (pink.manualOn ? pink.target : 0);
  output(PIN_WHITE, w);
  output(PIN_PINK, p);
}

void saveSettings() {
  prefs.putBool("auto", autoMode);
  prefs.putUChar("wlev", white.target);
  prefs.putUChar("plev", pink.target);
  prefs.putUShort("ss", sunriseStart);
  prefs.putUShort("se", sunriseEnd);
  prefs.putUShort("ds", sunsetStart);
  prefs.putUShort("de", sunsetEnd);
}

void tempFailure(const char *why) {
  temperatureFailures++;
  tempState = why;
  Serial.printf("DS18B20 HATA: %s (toplam %lu)\n", why, (unsigned long)temperatureFailures);
}

void updateTemperature() {
  const unsigned long ms = millis();
  if (lastTempCycle != 0 && ms - lastTempCycle < TEMP_PERIOD_MS) return;
  lastTempCycle = ms == 0 ? 1 : ms;

  // Standard DallasTemperature transaction: trigger conversion, wait up to
  // 750 ms for 12-bit data, and read the one attached sensor.
  // Rediscover devices every cycle; never cache a failed address.
  sensors.begin();
  if (sensors.getDeviceCount() == 0) {
    tempFailure("DS18B20 algilanamadi (1-Wire cevap yok)");
  } else {
    sensors.requestTemperatures();
    float v = sensors.getTempCByIndex(0);
    if (v == DEVICE_DISCONNECTED_C || isnan(v) || v < -40.0f || v >= 85.0f) {
      tempFailure("olcum okunamadi veya gecersiz");
    } else {
      waterTemp = v;
      lastGoodTemp = millis();
      temperatureReadings++;
      tempState = "olcum basarili";
      Serial.printf("DS18B20: %.2f C (olcum %lu)\n", v,
                    (unsigned long)temperatureReadings);
    }
  }
  if (lastGoodTemp == 0 || millis() - lastGoodTemp > TEMP_STALE_MS)
    waterTemp = NAN;
}

String jsonStatus() {
  struct tm now;
  bool valid = clockNow(now);
  int factor = valid ? factorPercent(now.tm_hour*60 + now.tm_min) : 0;
  char clockStr[9] = "--:--:--";
  if (valid) snprintf(clockStr, sizeof(clockStr), "%02d:%02d:%02d", now.tm_hour, now.tm_min, now.tm_sec);
  String j = "{";
  j += "\"auto\":" + String(autoMode ? "true" : "false");
  j += ",\"timeOk\":" + String(valid ? "true" : "false");
  j += ",\"clock\":\"" + String(clockStr) + "\"";
  j += ",\"temperature\":";
  if (isnan(waterTemp)) j += "null"; else j += String(waterTemp, 1);
  j += ",\"tempAgeSec\":" + String(lastGoodTemp ? (long)((millis()-lastGoodTemp)/1000) : -1);
  j += ",\"tempErrors\":" + String(temperatureFailures);
  j += ",\"tempReadings\":" + String(temperatureReadings);
  j += ",\"tempState\":\"" + tempState + "\"";
  j += ",\"wifi\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
  j += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  j += ",\"sunriseStart\":\"" + fmtTime(sunriseStart) + "\"";
  j += ",\"sunriseEnd\":\"" + fmtTime(sunriseEnd) + "\"";
  j += ",\"sunsetStart\":\"" + fmtTime(sunsetStart) + "\"";
  j += ",\"sunsetEnd\":\"" + fmtTime(sunsetEnd) + "\"";
  j += ",\"white\":{\"on\":" + String(white.manualOn ? "true" : "false") + ",\"target\":" + String(white.target) + ",\"actual\":" + String(autoMode ? white.target*factor/100 : (white.manualOn ? white.target : 0)) + "}";
  j += ",\"pink\":{\"on\":" + String(pink.manualOn ? "true" : "false") + ",\"target\":" + String(pink.target) + ",\"actual\":" + String(autoMode ? pink.target*factor/100 : (pink.manualOn ? pink.target : 0)) + "}";
  j += "}";
  return j;
}

const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="tr"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Aqua Control</title><style>
*{box-sizing:border-box}body{font-family:system-ui,Arial;background:#091523;color:#eff7ff;margin:0;padding:22px}main{max-width:720px;margin:auto}.card{background:#16283d;border:1px solid #29415a;border-radius:18px;padding:20px;margin:16px 0}h1{margin-bottom:4px}.temp{font-size:36px;font-weight:750;color:#63e4cc;margin:8px 0}h2{margin:0 0 12px}.muted{color:#abc1d7}.row{display:flex;justify-content:space-between;gap:15px;align-items:center;flex-wrap:wrap}.pill{background:#274a5e;padding:6px 11px;border-radius:20px}.btn{background:#43d5b5;color:#06251d;border:0;padding:12px 18px;border-radius:12px;font-size:15px;font-weight:bold;cursor:pointer}.btn.secondary{background:#334c67;color:white}.btn.off{background:#f6a7b7;color:#391521}.btn:disabled{opacity:.45;cursor:not-allowed}input[type=range]{width:100%;accent-color:#43d5b5}input[type=time]{color:white;background:#263c55;border:1px solid #476078;border-radius:8px;padding:9px}label{display:block;margin-top:8px}.times{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}.sub{font-size:13px;color:#9db7cc}#notice{min-height:22px;color:#ffd69e}
</style></head><body><main><div class="row"><div><h1>🐠 Aqua Control</h1><div class="muted">ESP32 • Akvaryum aydınlatması</div></div><span class="pill" id="clock">--:--:--</span></div>
<div class="card"><div class="row"><h2>🌡️ Su sıcaklığı</h2><span class="pill">DS18B20</span></div><div class="temp" id="temperature">--.- °C</div><div class="sub" id="tempInfo">Sensör bekleniyor…</div></div>
<div class="card"><div class="row"><h2>Çalışma modu</h2><button id="modeBtn" class="btn" onclick="setMode()">Otomatik</button></div><p class="muted">Otomatik: 08:00–10:00 açılır, 20:00–22:00 kapanır. Manuel: düğmelerle kontrol edilir.</p><div class="sub" id="info">Bağlanıyor…</div></div>
<div class="card"><div class="row"><h2>⚪ Beyaz kanal</h2><span class="pill" id="wa">0%</span></div><button id="wb" class="btn secondary" onclick="toggle('white')">Aç / Kapat</button><label>Hedef parlaklık: <b id="wt">80%</b><input id="ws" type="range" min="0" max="100" value="80" oninput="document.getElementById('wt').textContent=this.value+'%'" onchange="setLevel('white',this.value)"></label></div>
<div class="card"><div class="row"><h2>🌸 Pembe kanal</h2><span class="pill" id="pa">0%</span></div><button id="pb" class="btn secondary" onclick="toggle('pink')">Aç / Kapat</button><label>Hedef parlaklık: <b id="pt">50%</b><input id="ps" type="range" min="0" max="100" value="50" oninput="document.getElementById('pt').textContent=this.value+'%'" onchange="setLevel('pink',this.value)"></label></div>
<div class="card"><h2>⏰ Günlük saatler</h2><div class="times"><label>Gün doğumu başlangıcı<input id="sunriseStart" type="time" value="08:00"></label><label>Gün doğumu bitişi<input id="sunriseEnd" type="time" value="10:00"></label><label>Gün batımı başlangıcı<input id="sunsetStart" type="time" value="20:00"></label><label>Gün batımı bitişi<input id="sunsetEnd" type="time" value="22:00"></label></div><p><button class="btn" onclick="saveTimes()">Saatleri kaydet</button></p><div class="sub">Saatler ve hedef parlaklıklar elektrik kesilse de saklanır. Manuel açık/kapalı durumu saklanmaz.</div></div><div id="notice"></div></main>
<script>
let state=null;let isEditing=false;
async function api(path,params={}){let url=path+'?'+new URLSearchParams(params);let r=await fetch(url);let data=await r.json();if(!r.ok)throw new Error(data.error||'İşlem başarısız');return data}
async function send(path,params){try{state=await api(path,params);draw()}catch(e){document.getElementById('notice').textContent=e.message}}
function draw(){if(!state)return;document.getElementById('clock').textContent=state.clock;document.getElementById('temperature').textContent=state.temperature===null?'--.- °C':Number(state.temperature).toFixed(1)+' °C';document.getElementById('tempInfo').textContent=state.temperature===null?'Sensör: '+(state.tempState||'ölçüm alınamıyor')+' • Hata: '+state.tempErrors:(state.tempAgeSec>12?'Son geçerli ölçüm ('+state.tempAgeSec+' sn önce)':'Ölçüm otomatik yenileniyor (5 sn)');document.getElementById('info').textContent=(state.timeOk?'Türkiye saati senkronize':'Saat henüz alınamadı, otomatik ışık kapalı')+' • '+state.ip;document.getElementById('modeBtn').textContent=state.auto?'Otomatik mod':'Manuel mod';for(const [k,prefix] of [['white','w'],['pink','p']]){const d=state[k];document.getElementById(prefix+'a').textContent=d.actual+'%';document.getElementById(prefix+'b').textContent=d.on?'Kapat':'Aç';document.getElementById(prefix+'b').disabled=state.auto;const s=document.getElementById(prefix+'s');if(document.activeElement!==s)s.value=d.target;document.getElementById(prefix+'t').textContent=d.target+'%'}if(!isEditing)for(const key of ['sunriseStart','sunriseEnd','sunsetStart','sunsetEnd'])document.getElementById(key).value=state[key]}
async function refresh(){try{state=await api('/api/status');draw()}catch(e){document.getElementById('info').textContent='Bağlantı kesildi'}}
function setMode(){send('/api/mode',{auto:state.auto?0:1})}
function toggle(k){send('/api/channel',{channel:k,on:state[k].on?0:1})}
function setLevel(k,v){send('/api/channel',{channel:k,level:v})}
function saveTimes(){let params={};for(const key of ['sunriseStart','sunriseEnd','sunsetStart','sunsetEnd'])params[key]=document.getElementById(key).value;send('/api/schedule',params);isEditing=false;document.getElementById('notice').textContent='Saatler kaydedildi.'}
for(const x of document.querySelectorAll('input[type=time]'))x.addEventListener('focus',()=>{isEditing=true});
refresh();setInterval(refresh,2000);
</script></body></html>
)HTML";

void replyStatus() { server.send(200, "application/json", jsonStatus()); }
void bad(const char* message) { server.send(400, "application/json", String("{\"error\":\"") + message + "\"}"); }

void setup() {
  // Set both active-low inputs inactive before establishing PWM.
  digitalWrite(PIN_WHITE, HIGH);
  digitalWrite(PIN_PINK, HIGH);
  pinMode(PIN_WHITE, OUTPUT);
  pinMode(PIN_PINK, OUTPUT);
  ledcAttach(PIN_WHITE, PWM_FREQ, PWM_BITS);
  ledcAttach(PIN_PINK, PWM_FREQ, PWM_BITS);
  output(PIN_WHITE, 0);
  output(PIN_PINK, 0);
  Serial.begin(115200);
  sensors.begin();
  sensors.setResolution(12);
  sensors.setWaitForConversion(true);
  Serial.println("DS18B20 GPIO33, DallasTemperature ile 5 saniyede bir okunuyor");
  prefs.begin("aquacontrol", false);
  autoMode = prefs.getBool("auto", true);
  white.target = constrain((int)prefs.getUChar("wlev", 80), 0, 100);
  pink.target = constrain((int)prefs.getUChar("plev", 50), 0, 100);
  sunriseStart = prefs.getUShort("ss", 8*60);
  sunriseEnd = prefs.getUShort("se", 10*60);
  sunsetStart = prefs.getUShort("ds", 20*60);
  sunsetEnd = prefs.getUShort("de", 22*60);
  if (!(0 <= sunriseStart && sunriseStart < sunriseEnd && sunriseEnd <= sunsetStart && sunsetStart < sunsetEnd && sunsetEnd < 1440)) {
    sunriseStart=480;sunriseEnd=600;sunsetStart=1200;sunsetEnd=1320;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  // Do not block web server initialization if Wi-Fi is unavailable.
  configTzTime("TRT-3", "tr.pool.ntp.org", "pool.ntp.org", "time.google.com");
  server.on("/", HTTP_GET, [](){ server.send_P(200, "text/html; charset=utf-8", PAGE); });
  server.on("/api/status", HTTP_GET, replyStatus);
  server.on("/api/mode", HTTP_GET, [](){
    if (!server.hasArg("auto")) return bad("auto eksik");
    autoMode = server.arg("auto") == "1";
    prefs.putBool("auto", autoMode);
    updateLights();replyStatus();
  });
  server.on("/api/channel", HTTP_GET, [](){
    String key=server.arg("channel");
    Channel* c = key=="white" ? &white : (key=="pink" ? &pink : nullptr);
    if (!c) return bad("Gecersiz kanal");
    if (server.hasArg("on")) c->manualOn = server.arg("on") == "1";
    if (server.hasArg("level")) {
      int v=server.arg("level").toInt();
      if(v<0 || v>100) return bad("Seviye 0-100 olmali");
      c->target=v;
      prefs.putUChar(key=="white"?"wlev":"plev",c->target);
    }
    updateLights();replyStatus();
  });
  server.on("/api/schedule", HTTP_GET, [](){
    if(!server.hasArg("sunriseStart")||!server.hasArg("sunriseEnd")||!server.hasArg("sunsetStart")||!server.hasArg("sunsetEnd")) return bad("Saatler eksik");
    int a=parseTime(server.arg("sunriseStart"),-1),b=parseTime(server.arg("sunriseEnd"),-1);
    int c=parseTime(server.arg("sunsetStart"),-1),d=parseTime(server.arg("sunsetEnd"),-1);
    if(!(0<=a && a<b && b<=c && c<d && d<1440)) return bad("Saat sirasi yanlis");
    sunriseStart=a;sunriseEnd=b;sunsetStart=c;sunsetEnd=d;
    saveSettings();updateLights();replyStatus();
  });
  server.begin();
  Serial.println("Aqua Control basladi; Wi-Fi baglantisi bekleniyor...");
  updateLights();
}

void loop() {
  server.handleClient();
  updateTemperature();
  if (millis()-lastTick >= 1000) {lastTick=millis();updateLights();}
  if (WiFi.status()!=WL_CONNECTED && millis()-lastReconnect>15000) {lastReconnect=millis();WiFi.reconnect();}
  static bool shown=false;
  if(WiFi.status()==WL_CONNECTED && !shown){shown=true;Serial.print("Panel: http://");Serial.println(WiFi.localIP());}
  if(WiFi.status()!=WL_CONNECTED)shown=false;
}
