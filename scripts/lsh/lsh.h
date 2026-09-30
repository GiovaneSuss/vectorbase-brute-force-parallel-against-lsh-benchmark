#pragma once

// Busca aproximada via Locality-Sensitive Hashing (LSH), sequencial e paralela (OpenMP).
//
// Familia de hash: projecoes aleatorias p-estaveis (E2LSH, Datar et al. 2004), a familia LSH classica para
// distancia euclidiana:
//
//     h(v) = floor((a . v + b) / w),   a ~ N(0, I_d),  b ~ U[0, w)
//
// Vetores proximos tem projecoes a . v proximas, entao tendem a cair na mesma "fatia" de largura w. Cada tabela
// concatena K dessas funcoes (g = (h_1, ..., h_K): so colidem vetores que colidem nas K — menos falsos
// candidatos) e o indice usa L tabelas independentes (um vizinho verdadeiro so precisa colidir em UMA delas —
// recupera o recall que o K tira). A tupla g(v) e reduzida a um indice de bucket por hash em [0, num_buckets).
//
// A base e lida direto do banco em memoria compartilhada (ponteiro const, row-major n * d), como no brute-force.

#include <cstdint>
#include <vector>

#include "topk.h"

// Estruturas fixadas pela especificacao tecnica. Um LSHIndex e UMA tabela de hash; o indice completo e um
// std::vector<LSHIndex> com L tabelas.
struct Bucket {
    std::vector<int> vector_ids;
};

struct LSHIndex {
    std::vector<Bucket> buckets;
    int num_buckets;
};

// Padroes calibrados no SIFT1M (4 threads, 200 queries): recall@10 ~0.93 calculando distancia para ~8% da base.
// w maior / K menor / L maior = mais candidatos (recall sobe, busca fica mais lenta); o inverso, o contrario.
struct LSHParams {
    int tables = 32;      // L: numero de tabelas de hash
    int hashes = 10;      // K: funcoes de hash concatenadas por tabela
    float width = 800.0f; // w: largura da fatia de cada projecao (na escala das distancias do SIFT)
    int num_buckets = 0;  // buckets por tabela (arredondado para potencia de 2); 0 = automatico (~n/16)
    uint64_t seed = 42;   // semente das projecoes: mesma semente = mesmo indice em todas as configuracoes
};

// As L*K projecoes aleatorias sorteadas a partir da semente, e a funcao que leva um vetor aos seus L buckets.
class LSHFamily {
public:
    LSHFamily(const LSHParams& p, int n, int d);

    int tables() const { return L_; }
    int num_buckets() const { return num_buckets_; }
    const LSHParams& params() const { return p_; }

    // Bucket do vetor v em cada uma das L tabelas: out[0..L).
    void buckets_of(const float* v, uint32_t* out) const;

private:
    LSHParams p_;
    int L_, K_, d_;
    int num_buckets_;              // potencia de 2 (bucket = hash & (num_buckets - 1))
    std::vector<float> proj_;      // (L*K) x d, row-major: projecao r = linha r
    std::vector<float> offset_;    // L*K deslocamentos b
};

// Por thread, acumulado ao longo de uma repeticao. alignas(64): cada thread na sua linha de cache (sem
// false sharing entre os contadores de threads vizinhas).
struct alignas(64) LshThreadStats {
    // indexacao, fase 1 (hash): faixa de vetores da thread
    int64_t begin = 0, end = 0;
    double hash_s = 0, hash_cpu_s = 0;
    // indexacao, fase 2 (montagem dos buckets): tabelas montadas pela thread
    int tables_built = 0;
    double build_s = 0;
    // busca
    int queries = 0;          // queries processadas pela thread
    int64_t candidates = 0;   // candidatos distintos (distancias calculadas) somados nessas queries
    double search_s = 0, search_cpu_s = 0;
    int last_cpu = -1;
};

struct IndexTiming {
    double hash_s = 0;  // fase 1: calcular os L buckets de cada vetor (K*L produtos escalares por vetor)
    double build_s = 0; // fase 2: distribuir os ids nos buckets (merge dos resultados locais)
};

// Indexacao sequencial (sem OpenMP) — baseline do speedup da indexacao. Mesmas duas fases do paralelo.
std::vector<LSHIndex> lsh_build_sequential(const LSHFamily& fam, const float* base, int n, int d, IndexTiming& t,
                                           LshThreadStats& stats);

// Indexacao paralela, no esquema "cada thread acumula localmente e mescla no final" da especificacao:
//  1. cada thread calcula os buckets da sua faixa contigua de vetores e grava num array de codigos, na
//     posicao dos seus proprios vetores — nenhuma escrita compartilhada, sem locks;
//  2. depois da barreira, os codigos sao distribuidos nos buckets com uma tabela por thread (tabelas
//     diferentes nao compartilham nada). Os ids entram em ordem crescente: o indice e identico ao sequencial.
std::vector<LSHIndex> lsh_build_parallel(const LSHFamily& fam, const float* base, int n, int d, int threads,
                                         IndexTiming& t, LshThreadStats* stats);

// Memoria de trabalho de uma thread, reaproveitada entre queries.
struct QueryScratch {
    explicit QueryScratch(int n) : seen((size_t(n) + 63) / 64, 0) {}
    std::vector<int> cand;       // candidatos distintos da query atual
    std::vector<uint64_t> seen;  // bitmap "ja e candidato" (1 bit por vetor da base; limpo ao fim da query)
};

// Busca de uma query: percorre os L buckets da query, descarta repetidos (o mesmo vetor pode colidir em varias
// tabelas) pelo bitmap — O(1) por id, em vez de ordenar a lista —, calcula a distancia exata so para esses
// candidatos e devolve o top-k em ordem crescente (completado com NO_NEIGHBOR se houver menos de k
// candidatos). Retorna o numero de candidatos distintos.
int64_t lsh_query(const LSHFamily& fam, const std::vector<LSHIndex>& index, const float* base, int d,
                  const float* query, int k, QueryScratch& scratch, Neighbor* out);

// Q queries em sequencia, uma thread — baseline do speedup da busca.
void lsh_search_sequential(const LSHFamily& fam, const std::vector<LSHIndex>& index, const float* base, int n, int d,
                           const float* queries, int Q, int k, Neighbor* out, LshThreadStats& stats);

// Q queries em paralelo: o trabalho de uma query e pequeno demais para dividir entre threads, entao a
// paralelizacao e ENTRE queries — cada thread pega lotes de `batch` queries (schedule dynamic: queries com
// buckets maiores custam mais, e o dynamic reequilibra a carga).
void lsh_search_parallel(const LSHFamily& fam, const std::vector<LSHIndex>& index, const float* base, int n, int d,
                         const float* queries, int Q, int k, int threads, int batch, Neighbor* out,
                         LshThreadStats* stats);
