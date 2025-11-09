#include <WiFi.h>
#include <PubSubClient.h>
#include "DHT.h"
#include "time.h"

// ---- Config Wi-Fi ----
const char* ssid = "Ruzin_2.4g";
const char* password = "eu29192313";

// ---- Config MQTT ----
const char* mqtt_server = "192.168.18.27";
const int   mqtt_port   = 1883;
const char* TOPICO      = "sensores/estufa";
WiFiClient espClient;
PubSubClient client(espClient);

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
const int PIN_LED_GROW = 23;   // Gate do MOSFET

// ---- Calibração ----
const int seco   = 2200; // Solo seco
const int molhado= 910;  // Solo molhado
const int ldrMin = 0;
const int ldrMax = 4095;

// ---- NTP ----
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -3 * 3600; // UTC-3
const int  daylightOffset_sec = 0;

// ---- Controle ----
bool bombaLigada = false;
unsigned long tempoInicioBomba = 0;
const unsigned long duracaoBomba   = 2000;     // 2 s
const unsigned long intervaloEnvio = 600000;   // 10 min
unsigned long ultimoEnvio = 0;
int umidadeAnterior = 100;

// ---- Luz grow (12h–22h) ----
bool luzLigada = false;
const int horaOn  = 12;
const int horaOff = 22;

String agoraStr() {
  struct tm t;
  if (!getLocalTime(&t)) return "Sem hora";
  char buf[25];
  strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &t);
  return String(buf);
}

void publishJSON(const String& payload) {
  client.publish(TOPICO, payload.c_str());
  Serial.println("MQTT -> " + String(TOPICO) + ": " + payload);
}

void setup_wifi() {
  Serial.print("Conectando a "); Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000); Serial.print(".");
  }
  Serial.println("\nWiFi conectado");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Conectando ao broker MQTT...");
    String clientId = "ESP32Client-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      Serial.println(" conectado!");
      String p = String("{\"data_hora\":\"") + agoraStr() +
                 "\",\"tipo\":\"evento\",\"evento\":\"ESP32 online\"}";
      publishJSON(p);
    } else {
      Serial.print(" falhou, rc="); Serial.print(client.state());
      Serial.println(" tentando em 5s...");
      delay(5000);
    }
  }
}

// --- Luz grow controlada por horário (real)
void atualizaLuzGrowPorHorario() {
  struct tm t;
  if (!getLocalTime(&t)) return;
  int h = t.tm_hour;
  bool deveLigar = (h >= horaOn && h < horaOff);

  if (deveLigar && !luzLigada) {
    digitalWrite(PIN_LED_GROW, HIGH);
    luzLigada = true;
    String evento = String("{\"data_hora\":\"") + agoraStr() +
                    "\",\"tipo\":\"evento\",\"evento\":\"Luz grow ligada\"}";
    publishJSON(evento);
  } else if (!deveLigar && luzLigada) {
    digitalWrite(PIN_LED_GROW, LOW);
    luzLigada = false;
    String evento = String("{\"data_hora\":\"") + agoraStr() +
                    "\",\"tipo\":\"evento\",\"evento\":\"Luz grow desligada\"}";
    publishJSON(evento);
  }

  // Validação: se deveria estar ligada mas está off fisicamente
  int estadoReal = digitalRead(PIN_LED_GROW);
  if (deveLigar && estadoReal == LOW) {
    String alerta = String("{\"data_hora\":\"") + agoraStr() +
                    "\",\"tipo\":\"alerta\",\"alerta\":\"Luz programada para ligada, mas está desligada\"}";
    publishJSON(alerta);
  }
}

void setup() {
  Serial.begin(115200);
  dht.begin();

  // Configura bomba
  pinMode(motorAIN1, OUTPUT);
  pinMode(motorAIN2, OUTPUT);
  digitalWrite(motorAIN1, LOW);
  digitalWrite(motorAIN2, LOW);

  // Configura luz grow
  pinMode(PIN_LED_GROW, OUTPUT);
  digitalWrite(PIN_LED_GROW, LOW);

  setup_wifi();
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  client.setServer(mqtt_server, mqtt_port);
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
    int valorSoloADC  = analogRead(sensorUmidade);   // <-- SOLO BRUTO (ADC)

    int umidadeSolo = map(valorSoloADC, seco, molhado, 0, 100);
    umidadeSolo = constrain(umidadeSolo, 0, 100);

    int luminosidade = map(valorLDR, ldrMin, ldrMax, 0, 100);
    luminosidade = constrain(luminosidade, 0, 100);

    // --- Lógica da bomba ---
    if (umidadeSolo <= 30 && umidadeAnterior > 30 && !bombaLigada) {
      Serial.println("Umidade crítica. Ligando bomba...");
      digitalWrite(motorAIN1, HIGH);
      digitalWrite(motorAIN2, LOW);
      bombaLigada = true;
      tempoInicioBomba = millis();

      // Evento com solo bruto
      String e = String("{\"data_hora\":\"") + agoraStr() +
                 "\",\"tipo\":\"evento\",\"evento\":\"Bomba ativada\"" +
                 ",\"umidade_solo\":" + String(umidadeSolo) +
                 ",\"solo_bruto\":"   + String(valorSoloADC) + "}";
      publishJSON(e);
    }
    umidadeAnterior = umidadeSolo;

    String statusBomba = bombaLigada ? "Bomba ativada" : "Bomba desativada";
    String statusLuz   = digitalRead(PIN_LED_GROW) ? "Luz ligada" : "Luz desligada";

    // Leitura periódica com solo bruto
    String leitura = String("{\"data_hora\":\"") + agoraStr() + "\"" +
                     ",\"tipo\":\"leituras\"" +
                     ",\"temperatura\":"   + String(temperatura, 1) +
                     ",\"umidade_ar\":"    + String(umidadeAr, 1) +
                     ",\"luminosidade\":"  + String(luminosidade) +
                     ",\"umidade_solo\":"  + String(umidadeSolo) +
                     ",\"solo_bruto\":"    + String(valorSoloADC) +
                     ",\"status_bomba\":\"" + statusBomba + "\"" +
                     ",\"status_luz\":\""   + statusLuz   + "\"}";
    publishJSON(leitura);
  }

  // --- Desliga bomba após tempo configurado ---
  if (bombaLigada && millis() - tempoInicioBomba > duracaoBomba) {
    digitalWrite(motorAIN1, LOW);
    digitalWrite(motorAIN2, LOW);
    bombaLigada = false;

    String e = String("{\"data_hora\":\"") + agoraStr() +
               "\",\"tipo\":\"evento\",\"evento\":\"Bomba desativada\"}";
    publishJSON(e);
    Serial.println("Bomba desativada.");
  }
}
