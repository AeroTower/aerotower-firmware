"""Teste funcional: o simulador como a equipe usa na demonstracao (linha de comando)."""
import sys

import pytest


class _Parar(Exception):
    pass


def _rodar(sim, monkeypatch, *torres, rodadas=3, parar_com=KeyboardInterrupt):
    """Roda o main() com `rodadas` publicacoes e depois simula o Ctrl + C."""
    criadas, esperas = [], []
    original = sim.TorreSimulada

    def criar(torre_id, kit):
        t = original(torre_id, kit)
        criadas.append(t)
        return t

    def dormir(segundos):
        esperas.append(segundos)
        for t in criadas:
            if not t.conectada:
                t._conectou(t.cliente, None, None, 0)
        if len(esperas) > rodadas:
            raise parar_com

    monkeypatch.setattr(sim, "TorreSimulada", criar)
    monkeypatch.setattr(sim.time, "sleep", dormir)
    args = [a for t in torres for a in ("--torre", t)]
    monkeypatch.setattr(sys, "argv", ["simulador_torres.py", *args, "--intervalo", "2"])
    sim.main()
    return criadas, esperas


def test_duas_torres_publicam_e_saem_offline_no_ctrl_c(sim, monkeypatch):
    torres, esperas = _rodar(sim, monkeypatch, "at-simdemo01:pro", "AT-SIMDEMO02:BASICO")
    assert [t.id for t in torres] == ["AT-SIMDEMO01", "AT-SIMDEMO02"]
    assert [t.kit for t in torres] == ["pro", "basico"]
    assert set(esperas) == {2.0}
    for t in torres:
        publicacoes = [p for top, p, _ in t.cliente.publicados if top.endswith("/sensores")]
        assert len(publicacoes) == 3
        assert t.cliente.ultimo("/status") == {"status": "offline"} and t.cliente.parado


def test_kit_omitido_vira_pro(sim, monkeypatch):
    torres, _ = _rodar(sim, monkeypatch, "AT-SIMDEMO03", rodadas=0)
    assert torres[0].kit == "pro"


def test_kit_com_acento_e_recusado(sim, monkeypatch):
    monkeypatch.setattr(sys, "argv", ["simulador_torres.py", "--torre", "AT-SIMDEMO01:básico"])
    with pytest.raises(SystemExit, match="Kit invalido"):
        sim.main()


def test_sem_torre_mostra_o_uso(sim, monkeypatch, capsys):
    monkeypatch.setattr(sys, "argv", ["simulador_torres.py"])
    with pytest.raises(SystemExit):
        sim.main()
    assert "--torre" in capsys.readouterr().err
