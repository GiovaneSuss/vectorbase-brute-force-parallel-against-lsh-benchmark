# scripts/brute-force/

Busca exata (brute-force), Etapas 2 e 3 da especificação técnica. Lê a base direto do banco em memória
compartilhada (`attach_db`, ponteiros `const` — sem cópia).

- `brute_force.{h,cpp}` — as buscas:
  - `bf_search_sequential`: uma thread, sem OpenMP, varre os N vetores mantendo o top-K (baseline do speedup).
  - `bf_search_parallel`: a base é dividida em T faixas contíguas de índice de vetor (`[N*t/T, N*(t+1)/T)`);
    cada thread mantém um top-K local e, depois da barreira, um merge sequencial gera o top-K global.
- `main.cpp` — o experimento (`bin/brute_force`): aquecimento fora do tempo, R repetições sobre as Q primeiras
  queries, medições e CSVs.
- `bench.sh` — varredura: sequencial + paralelo para cada número de threads, com speedup e eficiência.

## O que é medido

Por execução (`results/brute-force/<data-hora>_<seq|tN>/`):

- `summary.csv` — por repetição: tempo de busca, tempo do merge, ms/query, CPU do processo, utilização, energia,
  trocas de contexto.
- `threads.csv` — por thread e repetição: faixa de vetores, tempo de varredura, CPU da thread, ocupação
  (fração do tempo total em que a thread trabalhou — o resto foi espera na barreira) e núcleo usado.
- `run.csv` — uma linha agregada (média/desvio/mínimo das repetições, recall, speedup, eficiência, energia).
  A mesma linha é acrescentada em `results/brute-force/runs.csv`, que junta todas as execuções.

Energia: lida do Intel RAPL (`/sys/class/powercap/intel-rapl:*`) quando o sistema expõe; senão fica `NA` e o
tempo de CPU serve como proxy. Não existe em WSL/VMs e pode exigir root em Linux nativo.

Recall: `recall_at_k` considera empates de distância (padrão ann-benchmarks); `recall_at_k_strict` compara
ids. Brute-force dá 1.0 no primeiro — o estrito pode dar 0.999 por empates exatos na fronteira do top-K.
