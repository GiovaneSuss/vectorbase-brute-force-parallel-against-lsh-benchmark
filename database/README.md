# database/

Código responsável por carregar o dataset em memória — não é um banco persistente/serviço, é a camada de
"criação e carga" da estrutura `VectorDataset` descrita em `docs/especificacao_tecnica.pdf`.

Responsabilidades previstas:

- Parser de `.fvecs`/`.ivecs` (dataset SIFT1M, queries e ground truth).
- Construção do `VectorDataset` (array achatado `N * D` em memória).
- Script de "população": lê os arquivos brutos em `data/` e valida/prepara o dataset para uso pelas buscas
  (brute-force e LSH), incluindo validação de N, D e amostras contra o arquivo original.

Não contém a lógica de busca (isso fica em `scripts/brute-force/` e `scripts/lsh/`) nem estruturas
específicas do LSH (`Bucket`, `LSHIndex`) além do que for compartilhado — código comum a mais de um
algoritmo vive em `common/`.
