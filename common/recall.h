#pragma once

// Recall@k de uma busca contra o ground truth do dataset, em duas versoes:
//
// - strict: fracao dos ids do ground truth (top-k) que apareceram no resultado.
// - tie-aware (padrao do ann-benchmarks): um vizinho devolvido conta como acerto se a sua distancia for
//   <= a distancia do k-esimo vizinho verdadeiro. Assim, empates exatos de distancia na fronteira do top-k
//   (comuns no SIFT, cujos valores sao inteiros) nao contam como erro — ex.: na query 93 do SIFT1M, os
//   vetores 196106 e 274922 estao ambos a distancia 42192 e disputam a 10a posicao.
//
// Por query tambem sai a razao de distancia (distancia euclidiana, com raiz): quanto mais longe que o
// verdadeiro esta o vizinho devolvido na mesma posicao. 1.0 = exato; 1.05 = o i-esimo devolvido esta 5% mais
// longe que o i-esimo verdadeiro. Mostra se um erro do LSH e "por pouco" (trocou o 10o pelo 11o) ou grosseiro.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "distance.h"
#include "topk.h"

struct Recall {
    double strict = 0;
    double tie_aware = 0;
};

struct QueryRecall {
    double strict = 0;
    double tie_aware = 0;
    double ratio_kth = NAN;  // dist(k-esimo devolvido) / dist(k-esimo verdadeiro)
    double ratio_mean = NAN; // media sobre i de dist(i-esimo devolvido) / dist(i-esimo verdadeiro)
};

// results: queries * k vizinhos (em ordem); gt: queries * gt_k ids; queries_data: queries * d.
inline std::vector<QueryRecall> per_query_recall(const Neighbor* results, int queries, int k, const int* gt, int gt_k,
                                                 const float* base, const float* queries_data, int d) {
    std::vector<QueryRecall> out(queries);
    for (int q = 0; q < queries; q++) {
        const int* truth = gt + size_t(q) * gt_k;
        const Neighbor* res = results + size_t(q) * k;
        const float* query = queries_data + size_t(q) * d;
        const float kth = l2_sq(base + int64_t(truth[k - 1]) * d, query, d);
        int strict_hits = 0, tie_hits = 0, ratios = 0;
        double ratio_sum = 0;
        bool missing = false;
        for (int i = 0; i < k; i++) {
            for (int j = 0; j < k; j++)
                if (res[i].id == truth[j]) {
                    strict_hits++;
                    break;
                }
            if (res[i].id >= 0 && res[i].dist <= kth) tie_hits++;
            if (res[i].id < 0) {
                missing = true; // menos de k candidatos: sem distancia para comparar
                continue;
            }
            const float td = l2_sq(base + int64_t(truth[i]) * d, query, d);
            // Vizinho verdadeiro a distancia 0 (vetor repetido da query): so da razao 1 se o devolvido tambem for 0.
            if (td > 0) {
                ratio_sum += std::sqrt(double(res[i].dist) / td);
                ratios++;
            } else if (res[i].dist == 0) {
                ratio_sum += 1;
                ratios++;
            }
        }
        QueryRecall& r = out[q];
        r.strict = double(strict_hits) / k;
        r.tie_aware = double(tie_hits) / k;
        if (ratios) r.ratio_mean = ratio_sum / ratios;
        if (!missing && kth > 0) r.ratio_kth = std::sqrt(double(res[k - 1].dist) / kth);
        else if (!missing) r.ratio_kth = res[k - 1].dist == 0 ? 1.0 : NAN;
    }
    return out;
}

inline Recall compute_recall(const Neighbor* results, int queries, int k, const int* gt, int gt_k,
                             const float* base, const float* queries_data, int d) {
    Recall r;
    for (const QueryRecall& q : per_query_recall(results, queries, k, gt, gt_k, base, queries_data, d)) {
        r.strict += q.strict;
        r.tie_aware += q.tie_aware;
    }
    r.strict /= queries;
    r.tie_aware /= queries;
    return r;
}

// Resumo da distribuicao por query, para o runs.csv.
struct RecallSummary {
    double min_tie_aware = 1;  // pior query
    double perfect_pct = 0;    // % de queries com recall (tie-aware) = 1
    double ratio_mean = NAN;   // media de ratio_mean sobre as queries
    double ratio_kth_max = NAN;
};

inline RecallSummary summarize_recall(const std::vector<QueryRecall>& v) {
    RecallSummary s;
    double sum = 0;
    int n = 0, perfect = 0;
    for (const QueryRecall& q : v) {
        if (q.tie_aware < s.min_tie_aware) s.min_tie_aware = q.tie_aware;
        if (q.tie_aware >= 1.0) perfect++;
        if (!std::isnan(q.ratio_mean)) {
            sum += q.ratio_mean;
            n++;
        }
        if (!std::isnan(q.ratio_kth) && (std::isnan(s.ratio_kth_max) || q.ratio_kth > s.ratio_kth_max))
            s.ratio_kth_max = q.ratio_kth;
    }
    s.perfect_pct = v.empty() ? 0 : 100.0 * perfect / v.size();
    if (n) s.ratio_mean = sum / n;
    return s;
}
