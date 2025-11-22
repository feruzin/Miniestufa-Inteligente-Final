#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include "DHT.h"
#include "time.h"

// ---- Wi-Fi ----
const char* ssid = "Rude";
const char* password = "eu29192313";

// ---- MQTT local (opcional) ----
const char* mqtt_server = "192.168.217.100";
const int   mqtt_port   = 1883;
const char* TOPICO      = "sensores/estufa"; // leitura/telemetria
WiFiClient espClient;
PubSubClient client(espClient);

// ---- Backend HTTP(S) ----
const char* BACKEND_URL = "https://miniestufa-backend.onrender.com/api/sensor/push"; // HTTPS!

// ---- Sensores ----
#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);
const int sensorUmidade = 34;  // Solo (ADC)
const int sensorLDR     = 33;  // Luminosidade (ADC)

// ---- Ponte H TB6612FNG (bomba) ----
const int motorAIN1 = 25;
const int motorAIN2 = 27;

// ---- Luz grow (MOSFET IRLZ44N) ----
const int PIN_LED_GROW = 23;

// ---- Calibração ----
const int seco   = 2200; // Solo seco
const int molhado= 910;  // Solo molhado
const int ldrMin = 0;
const int ldrMax = 4095;

// ---- NTP: configurar para UTC (offset 0) ----
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0; // <-- UTC
const int  daylightOffset_sec = 0;

// ---- Controle ----
bool bombaLigada = false;
unsigned long tempoInicioBomba = 0;
const unsigned long duracaoBomba   = 2000;     // 2 s (padrão)
const unsigned long intervaloEnvio = 600000;   // 10 min (para testar rápido, use 30000)
unsigned long ultimoEnvio = 0;
int umidadeAnterior = 100;

// ---- Luz grow (12h–22h) ----
bool luzLigada = false;
const int horaOn  = 12;
const int horaOff = 22;

// ---- Identificador do dispositivo (para tópicos MQTT) ----
String dispositivoId;

// variáveis para controlar duração customizada vinda do broker
unsigned long globalDuracaoBomba = duracaoBomba;

// ---------- Utilidades de data/hora ----------
// Retorna timestamp em UTC no formato ISO 8601 (ex.: 2025-11-22T14:30:00Z)
String agoraISO8601UTC() {
  struct tm t;
  if (!getLocalTime(&t)) return "SemHora";
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
  return String(buf);
}

// Formato legível dd/mm/yyyy hh:mm:ss (UTC)
String agoraStrDDMMYYYY() {
  struct tm t;
  if (!getLocalTime(&t)) return "SemHora";
  char buf[25];
  strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &t);
  return String(buf);
}

// ---------- MQTT helpers ----------
void publishJSON(const String& topic, const String& payload) {
  client.publish(topic.c_str(), payload.c_str());
  Serial.println("MQTT -> " + topic + ": " + payload);
}

// Callback MQTT: recebe comandos da Raspberry Pi/IA no tópico estufa/controle
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  Serial.println("MQTT recv topic: " + String(topic) + " payload: " + msg);

  // ------- COMANDO DE BOMBA -------
  if (msg.indexOf("\"cmd\":\"bomba\"") >= 0) {

    // LIGAR
    if (msg.indexOf("\"action\":\"on\"") >= 0) {

      unsigned long dur = duracaoBomba; // padrão
      int p = msg.indexOf("duration_ms");
      if (p >= 0) {
        int colon = msg.indexOf(':', p);
        if (colon >= 0) {
          // encontrar fim do número
          int comma = msg.indexOf(',', colon);
          int endbr = msg.indexOf('}', colon);
          int endpos = (comma >= 0 && comma < endbr) ? comma : endbr;
          if (endpos > colon) {
            String sdur = msg.substring(colon+1, endpos);
            sdur.trim();
            unsigned long tmp = (unsigned long) sdur.toInt();
            if (tmp > 0) dur = tmp;
          }
        }
      }

      Serial.println("IA -> LIGAR bomba por " + String(dur) + " ms");

      digitalWrite(motorAIN1, HIGH);
      digitalWrite(motorAIN2, LOW);
      bombaLigada = true;
      tempoInicioBomba = millis();
      globalDuracaoBomba = dur;

      // ACK
      publishJSON("estufa/controle/resposta",
        "{\"ok\":true,\"cmd\":\"bomba_on\",\"duration_ms\":" + String(dur) + "}"
      );

      return;
    }

    // DESLIGAR
    if (msg.indexOf("\"action\":\"off\"") >= 0) {
      Serial.println("IA -> DESLIGAR bomba");

      digitalWrite(motorAIN1, LOW);
      digitalWrite(motorAIN2, LOW);
      bombaLigada = false;

      publishJSON("estufa/controle/resposta",
        "{\"ok\":true,\"cmd\":\"bomba_off\"}"
      );

      return;
    }
  }

  // ------- COMANDO DE LUZ -------
  if (msg.indexOf("\"cmd\":\"luz\"") >= 0) {
    if (msg.indexOf("\"action\":\"on\"") >= 0) {
      digitalWrite(PIN_LED_GROW, HIGH);
      luzLigada = true;
      publishJSON("estufa/controle/resposta","{\"ok\":true,\"cmd\":\"luz_on\"}");
      return;
    }
    if (msg.indexOf("\"action\":\"off\"") >= 0) {
      digitalWrite(PIN_LED_GROW, LOW);
      luzLigada = false;
      publishJSON("estufa/controle/resposta","{\"ok\":true,\"cmd\":\"luz_off\"}");
      return;
    }
  }
}

// reconecta e se inscreve no tópico estufa/controle
void reconnect() {
  while (!client.connected()) {
    Serial.print("Conectando ao broker MQTT...");
    String clientId = "ESP32Client-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      Serial.println(" conectado!");
      // publicar evento online
      String p = String("{\"data_hora\":\"") + agoraStrDDMMYYYY() +
                 "\",\"tipo\":\"evento\",\"evento\":\"ESP32 online\"}";
      publishJSON(TOPICO, p);

      // inscrever no tópico de comando
      client.subscribe("estufa/controle");
      Serial.println("Inscrito em: estufa/controle");
    } else {
      Serial.print(" falhou, rc="); Serial.print(client.state());
      Serial.println(" tentando em 5s...");
      delay(5000);
    }
  }
}

// ---------- Luz grow por horário ----------
void atualizaLuzGrowPorHorario() {
  struct tm t;
  if (!getLocalTime(&t)) return;
  int h = t.tm_hour; // UTC hour
  bool deveLigar = (h >= horaOn && h < horaOff);

  if (deveLigar && !luzLigada) {
    digitalWrite(PIN_LED_GROW, HIGH);
    luzLigada = true;
    String evento = String("{\"data_hora\":\"") + agoraISO8601UTC() +
                    "\",\"tipo\":\"evento\",\"evento\":\"Luz grow ligada\"}";
    publishJSON(TOPICO, evento);
  } else if (!deveLigar && luzLigada) {
    digitalWrite(PIN_LED_GROW, LOW);
    luzLigada = false;
    String evento = String("{\"data_hora\":\"") + agoraISO8601UTC() +
                    "\",\"tipo\":\"evento\",\"evento\":\"Luz grow desligada\"}";
    publishJSON(TOPICO, evento);
  }

  // Validação física opcional
  int estadoReal = digitalRead(PIN_LED_GROW);
  if (deveLigar && estadoReal == LOW) {
    String alerta = String("{\"data_hora\":\"") + agoraISO8601UTC() +
                    "\",\"tipo\":\"alerta\",\"alerta\":\"Luz programada para ligada, mas está desligada\"}";
    publishJSON(TOPICO, alerta);
  }
}

// ---------- POST HTTPS para o backend ----------
bool postLeituraNoBackend(const String& payload) {
  WiFiClientSecure secureClient;
  secureClient.setTimeout(15000);
  secureClient.setInsecure(); // PARA PRODUÇÃO: troque por secureClient.setCACert(<ISRG Root X1>);

  HTTPClient http;
  if (!http.begin(secureClient, BACKEND_URL)) {
    Serial.println("http.begin() falhou");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "ESP32-Miniestufa/1.0");

  int code = http.POST((uint8_t*)payload.c_str(), payload.length());
  String resp = http.getString();

  Serial.printf("[HTTP] status=%d\n", code);
  Serial.println("Resposta backend: " + resp);

  http.end();
  return (code >= 200 && code < 300);
}

void setup() {
  Serial.begin(115200);
  dht.begin();

  pinMode(motorAIN1, OUTPUT);
  pinMode(motorAIN2, OUTPUT);
  digitalWrite(motorAIN1, LOW);
  digitalWrite(motorAIN2, LOW);

  pinMode(PIN_LED_GROW, OUTPUT);
  digitalWrite(PIN_LED_GROW, LOW);

  // id do dispositivo baseado no MAC (remove ':')
  // Note: para garantir que WiFi.macAddress() funcione, conectamos à rede primeiro
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Conectando a ");
  Serial.println(ssid);
  int wifi_tries = 0;
  while (WiFi.status() != WL_CONNECTED && wifi_tries < 40) { delay(250); Serial.print("."); wifi_tries++; }
  if (WiFi.status() == WL_CONNECTED) {
    dispositivoId = WiFi.macAddress();
    dispositivoId.replace(":", "");
    Serial.println("\nDeviceId: " + dispositivoId);
    Serial.print("IP: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFalha ao conectar WiFi na setup (continuando, pode reconectar depois)");
    dispositivoId = "unknown";
  }

  // NTP antes de TLS: configurar para UTC
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.print("Sincronizando NTP (UTC)");
  time_t now = time(nullptr);
  int tries = 0;
  while (now < 8 * 3600 * 2 && tries < 30) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    tries++;
  }
  Serial.println("\nHora atual (UTC): " + String(ctime(&now)));

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  atualizaLuzGrowPorHorario();

  unsigned long agora = millis();

  // --- Leitura e envio periódico ---
  if (agora - ultimoEnvio > intervaloEnvio) {
    ultimoEnvio = agora;

    float temperatura = dht.readTemperature();
    float umidadeAr   = dht.readHumidity();
    int valorLDR      = analogRead(sensorLDR);
    int valorSoloADC  = analogRead(sensorUmidade); // SOLO BRUTO (ADC)

    int umidadeSolo = map(valorSoloADC, seco, molhado, 0, 100);
    umidadeSolo = constrain(umidadeSolo, 0, 100);

    int luminosidade = map(valorLDR, ldrMin, ldrMax, 0, 100);
    luminosidade = constrain(luminosidade, 0, 100);

    // --- Lógica local da bomba (habilita só se IA não enviar comando) ---
    if (umidadeSolo <= 30 && umidadeAnterior > 30 && !bombaLigada) {
      Serial.println("Umidade crítica. Ligando bomba (lógica local)...");
      digitalWrite(motorAIN1, HIGH);
      digitalWrite(motorAIN2, LOW);
      bombaLigada = true;
      tempoInicioBomba = millis();
      globalDuracaoBomba = duracaoBomba; // usar padrão

      String e = String("{\"data_hora\":\"") + agoraISO8601UTC() +
                 "\",\"tipo\":\"evento\",\"evento\":\"Bomba ativada (local)\"" +
                 ",\"umidade_solo\":" + String(umidadeSolo) +
                 ",\"solo_bruto\":"   + String(valorSoloADC) + "}";
      publishJSON(TOPICO, e);
    }
    umidadeAnterior = umidadeSolo;

    String statusBomba = bombaLigada ? "Bomba ativada" : "Bomba desativada";
    String statusLuz   = digitalRead(PIN_LED_GROW) ? "Luz ligada" : "Luz desligada";

    // ---------- Payload para o BACKEND (ISO UTC) ----------
    String payloadBackend = String("{") +
      "\"data_hora\":\""         + agoraISO8601UTC() + "\"," +
      "\"temperatura\":"         + String(temperatura, 1) + "," +
      "\"umidade_ar\":"          + String(umidadeAr, 1) + "," +
      "\"luminosidade\":"        + String(luminosidade) + "," +
      "\"umidade_solo\":"        + String(umidadeSolo) + "," +
      "\"umidade_solo_bruto\":"  + String(valorSoloADC) + "," +
      "\"status_luz\":\""        + statusLuz + "\"," +
      "\"status_bomba\":\""      + statusBomba + "\"" +
    "}";

    // Envia para backend (HTTPS)
    bool ok = postLeituraNoBackend(payloadBackend);

    // (Opcional) também publica no MQTT local para logging/histórico interno
    String leituraMQTT = String("{\"data_hora\":\"") + agoraISO8601UTC() + "\"" +
                     ",\"tipo\":\"leituras\"" +
                     ",\"temperatura\":"   + String(temperatura, 1) +
                     ",\"umidade_ar\":"    + String(umidadeAr, 1) +
                     ",\"luminosidade\":"  + String(luminosidade) +
                     ",\"umidade_solo\":"  + String(umidadeSolo) +
                     ",\"solo_bruto\":"    + String(valorSoloADC) +
                     ",\"status_bomba\":\"" + statusBomba + "\"" +
                     ",\"status_luz\":\""   + statusLuz   + "\"" +
                     ",\"post_backend\":"   + String(ok ? "true" : "false") +
                     "}";
    publishJSON(TOPICO, leituraMQTT);
  }

  // --- Desliga bomba após tempo configurado ---
  if (bombaLigada && millis() - tempoInicioBomba > globalDuracaoBomba) {
    digitalWrite(motorAIN1, LOW);
    digitalWrite(motorAIN2, LOW);
    bombaLigada = false;

    String e = String("{\"data_hora\":\"") + agoraISO8601UTC() +
               "\",\"tipo\":\"evento\",\"evento\":\"Bomba desativada\"}";
    publishJSON(TOPICO, e);
    Serial.println("Bomba desativada.");
    // reset duracao para padrão
    globalDuracaoBomba = duracaoBomba;
  }
}
