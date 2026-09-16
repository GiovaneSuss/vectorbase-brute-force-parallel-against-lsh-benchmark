# data/

Diretório para os arquivos brutos do dataset (SIFT1M, ou subconjuntos SIFT100K/SIFT10K): `*.fvecs` (base e
queries) e `*.ivecs` (ground truth).

Esses arquivos não são versionados no git (ver `.gitignore`) por serem grandes binários baixados do
ann-benchmarks — apenas este README fica no repositório para documentar onde eles devem ser colocados antes
de rodar `make populate`.
