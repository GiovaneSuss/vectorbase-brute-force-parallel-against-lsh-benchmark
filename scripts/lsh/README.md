# scripts/lsh/

Implementação da busca aproximada via Locality-Sensitive Hashing (LSH).

Responsabilidades previstas (Etapas 4 e 5 da especificação técnica):

- Estruturas `Bucket` e `LSHIndex` (específicas do LSH, não compartilhadas com o brute-force).
- Famílias de hash (ex.: projeções aleatórias) e construção dos buckets.
- Indexação paralela: cada thread hasheia uma faixa de vetores; escrita cuidadosa em buckets compartilhados
  (lock por bucket, ou acumulação local com merge no final).
- Busca: comparação da query apenas com os vetores do(s) bucket(s) correspondente(s). Paralelização por lote
  de queries (não dentro de uma única consulta, já que o trabalho por busca é pequeno).
- Tempo de indexação é medido separadamente do tempo de busca (não somado na comparação principal com o
  brute-force).
- Recall esperado: menor que 1.0 contra o ground truth do SIFT1M.

Usa as estruturas e utilitários de `common/` e `database/`.
