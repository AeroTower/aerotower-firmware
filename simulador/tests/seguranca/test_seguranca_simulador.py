"""Testes de seguranca do simulador: comandos maliciosos e configuracao segura."""
import ssl

import pytest

from conftest import Mensagem


@pytest.mark.parametrize("payload", [b"{nao e json", b"\xff\xfe\x00", b"[1, 2, 3]", b'"desligar"', b"null"])
def test_comando_malformado_e_ignorado_sem_derrubar_a_torre(torre, payload):
    """Antes, a excecao derrubava a thread do MQTT e a torre parava de responder."""
    antes = (torre.motor, torre.ciclo, len(torre.cliente.publicados))
    torre._comando(None, None, Mensagem(payload))
    assert (torre.motor, torre.ciclo, len(torre.cliente.publicados)) == antes


@pytest.mark.parametrize("minutos", ["10", -5, 0, 99999, 2.5, True, None, [10]])
def test_minutos_invalidos_viram_o_padrao(torre, minutos):
    """Antes, minutos_ligado="10" quebrava o calculo do ciclo na publicacao seguinte."""
    torre._comando(None, None, Mensagem({"acao": "ciclo", "minutos_ligado": minutos,
                                         "minutos_desligado": minutos}))
    assert (torre.lig, torre.des) == (15, 45)
    torre.publicar()                                  # continua funcionando


def test_tls_com_certificado_e_o_padrao(sim):
    assert sim.USAR_TLS is True
    assert sim.TorreSimulada("AT-SIMTESTE01", "pro").cliente.tls == ssl.CERT_REQUIRED


def test_senha_do_broker_nao_esta_no_codigo():
    from pathlib import Path
    codigo = (Path(__file__).resolve().parents[2] / "simulador_torres.py").read_text(encoding="utf-8")
    assert 'os.getenv("MQTT_PASS", "")' in codigo
    assert "Hidro123" not in codigo                   # senha antiga que vazou no Backend


@pytest.mark.parametrize("torre_id", ["AT-../../#", "AT-+", "aerotower/#", "AT-X Y", "AT-" + "A" * 17])
def test_id_que_viraria_curinga_mqtt_e_recusado(sim, monkeypatch, torre_id):
    """'#' e '+' no id fariam a torre publicar em topicos de outras torres."""
    import sys
    monkeypatch.setattr(sys, "argv", ["simulador_torres.py", "--torre", f"{torre_id}:pro"])
    with pytest.raises(SystemExit, match="Id invalido"):
        sim.main()
