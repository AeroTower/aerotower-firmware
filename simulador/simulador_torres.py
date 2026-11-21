"""Simula uma ou mais torres no broker, com os topicos novos.

Uso:
    export MQTT_USER=... MQTT_PASS=...
    python simulador/simulador_torres.py --torre AT-5130AA000001:pro --torre AT-5130AA000002:basico

Cada torre:
  * publica aerotower/<id>/status (online, retido) e deixa o LWT "offline";
  * publica aerotower/<id>/info com firmware, kit e sensores;
  * publica aerotower/<id>/sensores a cada 5 s, SO com os sensores do kit,
    com valores que variam devagar (sem saltos);
  * obedece aerotower/<id>/motor/comando (ciclo/desligar) e responde em
    motor/status, como o ESP32.
O reservatorio esvazia com a bomba ligada e e reabastecido ao chegar a
20%, para o alerta de nivel baixo aparecer sozinho numa demonstracao.

Para a torre aparecer no app, cadastre-a antes com
scripts/provisionar_torre.py (repo de auth) --torre-id <id> --kit <kit>.
"""
import argparse
import json
import os
import random
import re
import ssl
import sys
import time

import paho.mqtt.client as mqtt

BROKER  = os.getenv("MQTT_BROKER", "47d5de0ce14d4654a95021e273720719.s1.eu.hivemq.cloud")
PORTA   = int(os.getenv("MQTT_PORT", "8883"))
PREFIXO = os.getenv("MQTT_PREFIXO", "aerotower")
USAR_TLS = os.getenv("MQTT_TLS", "true").lower() != "false"   # false so p/ broker local
ID_VALIDO = re.compile(r"^AT-[0-9A-Z]{4,16}$")   # mesma regra do Backend
ERROS_CONEXAO = {
    1: "versao do protocolo recusada", 2: "client id recusado", 3: "broker indisponivel",
    4: "usuario ou senha errados (confira MQTT_USER e MQTT_PASS)", 5: "nao autorizado (confira MQTT_USER e MQTT_PASS)",
}
SENSORES_POR_KIT = {"basico": [], "completo": ["nivel"], "pro": ["nivel", "ph", "tds", "clima"]}


class TorreSimulada:
    def __init__(self, torre_id: str, kit: str):
        self.id, self.kit = torre_id, kit
        self.base = f"{PREFIXO}/{torre_id}"
        self.ph, self.tds, self.temp, self.umid, self.nivel = 6.0, 700.0, 24.0, 60.0, 80.0
        self.motor, self.ciclo, self.lig, self.des, self.mudou = True, True, 15, 45, time.time()

        try:  # paho-mqtt 2.x
            self.cliente = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=torre_id)
        except AttributeError:  # paho-mqtt 1.x
            self.cliente = mqtt.Client(client_id=torre_id)
        self.cliente.username_pw_set(os.getenv("MQTT_USER", ""), os.getenv("MQTT_PASS", ""))
        if USAR_TLS:
            self.cliente.tls_set(cert_reqs=ssl.CERT_REQUIRED)
        self.cliente.will_set(f"{self.base}/status", json.dumps({"status": "offline"}), qos=1, retain=True)
        self.cliente.on_connect = self._conectou
        self.cliente.on_disconnect = self._caiu
        self.conectada = False
        self.cliente.on_message = self._comando

    def iniciar(self):
        self.cliente.connect(BROKER, PORTA, 30)
        self.cliente.loop_start()

    def _caiu(self, *_):
        if self.conectada:
            print(f"[{self.id}] desconectado do broker, tentando de novo...")
        self.conectada = False

    def _conectou(self, cliente, userdata, flags, rc, *_):
        if rc != 0:
            print(f"[{self.id}] ERRO ao conectar no broker: {ERROS_CONEXAO.get(rc, rc)}")
            return
        self.conectada = True
        print(f"[{self.id}] conectado")
        cliente.publish(f"{self.base}/status", json.dumps({"status": "online"}), qos=1, retain=True)
        cliente.publish(f"{self.base}/info", json.dumps({
            "firmware": "sim-2.0.0", "kit": self.kit, "sensores": SENSORES_POR_KIT[self.kit],
        }), qos=1, retain=True)
        cliente.subscribe(f"{self.base}/motor/comando", qos=1)
        self._status_motor()

    @staticmethod
    def _minutos(valor, padrao):
        """Como o firmware: valor que nao e numero inteiro vira o padrao."""
        if isinstance(valor, int) and not isinstance(valor, bool) and 1 <= valor <= 1440:
            return valor
        return padrao

    def _comando(self, cliente, userdata, msg):
        # Igual ao ESP32: comando malformado e ignorado. Sem isso, a excecao
        # derrubava a thread do MQTT e a torre parava de responder.
        try:
            dados = json.loads(msg.payload or b"{}")
        except (ValueError, UnicodeDecodeError):
            print(f"[{self.id}] comando com JSON invalido ignorado")
            return
        if not isinstance(dados, dict):
            print(f"[{self.id}] comando ignorado: nao e um objeto JSON")
            return
        print(f"[{self.id}] comando {dados}")
        if dados.get("acao") == "ciclo":
            self.lig = self._minutos(dados.get("minutos_ligado"), 15)
            self.des = self._minutos(dados.get("minutos_desligado"), 45)
            self.ciclo, self.motor, self.mudou = True, True, time.time()
        elif dados.get("acao") == "desligar":
            self.ciclo, self.motor = False, False
        self._status_motor()

    def _status_motor(self):
        self.cliente.publish(f"{self.base}/motor/status", json.dumps({
            "ligado": self.motor, "ciclo_ativo": self.ciclo,
            "minutos_ligado": self.lig, "minutos_desligado": self.des,
        }), qos=1, retain=True)

    @staticmethod
    def _passo(valor, alvo, ruido, minimo, maximo):
        valor += (alvo - valor) * 0.05 + random.uniform(-ruido, ruido)
        return max(minimo, min(maximo, valor))

    def _alternar_motor_se_venceu(self):
        """Ciclo automatico: troca ligado/desligado quando o tempo da fase acaba."""
        duracao = (self.lig if self.motor else self.des) * 60
        if self.ciclo and time.time() - self.mudou > duracao:
            self.motor, self.mudou = not self.motor, time.time()
            self._status_motor()

    # Um metodo por sensor: cada um atualiza o proprio valor e devolve os
    # campos que manda. Sensor novo = metodo novo + entrada em _LEITORES.
    def _ler_nivel(self):
        self.nivel -= 0.6 if self.motor else 0.1
        if self.nivel < 20:
            self.nivel = 95.0   # "reabasteceu"
        return {"nivelAgua": round(self.nivel, 1), "tanque_litros": round(self.nivel / 5, 2),
                "tanque_status": "baixo" if self.nivel < 35 else "ok"}

    def _ler_ph(self):
        self.ph = self._passo(self.ph, 6.0, 0.05, 4.5, 8.0)
        return {"ph": round(self.ph, 2)}

    def _ler_tds(self):
        self.tds = self._passo(self.tds, 700, 15, 100, 1500)
        return {"tds": round(self.tds), "ec": round(self.tds / 500, 2)}

    def _ler_clima(self):
        self.temp = self._passo(self.temp, 24, 0.2, 15, 35)
        self.umid = self._passo(self.umid, 60, 1, 30, 95)
        return {"temperaturaAmbiente": round(self.temp, 1), "umidadeRelativa": round(self.umid)}

    _LEITORES = {"nivel": _ler_nivel, "ph": _ler_ph, "tds": _ler_tds, "clima": _ler_clima}

    def leitura(self) -> dict:
        """Monta a mensagem de sensores so com os sensores do kit."""
        dados = {"motor": self.motor, "ciclo_ativo": self.ciclo}
        for sensor in SENSORES_POR_KIT[self.kit]:
            dados.update(self._LEITORES[sensor](self))
        return dados

    def publicar(self):
        if not self.conectada:
            print(f"[{self.id}] sem conexao com o broker: nada foi enviado")
            return
        self._alternar_motor_se_venceu()
        dados = self.leitura()
        self.cliente.publish(f"{self.base}/sensores", json.dumps(dados))
        print(f"[{self.id}] {dados}")

def criar_torre(item: str) -> TorreSimulada:
    """"AT-XXXX:kit" -> TorreSimulada. Sai com mensagem clara se for invalido."""
    torre_id, _, kit = item.partition(":")
    torre_id, kit = torre_id.strip().upper(), (kit or "pro").strip().lower()
    if not ID_VALIDO.match(torre_id):
        sys.exit(f"Id invalido: {torre_id!r}. Use AT- seguido de 4 a 16 letras/numeros "
                 "(ex.: AT-DEMO0001); o Backend ignora qualquer outro formato.")
    if kit not in SENSORES_POR_KIT:
        sys.exit(f"Kit invalido: {kit!r}. Use: {', '.join(SENSORES_POR_KIT)} (sem acento).")
    return TorreSimulada(torre_id, kit)


def encerrar(torres):
    for torre in torres:
        torre.cliente.publish(f"{torre.base}/status", json.dumps({"status": "offline"}), retain=True)
        torre.cliente.loop_stop()
        torre.cliente.disconnect()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--torre", action="append", required=True, metavar="ID:KIT",
                        help="ex.: AT-5130AA000001:pro (repita para várias)")
    parser.add_argument("--intervalo", type=float, default=5.0)
    args = parser.parse_args()

    torres = [criar_torre(item) for item in args.torre]
    for torre in torres:
        torre.iniciar()

    try:
        while True:
            time.sleep(args.intervalo)
            for torre in torres:
                torre.publicar()
    except KeyboardInterrupt:
        encerrar(torres)

if __name__ == "__main__":
    main()
