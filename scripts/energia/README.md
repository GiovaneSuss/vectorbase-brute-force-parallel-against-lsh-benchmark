# scripts/energia/

Energia no WSL, que não tem RAPL em `/sys/class/powercap`.

- `amostrador.ps1` — roda no Windows (o `make experimento` inicia em segundo plano): lê o contador de
  desempenho "Energy Meter" (`RAPL_Package0_PKG`, energia acumulada do pacote da CPU, em picowatt-hora) a
  cada 200 ms e grava em `results/<m>/energia/windows_rapl.csv`. Sem administrador e sem instalar nada.
  `-Testar` só confere se o contador existe.
- `energia.py` (`make energia MAQUINA=<m> [HWINFO=<log.CSV>]`, rodado sozinho no fim do experimento) —
  converte o relógio do Windows para o do WSL (`relogio.csv`) e calcula a energia de cada repetição, com a
  diferença do contador acumulado entre o fim e o início (ou, com `HWINFO=`, integrando a coluna
  `CPU Package Power [W]` de um log do HWiNFO64). Desconta a potência da janela ociosa (`fases.csv`). Saída
  em `results/<m>/energia/`: `repeticoes.csv` e `execucoes.csv`.
