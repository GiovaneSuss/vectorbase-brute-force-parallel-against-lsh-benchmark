#pragma once

#include <vector>

// Estrutura fixada pela especificacao tecnica: vetores achatados em row-major (N * D).
// Vetor i, dimensao j = data[i * d + j].
struct VectorDataset {
    std::vector<float> data; // flattened row-major: N * D
    int n; // number of vectors
    int d; // dimension (128 for SIFT1M)
};

// Mesmo layout do VectorDataset, mas com inteiros — usado para o ground truth (.ivecs),
// onde cada "vetor" i sao os ids dos d vizinhos mais proximos corretos da query i.
struct IntDataset {
    std::vector<int> data;
    int n;
    int d;
};
