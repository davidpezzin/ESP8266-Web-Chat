#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>
#include <SPI.h>
#include <SD.h>

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ===== Wi-Fi =====
const char* WIFI_SSID = "Florenca";
const char* WIFI_PASS = "Beneecuca1";

// ===== Ports =====
const uint16_t HTTP_PORT = 8090;
const uint16_t WS_PORT   = 8091;

ESP8266WebServer server(HTTP_PORT);
WebSocketsServer ws(WS_PORT);

// ===== Login (HTTP Basic Auth) =====
const char* AUTH_USER = "";
const char* AUTH_PASS = "";

// ===== SD =====
const int SD_CS = D0;            // CS no D0 (libera D2 para OLED SDA)
const char* LOG_PATH = "/chat.log";
bool sdOk = false;

// quantas mensagens reenviar quando alguém entra
const int SEND_LAST_LINES = 80;

// ===== OLED (NodeMCU V3 OLED integrada) =====
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ===== Stats OLED =====
uint32_t msgTotal = 0;   // total aproximado (linhas do log + novas)
uint32_t newMsgs  = 0;   // novas desde boot
uint32_t logBytes = 0;
uint32_t lastUiMs = 0;

// ---------- Utils ----------
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

// Extrai "key":"value" simples (para name/text). TS a gente trata separado.
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

uint32_t getNumberField(const String& raw, const char* key){
  String k = String("\"") + key + "\":";
  int p = raw.indexOf(k);
  if(p<0) return 0;
  p += k.length();
  while(p<(int)raw.length() && (raw[p]==' ')) p++;
  int e = p;
  while(e<(int)raw.length() && isDigit(raw[e])) e++;
  if(e<=p) return 0;
  return (uint32_t) raw.substring(p, e).toInt();
}

// ---------- Auth helper ----------
bool requireAuth(){
  if (server.authenticate(AUTH_USER, AUTH_PASS)) return true;
  server.requestAuthentication(BASIC_AUTH, "ESP Chat", "Senha requerida");
  return false;
}

// ---------- SD helpers ----------
void countExistingMessages(){
  msgTotal = 0;
  logBytes = 0;
  if(!sdOk) return;

  File f = SD.open(LOG_PATH, FILE_READ);
  if(!f) return;

  logBytes = f.size();
  while(f.available()){
    if(f.read() == '\n') msgTotal++;
  }
  f.close();
}

void setupSD(){
  SPI.begin(); // D5/D6/D7 (SPI do ESP8266)
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  delay(50);

  sdOk = SD.begin(SD_CS, SPI_HALF_SPEED);
  Serial.println(sdOk ? "SD: OK" : "SD: FALHOU");

  if(sdOk && !SD.exists(LOG_PATH)){
    File f = SD.open(LOG_PATH, FILE_WRITE);
    if(f) f.close();
  }

  countExistingMessages();
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

// Lê as últimas N linhas do arquivo e envia ao cliente WS
void sendLastLines(uint8_t client, int maxLines){
  if(!sdOk) return;

  File f = SD.open(LOG_PATH, FILE_READ);
  if(!f) return;

  size_t size = f.size();
  if(size == 0){ f.close(); return; }

  const size_t CHUNK = 256;
  long pos = (long)size;
  int linesFound = 0;
  String tail = "";

  while(pos > 0 && linesFound <= maxLines){
    size_t readSize = (pos >= (long)CHUNK) ? CHUNK : (size_t)pos;
    pos -= (long)readSize;
    f.seek(pos);

    char buf[CHUNK+1];
    size_t n = f.readBytes(buf, readSize);
    buf[n] = 0;

    tail = String(buf) + tail;

    for(size_t i=0;i<n;i++){
      if(buf[i] == '\n') linesFound++;
    }

    if(tail.length() > 120000) break; // limite de RAM
  }

  f.close();

  int total = 0;
  for(int i=0;i<(int)tail.length();i++){
    if(tail[i] == '\n') total++;
  }

  int skip = total - maxLines;
  if(skip < 0) skip = 0;

  int curLines = 0;
  int startIdx = 0;
  for(int i=0;i<(int)tail.length();i++){
    if(tail[i] == '\n'){
      if(curLines < skip){
        curLines++;
        startIdx = i+1;
      } else break;
    }
  }

  String line;
  line.reserve(512);

  for(int i=startIdx; i < (int)tail.length(); i++){
    char c = tail[i];
    if(c == '\n'){
      line.trim();
      if(line.length()){
        String payload = line;
        ws.sendTXT(client, payload);
        delay(2);
      }
      line = "";
    } else line += c;
  }
}

// ---------- OLED ----------
void drawOLED(){
  // se OLED falhar, não trava o projeto
  if(!display.width()) return;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0,0);
  display.print("ESP Chat");

  display.setCursor(0,12);
  display.print("WiFi: ");
  display.print(WiFi.status() == WL_CONNECTED ? "OK" : "OFF");

  display.setCursor(0,22);
  display.print("IP: ");
  display.print(WiFi.localIP());

  display.setCursor(0,32);
  display.print("SD: ");
  display.print(sdOk ? "OK" : "FAIL");

  display.setCursor(0,42);
  display.print("Log: ");
  display.print(logBytes/1024);
  display.print("KB");

  display.setCursor(0,52);
  display.print("Msgs: ");
  display.print(msgTotal);
  display.print(" New:");
  display.print(newMsgs);

  display.display();
}

// ---------- Web UI (responsivo) ----------
const char INDEX_HTML[] PROGMEM =
"<!doctype html><html lang='pt-br'><head>"
"<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1'>"
"<title>ESP Chat</title>"
"<style>"
":root{--bg:#0b141a;--card:#111b21;--top:#075e54;--accent:#25d366;--muted:#aebac1;}"
"*{box-sizing:border-box}"
"body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;background:var(--bg);color:#fff;height:100dvh;display:flex;flex-direction:column}"
"#top{background:var(--top);padding:12px 14px;display:flex;align-items:center;justify-content:space-between;gap:10px}"
"#title{font-weight:800;letter-spacing:.2px}"
"#st{font-size:13px;color:#e9f5ef;opacity:.95;white-space:nowrap}"
"#wrap{flex:1;display:flex;flex-direction:column;min-height:0}"
"#chat{flex:1;overflow:auto;padding:12px;display:flex;flex-direction:column;gap:10px;background:linear-gradient(180deg,#0b141a,#0a1015)}"
".msg{max-width:88%;padding:10px 12px;border-radius:14px;background:var(--card);box-shadow:0 1px 0 rgba(255,255,255,.06) inset}"
".me{align-self:flex-end;background:#1f2c34}"
".meta{font-size:12px;color:var(--muted);margin-bottom:4px}"
".text{white-space:pre-wrap;word-break:break-word;font-size:15px;line-height:1.25}"
"#bar{padding:10px;display:flex;gap:8px;align-items:flex-end;background:#0f171d;border-top:1px solid rgba(255,255,255,.06)}"
"#name{width:110px;max-width:34vw}"
"input,textarea{border:1px solid rgba(255,255,255,.12);background:#0b141a;color:#fff;border-radius:14px;padding:10px 12px;font-size:15px;outline:none}"
"textarea{flex:1;min-height:44px;max-height:120px;resize:none}"
"button{border:0;border-radius:14px;background:var(--accent);color:#062b12;font-weight:900;padding:11px 14px;font-size:15px;cursor:pointer}"
"button:active{transform:translateY(1px)}"
"@media (max-width:420px){#name{width:92px}.msg{max-width:94%}}"
"</style></head><body>"
"<div id='top'><div id='title'>ESP Chat</div><div id='st'>Conectando…</div></div>"
"<div id='wrap'>"
"  <div id='chat'></div>"
"  <div id='bar'>"
"    <input id='name' placeholder='Nome' maxlength='20'>"
"    <textarea id='text' placeholder='Mensagem...' maxlength='200'></textarea>"
"    <button id='send'>Enviar</button>"
"  </div>"
"</div>"
"<script src='/app.js'></script>"
"</body></html>";

const char APP_JS[] PROGMEM =
"const WS_PORT=8091;"
"const chat=document.getElementById('chat');"
"const nameEl=document.getElementById('name');"
"const textEl=document.getElementById('text');"
"const sendBtn=document.getElementById('send');"
"const st=document.getElementById('st');"
"let myName='';"
"nameEl.value=localStorage.getItem('espchat_name')||'';"
"nameEl.addEventListener('input',()=>{localStorage.setItem('espchat_name',nameEl.value); myName=nameEl.value.trim();});"
"myName=nameEl.value.trim();"
"const addMsg=(m)=>{"
" const d=document.createElement('div');"
" const isMe=(myName && (String(m.name||'')===myName));"
" d.className='msg'+(isMe?' me':'');"
" const t=new Date(m.ts||Date.now()).toLocaleTimeString();"
" const meta=document.createElement('div'); meta.className='meta';"
" meta.textContent=`${m.name||'Anon'} • ${t}`;"
" const body=document.createElement('div'); body.className='text';"
" body.textContent=String(m.text||'');"
" d.appendChild(meta); d.appendChild(body);"
" chat.appendChild(d);"
" chat.scrollTop=chat.scrollHeight;"
"};"
"const setStatus=(ok)=>{st.textContent=ok?'Conectado ✅':'Desconectado ❌';};"
"let ws;"
"const connect=()=>{"
" setStatus(false);"
" ws=new WebSocket(`ws://${location.hostname}:${WS_PORT}/`);"
" ws.onopen=()=>{setStatus(true);};"
" ws.onmessage=(e)=>{try{addMsg(JSON.parse(e.data));}catch{}};"
" ws.onclose=()=>{setStatus(false); setTimeout(connect,1500);};"
"};"
"connect();"
"const send=()=>{"
" const name=(nameEl.value.trim()||'Anon').slice(0,20);"
" const text=textEl.value.trim(); if(!text) return;"
" if(!ws||ws.readyState!==1){addMsg({name:'Sistema',text:'WebSocket nao conectou (porta 8091).',ts:Date.now()});return;}"
" ws.send(JSON.stringify({name:name,text:text,ts:Date.now()}));"
" textEl.value='';"
" textEl.style.height='44px';"
" textEl.focus();"
"};"
"sendBtn.onclick=send;"
"textEl.addEventListener('input',()=>{textEl.style.height='auto'; textEl.style.height=Math.min(textEl.scrollHeight,120)+'px';});"
"textEl.addEventListener('keydown',(e)=>{"
" if(e.key==='Enter' && !e.shiftKey){e.preventDefault(); send();}"
"});";

void handleRoot(){
  if(!requireAuth()) return;
  server.send(200, "text/html; charset=utf-8", INDEX_HTML);
}
void handleJs(){
  if(!requireAuth()) return;
  server.send(200, "application/javascript; charset=utf-8", APP_JS);
}

// ---------- WebSocket ----------
void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length){
  if(type == WStype_CONNECTED){
    sendLastLines(num, SEND_LAST_LINES);
    return;
  }
  if(type != WStype_TEXT) return;

  String raw = String((char*)payload).substring(0, length);
  String name = getField(raw, "name").substring(0, 20);
  String text = getField(raw, "text").substring(0, 200);
  uint32_t ts  = getNumberField(raw, "ts");
  if(ts == 0) ts = (uint32_t) (millis()/1000); // fallback
  if(text.length()==0) return;

  String msg = String("{\"name\":\"") + jsonEscape(name) +
               "\",\"text\":\"" + jsonEscape(text) +
               "\",\"ts\":" + String(ts) + "}";

  logAppendLine(msg);
  ws.broadcastTXT(msg);

  newMsgs++;
}

void setup(){
  Serial.begin(115200);

  // OLED integrada
  Wire.begin(D2, D1); // SDA, SCL
  bool oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if(!oledOk){
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

  setupSD();

  server.on("/", handleRoot);
  server.on("/app.js", handleJs);
  server.begin();

  ws.begin();
  ws.onEvent(onWsEvent);

  Serial.printf("Abra: http://%s:%u\n", WiFi.localIP().toString().c_str(), HTTP_PORT);

  drawOLED();
}

void loop(){
  server.handleClient();
  ws.loop();

  if(millis() - lastUiMs > 1000){
    lastUiMs = millis();
    drawOLED();
  }
}
