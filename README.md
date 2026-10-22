# Firmware da torre AeroTower (v2)

## Gravar uma placa

1. Instale as bibliotecas no Gerenciador da Arduino IDE: **PubSubClient**,
   **ArduinoJson 6.x**, **WiFiManager** (tzapu, 2.0.x) e, para o kit Pro,
   **Adafruit SHT31**.
2. Copie `segredos.h.exemplo` para `segredos.h` e preencha usuário e senha
   do HiveMQ. O `segredos.h` não vai para o Git.
3. Em `config.h`, escolha o kit: `#define KIT KIT_BASICO`, `KIT_COMPLETO`
   ou `KIT_PRO`.
4. Placa **ESP32 Dev Module**. Grave.
5. Rode o script de fábrica (`scripts/provisionar_torre.py` no repositório de auth)
   com a placa no USB: ele lê o `TORRE_ID` do monitor serial, cadastra a
   torre e gera o código do adesivo.

Testado compilando com o núcleo esp32 3.0.7 nos três kits (o Pro ocupa 91%
da memória de programa com a partição padrão; para atualização pela
internet no futuro, use a partição "Minimal SPIFFS").

## O que a torre faz sozinha

| Situação | Comportamento |
|---|---|
| Primeira vez ligada | Cria a rede `AeroTower-XXXX` (senha em `segredos.h`). O celular conecta e escolhe o Wi-Fi da casa. |
| Botão BOOT por 5 s | Esquece o Wi-Fi e reabre a rede de configuração. |
| Sem internet | A bomba continua no ciclo. Leituras não são enviadas. |
| Queda de energia | Volta no último ciclo configurado pelo app. |
| Caiu sem avisar | O broker publica `{"status":"offline"}` no lugar dela (LWT). |

## Tópicos

```
aerotower/<id>/sensores        torre -> servidor, a cada 5 s
aerotower/<id>/motor/comando   servidor -> torre  {"acao":"ciclo","minutos_ligado":15,"minutos_desligado":45} | {"acao":"desligar"}
aerotower/<id>/motor/status    torre -> servidor, retido
aerotower/<id>/status          {"status":"online"|"offline"}, retido
aerotower/<id>/info            {"firmware","kit","sensores":[...],"ip","rssi"}, retido
```

`<id>` = `AT-` + MAC da placa (aparece no monitor serial como `TORRE_ID=`).

## Pendências conhecidas

- **Certificado do broker**: `espClient.setInsecure()` cifra a conexão, mas
  não confere se o servidor é mesmo o HiveMQ. Trocar por
  `espClient.setCACert(...)` com a raiz ISRG Root X1 (Let's Encrypt), que o
  HiveMQ Cloud usa.
- **Credencial por torre**: hoje todas usam a mesma (plano Serverless). Ver
  `segredos.h.exemplo`.

## Simulador (sem hardware)

`simulador/simulador_torres.py` publica como uma ou mais torres, com os
mesmos tópicos e payloads do firmware, e obedece aos comandos da bomba.

```bash
pip install paho-mqtt
export MQTT_USER=... MQTT_PASS=...
python simulador/simulador_torres.py --torre AT-5130AA000001:pro --torre AT-5130AA000002:basico
```

Para a torre aparecer no app, cadastre o id antes com o script de fábrica
(`--torre-id AT-5130AA000001 --kit pro`) e vincule pelo app.
