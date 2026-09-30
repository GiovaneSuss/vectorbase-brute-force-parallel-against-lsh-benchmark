# Busca vetorial paralela: Brute-force vs LSH

Estudo comparativo de busca por vizinhos mais próximos com OpenMP sobre o SIFT1M (1 milhão de vetores) e o SIFT10M (10 milhões,
os primeiros 10M do SIFT1B/BIGANN). Pergunta: com quantas threads o brute-force paralelo (exato) alcança o LSH
sequencial (aproximado)?

## Pré-requisitos

Confira tudo **antes** de rodar o experimento. A parte de energia não dá para recuperar depois: se o log de
potência não estiver gravando durante a execução, a rodada inteira fica sem energia.

### Software

- Linux nativo (melhor: tem energia pelo RAPL) ou WSL2, com `g++` (C++17 + OpenMP), `make`, `curl` e `python3`
- Para os gráficos: `pip install matplotlib`
- Google Colab também serve, mas só para o SIFT1M (ver abaixo)

### Memória e disco

| Dataset | Disco | `/dev/shm` livre | RAM total recomendada |
|---|---|---|---|
| SIFT1M | ~1,5 GB | ~600 MB | 4 GB |
| SIFT10M | ~8 GB | ~5,2 GB | **16 GB**: banco 5,1 GB + índice do LSH (até ~5 GB na indexação) |

- `df -h /dev/shm` mostra o espaço livre. Se faltar e houver RAM: `sudo mount -o remount,size=8G /dev/shm`.
- **No WSL, a RAM do Windows também conta.** Se o Windows não tiver memória livre para o índice do LSH do
  SIFT10M, ele pagina em disco e o tempo do LSH fica inválido: foi o que aconteceu na máquina de 12 GB. O
  `make experimento` confere a memória livre do Linux e do Windows antes de cada configuração do LSH. Ele
  pula a configuração que não cabe no Linux e registra em `avisos.txt` a que corre risco no Windows.
- Feche navegador e outros programas pesados antes de rodar.

### Energia

| Ambiente | Como a energia é medida | O que fazer antes |
|---|---|---|
| Linux nativo, Intel/AMD | RAPL, direto pelo programa (colunas `energy_j` dos CSVs) | `sudo chmod a+r /sys/class/powercap/intel-rapl:*/energy_uj` (vale até reiniciar) |
| WSL2, Windows com o contador "Energy Meter" | RAPL **pelo Windows**, automático | nada (confira abaixo se o contador existe) |
| WSL2, Windows sem o contador | log de potência do **HWiNFO64**, cruzado depois com os horários do experimento | configurar e iniciar o log do HWiNFO (abaixo) |
| Colab / VM | não dá para medir | nada; fica só o tempo de CPU (`cpu_s_mean`) como proxy |

O WSL não expõe o RAPL (`/sys/class/powercap` não existe), mas o Windows expõe o mesmo contador de energia do
processador como contador de desempenho "Energy Meter" ("Medidor de Energia" no Windows em português). Ele não
precisa de administrador e está presente na maioria dos notebooks Intel. Para conferir se a sua máquina tem:

```bash
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w scripts/energia/amostrador.ps1)" -Saida x -Parar x -Testar
```

- **Imprimiu `ok`:** o `make experimento` grava a energia sozinho. Um amostrador em segundo plano lê a energia
  acumulada do pacote da CPU a cada 200 ms, grava em `energia/windows_rapl.csv`, e no fim a energia de cada
  repetição é calculada. O amostrador gasta um pouco de CPU; como está ligado o tempo todo, inclusive na janela
  ociosa, o custo entra igual em todas as execuções e sai na energia "dinâmica".
- **Imprimiu `indisponivel`:** use o HWiNFO64, como descrito abaixo.

**Em notebook, rode ligado na tomada.** Na bateria, o Windows limita a potência da CPU e muda os tempos.

**HWiNFO64 (só se o contador não existir):**

1. Baixe o HWiNFO64 (gratuito, https://www.hwinfo.com), abra em modo **Sensors-only** e aceite rodar como
   administrador.
2. Nas configurações da janela de sensores (ícone de engrenagem), mude o **Polling Period** para **250 ms**.
3. Confira se aparece o sensor **CPU Package Power [W]** na seção da CPU.
4. Logo antes do `make experimento`, clique em **Logging Start** e salve o `.CSV` num lugar fácil, ex.:
   `C:\pcd\hwinfo.csv` (no WSL: `/mnt/c/pcd/hwinfo.csv`).
5. No fim, clique em **Logging Stop** e rode `make energia MAQUINA=<nome> HWINFO=/mnt/c/pcd/hwinfo.csv`.

Nos dois casos do WSL, o experimento começa com **60 s parado** para medir a potência ociosa. Não mexa no
computador nessa hora, nem durante o resto do experimento. A diferença entre o relógio do WSL e o do Windows
fica gravada em `relogio.csv` e é corrigida no cálculo.

### Colab

- A máquina gratuita tem 2 vCPUs, então a curva de threads satura em 2 (vale como terceiro ponto de
  comparação de banda, não de escala).
- Rode só o SIFT1M: `make experimento MAQUINA=colab DATASETS=sift1m`. O `/dev/shm` e a RAM do Colab não
  comportam o SIFT10M com o índice do LSH.
- Não há energia.

## Rodando o experimento

**1. Baixar e construir os bancos** (só na primeira vez):

```bash
make download            # SIFT1M + SIFT10K (~160 MB)
make download-10m        # SIFT10M (~1,3 GB dos vetores, em streaming, + ~512 MB do ground truth)
make construct-db-1m     # converte e valida -> data/sift1m.db
make construct-db-10m    # -> data/sift10m.db
```

**2. Rodar o protocolo completo** (na tomada; com o log do HWiNFO gravando, se a máquina precisar dele):

```bash
make experimento MAQUINA=cedric     # use o nome de quem está rodando: cedric, suss, colab...
```

O comando faz tudo sozinho, na ordem:
1. anota a máquina: CPU, RAM, módulos de memória, compilador e commit;
2. mede 60 s ocioso e a banda de memória para cada número de threads;
3. para cada dataset, sobe o banco, roda o brute-force (sequencial + 1 a 32 threads) e o LSH sequencial
   nas 5 configurações de recall, e derruba o banco.

Tudo vai para `results/<MAQUINA>/`. O comando se recusa a escrever numa pasta que já tem resultados, para não
misturar rodadas.

Protocolo padrão: 1024 queries, top-10, 5 repetições (mais 1 aquecimento fora do tempo), threads
`1 2 4 8 16 32` (32 = CPUs lógicas da maior máquina, o i9), e LSH `L:K:w` em `8:8:1000 32:10:800 32:12:1000 32:14:1200 40:14:1300`
(recall ~0,87 a ~0,99; `32:10:800` é a configuração principal).

**Tempo estimado por rodada** (a maior parte é o brute-force do SIFT10M, com sequencial e poucas threads):

| Máquina | SIFT1M | SIFT10M |
|---|---|---|
| i9-14900KF | ~15 min | ~2 h 15 |
| i5-1135G7 (notebook) | ~25 min | ~3 h 30 |

**Várias rodadas:** `make experimento MAQUINA=cedric RODADAS=5` repete o protocolo inteiro 5 vezes, em
`results/cedric/rodada-1` … `rodada-5`, cada uma com sua janela ociosa e sua energia. No i9 isso dá ~1 h 15
só com o SIFT1M (`DATASETS=sift1m`) e ~12 h com os dois datasets.

Para rodar um dataset só: `DATASETS=sift1m`. Se a máquina não tiver RAM para o LSH do 10M: `PULAR_LSH_10M=1`.

**3. Energia:** com RAPL (Linux) ela já está nos CSVs, e com o contador do Windows é calculada no fim do
experimento. Só no caso do HWiNFO: pare o log e rode

```bash
make energia MAQUINA=cedric HWINFO=/mnt/c/pcd/hwinfo.csv
```

A energia vai para `results/<MAQUINA>/energia/execucoes.csv` (J por execução, J/query, potência média e energia acima
da ociosa) e `repeticoes.csv`.

**4. Gráficos e relatório:**

```bash
python3 results/charts/plot.py results/<MAQUINA>      # PNGs em results/charts/<MAQUINA>/
python3 results/charts/report.py results/<MAQUINA>    # relatorio.html
```

## O que sai em `results/<MAQUINA>/`

| Arquivo | Conteúdo |
|---|---|
| `maquina.txt` | CPU, RAM, módulos de memória (tipo e frequência, via Windows no WSL), SO, g++, commit, RAPL |
| `fases.csv`, `relogio.csv` | início/fim de cada fase e diferença de relógio WSL × Windows (para a energia) |
| `avisos.txt` | o que foi pulado ou pode ter ficado inválido |
| `membw/runs.csv` | banda de memória (leitura sequencial e triad) por nº de threads: o teto da máquina |
| `brute-force/`, `lsh/` | uma pasta por execução + `runs.csv` com uma linha por execução |
| `energia/` | `windows_rapl.csv` (amostras do contador do Windows), `execucoes.csv` e `repeticoes.csv` (J por execução/repetição) |

Cada pasta de execução tem:
- `summary.csv`: uma linha por repetição, com horários de início e fim;
- `threads.csv`: uma linha por thread;
- `queries.csv`: uma linha por query, com recall, recall estrito, razão de distância e, no LSH, candidatos;
- `run.csv`: a mesma linha que vai para o `runs.csv`.

Colunas principais do `runs.csv`:
- `search_s_mean`, `ms_per_query`, `queries_per_s`, `speedup`, `efficiency`, `recall_at_k`;
- `effective_gbps`: GB/s lidos pela busca, para comparar com o `membw`;
- distribuição do recall por query: `recall_min_query`, `queries_perfect_pct`, `dist_ratio_mean`;
- `cpu_s_mean`: tempo de CPU, o proxy de energia;
- `energy_j_mean`: energia, só com RAPL.

## Rodando partes à mão

O banco é um segmento de memória compartilhada somente leitura. O `make experimento` sobe e derruba o banco
sozinho, mas dá para rodar as partes separadas:

```bash
make up-db-1m                     # terminal 1: sobe o banco (Ctrl+C derruba); ou up-db-10m
make status-db-1m                 # terminal 2: confere se está no ar e íntegro
make bench-brute-force            # sequencial + THREAD_LIST, com speedup e eficiência
make lsh-seq                      # LSH sequencial na configuração padrão
make lsh-seq LSH_TABLES=40 LSH_HASHES=14 LSH_WIDTH=1300
make membw                        # banda de memória (não precisa do banco)
```

Resultados das partes à mão vão para `results/brute-force/`, `results/lsh/` e `results/membw/`.

## Opções

- `DATASET=sift1m|sift10m|siftsmall` escolhe o banco nos comandos do banco; os atalhos `-1m`, `-10m` e
  `-small` (`make up-db-small`, `make status-db-10m`...) são o mesmo. O `siftsmall` (SIFT10K) serve para testes
  rápidos. Nas buscas, sem `DATASET=` é usado o banco que estiver no ar.
- `QUERIES=1024`, `K=10`, `REPEATS=5`, `THREAD_LIST="1 2 4 8 16 32"`: padrões do protocolo. Mantenha
  iguais entre máquinas.
- `RODADAS=N`, `LSH_CONFIGS` / `LSH_CONFIGS_10M`, `THREAD_LIST_10M`, `DATASETS`, `IDLE_S=60`, `PULAR_LSH_10M=1`,
  `SEM_PAUSA=1` (não esperar o Enter do HWiNFO), `CONTINUAR=1` (aceitar pasta de resultados existente):
  opções do `make experimento`.
- `make down-db-1m` / `make down-db-10m`: remove o banco da memória se o `up-db` morreu sem Ctrl+C.
- `make clean`: apaga os binários compilados.
