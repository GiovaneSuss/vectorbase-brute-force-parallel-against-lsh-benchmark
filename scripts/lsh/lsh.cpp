#include "lsh.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>

#include <omp.h>
#include <sched.h>

#include "distance.h"
#include "metrics.h"

namespace {

constexpr int MAX_TABLES = 64; // limite para o buffer de buckets da query ficar na pilha

inline float dot(const float* a, const float* b, int d) {
    float s = 0.0f;
#pragma omp simd reduction(+ : s)
    for (int j = 0; j < d; j++) s += a[j] * b[j];
    return s;
}

// Finalizador do splitmix64: espalha bem os bits, para que tuplas (h_1..h_K) vizinhas nao caiam em buckets
// vizinhos da tabela.
inline uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

uint32_t next_pow2(uint64_t x) {
    uint64_t p = 1;
    while (p < x) p <<= 1;
    if (p > (1ULL << 30)) throw std::runtime_error("num_buckets grande demais");
    return uint32_t(p);
}

// Fase 2 de uma tabela: distribui os ids nos buckets a partir dos codigos ja calculados (codes[i] = bucket do
// vetor i nesta tabela). Conta antes para reservar cada bucket no tamanho exato — uma alocacao por bucket.
void build_table(const uint32_t* codes, int n, int num_buckets, LSHIndex& table) {
    table.num_buckets = num_buckets;
    table.buckets.assign(num_buckets, Bucket{});
    std::vector<int> count(num_buckets, 0);
    for (int i = 0; i < n; i++) count[codes[i]]++;
    for (int b = 0; b < num_buckets; b++)
        if (count[b]) table.buckets[b].vector_ids.reserve(count[b]);
    for (int i = 0; i < n; i++) table.buckets[codes[i]].vector_ids.push_back(i);
}

} // namespace

LSHFamily::LSHFamily(const LSHParams& p, int n, int d) : p_(p), L_(p.tables), K_(p.hashes), d_(d) {
    if (L_ < 1 || L_ > MAX_TABLES) throw std::runtime_error("--tables precisa estar em [1, 64]");
    if (K_ < 1) throw std::runtime_error("--hashes precisa ser >= 1");
    if (!(p.width > 0)) throw std::runtime_error("--width precisa ser > 0");
    // n/16 por padrao: com os parametros padrao no SIFT1M cada tabela so tem ~16 mil tuplas (h_1..h_K)
    // distintas, entao mais buckets so gastariam memoria (medido: n/2 e n/16 dao o mesmo recall e candidatos,
    // com 3x menos memoria). Buckets de sobra ficam vazios; de menos, tuplas diferentes dividem bucket.
    num_buckets_ = next_pow2(p.num_buckets > 0 ? uint64_t(p.num_buckets) : std::max<uint64_t>(n / 16, 1));

    // Tudo sai de um unico gerador com semente fixa, na mesma ordem: mesma semente = mesmas projecoes (no
    // mesmo compilador/biblioteca padrao), qualquer que seja o numero de threads.
    std::mt19937_64 rng(p.seed);
    std::normal_distribution<float> gauss(0.0f, 1.0f);
    std::uniform_real_distribution<float> unif(0.0f, p.width);
    proj_.resize(size_t(L_) * K_ * d);
    for (float& x : proj_) x = gauss(rng);
    offset_.resize(size_t(L_) * K_);
    for (float& x : offset_) x = unif(rng);
}

void LSHFamily::buckets_of(const float* v, uint32_t* out) const {
    const float inv_w = 1.0f / p_.width;
    for (int l = 0; l < L_; l++) {
        uint64_t key = mix64(uint64_t(l) + 1); // tabelas diferentes partem de chaves diferentes
        for (int j = 0; j < K_; j++) {
            const int r = l * K_ + j;
            const float proj = dot(&proj_[size_t(r) * d_], v, d_);
            const int32_t h = int32_t(std::floor((proj + offset_[r]) * inv_w));
            key = mix64(key ^ uint32_t(h));
        }
        out[l] = uint32_t(key) & uint32_t(num_buckets_ - 1);
    }
}

// ---------------------------------------------------------------------------------------------
// Indexacao
// ---------------------------------------------------------------------------------------------

std::vector<LSHIndex> lsh_build_sequential(const LSHFamily& fam, const float* base, int n, int d, IndexTiming& t,
                                           LshThreadStats& s) {
    const int L = fam.tables();
    const double w0 = wall_time(), c0 = thread_cpu_time();
    // Sem inicializar: new[] em vez de std::vector evita zerar L*n posicoes que serao todas sobrescritas.
    std::unique_ptr<uint32_t[]> codes(new uint32_t[size_t(L) * n]);
    uint32_t buf[MAX_TABLES];
    for (int i = 0; i < n; i++) {
        fam.buckets_of(base + int64_t(i) * d, buf);
        for (int l = 0; l < L; l++) codes[size_t(l) * n + i] = buf[l];
    }
    t.hash_s = wall_time() - w0;
    s.hash_s += t.hash_s;
    s.hash_cpu_s += thread_cpu_time() - c0;
    s.begin = 0;
    s.end = n;

    const double w1 = wall_time();
    std::vector<LSHIndex> index(L);
    for (int l = 0; l < L; l++) build_table(&codes[size_t(l) * n], n, fam.num_buckets(), index[l]);
    t.build_s = wall_time() - w1;
    s.build_s += t.build_s;
    s.tables_built += L;
    s.last_cpu = sched_getcpu();
    return index;
}

std::vector<LSHIndex> lsh_build_parallel(const LSHFamily& fam, const float* base, int n, int d, int threads,
                                         IndexTiming& t, LshThreadStats* stats) {
    const int L = fam.tables();
    const double w0 = wall_time();
    std::unique_ptr<uint32_t[]> codes(new uint32_t[size_t(L) * n]);

    // Fase 1: faixas contiguas de indice de vetor, como no brute-force. Cada thread escreve so os codigos dos
    // seus vetores (codes[l*n + i], i na sua faixa) — o array de codigos e a "lista local" de cada thread.
#pragma omp parallel num_threads(threads)
    {
        const int tid = omp_get_thread_num();
        const int64_t begin = int64_t(n) * tid / threads;
        const int64_t end = int64_t(n) * (tid + 1) / threads;
        const double tw = wall_time(), tc = thread_cpu_time();
        uint32_t buf[MAX_TABLES];
        for (int64_t i = begin; i < end; i++) {
            fam.buckets_of(base + i * d, buf);
            for (int l = 0; l < L; l++) codes[size_t(l) * n + i] = buf[l];
        }
        LshThreadStats& s = stats[tid];
        s.hash_s += wall_time() - tw;
        s.hash_cpu_s += thread_cpu_time() - tc;
        s.begin = begin;
        s.end = end;
    } // barreira: todos os codigos prontos
    t.hash_s = wall_time() - w0;

    // Fase 2 (merge nos buckets): cada tabela inteira fica com uma thread, que percorre os ids em ordem —
    // nenhum bucket e escrito por duas threads, entao nao ha lock. O paralelismo desta fase e limitado a L
    // tabelas: com mais threads que tabelas, as excedentes ficam ociosas (aparece na ocupacao por thread).
    const double w1 = wall_time();
    std::vector<LSHIndex> index(L);
#pragma omp parallel num_threads(threads)
    {
        LshThreadStats& s = stats[omp_get_thread_num()];
#pragma omp for schedule(dynamic, 1) nowait
        for (int l = 0; l < L; l++) {
            const double tw = wall_time();
            build_table(&codes[size_t(l) * n], n, fam.num_buckets(), index[l]);
            s.build_s += wall_time() - tw;
            s.tables_built++;
        }
    }
    t.build_s = wall_time() - w1;
    return index;
}

// ---------------------------------------------------------------------------------------------
// Busca
// ---------------------------------------------------------------------------------------------

int64_t lsh_query(const LSHFamily& fam, const std::vector<LSHIndex>& index, const float* base, int d,
                  const float* query, int k, QueryScratch& scratch, Neighbor* out) {
    uint32_t b[MAX_TABLES];
    fam.buckets_of(query, b);
    std::vector<int>& cand = scratch.cand;
    uint64_t* seen = scratch.seen.data();
    cand.clear();
    for (int l = 0; l < fam.tables(); l++)
        for (int id : index[l].buckets[b[l]].vector_ids) {
            const uint64_t bit = 1ULL << (id & 63);
            if (!(seen[id >> 6] & bit)) {
                seen[id >> 6] |= bit;
                cand.push_back(id);
            }
        }

    // Distancias em ordem crescente de id: os vetores sao lidos da base "para frente", e o prefetch da CPU
    // esconde parte da latencia da memoria (em ordem de bucket, cada candidato e um cache miss aleatorio).
    // Com muitos candidatos, varrer o bitmap inteiro em ordem sai mais barato que ordenar a lista; com poucos,
    // ordena. A ordem nao muda o resultado (o TopK desempata por id). Nos dois casos o bitmap termina zerado.
    const int64_t found = int64_t(cand.size());
    const size_t words = scratch.seen.size();
    TopK top(k);
    if (size_t(found) * 16 > words) {
        for (size_t w = 0; w < words; w++) {
            uint64_t x = seen[w];
            if (!x) continue;
            seen[w] = 0;
            do {
                const int id = int(w * 64 + __builtin_ctzll(x));
                top.push(l2_sq(base + int64_t(id) * d, query, d), id);
                x &= x - 1;
            } while (x);
        }
    } else {
        std::sort(cand.begin(), cand.end());
        for (int id : cand) {
            top.push(l2_sq(base + int64_t(id) * d, query, d), id);
            seen[id >> 6] = 0;
        }
    }
    top.drain_sorted(out);
    return found;
}

void lsh_search_sequential(const LSHFamily& fam, const std::vector<LSHIndex>& index, const float* base, int n, int d,
                           const float* queries, int Q, int k, Neighbor* out, LshThreadStats& s,
                           int64_t* candidates) {
    const double w0 = wall_time(), c0 = thread_cpu_time();
    QueryScratch cand(n);
    for (int q = 0; q < Q; q++) {
        const int64_t c = lsh_query(fam, index, base, d, queries + size_t(q) * d, k, cand, out + size_t(q) * k);
        s.candidates += c;
        if (candidates) candidates[q] = c;
    }
    s.queries += Q;
    s.search_s += wall_time() - w0;
    s.search_cpu_s += thread_cpu_time() - c0;
    s.last_cpu = sched_getcpu();
}

void lsh_search_parallel(const LSHFamily& fam, const std::vector<LSHIndex>& index, const float* base, int n, int d,
                         const float* queries, int Q, int k, int threads, int batch, Neighbor* out,
                         LshThreadStats* stats, int64_t* candidates) {
#pragma omp parallel num_threads(threads)
    {
        LshThreadStats& s = stats[omp_get_thread_num()];
        const double w0 = wall_time(), c0 = thread_cpu_time();
        QueryScratch cand(n); // memoria de trabalho local da thread (alocada dentro do tempo, como no sequencial)
        int nq = 0;
        int64_t nc = 0;
        // Cada query escreve so no seu proprio trecho de `out`: nao ha nada compartilhado para sincronizar.
        // nowait: a thread que acaba sai do loop e registra o seu tempo sem esperar as outras.
#pragma omp for schedule(dynamic, batch) nowait
        for (int q = 0; q < Q; q++) {
            const int64_t c = lsh_query(fam, index, base, d, queries + size_t(q) * d, k, cand, out + size_t(q) * k);
            nc += c;
            if (candidates) candidates[q] = c;
            nq++;
        }
        s.queries += nq;
        s.candidates += nc;
        s.search_s += wall_time() - w0;
        s.search_cpu_s += thread_cpu_time() - c0;
        s.last_cpu = sched_getcpu();
    }
}
