// ============================================
// ESP32 - Torre AeroTower  v2.1
// ============================================
// O que a v2.1 acrescenta à v2:
//  * Sensor desconectado não trava nem manda lixo. Cada sensor do kit é
//    verificado a cada leitura: se não responder (ou mandar valores
//    impossíveis), a torre publica null para ele, avisa no monitor serial
//    e tira ele da lista "sensores" do tópico info. Quando o sensor volta,
//    ele reaparece sozinho, sem reiniciar a placa.
//  * Wi-Fi que não conecta abre o portal sozinho. Se a rede salva sumir
//    (roteador trocado, senha mudada), depois de 3 min a torre cria de
//    novo a rede "AeroTower-XXXX" para ser reconfigurada. O portal fecha
//    após 5 min sem uso e a torre volta a tentar a rede salva.
//  * Tempo máximo de espera do broker MQTT, para a bomba nunca ficar
//    presa esperando a internet.
//
// Continua da v2:
//  * id da torre = "AT-" + MAC da placa; tópicos aerotower/<id>/...
//  * kit escolhido em config.h (básico, completo ou pro)
//  * Wi-Fi configurado pelo celular; BOOT por 5 s apaga o Wi-Fi salvo
//  * a bomba segue o ciclo sem internet e volta no último ciclo após
//    queda de energia
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
String redePortal;
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

// =============================================
// Presença dos sensores
// =============================================
// Um sensor só é dado como "ausente" depois de SENSOR_FALHAS_PARA_AUSENTE
// leituras ruins seguidas (um eco perdido não some com o card do app) e
// volta a "presente" na primeira leitura boa.
struct Sensor {
  const char*   nome;      // como aparece no tópico info
  const char*   rotulo;    // como aparece no monitor serial
  bool          presente;
  uint8_t       falhas;
  bool          jaTestado;
  unsigned long ultimoTeste;
};

bool infoPendente = false;   // republica o tópico info quando algo muda

void registrarLeitura(Sensor& s, bool ok) {
  s.jaTestado   = true;
  s.ultimoTeste = millis();
  if (ok) {
    s.falhas = 0;
    if (!s.presente) {
      s.presente   = true;
      infoPendente = true;
      Serial.printf("✅ %s conectado\n", s.rotulo);
    }
  } else if (s.presente || s.falhas < SENSOR_FALHAS_PARA_AUSENTE) {
    if (++s.falhas >= SENSOR_FALHAS_PARA_AUSENTE && s.presente) {
      s.presente   = false;
      infoPendente = true;
      Serial.printf("⚠ %s sem resposta: publicando null até ele voltar\n", s.rotulo);
    }
  }
}

// Sensor ausente é testado de novo a cada SENSOR_RETESTE_MS, não a cada 5 s,
// para não gastar tempo esperando um eco ou um I2C que não existe.
bool deveTestar(const Sensor& s) {
  if (s.presente || !s.jaTestado || s.falhas < SENSOR_FALHAS_PARA_AUSENTE) return true;
  return millis() - s.ultimoTeste >= SENSOR_RETESTE_MS;
}

// ── Clima (SHT3x) ─────────────────────────
float temperature = 25.0;   // também corrige o som (nível) e o TDS
float humidity    = 0.0;

#if TEM_PRO
Sensor sClima = {"clima", "SHT3x (temperatura/umidade)", false, 0, false, 0};
Sensor sPh    = {"ph",    "Sensor de pH",                false, 0, false, 0};
Sensor sTds   = {"tds",   "Sensor TDS",                  false, 0, false, 0};

#define VREF    3.3
#define ADC_RES 4096.0
#define SCOUNT  30
int   analogBufferTDS[SCOUNT], analogBufferPH[SCOUNT], bufferTemp[SCOUNT];
int   analogBufferIndex = 0;
float tdsValue = 0, phValue = 0;
bool  shtIniciado = false;
Adafruit_SHT31 sht31 = Adafruit_SHT31();
#endif

#if TEM_NIVEL
Sensor sNivel = {"nivel", "Sensor de nível (ultrassônico)", false, 0, false, 0};

#define ECHO_TIMEOUT_US 30000UL
#define N_LEITURAS_US   5
#define PAUSA_US_MS     30
#define TANQUE_CRITICO  10
#define TANQUE_BAIXO    35
#define TANQUE_CHEIO    90
float tanqueDistanciaCm = -1, tanqueNivelCm = 0, tanqueLitros = 0, tanquePercentual = 0;
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

void lerTanque(float tempAr) {
  if (!deveTestar(sNivel)) return;
  float dist = medirDistanciaCm(tempAr);
  bool ok = dist > 0;   // -1 = nenhum eco (sensor solto ou sem alimentação)
  registrarLeitura(sNivel, ok);
  if (!ok) return;
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
// Mediana e variação (máximo - mínimo) das últimas SCOUNT amostras.
// Pino analógico solto "flutua" e varia muito; sensor ligado varia pouco.
int mediana(int origem[], int n, int* variacao) {
  for (int i = 0; i < n; i++) bufferTemp[i] = origem[i];
  for (int j = 0; j < n - 1; j++)
    for (int i = 0; i < n - j - 1; i++)
      if (bufferTemp[i] > bufferTemp[i + 1]) {
        int t = bufferTemp[i]; bufferTemp[i] = bufferTemp[i + 1]; bufferTemp[i + 1] = t;
      }
  *variacao = bufferTemp[n - 1] - bufferTemp[0];
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
  int varTds, varPh;

  float v = mediana(analogBufferTDS, SCOUNT, &varTds) * VREF / ADC_RES;
  float comp = v / (1.0 + 0.02 * (temperature - 25.0));
  tdsValue = (133.42 * comp * comp * comp - 255.86 * comp * comp + 857.39 * comp) * 0.5;
  // Quase 0 V = sonda fora da água ou placa solta (solução nutritiva
  // nunca dá 0 ppm); variação alta = pino solto; acima do máximo = lixo.
  registrarLeitura(sTds, varTds <= ADC_VARIACAO_MAX && v >= TDS_V_MIN &&
                         tdsValue <= TDS_MAX_VALIDO);

  float vPh = mediana(analogBufferPH, SCOUNT, &varPh) * VREF / ADC_RES;
  float slope = (4.0 - 7.0) / (PH_CAL_V4 - PH_CAL_V7);
  phValue = 7.0 + slope * (vPh - PH_CAL_V7);
  // Módulo solto com o divisor ligado puxa o pino para 0 V, o que dá
  // um pH impossível (> 14); pino totalmente solto varia demais.
  registrarLeitura(sPh, varPh <= ADC_VARIACAO_MAX &&
                        phValue >= PH_MIN_VALIDO && phValue <= PH_MAX_VALIDO);
}

void lerSHT3x() {
  if (!deveTestar(sClima)) return;
  if (!shtIniciado) shtIniciado = sht31.begin(0x44);
  bool ok = false;
  if (shtIniciado) {
    float t = sht31.readTemperature(), h = sht31.readHumidity();
    ok = !isnan(t) && !isnan(h);
    if (ok) { temperature = t; humidity = h; }
  }
  registrarLeitura(sClima, ok);
  if (!sClima.presente) {
    shtIniciado = false;     // tenta o begin() de novo no próximo teste
    temperature = 25.0;      // compensações voltam ao padrão
  }
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

void lerSensores() {
#if TEM_PRO
  lerSHT3x();
  calcularPhTds();
#endif
#if TEM_NIVEL
  lerTanque(temperature);
#endif
}

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

// "sensores" lista só o que está respondendo agora; "ausentes", o que o
// kit deveria ter mas não respondeu. O app usa isso para esconder cards.
void publishInfo() {
  if (!mqttClient.connected()) return;
  StaticJsonDocument<384> doc;
  doc["firmware"] = FIRMWARE_VERSAO;
  doc["kit"]      = KIT_NOME;
  JsonArray presentes = doc.createNestedArray("sensores");
  JsonArray ausentes  = doc.createNestedArray("ausentes");
#if TEM_NIVEL
  (sNivel.presente ? presentes : ausentes).add(sNivel.nome);
#endif
#if TEM_PRO
  (sPh.presente    ? presentes : ausentes).add(sPh.nome);
  (sTds.presente   ? presentes : ausentes).add(sTds.nome);
  (sClima.presente ? presentes : ausentes).add(sClima.nome);
#endif
  doc["ip"]   = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  char payload[384];
  serializeJson(doc, payload, sizeof(payload));
  if (mqttClient.publish(topicoInfo.c_str(), payload, true)) infoPendente = false;
}

void publishSensores() {
  StaticJsonDocument<640> doc;

#if TEM_PRO
  if (sClima.presente) {
    doc["temperaturaAmbiente"] = serialized(String(temperature, 1));
    doc["umidadeRelativa"]     = serialized(String(humidity, 1));
    doc["ambiente"]            = classificarAmbiente(temperature, humidity);
  } else {
    doc["temperaturaAmbiente"] = nullptr;
    doc["umidadeRelativa"]     = nullptr;
    doc["ambiente"]            = "sensor_falhou";
  }
  if (sPh.presente) {
    doc["ph"]        = serialized(String(phValue, 2));
    doc["ph_status"] = classificarPH(phValue);
  } else {
    doc["ph"]        = nullptr;
    doc["ph_status"] = "sensor_falhou";
  }
  if (sTds.presente) {
    doc["tds"]        = round(tdsValue);
    doc["ec"]         = serialized(String(tdsValue / 500.0, 2));   // mS/cm
    doc["tds_status"] = classificarNutriente(tdsValue);
  } else {
    doc["tds"]        = nullptr;
    doc["ec"]         = nullptr;
    doc["tds_status"] = "sensor_falhou";
  }
#endif

#if TEM_NIVEL
  if (sNivel.presente) {
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

  char payload[640];
  serializeJson(doc, payload, sizeof(payload));
  mqttClient.publish(topicoSensores.c_str(), payload);
  Serial.printf("✔ [%s] %s\n", topicoSensores.c_str(), payload);
}

// Resumo no monitor serial quando a torre está sem internet, para dar
// para testar os sensores só com o cabo USB.
void imprimirLeiturasOffline() {
  Serial.print("📟 (sem internet) ");
#if TEM_PRO
  if (sPh.presente)    Serial.printf("pH %.2f | ", phValue);          else Serial.print("pH -- | ");
  if (sTds.presente)   Serial.printf("TDS %.0f ppm | ", tdsValue);    else Serial.print("TDS -- | ");
  if (sClima.presente) Serial.printf("%.1f°C %.0f%% | ", temperature, humidity);
  else                 Serial.print("clima -- | ");
#endif
#if TEM_NIVEL
  if (sNivel.presente) Serial.printf("tanque %.0f%% (%.1f cm) | ", tanquePercentual, tanqueDistanciaCm);
  else                 Serial.print("tanque -- | ");
#endif
  Serial.printf("bomba %s\n", motorLigado ? "ON" : "off");
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
// Wi-Fi (nada aqui trava o loop: a bomba não pode esperar a internet)
// =============================================
void abrirPortal(bool comTempoLimite) {
  // Com Wi-Fi salvo, o portal fecha sozinho se ninguém usar, e a torre
  // volta a tentar a rede salva (o roteador pode só ter reiniciado).
  wm.setConfigPortalTimeout(comTempoLimite ? WIFI_PORTAL_DURACAO_S : 0);
  Serial.printf("📶 Portal aberto: conecte o celular na rede \"%s\" (senha %s) "
                "e abra 192.168.4.1\n", redePortal.c_str(), SENHA_PORTAL_WIFI);
  wm.startConfigPortal(redePortal.c_str(), SENHA_PORTAL_WIFI);
}

void iniciarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  wm.setConfigPortalBlocking(false);
  wm.setTitle("AeroTower");

  if (wm.getWiFiIsSaved()) {
    Serial.println("📶 Wi-Fi salvo encontrado, conectando...");
    WiFi.begin();
  } else {
    Serial.println("📶 Nenhum Wi-Fi salvo ainda.");
    abrirPortal(false);
  }
}

void manterWiFi() {
  static unsigned long semWifiDesde   = 0;
  static bool          estavaOnline   = false;
  static bool          portalEstavaOn = false;

  bool online = WiFi.status() == WL_CONNECTED;
  bool portal = wm.getConfigPortalActive();

  if (online != estavaOnline) {
    estavaOnline = online;
    if (online) Serial.printf("📶 Wi-Fi conectado: %s (IP %s, sinal %d dBm)\n",
                              WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    else        Serial.println("📶 Wi-Fi caiu. A bomba continua no ciclo.");
  }

  // Portal fechou sem ninguém configurar: volta a tentar a rede salva.
  if (portalEstavaOn && !portal && !online && wm.getWiFiIsSaved()) {
    Serial.println("📶 Portal fechado, tentando a rede salva de novo...");
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    semWifiDesde = millis();
  }
  portalEstavaOn = portal;

  if (online || portal) { semWifiDesde = 0; return; }
  if (semWifiDesde == 0) { semWifiDesde = millis(); return; }

  if (millis() - semWifiDesde >= WIFI_PORTAL_APOS_MS) {
    Serial.println("📶 Não consegui entrar na rede salva.");
    abrirPortal(true);
    semWifiDesde = 0;
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

// =============================================
// MQTT
// =============================================
void manterMQTT() {
  static unsigned long ultimaTentativa = 0;
  if (WiFi.status() != WL_CONNECTED || mqttClient.connected()) return;
  if (ultimaTentativa != 0 && millis() - ultimaTentativa < MQTT_RETENTATIVA_MS) return;
  ultimaTentativa = millis();

  Serial.print("☁ Conectando ao broker... ");
  // Testamento (LWT): se a torre sumir sem avisar, o broker publica
  // "offline" no lugar dela.
  bool ok = mqttClient.connect(torreId.c_str(), MQTT_USUARIO, MQTT_SENHA,
                               topicoStatus.c_str(), 1, true, "{\"status\":\"offline\"}");
  if (!ok) {
    int st = mqttClient.state();
    Serial.printf("falhou (estado %d%s)\n", st,
                  st == 4 || st == 5 ? ": confira usuário e senha em segredos.h" : "");
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

  torreId    = lerTorreId();
  redePortal = "AeroTower-" + torreId.substring(torreId.length() - 4);
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
  Wire.setTimeOut(50);   // I2C sem sensor não pode segurar o loop
  // Enche o buffer analógico antes do primeiro diagnóstico
  for (int i = 0; i < SCOUNT; i++) {
    analogBufferTDS[i] = analogRead(TDS_PIN);
    analogBufferPH[i]  = analogRead(PH_PIN);
    delay(5);
  }
#endif

  // Primeiro diagnóstico: mostra no monitor o que foi encontrado
  lerSensores();
#if TEM_NIVEL || TEM_PRO
  Serial.println("------ Sensores ------");
#endif
#if TEM_NIVEL
  Serial.printf("%s %s\n", sNivel.presente ? "✅" : "⚠", sNivel.rotulo);
#endif
#if TEM_PRO
  Serial.printf("%s %s\n", sClima.presente ? "✅" : "⚠", sClima.rotulo);
  Serial.printf("%s %s\n", sPh.presente    ? "✅" : "⚠", sPh.rotulo);
  Serial.printf("%s %s\n", sTds.presente   ? "✅" : "⚠", sTds.rotulo);
  Serial.println("(⚠ = não respondeu ainda; é testado de novo sozinho)");
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
  mqttClient.setBufferSize(1024);
  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(MQTT_TIMEOUT_S);
  mqttClient.setCallback(mqttCallback);
}

// =============================================
// LOOP
// =============================================
void loop() {
  wm.process();               // portal de configuração, se estiver aberto
  verificarBotaoResetWiFi();
  manterWiFi();
  manterMQTT();
  mqttClient.loop();

  gerenciarMotor();           // roda com ou sem internet

#if TEM_PRO
  amostrarAnalogicos();
#endif

  static unsigned long ultimaPublicacao = 0;
  if (millis() - ultimaPublicacao >= PUBLISH_INTERVAL_MS) {
    ultimaPublicacao = millis();
    lerSensores();
    if (mqttClient.connected()) {
      if (infoPendente) publishInfo();
      publishSensores();
      publishMotorStatus();
    } else {
      imprimirLeiturasOffline();
    }
  }
}
