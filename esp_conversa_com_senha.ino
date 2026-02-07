#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>

const char* WIFI_SSID = "Florenca";
const char* WIFI_PASS = "Beneecuca1";

const uint16_t HTTP_PORT = 8090;
const uint16_t WS_PORT   = 8091;

ESP8266WebServer server(HTTP_PORT);
WebSocketsServer ws(WS_PORT);

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

const char INDEX_HTML[] PROGMEM =
"<!doctype html><html lang='pt-br'><head>"
"<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP Chat</title>"
"<style>"
"body{font-family:sans-serif;margin:0;height:100vh;display:flex;flex-direction:column}"
"#top{background:#075e54;color:#fff;padding:12px;font-weight:700;display:flex;justify-content:space-between}"
"#chat{flex:1;overflow:auto;padding:12px;background:#eee}"
".msg{background:#fff;margin:8px 0;padding:10px;border-radius:12px}"
".meta{font-size:12px;opacity:.7;margin-bottom:4px}"
"#bar{display:flex;gap:8px;padding:10px;background:#ddd}"
"input{padding:12px;border-radius:12px;border:1px solid #ccc}"
"#name{width:130px}#text{flex:1}"
"button{padding:12px 14px;border-radius:12px;border:0;cursor:pointer;background:#25d366;font-weight:700}"
// overlay senha
"#lock{position:fixed;inset:0;background:#111;display:flex;align-items:center;justify-content:center;z-index:9999;color:#fff}"
"#lockBox{background:#222;padding:18px;border-radius:12px;max-width:320px;width:90%}"
"#lockBox h3{margin:0 0 10px 0}"
"#lockBox input{width:100%;margin:8px 0;box-sizing:border-box;border:0}"
"#lockBox button{width:100%;background:#25d366}"
"#pwerr{color:#ff6b6b;font-size:13px;margin-top:6px}"
"</style></head><body>"
// overlay senha (some quando liberar)
"<div id='lock'>"
" <div id='lockBox'>"
"  <h3>🔒 Acesso restrito</h3>"
"  <input id='pw' type='password' placeholder='Senha'>"
"  <button id='unlock'>Entrar</button>"
"  <div id='pwerr'></div>"
" </div>"
"</div>"
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
// ===== senha (primeira entrada) =====
"const PASSWORD='batata';"
"const lock=document.getElementById('lock');"
"const pw=document.getElementById('pw');"
"const unlock=document.getElementById('unlock');"
"const pwerr=document.getElementById('pwerr');"
"const unlockOk=()=>{lock.style.display='none';};"
"if(localStorage.getItem('espchat_ok')==='1'){unlockOk();}"
"unlock.onclick=()=>{"
" if(pw.value===PASSWORD){localStorage.setItem('espchat_ok','1'); unlockOk();}"
" else {pwerr.textContent='Senha incorreta'; pw.value=''; pw.focus();}"
"};"
"pw.addEventListener('keydown',(e)=>{if(e.key==='Enter')unlock.click();});"

// ===== chat =====
"const chat=document.getElementById('chat');"
"const nameEl=document.getElementById('name');"
"const textEl=document.getElementById('text');"
"const sendBtn=document.getElementById('send');"
"const st=document.getElementById('st');"
"nameEl.value=localStorage.getItem('espchat_name')||'';"
"nameEl.addEventListener('input',()=>localStorage.setItem('espchat_name',nameEl.value));"
"const esc=(s)=>String(s||'').replace(/[&<>\"']/g,m=>({"
"'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[m]));"
"const addMsg=(m)=>{"
" const d=document.createElement('div'); d.className='msg';"
" const t=new Date(m.ts||Date.now()).toLocaleTimeString();"
" d.innerHTML=`<div class='meta'>${esc(m.name||'Anon')} • ${t}</div><div>${esc(m.text||'')}</div>`;"
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
" if(localStorage.getItem('espchat_ok')!=='1'){addMsg({name:'Sistema',text:'Digite a senha para entrar.',ts:Date.now()}); return;}"
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

void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length){
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
  ws.broadcastTXT(msg);
}

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
}

void loop(){
  server.handleClient();
  ws.loop();
}
