// brute_force — experimento de busca exata sobre o banco em memoria compartilhada.
//
//   brute_force [--dataset sift1m] [--threads N | --seq] [--queries Q] [--k K] [--repeats R]
//               [--baseline SEGUNDOS] [--out results/brute-force]
//
// Precisa do banco no ar (`make up-db`). Para cada repeticao, busca as Q primeiras queries do dataset,
// mede tempo, CPU e energia, confere o recall@K contra o ground truth e grava os CSVs em
// <out>/<data-hora>_<seq|tN>/ (summary.csv por repeticao, threads.csv por thread) e uma linha agregada em
// <out>/runs.csv.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <omp.h>
#include <sched.h>
#include <sys/stat.h>

#include "brute_force.h"
#include "metrics.h"
#include "recall.h"
#include "shm_db.h"

namespace {

struct Options {
    std::string dataset = "sift1m";
    bool sequential = false;
    int threads = 1;
    int queries = 100;
    int k = 10;
    int repeats = 3;
    double baseline_s = 0; // tempo medio do sequencial, para calcular speedup/eficiencia (0 = nao informado)
    std::string out_dir = "results/brute-force";
};

int usage() {
    std::fprintf(stderr,
                 "uso: brute_force [--dataset sift1m|siftsmall] [--threads N | --seq] [--queries Q] [--k K]\n"
                 "                 [--repeats R] [--baseline SEGUNDOS] [--out DIR]\n");
    return 2;
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("faltou o valor de " + a);
            return argv[++i];
        };
        if (a == "--dataset") o.dataset = next();
        else if (a == "--threads") o.threads = std::stoi(next());
        else if (a == "--seq") o.sequential = true;
        else if (a == "--queries") o.queries = std::stoi(next());
        else if (a == "--k") o.k = std::stoi(next());
        else if (a == "--repeats") o.repeats = std::stoi(next());
        else if (a == "--baseline") o.baseline_s = std::stod(next());
        else if (a == "--out") o.out_dir = next();
        else throw std::runtime_error("opcao desconhecida: " + a);
    }
    if (o.sequential) o.threads = 1;
    if (o.threads < 1 || o.queries < 1 || o.k < 1 || o.repeats < 1)
        throw std::runtime_error("--threads, --queries, --k e --repeats precisam ser >= 1");
    return o;
}

struct RepeatResult {
    double search_s = 0;   // tempo de parede de todas as queries (so a busca)
    double merge_s = 0;    // parte do search_s gasta no merge sequencial dos top-k locais
    double cpu_s = 0;      // CPU (user + sys) do processo inteiro durante a busca
    double energy_j = NAN; // energia do pacote da CPU (RAPL) ou NAN se indisponivel
    ProcessUsage usage_delta;
    std::vector<ThreadStats> threads;
};

double mean(const std::vector<double>& v) {
    double s = 0;
    for (double x : v) s += x;
    return s / v.size();
}

double stddev(const std::vector<double>& v) {
    if (v.size() < 2) return 0;
    double m = mean(v), s = 0;
    for (double x : v) s += (x - m) * (x - m);
    return std::sqrt(s / (v.size() - 1));
}

std::string fmt_or_na(double v, int precision = 6) { return std::isnan(v) ? "NA" : fmt(v, precision); }

std::string env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v ? v : fallback;
}

std::string unique_run_dir(const std::string& base) {
    std::string dir = base;
    struct stat st;
    for (int i = 2; stat(dir.c_str(), &st) == 0; i++) dir = base + "-" + std::to_string(i);
    return dir;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    try {
        opt = parse(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "erro: %s\n", e.what());
        return usage();
    }

    try {
        DbView db = attach_db(opt.dataset);
        const DbHeader& h = *db.hdr;
        if (opt.queries > h.n_query)
            throw std::runtime_error("--queries " + std::to_string(opt.queries) + " maior que as " +
                                     std::to_string(h.n_query) + " queries do dataset");
        if (opt.k > h.gt_k)
            throw std::runtime_error("--k " + std::to_string(opt.k) + " maior que o ground truth (k=" +
                                     std::to_string(h.gt_k) + ")");

        const int T = opt.threads;
        const int n = h.n_base, d = h.d, k = opt.k, Q = opt.queries;
        const std::string algorithm = opt.sequential ? "brute-force-seq" : "brute-force-par";

        std::printf("[brute-force] %s | banco '%s' (%d x %d) | %d queries | k=%d | %d repeticao(oes)\n",
                    opt.sequential ? "sequencial" : ("paralelo, " + std::to_string(T) + " threads").c_str(),
                    h.dataset, n, d, Q, k, opt.repeats);

        std::vector<Neighbor> results(size_t(Q) * k);
        std::vector<Neighbor> scratch(size_t(T) * k);
        std::vector<ThreadStats> warm(T);
        double warm_merge = 0;

        // Aquecimento (fora do tempo): uma query completa carrega a base na cache/TLB e, no paralelo, cria o
        // pool de threads do OpenMP — sem isso a primeira repeticao pagaria esses custos sozinha.
        if (opt.sequential) bf_search_sequential(db.base, n, d, db.queries, k, results.data());
        else bf_search_parallel(db.base, n, d, db.queries, k, T, scratch, warm.data(), warm_merge, results.data());

        EnergyMeter energy;
        if (!energy.available()) std::printf("  energia: %s — gravando NA\n", energy.unavailable_reason().c_str());

        std::vector<RepeatResult> reps;
        for (int r = 0; r < opt.repeats; r++) {
            RepeatResult rr;
            rr.threads.assign(T, ThreadStats{});

            ProcessUsage u0 = process_usage();
            if (energy.available()) energy.start();
            const double t0 = wall_time();

            for (int q = 0; q < Q; q++) {
                const float* query = db.queries + size_t(q) * d;
                Neighbor* out = &results[size_t(q) * k];
                if (opt.sequential) {
                    const double w0 = wall_time(), c0 = thread_cpu_time();
                    bf_search_sequential(db.base, n, d, query, k, out);
                    rr.threads[0].scan_s += wall_time() - w0;
                    rr.threads[0].cpu_s += thread_cpu_time() - c0;
                    rr.threads[0].end = n;
                    rr.threads[0].last_cpu = sched_getcpu();
                } else {
                    bf_search_parallel(db.base, n, d, query, k, T, scratch, rr.threads.data(), rr.merge_s, out);
                }
            }

            rr.search_s = wall_time() - t0;
            if (energy.available()) rr.energy_j = energy.stop_joules();
            ProcessUsage u1 = process_usage();
            rr.cpu_s = (u1.user_s - u0.user_s) + (u1.sys_s - u0.sys_s);
            rr.usage_delta.invol_ctx_switches = u1.invol_ctx_switches - u0.invol_ctx_switches;
            rr.usage_delta.vol_ctx_switches = u1.vol_ctx_switches - u0.vol_ctx_switches;
            rr.usage_delta.minor_faults = u1.minor_faults - u0.minor_faults;
            rr.usage_delta.max_rss_kb = u1.max_rss_kb;

            std::printf("  repeticao %d: %.3f s (%.2f ms/query, merge %.3f ms) | CPU %.2f s (%.0f%% de %d threads)%s\n",
                        r + 1, rr.search_s, rr.search_s * 1e3 / Q, rr.merge_s * 1e3, rr.cpu_s,
                        100.0 * rr.cpu_s / (rr.search_s * T), T,
                        std::isnan(rr.energy_j) ? "" : (" | " + fmt(rr.energy_j, 2) + " J").c_str());
            reps.push_back(std::move(rr));
        }

        // Brute-force e exato: o recall esperado e 1.0 (ver common/recall.h sobre empates de distancia).
        const Recall recall = compute_recall(results.data(), Q, k, db.gt, h.gt_k, db.base, db.queries, d);

        // ------------------------------------------------------------------------------------------
        // Agregados, impressao e CSVs
        // ------------------------------------------------------------------------------------------
        std::vector<double> search, merge, cpu, energy_v;
        for (const auto& rr : reps) {
            search.push_back(rr.search_s);
            merge.push_back(rr.merge_s);
            cpu.push_back(rr.cpu_s);
            if (!std::isnan(rr.energy_j)) energy_v.push_back(rr.energy_j);
        }
        const double search_mean = mean(search), cpu_mean = mean(cpu);
        const double energy_mean = energy_v.empty() ? NAN : mean(energy_v);
        const double speedup = opt.baseline_s > 0 ? opt.baseline_s / search_mean : NAN;
        const double efficiency = opt.baseline_s > 0 ? speedup / T : NAN;
        const double cpu_util = cpu_mean / (search_mean * T);

        const std::string run_id = timestamp_id() + "_" + (opt.sequential ? "seq" : "t" + std::to_string(T));
        const std::string run_dir = unique_run_dir(opt.out_dir + "/" + run_id);
        make_dirs(run_dir);

        std::printf("\n  resultado: %.3f s +- %.3f (%.2f ms/query, %.1f queries/s)\n", search_mean, stddev(search),
                    search_mean * 1e3 / Q, Q / search_mean);
        std::printf("  recall@%d = %.4f (considerando empates de distancia) | %.4f (estrito, por id)\n", k,
                    recall.tie_aware, recall.strict);
        // CPU do processo inclui o tempo em que as threads do OpenMP ficam "girando" esperando a proxima regiao
        // paralela — isso tambem gasta energia, por isso e esse o numero usado como proxy de energia.
        std::printf("  CPU: %.2f s por repeticao (%.0f%% de utilizacao media das %d threads)\n", cpu_mean,
                    100 * cpu_util, T);
        if (!std::isnan(energy_mean))
            std::printf("  energia: %.2f J por repeticao (%.2f W medios, %.3f mJ/query)\n", energy_mean,
                        energy_mean / search_mean, energy_mean * 1e3 / Q);
        if (!std::isnan(speedup))
            std::printf("  speedup %.2fx sobre o sequencial (%.3f s) | eficiencia %.0f%%\n", speedup, opt.baseline_s,
                        100 * efficiency);

        // Por thread (media das repeticoes). "ocupacao" = fracao do tempo total de busca em que a thread
        // estava varrendo a sua fatia; o resto ela passou esperando as outras na barreira ou o merge.
        // Ocupacoes desiguais entre threads = desbalanceamento de carga.
        std::printf("\n  %-6s %-19s %9s %9s %8s %6s\n", "thread", "faixa de vetores", "varredura", "CPU", "ocupacao",
                    "nucleo");
        CsvTable threads_csv({"run_id", "repeat", "thread", "begin", "end", "vectors", "scan_s", "cpu_s",
                              "occupancy_pct", "last_cpu"});
        for (int t = 0; t < T; t++) {
            double scan = 0, tcpu = 0;
            for (size_t r = 0; r < reps.size(); r++) {
                const ThreadStats& s = reps[r].threads[t];
                scan += s.scan_s;
                tcpu += s.cpu_s;
                threads_csv.add_row({run_id, std::to_string(r + 1), std::to_string(t), std::to_string(s.begin),
                                     std::to_string(s.end), std::to_string(s.end - s.begin), fmt(s.scan_s),
                                     fmt(s.cpu_s), fmt(100 * s.scan_s / reps[r].search_s, 1),
                                     std::to_string(s.last_cpu)});
            }
            scan /= reps.size();
            tcpu /= reps.size();
            const ThreadStats& last = reps.back().threads[t];
            std::string range = std::to_string(last.begin) + "-" + std::to_string(last.end);
            std::printf("  %-6d %-19s %8.3fs %8.3fs %7.0f%% %6d\n", t, range.c_str(), scan, tcpu,
                        100 * scan / search_mean, last.last_cpu);
        }

        CsvTable summary_csv({"run_id", "repeat", "search_s", "merge_s", "ms_per_query", "cpu_s", "cpu_util_pct",
                              "energy_j", "invol_ctx_switches", "vol_ctx_switches", "minor_faults"});
        for (size_t r = 0; r < reps.size(); r++) {
            const RepeatResult& rr = reps[r];
            summary_csv.add_row({run_id, std::to_string(r + 1), fmt(rr.search_s), fmt(rr.merge_s),
                                 fmt(rr.search_s * 1e3 / Q, 4), fmt(rr.cpu_s), fmt(100 * rr.cpu_s / (rr.search_s * T), 1),
                                 fmt_or_na(rr.energy_j, 3), std::to_string(rr.usage_delta.invol_ctx_switches),
                                 std::to_string(rr.usage_delta.vol_ctx_switches),
                                 std::to_string(rr.usage_delta.minor_faults)});
        }

        CsvTable runs_csv({"run_id", "timestamp", "host", "cpu_model", "algorithm", "dataset", "threads", "n_base", "d",
                           "queries", "k", "repeats", "search_s_mean", "search_s_std", "search_s_min", "ms_per_query",
                           "queries_per_s", "merge_s_mean", "recall_at_k", "recall_at_k_strict", "cpu_s_mean", "cpu_util_pct", "energy_j_mean",
                           "avg_power_w", "energy_mj_per_query", "baseline_s", "speedup", "efficiency",
                           "max_rss_mb", "omp_proc_bind", "omp_places"});
        runs_csv.add_row({run_id, timestamp_iso(), hostname(), cpu_model(), algorithm, h.dataset, std::to_string(T),
                          std::to_string(n), std::to_string(d), std::to_string(Q), std::to_string(k),
                          std::to_string(opt.repeats), fmt(search_mean), fmt(stddev(search)),
                          fmt(*std::min_element(search.begin(), search.end())), fmt(search_mean * 1e3 / Q, 4),
                          fmt(Q / search_mean, 2), fmt(mean(merge)), fmt(recall.tie_aware, 4), fmt(recall.strict, 4), fmt(cpu_mean),
                          fmt(100 * cpu_util, 1), fmt_or_na(energy_mean, 3),
                          fmt_or_na(energy_mean / search_mean, 3), fmt_or_na(energy_mean * 1e3 / Q, 4),
                          opt.baseline_s > 0 ? fmt(opt.baseline_s) : "NA", fmt_or_na(speedup, 3),
                          fmt_or_na(efficiency, 3), fmt(reps.back().usage_delta.max_rss_kb / 1024.0, 1),
                          env_or("OMP_PROC_BIND", "unset"), env_or("OMP_PLACES", "unset")});

        summary_csv.write(run_dir + "/summary.csv");
        threads_csv.write(run_dir + "/threads.csv");
        runs_csv.write(run_dir + "/run.csv");
        runs_csv.append(opt.out_dir + "/runs.csv");
        std::printf("\n  CSVs gravados em %s/ (e uma linha em %s/runs.csv)\n", run_dir.c_str(), opt.out_dir.c_str());

        detach_db(db);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "erro: %s\n", e.what());
        return 1;
    }
}
