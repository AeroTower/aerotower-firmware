// ============================================
// config.h — o que muda entre kits e montagens
// ============================================
// Um build por kit: troque KIT abaixo e grave. O id da torre NÃO fica
// aqui: ele sai do MAC da placa, então o mesmo .bin serve para todas as
// torres daquele kit.
#pragma once

#define FIRMWARE_VERSAO "2.1.0"

// ── Kit ───────────────────────────────────
#define KIT_BASICO   1   // bomba com ciclos
#define KIT_COMPLETO 2   // + nível do reservatório (ultrassônico)
#define KIT_PRO      3   // + pH, TDS, temperatura e umidade do ar

#ifndef KIT
#define KIT KIT_PRO
#endif

#define TEM_NIVEL (KIT >= KIT_COMPLETO)
#define TEM_PRO   (KIT >= KIT_PRO)     // pH, TDS e SHT3x

#if KIT == KIT_BASICO
  #define KIT_NOME "basico"
#elif KIT == KIT_COMPLETO
  #define KIT_NOME "completo"
#else
  #define KIT_NOME "pro"
#endif

// ── Broker ────────────────────────────────
// Endereço não é segredo. Usuário e senha ficam em segredos.h.
#define MQTT_HOST    "47d5de0ce14d4654a95021e273720719.s1.eu.hivemq.cloud"
#define MQTT_PORT    8883
#define MQTT_PREFIXO "aerotower"   // tópicos: aerotower/<torre_id>/...

// ── Pinos ─────────────────────────────────
#define RELE_PIN  26
#define BOTAO_PIN 0    // botão BOOT da placa: segurar 5 s apaga o Wi-Fi salvo
#define TDS_PIN   34
#define PH_PIN    35
#define SDA_PIN   25
#define SCL_PIN   27
#define TRIG_PIN  18
#define ECHO_PIN  19   // divisor de tensão se o sensor for 5 V!

// ── Ciclo padrão da bomba ─────────────────
// Usado na primeira vez que a torre liga. Depois vale o último ciclo
// enviado pelo app (fica salvo na memória da placa e volta após queda
// de energia). A torre irriga mesmo sem Wi-Fi.
#define CICLO_PADRAO_LIGADO_MIN    15
#define CICLO_PADRAO_DESLIGADO_MIN 45

// ── Publicação ────────────────────────────
#define PUBLISH_INTERVAL_MS 5000

// ── Wi-Fi e broker ────────────────────────
// Com Wi-Fi salvo que não conecta, a torre reabre a rede "AeroTower-XXXX"
// depois de WIFI_PORTAL_APOS_MS. Se ninguém configurar em
// WIFI_PORTAL_DURACAO_S, o portal fecha e ela tenta a rede salva de novo.
#define WIFI_PORTAL_APOS_MS   180000UL   // 3 min
#define WIFI_PORTAL_DURACAO_S 300        // 5 min
#define MQTT_RETENTATIVA_MS   10000UL
#define MQTT_TIMEOUT_S        5          // espera máxima por resposta do broker

// ── Sensores desconectados ────────────────
// Leituras ruins seguidas até o sensor ser dado como ausente (publica null).
#define SENSOR_FALHAS_PARA_AUSENTE 3
// Sensor ausente é testado de novo a cada:
#define SENSOR_RETESTE_MS          30000UL
// Pino analógico solto oscila muito. Se (máx - mín) das últimas 30
// amostras passar disso (escala 0-4095), pH/TDS são tratados como soltos.
// Se um sensor ligado for marcado como ausente, aumente este valor.
#define ADC_VARIACAO_MAX 400
#define PH_MIN_VALIDO    1.0
#define PH_MAX_VALIDO    13.0
#define TDS_MAX_VALIDO   1500.0   // ppm; o sensor mede até ~1000
#define TDS_V_MIN        0.02     // V; abaixo disso a sonda está fora da água

// ── Calibração pH ─────────────────────────
#define PH_CAL_V7 2.64
#define PH_CAL_V4 3.08

// ── Tanque (balde 34 cm / 20 L) ───────────
#define DIST_SENSOR_FUNDO_CM 36.0
#define ALTURA_BALDE_CM      34.0
#define ALTURA_MAX_AGUA_CM   34.0
#define DIAMETRO_FUNDO_CM    27.4
#define DIAMETRO_TOPO_CM     27.4
