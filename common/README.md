# common/

Código compartilhado entre `database/`, `scripts/brute-force/` e `scripts/lsh/`, para não duplicar entre os
dois algoritmos.

Já implementado:

- `vector_dataset.h` — `VectorDataset` (struct da especificação) e `IntDataset` (mesmo layout, para o ground truth).
- `db_format.h` — layout binário do banco (`DbHeader` + seções base/queries/ground truth alinhadas a 4 KiB),
  idêntico no arquivo `data/<dataset>.db` e no segmento `/dev/shm/pcd_<dataset>`; checksum FNV-1a.
- `shm_db.{h,cpp}` — `attach_db(dataset)` / `detach_db()`: API que as buscas usam para se plugar no banco no
  ar, com mapeamento somente leitura e ponteiros `const`.

- `distance.h` — `l2_sq`: distância euclidiana ao quadrado (vetorizada com `omp simd`).
- `topk.h` — `Neighbor`, `TopK` (max-heap limitada a k) e `merge_topk` (reduz os top-K locais no global).
- `recall.h` — `compute_recall`: recall@k estrito e considerando empates de distância.
- `metrics.{h,cpp}` — relógio, CPU por processo/thread, energia via RAPL, data-hora e escrita de CSV.

Estruturas específicas de um único algoritmo (ex.: `Bucket`, `LSHIndex` do LSH) não entram aqui — ficam em
`scripts/lsh/`.
