# scripts/membw/

Banda de memória da máquina por número de threads (`bin/membw`, `make membw`), no estilo do STREAM. É o teto
contra o qual se compara a banda efetiva do brute-force (`effective_gbps` no `runs.csv`): se o brute-force
encosta nesse teto, o gargalo é a memória e mais threads não ajudam.

- `read` — soma um vetor de 2 GiB (bem maior que a L3): só leitura sequencial, o padrão de acesso do
  brute-force. É o número usado na comparação.
- `triad` — `a[i] = b[i] + s*c[i]` do STREAM, para comparar com números publicados.

1 aquecimento + 10 repetições por teste; `best_gbps` (melhor repetição, convenção do STREAM) e `mean_gbps`.
Uma linha por teste em `<out>/runs.csv`.
