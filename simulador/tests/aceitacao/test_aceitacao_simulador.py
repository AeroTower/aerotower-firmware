"""Testes de aceitacao (BDD) da historia "demonstrar a torre sem o hardware"."""
from pytest_bdd import given, parsers, scenarios, then, when

from conftest import Mensagem

scenarios("demonstracao_sem_hardware.feature")


@given(parsers.parse('uma torre simulada do kit "{kit}" conectada ao broker'), target_fixture="torre_bdd")
def torre_conectada(sim, kit):
    t = sim.TorreSimulada("AT-SIMDEMO01", kit)
    t._conectou(t.cliente, None, None, 0)
    t.ciclo = False          # bomba parada no estado atual durante o cenario
    return t


@given(parsers.parse("o reservatório está em {nivel:d}%"))
def reservatorio(torre_bdd, nivel):
    torre_bdd.nivel = float(nivel)


@when("passa um intervalo de publicação")
def um_intervalo(torre_bdd):
    torre_bdd.publicar()


@when(parsers.parse("passam {n:d} intervalos com a bomba ligada"))
def varios_intervalos(torre_bdd, n):
    torre_bdd.motor = True
    for _ in range(n):
        torre_bdd.publicar()


@when(parsers.parse('o app manda o comando "{acao}"'))
def app_manda(torre_bdd, acao):
    torre_bdd._comando(None, None, Mensagem({"acao": acao}))


@then("o painel recebe só o estado da bomba")
def so_bomba(torre_bdd):
    assert set(torre_bdd.cliente.ultimo("/sensores")) == {"motor", "ciclo_ativo"}


@then(parsers.parse('o painel recebe o tanque com status "{status}"'))
def tanque_status(torre_bdd, status):
    assert torre_bdd.cliente.ultimo("/sensores")["tanque_status"] == status


@then("a torre informa a bomba desligada e o ciclo parado")
def bomba_desligada(torre_bdd):
    status = torre_bdd.cliente.ultimo("/motor/status")
    assert status["ligado"] is False and status["ciclo_ativo"] is False
