#pragma once

// Utilitarios de top-K usados pelas buscas: cada thread mantem um TopK local sobre a sua fatia do dataset,
// e merge_topk junta os top-K locais no top-K global (a etapa de "reducao" do protocolo).

#include <algorithm>
#include <limits>
#include <queue>
#include <vector>

struct Neighbor {
    float dist; // distancia euclidiana ao quadrado
    int id;     // indice do vetor na base

    // Ordem total: por distancia e, no empate, pelo menor id — deixa o resultado deterministico
    // independente de quantas threads e de como a base foi dividida.
    bool operator<(const Neighbor& o) const { return dist < o.dist || (dist == o.dist && id < o.id); }
};

inline constexpr Neighbor NO_NEIGHBOR = {std::numeric_limits<float>::infinity(), -1};

// Max-heap limitada a k elementos: o topo e o PIOR dos k melhores vistos ate agora, entao um candidato
// novo so entra se for melhor que o topo (custo O(log k) por insercao, O(1) na rejeicao — o caso comum).
class TopK {
public:
    explicit TopK(int k) : k_(k) {}

    void push(float dist, int id) {
        Neighbor c{dist, id};
        if (int(heap_.size()) < k_) {
            heap_.push(c);
        } else if (c < heap_.top()) {
            heap_.pop();
            heap_.push(c);
        }
    }

    // Esvazia a heap escrevendo os k vizinhos em ordem crescente em out[0..k); completa com NO_NEIGHBOR
    // se a fatia tinha menos de k vetores.
    void drain_sorted(Neighbor* out) {
        int m = int(heap_.size());
        for (int i = m; i < k_; i++) out[i] = NO_NEIGHBOR;
        for (int i = m - 1; i >= 0; i--) {
            out[i] = heap_.top();
            heap_.pop();
        }
    }

private:
    int k_;
    std::priority_queue<Neighbor> heap_;
};

// Junta `lists` listas de k candidatos (contiguas em `candidates`, lists * k) no top-k global, em ordem
// crescente, escrito em out[0..k).
inline void merge_topk(std::vector<Neighbor>& candidates, int lists, int k, Neighbor* out) {
    auto end = candidates.begin() + size_t(lists) * k;
    std::partial_sort(candidates.begin(), candidates.begin() + k, end);
    std::copy(candidates.begin(), candidates.begin() + k, out);
}
