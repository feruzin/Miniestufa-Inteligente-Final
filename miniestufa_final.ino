#include <WiFi.h>
#include <PubSubClient.h>
#include "DHT.h"
#include "time.h"

// ---- Config Wi-Fi ----
const char* ssid = "Ruzin_2.4g";
const char* password = "eu29192313";

// ---- Config MQTT (Broker local na Raspberry Pi) ----
const char* mqtt_server = "192.168.18.33";  // IP da Raspberry Pi
const int mqtt_port = 1883;
WiFiClient espClient;
PubSubClient client(espClient);

// ---- Sensores ----
#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

const int sensorUmidade = 34;  // Solo (analógico)
const int sensorLDR = 33;      // Luminosidade (analógico)

// ---- Ponte H TB6612FNG (bomba) ----
const int motorAIN1 = 25;
const int motorAIN2 = 27;

// ---- Luz grow (MOSFET IRLZ44N) ----
const int PIN_LED_GROW = 23;   // gate do MOSFET

// ---- Calibração ----
const int seco = 2200;    // Solo seco
const int molhado = 910;  // Solo molhado
const int ldrMin = 0;     // Escuro
const int ldrMax = 4095;  // Máxima luminosidade do ADC

// ---- NTP (para data e hora) ----
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -3 * 3600;  // Brasília (UTC-3)
const int daylightOffset_sec = 0;

// ---- Controle ----
bool bombaLigada = false;
unsigned long tempoInicioBomba = 0;
const unsigned long duracaoBomba = 3000;     // <<< 3 segundos
const unsigned long intervaloEnvio = 600000; // 10 minutos
unsigned long ultimoEnvio = 0;
int umidadeAnterior = 100;                   // Valor inicial fictício

// ---- Controle da luz grow (06:00–16:00) ----
bool luzLigada = false;  // estado atual (para evitar reenvios)
const int horaOn  = 7;   // liga às 06:00
const int horaOff = 17;  // desliga às 16:00

// ---- Função para obter data/hora formatada ----
String getTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "Sem hora";
  }
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%d/%m/%Y %H:%M:%S", &timeinfo);
  return String(buffer);
}

void setup_wifi() {
  delay(100);
  Serial.println();
  Serial.print("Conectando a ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("\nWiFi conectado");
  Serial.print("IP obtido: ");
  Serial.println(WiFi.localIP());
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Conectando ao broker MQTT...");
    String clientId = "ESP32Client-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      Serial.println(" conectado!");
      client.publish("miniEstufaFelipe/status", "ESP32 online");
    } else {
      Serial.print(" falhou, rc=");
      Serial.print(client.state());
      Serial.println(" tentando em 5s...");
      delay(5000);
    }
  }
}

// Liga/desliga a luz grow com base no horário local (06:00–16:00)
void atualizaLuzGrowPorHorario() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    // sem hora válida: não altera o estado atual
    return;
  }
  int h = timeinfo.tm_hour;

  bool deveLigar = (h >= horaOn && h < horaOff);
  if (deveLigar != luzLigada) {
    luzLigada = deveLigar;
    digitalWrite(PIN_LED_GROW, luzLigada ? HIGH : LOW);

    // publica evento uma única vez por mudança
    String dataHora = getTimeString();
    String evento = String("{\"data_hora\": \"") + dataHora +
                    "\", \"evento\": \"Luz grow " + (luzLigada ? "ligada" : "desligada") +
                    "\"}";
    client.publish("miniEstufaFelipe/eventos", evento.c_str());
    Serial.println(evento);
  }
}

void setup() {
  Serial.begin(115200);
  dht.begin();

  // bomba
  pinMode(motorAIN1, OUTPUT);
  pinMode(motorAIN2, OUTPUT);
  digitalWrite(motorAIN1, LOW);
  digitalWrite(motorAIN2, LOW);

  // luz grow
  pinMode(PIN_LED_GROW, OUTPUT);
  digitalWrite(PIN_LED_GROW, LOW);
  luzLigada = false;

  setup_wifi();
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  client.setServer(mqtt_server, mqtt_port);
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  // Atualiza luz grow conforme horário local
  atualizaLuzGrowPorHorario();

  unsigned long agora = millis();

  // Envio periódico das leituras
  if (agora - ultimoEnvio > intervaloEnvio) {
    ultimoEnvio = agora;

    // ---- Leituras ----
    float temperatura = dht.readTemperature();
    float umidadeAr = dht.readHumidity();
    int valorLDR = analogRead(sensorLDR);
    int valorSoloBruto = analogRead(sensorUmidade);

    // ---- Processamento ----
    int umidadeSolo = map(valorSoloBruto, seco, molhado, 0, 100);
    umidadeSolo = constrain(umidadeSolo, 0, 100);

    int luminosidade = map(valorLDR, ldrMin, ldrMax, 0, 100);
    luminosidade = constrain(luminosidade, 0, 100);

    // ---- Lógica da bomba ----
    if (umidadeSolo <= 30 && umidadeAnterior > 30 && !bombaLigada) {
      Serial.println("Umidade atingiu limite crítico. Ligando bomba...");
      digitalWrite(motorAIN1, HIGH);
      digitalWrite(motorAIN2, LOW);
      bombaLigada = true;
      tempoInicioBomba = millis();

      String dataHoraEvento = getTimeString();
      String evento = "{\"data_hora\": \"" + dataHoraEvento + "\", \"evento\": \"Bomba ativada\", \"umidade_solo\": " + String(umidadeSolo) + "}";
      client.publish("miniEstufaFelipe/eventos", evento.c_str());
      client.publish("miniEstufaFelipe/status", "Bomba ativada");
      Serial.println(evento);
    }

    umidadeAnterior = umidadeSolo;

    // ---- Determina status atualizado ----
    String statusBomba = bombaLigada ? "Bomba ativada" : "Bomba desativada";
    String statusLuz = luzLigada ? "Luz ligada" : "Luz desligada";

    // ---- Publicação MQTT (leituras + status + valor bruto + data/hora) ----
    String dataHora = getTimeString();
    String leituraJSON = "{\"data_hora\": \"" + dataHora + "\"" +
                         ", \"temperatura\": " + String(temperatura, 1) +
                         ", \"umidade_ar\": " + String(umidadeAr, 1) +
                         ", \"luminosidade\": " + String(luminosidade) +
                         ", \"umidade_solo\": " + String(umidadeSolo) +
                         ", \"umidade_solo_bruto\": " + String(valorSoloBruto) +
                         ", \"status_bomba\": \"" + statusBomba + "\"" +
                         ", \"status_luz\": \"" + statusLuz + "\"}";
    client.publish("miniEstufaFelipe/leituras", leituraJSON.c_str());
    Serial.println("Leitura enviada: " + leituraJSON);
  }

  // ---- Desliga bomba após 3 s ----
  if (bombaLigada && millis() - tempoInicioBomba > duracaoBomba) {
    digitalWrite(motorAIN1, LOW);
    digitalWrite(motorAIN2, LOW);
    bombaLigada = false;

    String dataHoraDesl = getTimeString();
    String eventoDesl = "{\"data_hora\": \"" + dataHoraDesl + "\", \"evento\": \"Bomba desativada\"}";
    client.publish("miniEstufaFelipe/eventos", eventoDesl.c_str());
    client.publish("miniEstufaFelipe/status", "Bomba desativada");
    Serial.println("Bomba desativada.");
  }
}
