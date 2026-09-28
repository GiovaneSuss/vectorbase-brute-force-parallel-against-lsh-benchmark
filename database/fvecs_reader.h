#pragma once

// Parser dos formatos .fvecs (float) e .ivecs (int) do corpus TEXMEX / ann-benchmarks.
// Cada vetor no arquivo e [int32 d][d x valores de 4 bytes], repetido; o prefixo de dimensao e
// conferido e descartado — so os valores vao para o array achatado (row-major, n * d).

#include <string>

#include "vector_dataset.h"

// Lancam std::runtime_error se o arquivo nao existir, tiver tamanho inconsistente ou algum vetor
// com dimensao diferente do primeiro.
VectorDataset read_fvecs(const std::string& path);
IntDataset read_ivecs(const std::string& path);
