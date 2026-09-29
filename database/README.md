# database/

Construção e ciclo de vida do banco vetorial em memória. O "banco" não é um serviço: é um segmento de
memória compartilhada POSIX (`/dev/shm/pcd_<dataset>`) somente leitura, no qual os programas de busca se
plugam via `attach_db()` (`common/shm_db.h`) sem cópia, rede ou serialização.

- `download.sh` — baixa SIFT1M/SIFT10K do TEXMEX para `data/` (`make download`) e o SIFT10M (`make download-10m`):
  os primeiros 10M vetores do `bigann_base.bvecs.gz` (98 GB) em streaming — o `head` corta o pipe e o `curl`
  para, então só ~1 GB é baixado — + queries e `gnd/idx_10M.ivecs` do BIGANN.
- `fvecs_reader.{h,cpp}` — parser de `.fvecs`/`.bvecs`/`.ivecs`: confere e descarta o prefixo de dimensão de
  cada vetor e lê os valores no array achatado. `VecsReader` lê em blocos (streaming) e converte `.bvecs`
  (uint8, formato do BIGANN) para float; `read_fvecs`/`read_ivecs` leem o arquivo inteiro num
  `VectorDataset`/`IntDataset`.
- `pcd_db.cpp` — ferramenta `bin/pcd_db` com os subcomandos:

| Comando make | Subcomando | O que faz |
|---|---|---|
| `make construct-db` | `construct` | `.fvecs`/`.bvecs`/`.ivecs` → `data/<dataset>.db` em streaming (~60 MB de RAM mesmo no 10M), validando N, D, valores [0,255] e ids do ground truth |
| `make up-db` | `up` | sobe o `.db` para `/dev/shm`, confere checksum, sela como read-only (`mprotect` + `0444`) e fica em primeiro plano; Ctrl+C derruba |
| `make down-db` | `down` | remove o segmento se o `up` morreu sem limpar (ex.: `kill -9`) |
| `make status-db` | `status` | conecta read-only, reconfere checksum e mostra amostras |
| `make test-db` | `test-readonly` | prova que o banco não aceita escrita (`O_RDWR` e `mprotect` recusados, escrita direta → SIGSEGV) |

Todos aceitam `DATASET=sift1m` (padrão), `DATASET=sift10m` ou `DATASET=siftsmall`, ou os atalhos
`make <comando>-db-1m`, `-10m`, `-small` (ex.: `make up-db-10m`).

No SIFT10M o banco guarda 100 vizinhos por query (o `idx_10M.ivecs` traz 1000; o `construct` trunca, igual
aos outros datasets), e os vetores ficam em float32 como nos demais — 5,1 GB em `/dev/shm`.

A proteção contra escrita protege contra bugs, não contra sabotagem: root, ou o próprio dono do segmento
fazendo `chmod` de volta, conseguiria escrever — e nesse caso o checksum do `make status-db` acusaria.
