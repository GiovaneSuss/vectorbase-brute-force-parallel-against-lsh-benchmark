# common/

Código compartilhado entre `database/`, `scripts/brute-force/` e `scripts/lsh/`, para não duplicar entre os
dois algoritmos.

Já implementado:

- `vector_dataset.h` — `VectorDataset` (struct da especificação) e `IntDataset` (mesmo layout, para o ground truth).
- `db_format.h` — layout binário do banco (`DbHeader` + seções base/queries/ground truth alinhadas a 4 KiB),
  idêntico no arquivo `data/<dataset>.db` e no segmento `/dev/shm/pcd_<dataset>`; checksum FNV-1a.
- `shm_db.{h,cpp}` — `attach_db(dataset)` / `detach_db()`: API que as buscas usam para se plugar no banco no
  ar, com mapeamento somente leitura e ponteiros `const`.

Previsto:

- Função de distância euclidiana entre dois vetores.
- Utilitários de top-K (heap / ordenação parcial) usados no merge dos resultados locais de cada thread.
- Medição de tempo/energia e escrita das métricas (tempo, speedup, eficiência paralela, recall) em CSV,
  conforme protocolo experimental da Etapa 6 da especificação técnica.

Estruturas específicas de um único algoritmo (ex.: `Bucket`, `LSHIndex` do LSH) não entram aqui — ficam em
`scripts/lsh/`.
