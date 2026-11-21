# Testes do simulador (e do firmware)

**Responsável:** Fernão · **Cobertura do simulador:** 99% (meta da disciplina: 90%) · **50 testes**

O `simulador/simulador_torres.py` faz o papel do ESP32: mesmos tópicos, mesmos payloads e a mesma resposta aos comandos. É o que a equipe usa na demonstração sem a torre física e para gerar carga no Backend. Por isso ele é testado como o firmware deveria se comportar.

| Tipo | Pasta | Testes | O que confere |
|---|---|---:|---|
| Unitário | `simulador/tests/unitarios/` | 20 | sensores de cada kit, ciclo da bomba, reservatório, comandos, mensagens de erro de conexão |
| Integração | `simulador/tests/integracao/` | 1 | simulador + **broker MQTT de verdade** (amqtt, local): publica, um assinante recebe, comando volta pelo broker |
| Funcional | `simulador/tests/funcionais/` | 4 | a linha de comando da demonstração: várias torres, Ctrl + C publica "offline", kit inválido |
| Aceitação | `simulador/tests/aceitacao/` | 3 | história "demonstrar a torre sem o hardware", em BDD |
| Desempenho | `simulador/tests/desempenho/` | 2 | 200 torres × 50 publicações; 1 dia simulado (17.280 leituras) sem sair das faixas |
| Segurança | `simulador/tests/seguranca/` | 20 | comando malformado, minutos inválidos, TLS, senha fora do código, id com curinga MQTT (`#`, `+`) |

## Defeitos encontrados pelos testes (e corrigidos)

| # | Defeito | Teste que achou | Correção |
|---|---|---|---|
| 1 | Comando com JSON inválido derrubava a thread do MQTT: a torre simulada parava de responder | `test_comando_malformado_e_ignorado_sem_derrubar_a_torre` | Ignora o comando, como o firmware já faz |
| 2 | `minutos_ligado: "10"` (texto) quebrava o cálculo do ciclo na publicação seguinte | `test_minutos_invalidos_viram_o_padrao` | Valor que não é inteiro de 1 a 1440 vira o padrão (15/45), como o firmware |

**Achado no firmware (não corrigido):** o ESP32 aceita `minutos_ligado: 0` e ciclos negativos (ArduinoJson converte, mas nada confere a faixa). O Backend já barra esses valores (`ComandoCiclo` aceita de 1 a 1440), então o risco é baixo. Se alguém publicar direto no broker, porém, passa. Sugestão: conferir a faixa em `iniciarCiclo()`.

## Firmware (C++)

O `.ino` roda no ESP32 e depende do hardware (Wi-Fi, relé, sensores), então não entra na cobertura automática. Ele é testado na placa, com o monitor serial e o app:

| ID | Cenário | Esperado |
|---|---|---|
| FW-01 | Placa nova ligada | Cria a rede `AeroTower-XXXX` e abre o portal em 192.168.4.1 |
| FW-02 | Wi-Fi configurado | Publica `status: online` e `info` no broker; painel mostra a torre online |
| FW-03 | Comando de ciclo pelo app | Relé liga e desliga nos tempos pedidos; `motor/status` acompanha |
| FW-04 | Tomada desligada e religada | Volta com o mesmo ciclo (memória `Preferences`) |
| FW-05 | Wi-Fi cai | A bomba continua no ciclo; reconecta sozinha |

## Como rodar

```bash
pip install -r simulador/requirements.txt -r requirements-dev.txt
pytest --cov                  # tudo, com cobertura (falha abaixo de 90%)
pytest -m integracao          # só um tipo: unitario, integracao, funcional, aceitacao, desempenho, seguranca
```

**PyCharm**: *Settings › Tools › Python Integrated Tools › Testing › Default test runner* = **pytest**; botão direito em `simulador/tests` › **Run with Coverage**.

**GitHub**: o workflow `.github/workflows/testes.yml` roda em todo pull request, com o broker local, sem usar o HiveMQ nem senha. A aba *Actions* mostra a tabela por tipo.
