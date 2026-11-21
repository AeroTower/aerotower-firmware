"""Teste de integracao: simulador + broker MQTT de verdade (amqtt, local).

Sobe um broker na maquina, conecta a torre simulada como o ESP32 faria e
confere o que um assinante (o papel do Backend) recebe e o que a torre faz
com um comando enviado pelo broker.
"""
import asyncio
import json
import socket
import threading
import time

import paho.mqtt.client as mqtt
import pytest

import simulador_torres


def _porta_livre():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture(scope="module")
def broker():
    from amqtt.broker import Broker
    porta = _porta_livre()
    config = {"listeners": {"default": {"type": "tcp", "bind": f"127.0.0.1:{porta}"}},
              "sys_interval": 0, "auth": {"allow-anonymous": True},
              "topic-check": {"enabled": False}}
    loop = asyncio.new_event_loop()
    pronto = threading.Event()

    def rodar():
        asyncio.set_event_loop(loop)
        loop.run_until_complete(Broker(config, loop=loop).start())
        pronto.set()
        loop.run_forever()

    threading.Thread(target=rodar, daemon=True).start()
    assert pronto.wait(10), "broker não subiu"
    yield porta
    loop.call_soon_threadsafe(loop.stop)


def _esperar(condicao, segundos=5):
    limite = time.time() + segundos
    while time.time() < limite:
        if condicao():
            return True
        time.sleep(0.05)
    return False


def test_torre_publica_no_broker_e_obedece_comando(broker, monkeypatch):
    monkeypatch.setattr(simulador_torres, "BROKER", "127.0.0.1")
    monkeypatch.setattr(simulador_torres, "PORTA", broker)
    monkeypatch.setattr(simulador_torres, "USAR_TLS", False)

    recebido = {}
    backend = mqtt.Client(client_id="backend-teste")
    backend.on_message = lambda c, u, m: recebido.__setitem__(m.topic, json.loads(m.payload))
    backend.connect("127.0.0.1", broker)
    backend.subscribe("aerotower/+/#", qos=1)
    backend.loop_start()

    torre = simulador_torres.TorreSimulada("AT-SIMBROKER1", "completo")
    torre.iniciar()
    try:
        assert _esperar(lambda: torre.conectada), "torre não conectou"
        torre.publicar()
        base = "aerotower/AT-SIMBROKER1"
        assert _esperar(lambda: f"{base}/sensores" in recebido and f"{base}/info" in recebido)
        assert recebido[f"{base}/status"] == {"status": "online"}
        assert recebido[f"{base}/info"]["sensores"] == ["nivel"]
        assert "nivelAgua" in recebido[f"{base}/sensores"] and "ph" not in recebido[f"{base}/sensores"]

        # O "Backend" manda desligar pelo broker; a torre obedece e responde
        backend.publish(f"{base}/motor/comando", json.dumps({"acao": "desligar"}), qos=1)
        assert _esperar(lambda: recebido.get(f"{base}/motor/status", {}).get("ligado") is False)
        assert torre.motor is False and torre.ciclo is False
    finally:
        torre.cliente.loop_stop()
        torre.cliente.disconnect()
        backend.loop_stop()
        backend.disconnect()
