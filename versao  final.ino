#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <SPI.h>
#include <SD.h>

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <time.h>

// ===== Wi-Fi =====
const char* WIFI_SSID = "Florenca";
const char* WIFI_PASS = "Beneecuca1";

// ===== HTTP Port =====
const uint16_t HTTP_PORT = 8090;
ESP8266WebServer server(HTTP_PORT);

// ===== Users (fixos) + SENHAS SEPARADAS =====
const char* USER1 = "";
const char* PASS1 = "";     // <-- senha da Anja

const char* USER2 = "";
const char* PASS2 = "";     // <-- senha do David (troque aqui)

// ===== SD =====
const int SD_CS = D0;
const char* LOG_PATH = "/chat.log";
bool sdOk = false;

// ===== OLED =====
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ===== Stats =====
uint32_t msgTotal = 0;
uint32_t newMsgs  = 0;
uint32_t logBytes = 0;
uint32_t lastUiMs = 0;

// ===== Chat buffer (RAM) =====
struct Msg {
  uint32_t id;
  uint64_t ts;   // ms
  String name;
  String text;
};
const uint16_t BUF_MAX = 200;
Msg buf[BUF_MAX];
uint16_t bufCount = 0;
uint16_t bufHead  = 0;
uint32_t nextId   = 1;

// ===== Sessions (token RAM) =====
struct ChatSession {
  bool used = false;
  String token;
  String user;       // "Anja" / "David"
  IPAddress ip;
  uint32_t issuedMs = 0;
  bool pageServed = false; // se já serviu /chat uma vez -> refresh volta pro login
};
ChatSession chatSessions[6];

const uint32_t SESSION_TTL_MS = 60UL * 60UL * 1000UL; // 1h

// ---------- Headers ----------
void noCacheHeaders(){
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
}

// ---------- JSON escape ----------
String jsonEscape(const String& s){
  String o; o.reserve(s.length()+8);
  for (size_t i=0;i<s.length();i++){
    char c = s[i];
    if(c=='\\' || c=='"') { o += '\\'; o += c; }
    else if(c=='\n') o += "\\n";
    else if(c=='\r') o += "\\r";
    else if(c=='\t') o += "\\t";
    else o += c;
  }
  return o;
}

// ---------- Extract fields ----------
String getField(const String& raw, const char* key){
  String k = String("\"") + key + "\":";
  int p = raw.indexOf(k);
  if(p<0) return "";
  p += k.length();
  while(p<(int)raw.length() && raw[p]==' ') p++;
  if(p<(int)raw.length() && raw[p]=='"'){
    p++;
    int e = raw.indexOf("\"", p);
    if(e<0) return "";
    return raw.substring(p, e);
  }
  return "";
}

uint64_t parseUint64(const String& s){
  uint64_t v=0;
  for(size_t i=0;i<s.length();i++){
    char c=s[i];
    if(c<'0'||c>'9') break;
    v = v*10 + (uint64_t)(c-'0');
  }
  return v;
}

uint64_t getNumberField64Safe(const String& raw, const char* key){
  String k = String("\"") + key + "\":";
  int p = raw.indexOf(k);
  if(p<0) return 0;
  p += k.length();
  while(p<(int)raw.length() && raw[p]==' ') p++;
  int e = p;
  while(e<(int)raw.length() && isDigit(raw[e])) e++;
  if(e<=p) return 0;
  return parseUint64(raw.substring(p,e));
}

uint64_t normalizeTsToMs(uint64_t ts){
  if(ts > 0 && ts < 4000000000ULL) return ts * 1000ULL;
  return ts;
}

// ---------- Time (OLED) ----------
void syncTime(){
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  time_t now = time(nullptr);
  uint32_t start = millis();
  while (now < 100000 && millis() - start < 15000) {
    delay(200);
    now = time(nullptr);
  }
}

String hhmm(){
  time_t now = time(nullptr);
  if(now < 100000) return "--:--";
  struct tm* t = localtime(&now);
  char b[6];
  snprintf(b, sizeof(b), "%02d:%02d", t->tm_hour, t->tm_min);
  return String(b);
}

// ---------- SD ----------
void setupSD(){
  SPI.begin();
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  delay(50);

  sdOk = SD.begin(SD_CS, SPI_HALF_SPEED);
  Serial.println(sdOk ? "SD: OK" : "SD: FALHOU");

  if(sdOk && !SD.exists(LOG_PATH)){
    File f = SD.open(LOG_PATH, FILE_WRITE);
    if(f) f.close();
  }

  if(sdOk){
    File f = SD.open(LOG_PATH, FILE_READ);
    if(f){ logBytes = f.size(); f.close(); }
  }
}

void logAppendLine(const String& line){
  if(!sdOk) return;
  File f = SD.open(LOG_PATH, FILE_WRITE);
  if(!f) return;
  f.println(line);
  logBytes = f.size();
  f.close();
  msgTotal++;
}

// ---------- Buffer ----------
void pushMsgToBuffer(uint32_t id, uint64_t ts, const String& name, const String& text){
  buf[bufHead].id = id;
  buf[bufHead].ts = ts;
  buf[bufHead].name = name;
  buf[bufHead].text = text;

  bufHead = (bufHead + 1) % BUF_MAX;
  if(bufCount < BUF_MAX) bufCount++;
}

int findOldestIndex(){
  if(bufCount == 0) return -1;
  int start = (int)bufHead - (int)bufCount;
  while(start < 0) start += BUF_MAX;
  return start;
}

uint32_t oldestId(){
  int idx = findOldestIndex();
  if(idx < 0) return 0;
  return buf[idx].id;
}

void loadLastLinesToBuffer(uint16_t maxLines){
  if(!sdOk) return;

  File f = SD.open(LOG_PATH, FILE_READ);
  if(!f) return;

  size_t size = f.size();
  logBytes = size;
  if(size == 0){ f.close(); return; }

  const size_t CHUNK = 256;
  long pos = (long)size;
  int linesFound = 0;
  String tail = "";

  while(pos > 0 && linesFound <= (int)maxLines){
    size_t readSize = (pos >= (long)CHUNK) ? CHUNK : (size_t)pos;
    pos -= (long)readSize;
    f.seek(pos);

    char bufc[CHUNK+1];
    size_t n = f.readBytes(bufc, readSize);
    bufc[n] = 0;

    tail = String(bufc) + tail;

    for(size_t i=0;i<n;i++){
      if(bufc[i] == '\n') linesFound++;
    }
    if(tail.length() > 120000) break;
  }
  f.close();

  int total = 0;
  for(int i=0;i<(int)tail.length();i++) if(tail[i]=='\n') total++;
  int skip = total - (int)maxLines;
  if(skip < 0) skip = 0;

  int cur = 0;
  int startIdx = 0;
  for(int i=0;i<(int)tail.length();i++){
    if(tail[i]=='\n'){
      if(cur < skip){ cur++; startIdx = i+1; }
      else break;
    }
  }

  String line;
  line.reserve(512);

  for(int i=startIdx; i < (int)tail.length(); i++){
    char c = tail[i];
    if(c == '\n'){
      line.trim();
      if(line.length()){
        String name = getField(line, "name");
        String text = getField(line, "text");

        uint64_t ts = getNumberField64Safe(line, "ts");
        ts = normalizeTsToMs(ts);
        if(ts == 0){
          time_t now = time(nullptr);
          ts = (now >= 100000) ? (uint64_t)now * 1000ULL : (uint64_t)millis();
        }

        uint32_t id = nextId++;
        pushMsgToBuffer(id, ts, name, text);
      }
      line = "";
    } else line += c;
  }

  if(msgTotal < (uint32_t)bufCount) msgTotal = bufCount;
}

// ---------- OLED ----------
void drawOLED(){
  if(!display.width()) return;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0,0);
  display.print("ESP Chat ");
  display.print(hhmm());

  display.setCursor(0,12);
  display.print("IP: ");
  display.print(WiFi.localIP());

  display.setCursor(0,22);
  display.print("SD: ");
  display.print(sdOk ? "OK" : "FAIL");

  display.setCursor(0,32);
  display.print("Log: ");
  display.print(logBytes/1024);
  display.print("KB");

  display.setCursor(0,42);
  display.print("Msgs:");
  display.print(msgTotal);

  display.setCursor(0,52);
  display.print("New:");
  display.print(newMsgs);

  display.display();
}

// ---------- Sessions ----------
String makeToken(){
  uint32_t r1 = ESP.getCycleCount() ^ micros() ^ millis();
  uint32_t r2 = random(0xFFFFFFFF);
  return String(r1, HEX) + String(r2, HEX);
}

void cleanupSessions(){
  uint32_t now = millis();
  for(auto &s : chatSessions){
    if(!s.used) continue;
    if(now - s.issuedMs > SESSION_TTL_MS){
      s.used = false;
      s.token = "";
      s.user = "";
      s.pageServed = false;
    }
  }
}

String createSession(const String& user){
  cleanupSessions();
  IPAddress ip = server.client().remoteIP();
  String t = makeToken();

  for(auto &s : chatSessions){
    if(!s.used){
      s.used = true;
      s.token = t;
      s.user = user;
      s.ip = ip;
      s.issuedMs = millis();
      s.pageServed = false;
      return t;
    }
  }
  chatSessions[0].used = true;
  chatSessions[0].token = t;
  chatSessions[0].user = user;
  chatSessions[0].ip = ip;
  chatSessions[0].issuedMs = millis();
  chatSessions[0].pageServed = false;
  return t;
}

ChatSession* findSession(const String& token){
  cleanupSessions();
  IPAddress ip = server.client().remoteIP();
  for(auto &s : chatSessions){
    if(s.used && s.token == token && s.ip == ip){
      return &s;
    }
  }
  return nullptr;
}

bool requireToken(String &userOut){
  if(!server.hasArg("token")){
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Login required");
    return false;
  }
  String token = server.arg("token");
  ChatSession* s = findSession(token);
  if(!s){
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Login required");
    return false;
  }
  userOut = s->user;
  return true;
}

bool requireChatPageToken(String &userOut, String &tokenOut){
  if(!server.hasArg("token")){
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Login required");
    return false;
  }
  String token = server.arg("token");
  ChatSession* s = findSession(token);
  if(!s){
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Login required");
    return false;
  }
  if(s->pageServed){
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Login required");
    return false;
  }
  s->pageServed = true;
  userOut = s->user;
  tokenOut = token;
  return true;
}

// ---------- Pages ----------
const char LOGIN_HTML[] PROGMEM =
"<!doctype html><html lang='pt-br'><head>"
"<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Login - ESP Chat</title>"
"<style>"
":root{--bg:#0b141a;--card:#111b21;--accent:#25d366;--muted:#aebac1;}"
"*{box-sizing:border-box}"
"body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;background:linear-gradient(180deg,#081015,#0b141a);color:#fff;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:16px}"
".card{width:min(420px,100%);background:rgba(17,27,33,.92);border:1px solid rgba(255,255,255,.08);border-radius:18px;padding:18px;box-shadow:0 20px 60px rgba(0,0,0,.35)}"
".title{font-weight:900;font-size:22px;margin:0 0 6px}"
".sub{color:var(--muted);margin:0 0 16px;font-size:13px}"
"label{display:block;color:var(--muted);font-size:12px;margin:10px 0 6px}"
"select,input{width:100%;padding:12px 12px;border-radius:14px;border:1px solid rgba(255,255,255,.12);background:#0b141a;color:#fff;font-size:15px;outline:none}"
".btn{margin-top:14px;width:100%;padding:12px 14px;border-radius:14px;border:0;background:var(--accent);color:#062b12;font-weight:900;font-size:15px;cursor:pointer}"
".hint{margin-top:10px;color:var(--muted);font-size:12px;line-height:1.35}"
"</style></head><body>"
"<div class='card'>"
"  <div class='title'>ESP Chat</div>"
"  <div class='sub'>Login obrigatorio (sempre). Escolha o usuario e digite a senha.</div>"
"  <form method='POST' action='/auth'>"
"    <label>Usuario</label>"
"    <select name='u'>"
"      <option value='anja'>Anja</option>"
"      <option value='david'>David</option>"
"    </select>"
"    <label>Senha</label>"
"    <input type='password' name='p' placeholder='Senha' autocomplete='off' required>"
"    <button class='btn' type='submit'>Entrar</button>"
"  </form>"
"  <div class='hint'>O nome no chat vem do login e nao pode ser mudado.</div>"
"</div></body></html>";

String chatPageHtml(const String& me, const String& token){
  String html;
  html.reserve(10500);

  html += "<!doctype html><html lang='pt-br'><head>";
  html += "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1'>";
  html += "<title>ESP Chat</title><style>";
  html += ":root{--bg:#0b141a;--card:#111b21;--top:#075e54;--accent:#25d366;--muted:#aebac1;}";
  html += "*{box-sizing:border-box}";
  html += "body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;background:var(--bg);color:#fff;height:100dvh;display:flex;flex-direction:column}";
  html += "#top{background:var(--top);padding:12px 14px;display:flex;align-items:center;justify-content:space-between;gap:10px}";
  html += "#title{font-weight:900;letter-spacing:.2px}";
  html += "#st{font-size:13px;color:#e9f5ef;opacity:.95;white-space:nowrap}";
  html += "#wrap{flex:1;display:flex;flex-direction:column;min-height:0}";
  html += "#chat{flex:1;overflow:auto;padding:12px;display:flex;flex-direction:column;gap:10px;background:linear-gradient(180deg,#0b141a,#0a1015)}";
  html += ".msg{max-width:88%;padding:10px 12px;border-radius:14px;background:var(--card);box-shadow:0 1px 0 rgba(255,255,255,.06) inset}";
  html += ".me{align-self:flex-end;background:#1f2c34}";
  html += ".meta{font-size:12px;color:var(--muted);margin-bottom:4px}";
  html += ".text{white-space:pre-wrap;word-break:break-word;font-size:15px;line-height:1.25}";
  html += "#bar{padding:10px;display:flex;gap:8px;align-items:flex-end;background:#0f171d;border-top:1px solid rgba(255,255,255,.06)}";
  html += "textarea{flex:1;min-height:44px;max-height:120px;resize:none;border:1px solid rgba(255,255,255,.12);background:#0b141a;color:#fff;border-radius:14px;padding:10px 12px;font-size:15px;outline:none}";
  html += "button{border:0;border-radius:14px;background:var(--accent);color:#062b12;font-weight:900;padding:11px 14px;font-size:15px;cursor:pointer}";
  html += "button:active{transform:translateY(1px)}";
  html += ".pill{font-size:12px;color:#e9f5ef;opacity:.95}";
  html += "</style></head><body>";

  html += "<div id='top'><div id='title'>ESP Chat</div><div style='display:flex;gap:10px;align-items:center'>";
  html += "<div class='pill'>Voce: " + me + "</div><div id='st'>Conectando…</div></div></div>";
  html += "<div id='wrap'><div id='chat'></div><div id='bar'>";
  html += "<textarea id='text' placeholder='Mensagem...' maxlength='200'></textarea>";
  html += "<button id='send'>Enviar</button>";
  html += "</div></div>";

  html += "<script>";
  html += "const TOKEN='" + token + "';";
  html += "const MY='" + me + "';";
  html += "const chat=document.getElementById('chat');";
  html += "const textEl=document.getElementById('text');";
  html += "const sendBtn=document.getElementById('send');";
  html += "const st=document.getElementById('st');";
  html += "let lastId=0, polling=false, backoff=600;";
  html += "function addMsg(m){";
  html += " const d=document.createElement('div');";
  html += " const isMe=(String(m.name||'')===MY);";
  html += " d.className='msg'+(isMe?' me':'');";
  html += " const t=new Date(m.ts||Date.now()).toLocaleTimeString([], {hour:'2-digit', minute:'2-digit'});";
  html += " const meta=document.createElement('div'); meta.className='meta'; meta.textContent=`${m.name||'Anon'} • ${t}`;";
  html += " const body=document.createElement('div'); body.className='text'; body.textContent=String(m.text||'');";
  html += " d.appendChild(meta); d.appendChild(body); chat.appendChild(d); chat.scrollTop=chat.scrollHeight; }";
  html += "async function poll(){";
  html += " if(polling) return; polling=true;";
  html += " try{";
  html += "  const r=await fetch('/poll?token='+encodeURIComponent(TOKEN)+'&after='+lastId,{cache:'no-store'});";
  html += "  if(!r.ok) throw new Error('bad');";
  html += "  const data=await r.json();";
  html += "  if(Array.isArray(data.msgs)){ for(const m of data.msgs){ addMsg(m); if(m.id) lastId=m.id; }}";
  html += "  st.textContent='Conectado ✅'; backoff=600;";
  html += " }catch(e){ st.textContent='Desconectado ❌'; backoff=Math.min(backoff*1.6, 4000); }";
  html += " finally{ polling=false; setTimeout(poll, backoff);} }";
  html += "async function send(){";
  html += " const text=textEl.value.trim(); if(!text) return;";
  html += " try{";
  html += "  const r=await fetch('/send?token='+encodeURIComponent(TOKEN),{method:'POST',headers:{'Content-Type':'application/json','Cache-Control':'no-store'},body:JSON.stringify({text:text,ts:Date.now()})});";
  html += "  if(!r.ok) throw new Error('send');";
  html += "  textEl.value=''; textEl.style.height='44px'; textEl.focus();";
  html += " }catch{ addMsg({name:'Sistema',text:'Falhou enviar. Sem conexao?',ts:Date.now()}); } }";
  html += "sendBtn.onclick=send;";
  html += "textEl.addEventListener('input',()=>{textEl.style.height='auto'; textEl.style.height=Math.min(textEl.scrollHeight,120)+'px';});";
  html += "textEl.addEventListener('keydown',(e)=>{ if(e.key==='Enter' && !e.shiftKey){e.preventDefault(); send();}});";
  html += "poll();";
  html += "</script>";

  html += "</body></html>";
  return html;
}

// ---------- Handlers ----------
void handleLoginPage(){
  noCacheHeaders();
  server.send(200, "text/html; charset=utf-8", LOGIN_HTML);
}

void handleAuth(){
  noCacheHeaders();

  String u = server.arg("u"); u.trim(); u.toLowerCase();
  String p = server.arg("p"); p.trim();

  bool ok = false;
  String display;

  if(u == USER1 && p == PASS1){ ok = true; display = "Anja"; }
  else if(u == USER2 && p == PASS2){ ok = true; display = "David"; }

  if(!ok){
    server.send(401, "text/plain; charset=utf-8", "Login invalido");
    return;
  }

  String token = createSession(display);
  server.sendHeader("Location", String("/chat?token=") + token);
  server.send(302, "text/plain", "OK");
}

void handleChat(){
  String me, token;
  if(!requireChatPageToken(me, token)) return;
  noCacheHeaders();
  server.send(200, "text/html; charset=utf-8", chatPageHtml(me, token));
}

void handleSend(){
  String user;
  if(!requireToken(user)) return;
  noCacheHeaders();

  String body = server.arg("plain");
  String text = getField(body, "text").substring(0, 200);

  uint64_t ts = getNumberField64Safe(body, "ts");
  ts = normalizeTsToMs(ts);

  if(ts == 0){
    time_t now = time(nullptr);
    ts = (now >= 100000) ? (uint64_t)now * 1000ULL : (uint64_t)millis();
  }

  if(text.length()==0){
    server.send(400, "application/json; charset=utf-8", "{\"ok\":false}");
    return;
  }

  uint32_t id = nextId++;

  String line = String("{\"name\":\"") + jsonEscape(user) +
                "\",\"text\":\"" + jsonEscape(text) +
                "\",\"ts\":" + String((unsigned long long)ts) + "}";
  logAppendLine(line);
  pushMsgToBuffer(id, ts, user, text);
  newMsgs++;

  server.send(200, "application/json; charset=utf-8",
              String("{\"ok\":true,\"id\":") + String(id) + "}");
}

void handlePoll(){
  String user;
  if(!requireToken(user)) return;
  noCacheHeaders();

  uint32_t after = 0;
  if(server.hasArg("after")) after = (uint32_t)server.arg("after").toInt();

  uint32_t oId = oldestId();
  bool tooOld = (bufCount > 0 && after < oId - 1);

  String out = "{\"msgs\":[";
  bool first = true;

  int idx = findOldestIndex();
  for(uint16_t i=0; i<bufCount; i++){
    Msg &m = buf[idx];
    if(m.id > after){
      if(!first) out += ",";
      first = false;
      out += String("{\"id\":") + m.id +
             ",\"name\":\"" + jsonEscape(m.name) +
             "\",\"text\":\"" + jsonEscape(m.text) +
             "\",\"ts\":" + String((unsigned long long)m.ts) + "}";
    }
    idx = (idx + 1) % BUF_MAX;
  }

  out += "],\"tooOld\":";
  out += (tooOld ? "true" : "false");
  out += "}";

  server.send(200, "application/json; charset=utf-8", out);
}

// ---------- Setup/Loop ----------
void setup(){
  Serial.begin(115200);
  randomSeed(ESP.getCycleCount() ^ micros() ^ millis());

  // OLED (I2C)
  Wire.begin(D2, D1);
  if(!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)){
    Serial.println("OLED nao achou (0x3C).");
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0,0);
    display.println("ESP Chat");
    display.println("Boot...");
    display.display();
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Conectando");
  while(WiFi.status()!=WL_CONNECTED){ delay(300); Serial.print("."); }
  Serial.println();
  Serial.print("IP local: ");
  Serial.println(WiFi.localIP());

  syncTime();
  setupSD();
  loadLastLinesToBuffer(BUF_MAX);

  server.on("/", HTTP_GET, handleLoginPage);
  server.on("/auth", HTTP_POST, handleAuth);
  server.on("/chat", HTTP_GET, handleChat);
  server.on("/send", HTTP_POST, handleSend);
  server.on("/poll", HTTP_GET, handlePoll);

  server.begin();
  Serial.printf("Abra: http://%s:%u\n", WiFi.localIP().toString().c_str(), HTTP_PORT);

  drawOLED();
}

void loop(){
  server.handleClient();

  if(millis() - lastUiMs > 1000){
    lastUiMs = millis();
    drawOLED();
  }
}
