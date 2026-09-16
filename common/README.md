# common/

Código compartilhado entre `database/`, `scripts/brute-force/` e `scripts/lsh/`, para não duplicar entre os
dois algoritmos.

Responsabilidades previstas:

- Definição de `VectorDataset` (struct usada por ambos os algoritmos).
- Função de distância euclidiana entre dois vetores.
- Utilitários de top-K (heap / ordenação parcial) usados no merge dos resultados locais de cada thread.
- Medição de tempo/energia e escrita das métricas (tempo, speedup, eficiência paralela, recall) em CSV,
  conforme protocolo experimental da Etapa 6 da especificação técnica.

Estruturas específicas de um único algoritmo (ex.: `Bucket`, `LSHIndex` do LSH) não entram aqui — ficam em
`scripts/lsh/`.
