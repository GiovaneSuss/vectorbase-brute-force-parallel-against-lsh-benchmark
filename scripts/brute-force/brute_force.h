#pragma once

// Busca exata (brute-force) dos k vizinhos mais proximos de uma query, sequencial e paralela (OpenMP).
// A base e lida direto do banco em memoria compartilhada (ponteiro const, row-major n * d).

#include <cstdint>
#include <vector>

#include "topk.h"

// Estatisticas acumuladas por thread ao longo de todas as queries de uma repeticao. alignas(64) coloca
// cada thread na sua propria linha de cache: sem isso, threads vizinhas escrevendo nos seus contadores
// invalidariam a cache umas das outras (false sharing).
struct alignas(64) ThreadStats {
    int64_t begin = 0, end = 0; // fatia [begin, end) da base atribuida a thread
    double scan_s = 0;          // tempo de parede gasto varrendo a fatia
    double cpu_s = 0;           // tempo de CPU da thread (CLOCK_THREAD_CPUTIME_ID) no mesmo trecho
    int last_cpu = -1;          // nucleo em que a thread rodou por ultimo (sched_getcpu)
};

// Varre a base inteira com uma unica thread, sem OpenMP — baseline do speedup.
void bf_search_sequential(const float* base, int n, int d, const float* query, int k, Neighbor* out);

// Divide a base em `threads` faixas contiguas de indices de vetor (nenhum vetor e partido entre threads);
// cada thread mantem seu top-k local e, depois da regiao paralela, um merge sequencial produz o top-k global.
// `scratch` guarda os top-k locais (threads * k); `stats` tem `threads` posicoes; `merge_s` acumula o tempo
// do merge.
void bf_search_parallel(const float* base, int n, int d, const float* query, int k, int threads,
                        std::vector<Neighbor>& scratch, ThreadStats* stats, double& merge_s, Neighbor* out);
