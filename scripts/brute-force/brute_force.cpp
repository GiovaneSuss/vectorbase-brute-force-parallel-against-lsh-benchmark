#include "brute_force.h"

#include <omp.h>
#include <sched.h>

#include "distance.h"
#include "metrics.h"

void bf_search_sequential(const float* base, int n, int d, const float* query, int k, Neighbor* out) {
    TopK top(k);
    for (int i = 0; i < n; i++) top.push(l2_sq(base + int64_t(i) * d, query, d), i);
    top.drain_sorted(out);
}

void bf_search_parallel(const float* base, int n, int d, const float* query, int k, int threads,
                        std::vector<Neighbor>& scratch, ThreadStats* stats, double& merge_s, Neighbor* out) {
#pragma omp parallel num_threads(threads)
    {
        const int t = omp_get_thread_num();
        // Divisao por indice de vetor: a thread t fica com [n*t/T, n*(t+1)/T). So depois isso vira offset de
        // memoria (i * d), entao a fronteira entre threads sempre cai entre dois vetores.
        const int64_t begin = int64_t(n) * t / threads;
        const int64_t end = int64_t(n) * (t + 1) / threads;

        const double w0 = wall_time();
        const double c0 = thread_cpu_time();
        TopK top(k);
        for (int64_t i = begin; i < end; i++) top.push(l2_sq(base + i * d, query, d), int(i));
        top.drain_sorted(&scratch[size_t(t) * k]);

        ThreadStats& s = stats[t];
        s.scan_s += wall_time() - w0;
        s.cpu_s += thread_cpu_time() - c0;
        s.begin = begin;
        s.end = end;
        s.last_cpu = sched_getcpu();
    } // barreira implicita: todas as threads terminaram suas fatias

    const double m0 = wall_time();
    merge_topk(scratch, threads, k, out);
    merge_s += wall_time() - m0;
}
