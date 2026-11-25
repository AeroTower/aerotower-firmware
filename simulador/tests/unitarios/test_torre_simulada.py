"""Testes unitarios da TorreSimulada (paho trocado por duble, sem rede)."""
import ssl
import types

import pytest

from conftest import Mensagem


def test_cria_cliente_com_tls_senha_e_aviso_de_queda(sim, monkeypatch):
    monkeypatch.setenv("MQTT_USER", "usuario")
    monkeypatch.setenv("MQTT_PASS", "senha")
    t = sim.TorreSimulada("AT-SIMTESTE01", "pro")
    assert t.cliente.client_id == "AT-SIMTESTE01"
    assert t.cliente.credenciais == ("usuario", "senha")
    assert t.cliente.tls == ssl.CERT_REQUIRED
    assert t.cliente.will == ("aerotower/AT-SIMTESTE01/status", {"status": "offline"}, 1, True)


def test_usa_a_api_nova_do_paho_2_quando_existe(sim, monkeypatch):
    monkeypatch.setattr(sim.mqtt, "CallbackAPIVersion", types.SimpleNamespace(VERSION1="v1"), raising=False)
    assert sim.TorreSimulada("AT-SIMTESTE01", "pro").cliente.args == ("v1",)


def test_sem_tls_para_broker_local(sim, monkeypatch):
    monkeypatch.setattr(sim, "USAR_TLS", False)
    assert sim.TorreSimulada("AT-SIMTESTE01", "pro").cliente.tls is None


def test_iniciar_conecta_no_broker_configurado(sim):
    t = sim.TorreSimulada("AT-SIMTESTE01", "pro")
    t.iniciar()
    assert t.cliente.conectado_em == (sim.BROKER, sim.PORTA)


def test_ao_conectar_avisa_online_info_e_assina_comandos(torre):
    c = torre.cliente
    assert c.ultimo("/status") == {"status": "online"}
    assert c.ultimo("/info") == {"firmware": "sim-2.0.0", "kit": "pro",
                                 "sensores": ["nivel", "ph", "tds", "clima"]}
    assert c.assinados == ["aerotower/AT-SIMTESTE01/motor/comando"]
    assert c.ultimo("/motor/status")["ligado"] is True
    assert torre.conectada


@pytest.mark.parametrize("rc, trecho", [(4, "usuario ou senha"), (5, "nao autorizado"), (99, "99")])
def test_erro_de_conexao_explica_o_motivo(sim, capsys, rc, trecho):
    t = sim.TorreSimulada("AT-SIMTESTE01", "pro")
    t._conectou(t.cliente, None, None, rc)
    assert trecho in capsys.readouterr().out and not t.conectada and t.cliente.publicados == []


def test_queda_so_avisa_se_estava_conectada(torre, capsys):
    torre._caiu()
    torre._caiu()
    assert capsys.readouterr().out.count("desconectado") == 1


@pytest.mark.parametrize("kit, campos", [
    ("basico", {"motor", "ciclo_ativo"}),
    ("completo", {"motor", "ciclo_ativo", "nivelAgua", "tanque_litros", "tanque_status"}),
    ("pro", {"motor", "ciclo_ativo", "nivelAgua", "tanque_litros", "tanque_status",
             "ph", "tds", "ec", "temperaturaAmbiente", "umidadeRelativa"}),
])
def test_cada_kit_publica_so_os_seus_sensores(sim, kit, campos):
    t = sim.TorreSimulada("AT-SIMTESTE01", kit)
    t._conectou(t.cliente, None, None, 0)
    t.publicar()
    assert set(t.cliente.ultimo("/sensores")) == campos


def test_valores_variam_devagar_e_ficam_nos_limites(sim):
    for _ in range(1_000):
        valor = sim.TorreSimulada._passo(7.9, 6.0, 0.05, 4.5, 8.0)
        assert 4.5 <= valor <= 8.0 and abs(valor - 7.9) < 0.2
    assert sim.TorreSimulada._passo(100, 6.0, 0, 4.5, 8.0) == 8.0


def test_reservatorio_baixo_e_reabastecimento(torre):
    torre.nivel = 30
    torre.publicar()
    assert torre.cliente.ultimo("/sensores")["tanque_status"] == "baixo"
    torre.nivel = 20.2
    torre.publicar()
    assert torre.cliente.ultimo("/sensores")["nivelAgua"] == 95.0


def test_bomba_ligada_esvazia_mais_rapido(sim, torre):
    torre.ciclo = False
    torre.motor, torre.nivel = True, 80
    torre.publicar()
    torre.motor, antes = False, torre.nivel
    torre.publicar()
    assert round(80 - antes, 1) == 0.6 and round(antes - torre.nivel, 1) == 0.1


def test_ciclo_troca_a_bomba_quando_o_tempo_acaba(torre, monkeypatch):
    torre.lig, torre.des, torre.motor = 1, 2, True
    torre.mudou = torre.mudou - 61          # passou 1 min ligada
    torre.publicar()
    assert torre.motor is False and torre.cliente.ultimo("/motor/status")["ligado"] is False


def test_comando_de_ciclo_e_de_desligar(torre):
    torre._comando(None, None, Mensagem({"acao": "ciclo", "minutos_ligado": 10, "minutos_desligado": 50}))
    assert (torre.lig, torre.des, torre.ciclo, torre.motor) == (10, 50, True, True)
    torre._comando(None, None, Mensagem({"acao": "desligar"}))
    status = torre.cliente.ultimo("/motor/status")
    assert status == {"ligado": False, "ciclo_ativo": False, "minutos_ligado": 10, "minutos_desligado": 50}


def test_comando_sem_minutos_usa_o_padrao_do_firmware(torre):
    torre._comando(None, None, Mensagem({"acao": "ciclo"}))
    assert (torre.lig, torre.des) == (15, 45)


def test_acao_desconhecida_so_republica_o_status(torre):
    antes = len(torre.cliente.publicados)
    torre._comando(None, None, Mensagem({"acao": "dançar"}))
    assert len(torre.cliente.publicados) == antes + 1 and torre.motor is True


def test_sem_conexao_nada_e_enviado(sim, capsys):
    t = sim.TorreSimulada("AT-SIMTESTE01", "pro")
    t.publicar()
    assert t.cliente.publicados == [] and "sem conexao" in capsys.readouterr().out


def test_leitura_monta_os_campos_na_ordem_dos_sensores_do_kit(sim):
    # Depois da refatoracao cada sensor tem o seu metodo; a mensagem continua igual.
    dados = sim.TorreSimulada("AT-SIMTESTE01", "pro").leitura()
    assert list(dados) == ["motor", "ciclo_ativo", "nivelAgua", "tanque_litros", "tanque_status",
                           "ph", "tds", "ec", "temperaturaAmbiente", "umidadeRelativa"]
    assert sim.TorreSimulada("AT-SIMTESTE01", "basico").leitura() == {"motor": True, "ciclo_ativo": True}


def test_todo_sensor_de_kit_tem_um_leitor(sim):
    for sensores in sim.SENSORES_POR_KIT.values():
        assert set(sensores) <= set(sim.TorreSimulada._LEITORES)
