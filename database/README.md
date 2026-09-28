# database/

Construção e ciclo de vida do banco vetorial em memória. O "banco" não é um serviço: é um segmento de
memória compartilhada POSIX (`/dev/shm/pcd_<dataset>`) somente leitura, no qual os programas de busca se
plugam via `attach_db()` (`common/shm_db.h`) sem cópia, rede ou serialização.

- `download.sh` — baixa SIFT1M/SIFT10K do TEXMEX para `data/` (`make download`).
- `fvecs_reader.{h,cpp}` — parser de `.fvecs`/`.ivecs` para `VectorDataset`/`IntDataset`: confere e descarta o
  prefixo de dimensão de cada vetor e lê os valores direto no array achatado (uma única alocação).
- `pcd_db.cpp` — ferramenta `bin/pcd_db` com os subcomandos:

| Comando make | Subcomando | O que faz |
|---|---|---|
| `make construct-db` | `construct` | `.fvecs`/`.ivecs` → `data/<dataset>.db`, validando N, D, valores [0,255] e ids do ground truth |
| `make up-db` | `up` | sobe o `.db` para `/dev/shm`, confere checksum, sela como read-only (`mprotect` + `0444`) e fica em primeiro plano; Ctrl+C derruba |
| `make down-db` | `down` | remove o segmento se o `up` morreu sem limpar (ex.: `kill -9`) |
| `make status-db` | `status` | conecta read-only, reconfere checksum e mostra amostras |
| `make test-db` | `test-readonly` | prova que o banco não aceita escrita (`O_RDWR` e `mprotect` recusados, escrita direta → SIGSEGV) |

Todos aceitam `DATASET=sift1m` (padrão) ou `DATASET=siftsmall`.

A proteção contra escrita protege contra bugs, não contra sabotagem: root, ou o próprio dono do segmento
fazendo `chmod` de volta, conseguiria escrever — e nesse caso o checksum do `make status-db` acusaria.
