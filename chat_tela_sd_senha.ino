#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>
#include <SPI.h>
#include <SD.h>

const char* WIFI_SSID = "Florenca";
const char* WIFI_PASS = "Beneecuca1";

const uint16_t HTTP_PORT = 8090;
const uint16_t WS_PORT   = 8091;

ESP8266WebServer server(HTTP_PORT);
WebSocketsServer ws(WS_PORT);

// ===== Auth =====
const char* AUTH_USER = "";
const char* AUTH_PASS = "";

// ===== SD =====
const int SD_CS = D0;                 // CS no D0 (GPIO16) (bom com OLED I2C)
const char* LOG_PATH = "/chat.log";
bool sdOk = false;

const int SEND_LAST_LINES = 80;

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

// ---------- Auth helper ----------
bool requireAuth(){
  if (server.authenticate(AUTH_USER, AUTH_PASS)) return true;

  // força pedir senha
  server.requestAuthentication(BASIC_AUTH, "ESP Chat", "Senha requerida");
  return false;
}

// ---------- SD helpers ----------
void setupSD(){
  SPI.begin(); // D5/D6/D7
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  delay(50);

  sdOk = SD.begin(SD_CS, SPI_HALF_SPEED);
  Serial.println(sdOk ? "SD: OK" : "SD: FALHOU");

  if(sdOk && !SD.exists(LOG_PATH)){
    File f = SD.open(LOG_PATH, FILE_WRITE);
    if(f) f.close();
  }
}

void logAppendLine(const String& line){
  if(!sdOk) return;
  File f = SD.open(LOG_PATH, FILE_WRITE);
  if(!f) return;
  f.println(line);
  f.close();
}

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

    if(tail.length() > 120000) break;
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

// ---------- Página ----------
const char INDEX_HTML[] PROGMEM =
"<!doctype html><html lang='pt-br'><head>"
"<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP Chat</title>"
"<style>"
"body{font-family:sans-serif;margin:0;height:100vh;display:flex;flex-direction:column}"
"#top{background:#075e54;color:#fff;padding:12px;font-weight:700;display:flex;justify-content:space-between;gap:10px}"
"#st{opacity:.95;font-weight:600}"
"#chat{flex:1;overflow:auto;padding:12px;background:#eee}"
".msg{background:#fff;margin:8px 0;padding:10px;border-radius:12px}"
".meta{font-size:12px;opacity:.7;margin-bottom:6px}"
"#bar{display:flex;gap:8px;padding:10px;background:#ddd}"
"input{padding:12px;border-radius:12px;border:1px solid #ccc}"
"#name{width:130px}#text{flex:1}"
"button{padding:12px 14px;border-radius:12px;border:0;cursor:pointer;background:#25d366;font-weight:700}"
"</style></head><body>"
"<div id='top'><div>ESP Chat</div><div id='st'>Conectando…</div></div>"
"<div id='chat'></div>"
"<div id='bar'>"
" <input id='name' placeholder='Nome' maxlength='20'>"
" <input id='text' placeholder='Mensagem...' maxlength='200'>"
" <button id='send'>Enviar</button>"
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
"nameEl.value=localStorage.getItem('espchat_name')||'';"
"nameEl.addEventListener('input',()=>localStorage.setItem('espchat_name',nameEl.value));"
"const addMsg=(m)=>{"
" const d=document.createElement('div'); d.className='msg';"
" const t=new Date(m.ts||Date.now()).toLocaleTimeString();"
" const meta=document.createElement('div'); meta.className='meta';"
" meta.textContent=`${m.name||'Anon'} • ${t}`;"
" const body=document.createElement('div');"
" body.textContent=String(m.text||'');"
" d.appendChild(meta); d.appendChild(body);"
" chat.appendChild(d); chat.scrollTop=chat.scrollHeight;"
"};"
"const setStatus=(ok)=>{st.textContent=ok?'Conectado ✅':'Desconectado ❌';};"
"let ws;"
"const connect=()=>{"
" setStatus(false);"
" ws=new WebSocket(`ws://${location.hostname}:${WS_PORT}/`);"
" ws.onopen=()=>{setStatus(true); addMsg({name:'Sistema',text:'Conectado ✅',ts:Date.now()});};"
" ws.onmessage=(e)=>{try{addMsg(JSON.parse(e.data));}catch{}};"
" ws.onclose=()=>{setStatus(false); addMsg({name:'Sistema',text:'Caiu. Reconectando…',ts:Date.now()}); setTimeout(connect,1500);};"
"};"
"connect();"
"const send=()=>{"
" const name=(nameEl.value.trim()||'Anon').slice(0,20);"
" const text=textEl.value.trim(); if(!text) return;"
" if(!ws||ws.readyState!==1){addMsg({name:'Sistema',text:'WebSocket nao conectou (porta 8091).',ts:Date.now()});return;}"
" ws.send(JSON.stringify({name:name,text:text,ts:Date.now()}));"
" textEl.value=''; textEl.focus();"
"};"
"sendBtn.onclick=send;"
"textEl.addEventListener('keydown',(e)=>{if(e.key==='Enter')send();});";

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
  String tsS  = getField(raw, "ts");
  uint32_t ts = (uint32_t) tsS.toInt();
  if(text.length()==0) return;

  String msg = String("{\"name\":\"") + jsonEscape(name) +
               "\",\"text\":\"" + jsonEscape(text) +
               "\",\"ts\":" + String(ts) + "}";

  logAppendLine(msg);
  ws.broadcastTXT(msg);
}

void setup(){
  Serial.begin(115200);

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
}

void loop(){
  server.handleClient();
  ws.loop();
}
