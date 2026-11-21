"""Teste de desempenho: o simulador aguenta muitas torres para testar o Backend.

Serve para o teste de carga do Backend: 200 torres x 50 publicacoes. O
limite e folgado (cerca de 10x o medido).
"""
import time


def test_200_torres_publicando_50_vezes(sim):
    torres = [sim.TorreSimulada(f"AT-SIMCARGA{i:04d}", "pro") for i in range(200)]
    for t in torres:
        t._conectou(t.cliente, None, None, 0)

    inicio = time.perf_counter()
    for _ in range(50):
        for t in torres:
            t.publicar()
    duracao = time.perf_counter() - inicio

    total = sum(1 for t in torres for top, _, _ in t.cliente.publicados if top.endswith("/sensores"))
    assert total == 10_000
    assert duracao < 10, f"10.000 publicações levaram {duracao:.2f}s"


def test_valores_estaveis_depois_de_um_dia_simulado(sim):
    """Endurance: 17.280 leituras (1 dia a cada 5 s) sem sair das faixas fisicas."""
    t = sim.TorreSimulada("AT-SIMLONGO01", "pro")
    t._conectou(t.cliente, None, None, 0)
    t.ciclo = False
    for _ in range(17_280):
        t.publicar()
        t.cliente.publicados.clear()          # nao acumula memoria no teste
    assert 4.5 <= t.ph <= 8.0 and 100 <= t.tds <= 1500 and 15 <= t.temp <= 35 and 20 <= t.nivel <= 95
