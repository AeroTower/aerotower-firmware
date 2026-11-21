"""Configuracao comum dos testes do simulador.

Marca cada teste com o tipo, pela pasta onde ele esta.

`pytest -m seguranca` roda so os de seguranca. No PyCharm, o mesmo filtro
vai em Run Configuration > Additional Arguments.
"""
import json
import sys
from pathlib import Path

import pytest

# simulador/ no caminho de import: `import simulador_torres`.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

_TIPO_POR_PASTA = {
    "unitarios":  "unitario",
    "integracao": "integracao",
    "funcionais": "funcional",
    "aceitacao":  "aceitacao",
    "desempenho": "desempenho",
    "seguranca":  "seguranca",
}


def pytest_collection_modifyitems(config, items):
    for item in items:
        tipo = _TIPO_POR_PASTA.get(Path(str(item.fspath)).parent.name)
        if tipo:
            item.add_marker(getattr(pytest.mark, tipo))


# ── Duble do cliente MQTT (paho) ───────────────────────────────
class ClienteFalso:
    """Faz o papel do paho.mqtt.Client: guarda o que seria enviado ao broker."""

    def __init__(self, *args, client_id=None):
        self.args, self.client_id = args, client_id
        self.publicados, self.assinados, self.tls, self.will = [], [], None, None
        self.conectado_em, self.parado = None, False

    def username_pw_set(self, usuario, senha):
        self.credenciais = (usuario, senha)

    def tls_set(self, cert_reqs):
        self.tls = cert_reqs

    def will_set(self, topico, payload, qos, retain):
        self.will = (topico, json.loads(payload), qos, retain)

    def connect(self, host, porta, keepalive):
        self.conectado_em = (host, porta)

    def loop_start(self):
        pass

    def loop_stop(self):
        self.parado = True

    def disconnect(self):
        pass

    def publish(self, topico, payload, qos=0, retain=False):
        self.publicados.append((topico, json.loads(payload), retain))

    def subscribe(self, topico, qos):
        self.assinados.append(topico)

    def ultimo(self, sufixo):
        """Ultimo payload publicado em aerotower/<id>/<tipo>, com tipo = `sufixo` sem a barra."""
        tipo = sufixo.lstrip("/")
        return next(p for t, p, _ in reversed(self.publicados) if t.split("/", 2)[2] == tipo)


class Mensagem:
    def __init__(self, payload):
        self.payload = payload if isinstance(payload, bytes) else json.dumps(payload).encode()


@pytest.fixture()
def sim(monkeypatch):
    """Modulo do simulador com o paho trocado pelo duble."""
    import simulador_torres
    monkeypatch.setattr(simulador_torres.mqtt, "Client", ClienteFalso)
    return simulador_torres


@pytest.fixture()
def torre(sim):
    """Torre do kit pro ja conectada."""
    t = sim.TorreSimulada("AT-SIMTESTE01", "pro")
    t._conectou(t.cliente, None, None, 0)
    return t
