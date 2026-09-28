#pragma once

// Recall@k de uma busca contra o ground truth do dataset, em duas versoes:
//
// - strict: fracao dos ids do ground truth (top-k) que apareceram no resultado.
// - tie-aware (padrao do ann-benchmarks): um vizinho devolvido conta como acerto se a sua distancia for
//   <= a distancia do k-esimo vizinho verdadeiro. Assim, empates exatos de distancia na fronteira do top-k
//   (comuns no SIFT, cujos valores sao inteiros) nao contam como erro — ex.: na query 93 do SIFT1M, os
//   vetores 196106 e 274922 estao ambos a distancia 42192 e disputam a 10a posicao.

#include <cstddef>
#include <cstdint>

#include "distance.h"
#include "topk.h"

struct Recall {
    double strict = 0;
    double tie_aware = 0;
};

// results: queries * k vizinhos (em ordem); gt: queries * gt_k ids; queries_data: queries * d.
inline Recall compute_recall(const Neighbor* results, int queries, int k, const int* gt, int gt_k,
                             const float* base, const float* queries_data, int d) {
    int64_t strict_hits = 0, tie_hits = 0;
    for (int q = 0; q < queries; q++) {
        const int* truth = gt + size_t(q) * gt_k;
        const Neighbor* res = results + size_t(q) * k;
        const float* query = queries_data + size_t(q) * d;
        const float kth = l2_sq(base + int64_t(truth[k - 1]) * d, query, d);
        for (int i = 0; i < k; i++) {
            for (int j = 0; j < k; j++)
                if (res[i].id == truth[j]) {
                    strict_hits++;
                    break;
                }
            if (res[i].id >= 0 && res[i].dist <= kth) tie_hits++;
        }
    }
    const double total = double(queries) * k;
    return {strict_hits / total, tie_hits / total};
}
