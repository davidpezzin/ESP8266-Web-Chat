# ESP8266 Web Chat

Um chat em tempo real rodando diretamente em um ESP8266 (NodeMCU / D1 mini), acessível de qualquer lugar da internet — sem banco de dados, sem login e sem servidor externo.

> Projeto simples para transformar um ESP8266 em um servidor WebSocket + página web de chat. Ideal para demonstrações, prototipagem e aprendizado de IoT.

---

## Recursos
- Chat em tempo real direto do ESP8266 via WebSocket
- Interface web responsiva que roda no próprio módulo
- Sem dependência de servidor externo ou banco de dados
- Configuração mínima: conexão Wi‑Fi (SSID + senha)

---

## Requisitos de hardware
- ESP8266 (NodeMCU, Wemos D1 mini ou equivalente)
- Cabo micro‑USB para alimentação/programação
- (Opcional) Fonte externa 5V para projetos fora do USB

Recomendação: Use o NodeMCU ou D1 mini para facilitar programação via USB.

---

## Requisitos de software
- Arduino IDE ou PlatformIO
- Placa ESP8266 instalada no Arduino IDE (via Boards Manager) ou suporte ESP8266 no PlatformIO
- Bibliotecas usadas no projeto (ver o sketch): (ex.: ESP8266WiFi, ESPAsyncWebServer, AsyncTCP) — instale pelo Library Manager quando necessário

---

## Instalação e upload
1. Abra o projeto/arquivo `.ino` no Arduino IDE ou importe o projeto no PlatformIO.
2. Se necessário, atualize as credenciais Wi‑Fi:
   - Procure constantes como `WIFI_SSID` / `WIFI_PASSWORD` no topo do sketch e substitua pelos seus dados.
   - Se o projeto usar um arquivo `config.h`, edite esse arquivo.
3. Selecione a placa correta (ex.: Wemos D1 R2 & Mini) e a porta serial.
4. Compile e faça upload para o ESP8266.

Exemplo (no Arduino IDE):
```cpp
#define WIFI_SSID "sua_rede"
#define WIFI_PASSWORD "sua_senha"
```

---

## Uso
- Após o upload, abra o monitor serial (115200 bauds) para ver o IP atribuído pelo roteador.
- Acesse o chat pelo navegador usando o IP mostrado, por exemplo:
  - http://192.168.1.42
  - ou, se o mDNS estiver configurado, http://esp8266-chat.local
- Abra a página web, digite um apelido e comece a conversar. Mensagens são propagadas entre todos os clientes conectados ao ESP via WebSocket.

---

## Customização
- Layout/JS/HTML: os arquivos da interface estão embutidos no sketch (ou na pasta `data`); edite para alterar aparência e comportamento.
- Mensagens e formatos: modifique a lógica de WebSocket no arquivo principal para filtrar, transformar ou registrar mensagens.
- Recursos extras: você pode adicionar persistência (opcional), autenticação básica ou uma página de administração.

---

## Segurança e privacidade
- Comunicação padrão é por HTTP/WebSocket sem criptografia — tráfego pode ser interceptado na rede local/internet.
- Como o projeto não usa login, qualquer pessoa com acesso à URL pode participar do chat.
- Recomendações:
  - Use uma rede separada ou firewall para expor o dispositivo à internet.
  - Se precisar de HTTPS, coloque o ESP por trás de um proxy reverso (Caddy, nginx) que faça TLS.
  - Adicione autenticação se o uso não for público/demonstração.

---

## Solução de problemas
- Não obtém IP?
  - Verifique SSID/senha no sketch.
  - Confira se o roteador permite novos dispositivos e não está bloqueando MACs.
- Erro ao compilar?
  - Instale as bibliotecas necessárias e confirme a versão do core ESP8266.
- Página não carrega, mas o ESP está no ar?
  - Verifique o console do navegador (DevTools) por erros de WebSocket/JS.
  - Abra o monitor serial para mensagens de log do servidor.

---

## Contribuições
Contribuições são bem‑vindas! Abra uma issue para discutir alterações ou envie um pull request com melhorias (README, estabilidade, novos recursos).

---

## Licença


---
