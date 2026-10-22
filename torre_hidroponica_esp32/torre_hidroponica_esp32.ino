// ============================================
// ESP32 - Torre AeroTower  v2 (várias torres)
// ============================================
// O que mudou em relação à v1:
//  * id da torre = "AT-" + MAC da placa (ex.: AT-24DCC3A1B2C4). O mesmo
//    firmware serve para qualquer torre, sem recompilar.
//  * cada torre tem a sua "caixa" no broker:
//      aerotower/<id>/sensores        (publica a cada 5 s)
//      aerotower/<id>/motor/comando   (assina)
//      aerotower/<id>/motor/status    (publica, retido)
//      aerotower/<id>/status          (online; o broker publica offline sozinho)
//      aerotower/<id>/info            (firmware, kit e sensores, retido)
//  * client ID do MQTT = id da torre. Na v1 todas usavam "esp32-hidroponia"
//    e uma derrubava a outra do broker.
//  * kit escolhido em config.h: sensor que o kit não tem não é lido nem
//    publicado.
//  * Wi-Fi configurado pelo celular (portal "AeroTower-XXXX"), sem senha
//    no código. Segure o botão BOOT por 5 s para trocar de rede.
//  * a bomba segue o ciclo mesmo sem internet, e o último ciclo fica salvo
//    para voltar depois de uma queda de energia.
//
// Bibliotecas (Gerenciador de Bibliotecas da Arduino IDE):
//   PubSubClient (Nick O'Leary), ArduinoJson 6.x, WiFiManager (tzapu) 2.0.x,
//   Adafruit SHT31 (só para o kit Pro)
// Placa: "ESP32 Dev Module" (núcleo esp32 da Espressif 2.x ou 3.x)
// ============================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_mac.h>

#include "config.h"
#include "segredos.h"   // copie de segredos.h.exemplo

#if TEM_PRO
#include <Wire.h>
#include "Adafruit_SHT31.h"
#endif

#define MINUTOS_EM_MS(m) ((unsigned long)(m) * 60UL * 1000UL)

// ── Identidade e tópicos (montados no boot) ──
String torreId;
String topicoSensores, topicoComando, topicoMotorStatus, topicoStatus, topicoInfo;

WiFiManager      wm;
WiFiClientSecure espClient;
PubSubClient     mqttClient(espClient);
Preferences      prefs;

// ── Bomba ─────────────────────────────────
bool          motorLigado        = false;
bool          cicloAtivo         = false;
unsigned long minutosCicloLigado = CICLO_PADRAO_LIGADO_MIN;
unsigned long minutosCicloDeslig = CICLO_PADRAO_DESLIGADO_MIN;
unsigned long motorMudouEm       = 0;

// ── Sensores ──────────────────────────────
float temperature = 25.0;   // também corrige o som (nível) e o TDS
float humidity    = 0.0;
bool  shtOk       = false;

#if TEM_PRO
#define VREF    3.3
#define ADC_RES 4096.0
#define SCOUNT  30
int   analogBufferTDS[SCOUNT], analogBufferPH[SCOUNT], bufferTemp[SCOUNT];
int   analogBufferIndex = 0;
float tdsValue = 0, phValue = 0;
Adafruit_SHT31 sht31 = Adafruit_SHT31();
#endif

#if TEM_NIVEL
#define ECHO_TIMEOUT_US 30000UL
#define N_LEITURAS_US   5
#define PAUSA_US_MS     30
#define TANQUE_CRITICO  10
#define TANQUE_BAIXO    35
#define TANQUE_CHEIO    90
float tanqueDistanciaCm = -1, tanqueNivelCm = 0, tanqueLitros = 0, tanquePercentual = 0;
bool  tanqueOk = false;
#endif

void publishMotorStatus();

// =============================================
// Identidade
// =============================================
String lerTorreId() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);   // gravado de fábrica no chip
  char id[16];
  snprintf(id, sizeof(id), "AT-%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(id);
}

void montarTopicos() {
  String base = String(MQTT_PREFIXO) + "/" + torreId;
  topicoSensores    = base + "/sensores";
  topicoComando     = base + "/motor/comando";
  topicoMotorStatus = base + "/motor/status";
  topicoStatus      = base + "/status";
  topicoInfo        = base + "/info";
}

// =============================================
// Bomba: relé, ciclo e memória
// =============================================
void salvarCiclo() {
  prefs.putBool("ativo", cicloAtivo);
  prefs.putULong("lig", minutosCicloLigado);
  prefs.putULong("des", minutosCicloDeslig);
}

void _ligarRele() {
  digitalWrite(RELE_PIN, HIGH);
  motorLigado  = true;
  motorMudouEm = millis();
  Serial.printf("⚡ Bomba LIGADA (%lu min)\n", minutosCicloLigado);
  publishMotorStatus();
}

void _desligarRele() {
  digitalWrite(RELE_PIN, LOW);
  motorLigado  = false;
  motorMudouEm = millis();
  Serial.println("🔌 Bomba DESLIGADA");
  publishMotorStatus();
}

void iniciarCiclo(unsigned long minLigado, unsigned long minDesligado) {
  minutosCicloLigado = constrain(minLigado, 1UL, 1440UL);
  minutosCicloDeslig = constrain(minDesligado, 1UL, 1440UL);
  cicloAtivo = true;
  salvarCiclo();
  Serial.printf("🔄 Ciclo: %lu min ligada / %lu min desligada\n",
                minutosCicloLigado, minutosCicloDeslig);
  _ligarRele();
}

void pararTudo() {
  cicloAtivo = false;
  salvarCiclo();
  _desligarRele();
}

void gerenciarMotor() {
  if (!cicloAtivo) return;
  unsigned long decorrido = millis() - motorMudouEm;
  if (motorLigado && decorrido >= MINUTOS_EM_MS(minutosCicloLigado)) {
    _desligarRele();
  } else if (!motorLigado && decorrido >= MINUTOS_EM_MS(minutosCicloDeslig)) {
    _ligarRele();
  }
}

// =============================================
// Nível (kit Completo e Pro)
// =============================================
#if TEM_NIVEL
float medirUmaVezCm(float tempC) {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long duracao = pulseIn(ECHO_PIN, HIGH, ECHO_TIMEOUT_US);
  if (duracao == 0) return -1.0;
  return duracao * (331.3 + 0.606 * tempC) / 20000.0;
}

float medirDistanciaCm(float tempC) {
  float leituras[N_LEITURAS_US];
  int validas = 0;
  for (int i = 0; i < N_LEITURAS_US; i++) {
    float d = medirUmaVezCm(tempC);
    if (d > 1.0 && d < 400.0) leituras[validas++] = d;
    delay(PAUSA_US_MS);
  }
  if (validas < (N_LEITURAS_US / 2 + 1)) return -1.0;
  for (int i = 1; i < validas; i++) {
    float x = leituras[i]; int j = i - 1;
    while (j >= 0 && leituras[j] > x) { leituras[j + 1] = leituras[j]; j--; }
    leituras[j + 1] = x;
  }
  return (validas & 1) ? leituras[validas / 2]
                       : (leituras[validas / 2 - 1] + leituras[validas / 2]) / 2.0;
}

float volumeAteLitros(float h) {
  if (h <= 0) return 0;
  float r1 = DIAMETRO_FUNDO_CM / 2.0, rTopo = DIAMETRO_TOPO_CM / 2.0;
  float r2 = r1 + (rTopo - r1) * (h / ALTURA_BALDE_CM);
  return PI * h / 3.0 * (r1 * r1 + r1 * r2 + r2 * r2) / 1000.0;
}

void lerTanque() {
  float dist = medirDistanciaCm(shtOk ? temperature : 25.0);
  if (dist < 0) { tanqueOk = false; tanqueDistanciaCm = -1; return; }
  tanqueOk          = true;
  tanqueDistanciaCm = dist;
  tanqueNivelCm     = constrain(DIST_SENSOR_FUNDO_CM - dist, 0.0, ALTURA_MAX_AGUA_CM);
  tanqueLitros      = volumeAteLitros(tanqueNivelCm);
  tanquePercentual  = tanqueLitros / volumeAteLitros(ALTURA_MAX_AGUA_CM) * 100.0;
}

const char* classificarTanque(float pct) {
  if (pct < TANQUE_CRITICO) return "vazio";
  if (pct < TANQUE_BAIXO)   return "baixo";
  if (pct <= TANQUE_CHEIO)  return "ok";
  return "cheio";
}
#endif

// =============================================
// pH, TDS e clima (kit Pro)
// =============================================
#if TEM_PRO
int mediana(int origem[], int n) {
  for (int i = 0; i < n; i++) bufferTemp[i] = origem[i];
  for (int j = 0; j < n - 1; j++)
    for (int i = 0; i < n - j - 1; i++)
      if (bufferTemp[i] > bufferTemp[i + 1]) {
        int t = bufferTemp[i]; bufferTemp[i] = bufferTemp[i + 1]; bufferTemp[i + 1] = t;
      }
  return (n & 1) ? bufferTemp[(n - 1) / 2] : (bufferTemp[n / 2] + bufferTemp[n / 2 - 1]) / 2;
}

void amostrarAnalogicos() {
  static unsigned long ultima = 0;
  if (millis() - ultima < 40U) return;
  ultima = millis();
  analogBufferTDS[analogBufferIndex] = analogRead(TDS_PIN);
  analogBufferPH[analogBufferIndex]  = analogRead(PH_PIN);
  if (++analogBufferIndex == SCOUNT) analogBufferIndex = 0;
}

void calcularPhTds() {
  float v = mediana(analogBufferTDS, SCOUNT) * VREF / ADC_RES;
  float comp = v / (1.0 + 0.02 * (temperature - 25.0));
  tdsValue = (133.42 * comp * comp * comp - 255.86 * comp * comp + 857.39 * comp) * 0.5;

  float vPh = mediana(analogBufferPH, SCOUNT) * VREF / ADC_RES;
  float slope = (4.0 - 7.0) / (PH_CAL_V4 - PH_CAL_V7);
  phValue = 7.0 + slope * (vPh - PH_CAL_V7);
}

void lerSHT3x() {
  float t = sht31.readTemperature(), h = sht31.readHumidity();
  shtOk = !isnan(t) && !isnan(h);
  if (shtOk) { temperature = t; humidity = h; }
}

const char* classificarNutriente(float tds) {
  if (tds < 50)  return "deficiencia_grave";
  if (tds < 100) return "deficiencia_moderada";
  if (tds < 200) return "abaixo_do_ideal";
  if (tds < 400) return "otimo";
  if (tds < 700) return "excesso_leve";
  return "excesso_grave";
}

const char* classificarPH(float ph) {
  if (ph < 0 || ph > 14) return "leitura_invalida";
  if (ph < 5.5)  return "muito_acido";
  if (ph < 6.0)  return "acido";
  if (ph < 6.5)  return "levemente_acido";
  if (ph <= 7.5) return "ideal";
  if (ph <= 8.0) return "levemente_alcalino";
  if (ph <= 9.0) return "alcalino";
  return "muito_alcalino";
}

const char* classificarAmbiente(float temp, float hum) {
  bool tOk = temp >= 18.0 && temp <= 28.0, hOk = hum >= 40.0 && hum <= 70.0;
  if (tOk && hOk)   return "ideal";
  if (!tOk && !hOk) return "temp_e_umidade_fora";
  if (!tOk)         return temp < 18.0 ? "frio" : "quente";
  return hum < 40.0 ? "seco" : "umido";
}
#endif

// =============================================
// Publicações
// =============================================
void publishMotorStatus() {
  if (!mqttClient.connected()) return;
  StaticJsonDocument<256> doc;
  doc["ligado"]            = motorLigado;
  doc["ciclo_ativo"]       = cicloAtivo;
  doc["minutos_ligado"]    = minutosCicloLigado;
  doc["minutos_desligado"] = minutosCicloDeslig;
  unsigned long decorrido = millis() - motorMudouEm;
  if (cicloAtivo) {
    unsigned long ms = MINUTOS_EM_MS(motorLigado ? minutosCicloLigado : minutosCicloDeslig);
    doc[motorLigado ? "segundos_ate_desligar" : "segundos_ate_ligar"] =
        decorrido < ms ? (ms - decorrido) / 1000 : 0;
  }
  char payload[256];
  serializeJson(doc, payload, sizeof(payload));
  mqttClient.publish(topicoMotorStatus.c_str(), payload, true);
}

void publishInfo() {
  StaticJsonDocument<256> doc;
  doc["firmware"] = FIRMWARE_VERSAO;
  doc["kit"]      = KIT_NOME;
  JsonArray sensores = doc.createNestedArray("sensores");
#if TEM_NIVEL
  sensores.add("nivel");
#endif
#if TEM_PRO
  sensores.add("ph");
  sensores.add("tds");
  if (shtOk) sensores.add("clima");   // só se o SHT3x respondeu no boot
#endif
  doc["ip"]   = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  char payload[256];
  serializeJson(doc, payload, sizeof(payload));
  mqttClient.publish(topicoInfo.c_str(), payload, true);
}

void publishSensores() {
  StaticJsonDocument<512> doc;

#if TEM_PRO
  if (shtOk) {
    doc["temperaturaAmbiente"] = serialized(String(temperature, 1));
    doc["umidadeRelativa"]     = serialized(String(humidity, 1));
    doc["ambiente"]            = classificarAmbiente(temperature, humidity);
  }
  doc["ph"]         = serialized(String(phValue, 2));
  doc["ph_status"]  = classificarPH(phValue);
  doc["tds"]        = round(tdsValue);
  doc["ec"]         = serialized(String(tdsValue / 500.0, 2));   // mS/cm
  doc["tds_status"] = classificarNutriente(tdsValue);
#endif

#if TEM_NIVEL
  if (tanqueOk) {
    doc["nivelAgua"]           = serialized(String(tanquePercentual, 1));
    doc["tanque_litros"]       = serialized(String(tanqueLitros, 2));
    doc["tanque_nivel_cm"]     = serialized(String(tanqueNivelCm, 1));
    doc["tanque_distancia_cm"] = serialized(String(tanqueDistanciaCm, 1));
    doc["tanque_status"]       = classificarTanque(tanquePercentual);
  } else {
    doc["nivelAgua"]     = nullptr;   // sem leitura != tanque vazio
    doc["tanque_status"] = "sensor_falhou";
  }
#endif

  doc["motor"]       = motorLigado;
  doc["ciclo_ativo"] = cicloAtivo;
  doc["rssi"]        = WiFi.RSSI();

  char payload[512];
  serializeJson(doc, payload, sizeof(payload));
  mqttClient.publish(topicoSensores.c_str(), payload);
  Serial.printf("✔ [%s] %s\n", topicoSensores.c_str(), payload);
}

// =============================================
// Comandos do app (via back-end)
// =============================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (topicoComando != topic) return;

  StaticJsonDocument<200> doc;
  if (deserializeJson(doc, payload, length)) {
    Serial.println("⚠ Comando com JSON inválido");
    return;
  }
  const char* acao = doc["acao"] | "";
  Serial.printf("📩 Comando: %s\n", acao);

  if (strcmp(acao, "ciclo") == 0) {
    iniciarCiclo(doc["minutos_ligado"] | 15UL, doc["minutos_desligado"] | 45UL);
  } else if (strcmp(acao, "desligar") == 0) {
    pararTudo();
  } else {
    Serial.println("⚠ Ação desconhecida");
  }
}

// =============================================
// Conectividade (nada aqui trava o loop: a bomba não pode esperar o Wi-Fi)
// =============================================
void iniciarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  wm.setConfigPortalBlocking(false);
  wm.setTitle("AeroTower");

  if (wm.getWiFiIsSaved()) {
    Serial.println("Wi-Fi salvo encontrado, conectando...");
    WiFi.begin();
  } else {
    String rede = "AeroTower-" + torreId.substring(torreId.length() - 4);
    Serial.printf("Sem Wi-Fi salvo. Conecte o celular na rede \"%s\" (senha %s)\n",
                  rede.c_str(), SENHA_PORTAL_WIFI);
    wm.startConfigPortal(rede.c_str(), SENHA_PORTAL_WIFI);
  }
}

void verificarBotaoResetWiFi() {
  static unsigned long apertadoDesde = 0;
  if (digitalRead(BOTAO_PIN) == LOW) {
    if (apertadoDesde == 0) apertadoDesde = millis();
    if (millis() - apertadoDesde > 5000) {
      Serial.println("🔁 Apagando Wi-Fi salvo e reiniciando...");
      wm.resetSettings();
      delay(300);
      ESP.restart();
    }
  } else {
    apertadoDesde = 0;
  }
}

void manterMQTT() {
  static unsigned long ultimaTentativa = 0;
  if (WiFi.status() != WL_CONNECTED || mqttClient.connected()) return;
  if (ultimaTentativa != 0 && millis() - ultimaTentativa < 5000) return;
  ultimaTentativa = millis();

  Serial.print("Conectando ao broker... ");
  // Testamento (LWT): se a torre sumir sem avisar, o broker publica
  // "offline" no lugar dela.
  bool ok = mqttClient.connect(torreId.c_str(), MQTT_USUARIO, MQTT_SENHA,
                               topicoStatus.c_str(), 1, true, "{\"status\":\"offline\"}");
  if (!ok) {
    Serial.printf("falhou (estado %d)\n", mqttClient.state());
    return;
  }
  Serial.println("ok");
  mqttClient.publish(topicoStatus.c_str(), "{\"status\":\"online\"}", true);
  publishInfo();
  mqttClient.subscribe(topicoComando.c_str(), 1);
  publishMotorStatus();
}

// =============================================
// SETUP
// =============================================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(RELE_PIN, OUTPUT);
  digitalWrite(RELE_PIN, LOW);
  pinMode(BOTAO_PIN, INPUT_PULLUP);

  torreId = lerTorreId();
  montarTopicos();

  Serial.println("\n==========================================");
  Serial.printf("TORRE_ID=%s\n", torreId.c_str());   // lido pelo script de fábrica
  Serial.printf("KIT=%s FIRMWARE=%s\n", KIT_NOME, FIRMWARE_VERSAO);
  Serial.println("==========================================");

#if TEM_NIVEL
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);
#endif

#if TEM_PRO
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  pinMode(TDS_PIN, INPUT);
  pinMode(PH_PIN, INPUT);
  Wire.begin(SDA_PIN, SCL_PIN);
  shtOk = sht31.begin(0x44);
  Serial.println(shtOk ? "✅ SHT3x ok" : "⚠ SHT3x não encontrado");
#endif

  // Retoma o último ciclo (ou o padrão, na primeira vez)
  prefs.begin("torre", false);
  cicloAtivo         = prefs.getBool("ativo", true);
  minutosCicloLigado = prefs.getULong("lig", CICLO_PADRAO_LIGADO_MIN);
  minutosCicloDeslig = prefs.getULong("des", CICLO_PADRAO_DESLIGADO_MIN);
  if (cicloAtivo) _ligarRele();

  iniciarWiFi();

  espClient.setInsecure();   // TODO: setCACert com a raiz do HiveMQ (ver README)
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setBufferSize(768);
  mqttClient.setKeepAlive(30);
  mqttClient.setCallback(mqttCallback);
}

// =============================================
// LOOP
// =============================================
void loop() {
  wm.process();               // portal de configuração, se estiver aberto
  verificarBotaoResetWiFi();
  manterMQTT();
  mqttClient.loop();

  gerenciarMotor();           // roda com ou sem internet

#if TEM_PRO
  amostrarAnalogicos();
#endif

  static unsigned long ultimaPublicacao = 0;
  if (millis() - ultimaPublicacao >= PUBLISH_INTERVAL_MS) {
    ultimaPublicacao = millis();
#if TEM_PRO
    lerSHT3x();
    calcularPhTds();
#endif
#if TEM_NIVEL
    lerTanque();
#endif
    if (mqttClient.connected()) {
      publishSensores();
      publishMotorStatus();
    }
  }
}
