/*
  ESP8266 Web Chat (HTTP + WebSocket)
  - Acesso externo via Port Forward
  - Histórico em RAM (quem entra depois vê as últimas mensagens)
  - Sem login, sem senha, sem foto
  - Some ao desligar (histórico só na RAM)

  Portas:
    HTTP: 8090
    WS:   8091

  NAT/Port Forward (roteador):
    TCP 8090 -> 192.168.15.20:8090
    TCP 8091 -> 192.168.15.20:8091
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>

const char* WIFI_SSID = "Florenca";
const char* WIFI_PASS = "Beneecuca1";

const uint16_t HTTP_PORT = 8090;
const uint16_t WS_PORT   = 8091;

ESP8266WebServer server(HTTP_PORT);
WebSocketsServer ws(WS_PORT);

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

// ---------- Histórico em RAM (some ao desligar) ----------
struct ChatMsg { String json; };
const uint8_t HISTORY_MAX = 40;  // ajuste: 20~80 (mais = mais RAM)
ChatMsg history[HISTORY_MAX];
uint8_t histCount = 0;

void pushHistory(const String& json){
  if(histCount < HISTORY_MAX){
    history[histCount++].json = json;
  } else {
    for(int i=1;i<HISTORY_MAX;i++) history[i-1].json = history[i].json;
    history[HISTORY_MAX-1].json = json;
  }
}

void sendHistory(uint8_t client){
  for(int i=0;i<histCount;i++){
    String msg = history[i].json;   // variável (compatível com libs antigas)
    ws.sendTXT(client, msg);
    delay(2);
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
"button{padding:12px 14px;border-radius:12px;border:0;cursor:pointer;background:#25d366;font-weight:700;white-space:nowrap}"
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
" ws.onmessage=(e)=>{"
"   try{"
"     const m=JSON.parse(e.data);"
"     m.name=String(m.name||'Anon');"
"     m.text=String(m.text||'');"
"     addMsg(m);"
"   }catch{}"
" };"
" ws.onclose=()=>{setStatus(false); addMsg({name:'Sistema',text:'Caiu. Reconectando…',ts:Date.now()}); setTimeout(connect,1500);};"
"};"
"connect();"

"const send=()=>{"
" const name=(nameEl.value.trim()||'Anon').slice(0,20);"
" const text=textEl.value.trim(); if(!text) return;"
" if(!ws||ws.readyState!==1){addMsg({name:'Sistema',text:'WebSocket não conectou (porta 8091).',ts:Date.now()});return;}"
" ws.send(JSON.stringify({name:name,text:text,ts:Date.now()}));"
" textEl.value=''; textEl.focus();"
"};"

"sendBtn.onclick=send;"
"textEl.addEventListener('keydown',(e)=>{if(e.key==='Enter')send();});";

void handleRoot(){ server.send(200, "text/html; charset=utf-8", INDEX_HTML); }
void handleJs(){ server.send(200, "application/javascript; charset=utf-8", APP_JS); }

// ---------- WebSocket ----------
void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length){

  if(type == WStype_CONNECTED){
    // manda o histórico para quem acabou de entrar
    sendHistory(num);
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

  pushHistory(msg);
  ws.broadcastTXT(msg);
}

// ---------- Setup/Loop ----------
void setup(){
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Conectando");
  while(WiFi.status()!=WL_CONNECTED){ delay(300); Serial.print("."); }
  Serial.println();
  Serial.print("IP local do ESP: ");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.on("/app.js", handleJs);
  server.begin();

  ws.begin();
  ws.onEvent(onWsEvent);

  Serial.printf("Local: http://%s:%u\n", WiFi.localIP().toString().c_str(), HTTP_PORT);
  Serial.printf("WS:    ws://%s:%u\n", WiFi.localIP().toString().c_str(), WS_PORT);
}

void loop(){
  server.handleClient();
  ws.loop();
}
