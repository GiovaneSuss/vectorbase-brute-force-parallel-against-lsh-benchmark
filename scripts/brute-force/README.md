# scripts/brute-force/

Implementação da busca exata (brute-force / busca exaustiva).

Responsabilidades previstas (Etapas 2 e 3 da especificação técnica):

- Busca sequencial: distância da query contra todo o dataset, mantendo o top-K.
- Busca paralela com OpenMP: divisão do array por faixa de índice de vetor (não por posição de memória crua),
  top-K local por thread, e merge sequencial final dos top-K locais em um top-K global.
- Não há fase de indexação — o tempo medido aqui é tempo de busca puro, comparado diretamente com o tempo
  de busca do LSH.
- Recall esperado: 1.0 contra o ground truth do SIFT1M.

Usa as estruturas e utilitários de `common/` e `database/`; não redefine `VectorDataset`.
