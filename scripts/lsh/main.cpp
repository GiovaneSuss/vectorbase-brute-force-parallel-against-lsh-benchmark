// lsh — experimento de busca aproximada (LSH) sobre o banco em memoria compartilhada.
//
//   lsh [--dataset auto|sift1m|sift10m|siftsmall] [--threads N | --seq] [--queries Q] [--k K] [--repeats R]
//       [--tables L] [--hashes K] [--width W] [--buckets B] [--seed S] [--batch B]
//       [--baseline SEGUNDOS] [--index-baseline SEGUNDOS] [--out results/lsh]
//
// Precisa do banco no ar (`make up-db-1m` / `make up-db-10m`); com --dataset auto (padrao) usa o unico banco
// que estiver no ar. Cada repeticao constroi o indice do zero (tempo de INDEXACAO, medido e reportado a parte)
// e depois busca as Q primeiras queries (tempo de BUSCA — o que se compara com o brute-force). Grava os CSVs
// em <out>/<data-hora>_<seq|tN>/ (summary.csv por repeticao, threads.csv por thread) e uma linha agregada em
// <out>/runs.csv.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "lsh.h"
#include "metrics.h"
#include "recall.h"
#include "shm_db.h"

namespace {

struct Options {
    std::string dataset = "auto"; // auto = o unico banco no ar
    bool sequential = false;
    int threads = 1;
    int queries = 100;
    int k = 10;
    int repeats = 3;
    int batch = 1;               // queries por lote no schedule(dynamic) da busca paralela
    LSHParams lsh;
    double baseline_s = 0;       // busca sequencial media, para speedup/eficiencia da busca (0 = nao informado)
    double index_baseline_s = 0; // indexacao sequencial media, idem para a indexacao
    std::string out_dir = "results/lsh";
};

int usage() {
    std::fprintf(stderr,
                 "uso: lsh [--dataset auto|sift1m|sift10m|siftsmall] [--threads N | --seq] [--queries Q] [--k K]\n"
                 "         [--repeats R] [--tables L] [--hashes K] [--width W] [--buckets B] [--seed S]\n"
                 "         [--batch B] [--baseline SEGUNDOS] [--index-baseline SEGUNDOS] [--out DIR]\n");
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
        else if (a == "--tables") o.lsh.tables = std::stoi(next());
        else if (a == "--hashes") o.lsh.hashes = std::stoi(next());
        else if (a == "--width") o.lsh.width = std::stof(next());
        else if (a == "--buckets") o.lsh.num_buckets = std::stoi(next());
        else if (a == "--seed") o.lsh.seed = std::stoull(next());
        else if (a == "--batch") o.batch = std::stoi(next());
        else if (a == "--baseline") o.baseline_s = std::stod(next());
        else if (a == "--index-baseline") o.index_baseline_s = std::stod(next());
        else if (a == "--out") o.out_dir = next();
        else throw std::runtime_error("opcao desconhecida: " + a);
    }
    if (o.sequential) o.threads = 1;
    if (o.threads < 1 || o.queries < 1 || o.k < 1 || o.repeats < 1 || o.batch < 1)
        throw std::runtime_error("--threads, --queries, --k, --repeats e --batch precisam ser >= 1");
    return o;
}

struct RepeatResult {
    IndexTiming idx;              // fases da indexacao
    double index_s = 0;           // tempo de parede da indexacao inteira
    double index_cpu_s = 0;       // CPU (user + sys) do processo durante a indexacao
    double index_energy_j = NAN;
    double search_s = 0;          // tempo de parede de todas as queries (so a busca)
    double cpu_s = 0;             // CPU do processo durante a busca
    double energy_j = NAN;
    ProcessUsage usage_delta;     // da busca
    std::vector<LshThreadStats> threads;
};

struct IndexShape {
    int64_t ids = 0;          // L * n
    int64_t nonempty = 0;     // buckets nao vazios, somando as L tabelas
    int64_t max_bucket = 0;   // maior bucket de todas as tabelas
    double mb = 0;            // memoria do indice (vetores de Bucket + ids)
};

IndexShape shape_of(const std::vector<LSHIndex>& index) {
    IndexShape s;
    for (const LSHIndex& t : index) {
        s.mb += double(t.buckets.size()) * sizeof(Bucket);
        for (const Bucket& b : t.buckets) {
            const int64_t sz = int64_t(b.vector_ids.size());
            s.ids += sz;
            if (sz) s.nonempty++;
            s.max_bucket = std::max(s.max_bucket, sz);
        }
    }
    s.mb = (s.mb + double(s.ids) * sizeof(int)) / (1024.0 * 1024.0);
    return s;
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
        DbView db = attach_db(resolve_dataset(opt.dataset));
        const DbHeader& h = *db.hdr;
        if (opt.queries > h.n_query)
            throw std::runtime_error("--queries " + std::to_string(opt.queries) + " maior que as " +
                                     std::to_string(h.n_query) + " queries do dataset");
        if (opt.k > h.gt_k)
            throw std::runtime_error("--k " + std::to_string(opt.k) + " maior que o ground truth (k=" +
                                     std::to_string(h.gt_k) + ")");

        const int T = opt.threads;
        const int n = h.n_base, d = h.d, k = opt.k, Q = opt.queries;
        const std::string algorithm = opt.sequential ? "lsh-seq" : "lsh-par";

        // Sorteio das projecoes: parte dos parametros fixos do experimento (mesma semente em todas as
        // configuracoes), nao da indexacao — fica fora do tempo medido.
        const LSHFamily fam(opt.lsh, n, d);
        const LSHParams& P = fam.params();

        std::printf("[lsh] %s | banco '%s' (%d x %d) | %d queries | k=%d | %d repeticao(oes)\n",
                    opt.sequential ? "sequencial" : ("paralelo, " + std::to_string(T) + " threads").c_str(), h.dataset,
                    n, d, Q, k, opt.repeats);
        std::printf("  L=%d tabelas | K=%d hashes/tabela | w=%g | %d buckets/tabela | seed %llu%s\n", P.tables,
                    P.hashes, P.width, fam.num_buckets(), (unsigned long long)P.seed,
                    opt.sequential ? "" : (" | lote de " + std::to_string(opt.batch) + " query(s)").c_str());

        // Aquecimento (fora do tempo): cria o pool de threads do OpenMP, para a indexacao da primeira repeticao
        // nao pagar esse custo sozinha. A base ja esta mapeada com MAP_POPULATE (sem page faults).
        if (!opt.sequential) {
#pragma omp parallel num_threads(T)
            { (void)0; }
        }

        EnergyMeter energy;
        if (!energy.available()) std::printf("  energia: %s — gravando NA\n", energy.unavailable_reason().c_str());

        std::vector<Neighbor> results(size_t(Q) * k);
        std::vector<LSHIndex> index;
        IndexShape shape;
        std::vector<RepeatResult> reps;
        for (int r = 0; r < opt.repeats; r++) {
            RepeatResult rr;
            rr.threads.assign(T, LshThreadStats{});
            std::vector<LSHIndex>().swap(index); // libera o indice anterior fora do tempo medido

            // ---- indexacao ----
            ProcessUsage u0 = process_usage();
            if (energy.available()) energy.start();
            const double t0 = wall_time();
            if (opt.sequential) index = lsh_build_sequential(fam, db.base, n, d, rr.idx, rr.threads[0]);
            else index = lsh_build_parallel(fam, db.base, n, d, T, rr.idx, rr.threads.data());
            rr.index_s = wall_time() - t0;
            if (energy.available()) rr.index_energy_j = energy.stop_joules();
            ProcessUsage u1 = process_usage();
            rr.index_cpu_s = (u1.user_s - u0.user_s) + (u1.sys_s - u0.sys_s);
            if (r == 0) shape = shape_of(index);

            // ---- busca ----
            ProcessUsage u2 = process_usage();
            if (energy.available()) energy.start();
            const double t1 = wall_time();
            if (opt.sequential)
                lsh_search_sequential(fam, index, db.base, n, d, db.queries, Q, k, results.data(), rr.threads[0]);
            else
                lsh_search_parallel(fam, index, db.base, n, d, db.queries, Q, k, T, opt.batch, results.data(),
                                    rr.threads.data());
            rr.search_s = wall_time() - t1;
            if (energy.available()) rr.energy_j = energy.stop_joules();
            ProcessUsage u3 = process_usage();
            rr.cpu_s = (u3.user_s - u2.user_s) + (u3.sys_s - u2.sys_s);
            rr.usage_delta.invol_ctx_switches = u3.invol_ctx_switches - u2.invol_ctx_switches;
            rr.usage_delta.vol_ctx_switches = u3.vol_ctx_switches - u2.vol_ctx_switches;
            rr.usage_delta.minor_faults = u3.minor_faults - u2.minor_faults;
            rr.usage_delta.max_rss_kb = u3.max_rss_kb;

            std::printf("  repeticao %d: indexacao %.3f s (hash %.3f + buckets %.3f) | busca %.4f s (%.3f ms/query)"
                        " | CPU busca %.3f s (%.0f%% de %d threads)%s\n",
                        r + 1, rr.index_s, rr.idx.hash_s, rr.idx.build_s, rr.search_s, rr.search_s * 1e3 / Q,
                        rr.cpu_s, 100.0 * rr.cpu_s / (rr.search_s * T), T,
                        std::isnan(rr.energy_j) ? ""
                                                : (" | " + fmt(rr.index_energy_j, 2) + " J indexacao, " +
                                                   fmt(rr.energy_j, 2) + " J busca").c_str());
            reps.push_back(std::move(rr));
        }

        // LSH e aproximado: recall < 1.0 e o esperado (ver common/recall.h sobre empates de distancia).
        const Recall recall = compute_recall(results.data(), Q, k, db.gt, h.gt_k, db.base, db.queries, d);

        // ------------------------------------------------------------------------------------------
        // Agregados, impressao e CSVs
        // ------------------------------------------------------------------------------------------
        std::vector<double> index_v, hash_v, build_v, index_cpu, index_energy, search, cpu, energy_v;
        int64_t cand_total = 0;
        for (const auto& rr : reps) {
            index_v.push_back(rr.index_s);
            hash_v.push_back(rr.idx.hash_s);
            build_v.push_back(rr.idx.build_s);
            index_cpu.push_back(rr.index_cpu_s);
            if (!std::isnan(rr.index_energy_j)) index_energy.push_back(rr.index_energy_j);
            search.push_back(rr.search_s);
            cpu.push_back(rr.cpu_s);
            if (!std::isnan(rr.energy_j)) energy_v.push_back(rr.energy_j);
        }
        for (const auto& s : reps.back().threads) cand_total += s.candidates;
        const double cand_per_query = double(cand_total) / Q;

        const double index_mean = mean(index_v), search_mean = mean(search), cpu_mean = mean(cpu);
        const double index_energy_mean = index_energy.empty() ? NAN : mean(index_energy);
        const double energy_mean = energy_v.empty() ? NAN : mean(energy_v);
        const double speedup = opt.baseline_s > 0 ? opt.baseline_s / search_mean : NAN;
        const double efficiency = opt.baseline_s > 0 ? speedup / T : NAN;
        const double index_speedup = opt.index_baseline_s > 0 ? opt.index_baseline_s / index_mean : NAN;
        const double index_efficiency = opt.index_baseline_s > 0 ? index_speedup / T : NAN;
        const double cpu_util = cpu_mean / (search_mean * T);

        const std::string run_id = timestamp_id() + "_" + (opt.sequential ? "seq" : "t" + std::to_string(T));
        const std::string run_dir = unique_run_dir(opt.out_dir + "/" + run_id);
        make_dirs(run_dir);

        std::printf("\n  indexacao: %.3f s +- %.3f (hash %.3f s + buckets %.3f s) | CPU %.2f s", index_mean,
                    stddev(index_v), mean(hash_v), mean(build_v), mean(index_cpu));
        if (!std::isnan(index_energy_mean)) std::printf(" | %.2f J", index_energy_mean);
        std::printf("\n  indice: %.1f MiB | %lld buckets nao vazios de %lld (%.1f vetores/bucket, maior %lld)\n",
                    shape.mb, (long long)shape.nonempty, (long long)fam.num_buckets() * P.tables,
                    double(shape.ids) / std::max<int64_t>(shape.nonempty, 1), (long long)shape.max_bucket);
        if (!std::isnan(index_speedup))
            std::printf("  indexacao: speedup %.2fx sobre o sequencial (%.3f s) | eficiencia %.0f%%\n", index_speedup,
                        opt.index_baseline_s, 100 * index_efficiency);

        std::printf("\n  busca: %.4f s +- %.4f (%.3f ms/query, %.1f queries/s)\n", search_mean, stddev(search),
                    search_mean * 1e3 / Q, Q / search_mean);
        std::printf("  candidatos: %.0f por query (%.3f%% da base — o brute-force calcula 100%%)\n", cand_per_query,
                    100.0 * cand_per_query / n);
        std::printf("  recall@%d = %.4f (considerando empates de distancia) | %.4f (estrito, por id)\n", k,
                    recall.tie_aware, recall.strict);
        std::printf("  CPU: %.3f s por repeticao (%.0f%% de utilizacao media das %d threads)\n", cpu_mean,
                    100 * cpu_util, T);
        if (!std::isnan(energy_mean))
            std::printf("  energia: %.2f J por repeticao (%.2f W medios, %.3f mJ/query)\n", energy_mean,
                        energy_mean / search_mean, energy_mean * 1e3 / Q);
        if (!std::isnan(speedup))
            std::printf("  busca: speedup %.2fx sobre o sequencial (%.4f s) | eficiencia %.0f%%\n", speedup,
                        opt.baseline_s, 100 * efficiency);

        // Por thread (media das repeticoes). Ocupacao da busca = fracao do tempo total de busca em que a thread
        // estava processando queries; o resto foi espera pelas outras. Com mais threads que tabelas (T > L),
        // algumas threads montam 0 tabelas na fase 2 da indexacao.
        std::printf("\n  %-6s %-19s %8s %7s %7s %11s %8s %8s %6s\n", "thread", "faixa (indexacao)", "hash", "tabelas",
                    "queries", "candidatos", "busca", "ocupacao", "nucleo");
        CsvTable threads_csv({"run_id", "repeat", "thread", "begin", "end", "vectors", "hash_s", "hash_cpu_s",
                              "tables_built", "build_s", "queries", "candidates", "search_s", "search_cpu_s",
                              "search_occupancy_pct", "last_cpu"});
        for (int t = 0; t < T; t++) {
            double hash = 0, busy = 0;
            for (size_t r = 0; r < reps.size(); r++) {
                const LshThreadStats& s = reps[r].threads[t];
                hash += s.hash_s;
                busy += s.search_s;
                threads_csv.add_row({run_id, std::to_string(r + 1), std::to_string(t), std::to_string(s.begin),
                                     std::to_string(s.end), std::to_string(s.end - s.begin), fmt(s.hash_s),
                                     fmt(s.hash_cpu_s), std::to_string(s.tables_built), fmt(s.build_s),
                                     std::to_string(s.queries), std::to_string(s.candidates), fmt(s.search_s),
                                     fmt(s.search_cpu_s), fmt(100 * s.search_s / reps[r].search_s, 1),
                                     std::to_string(s.last_cpu)});
            }
            hash /= reps.size();
            busy /= reps.size();
            const LshThreadStats& last = reps.back().threads[t];
            std::string range = std::to_string(last.begin) + "-" + std::to_string(last.end);
            std::printf("  %-6d %-19s %7.3fs %7d %7d %11lld %7.4fs %7.0f%% %6d\n", t, range.c_str(), hash,
                        last.tables_built, last.queries, (long long)last.candidates, busy, 100 * busy / search_mean,
                        last.last_cpu);
        }

        CsvTable summary_csv({"run_id", "repeat", "index_s", "index_hash_s", "index_build_s", "index_cpu_s",
                              "index_energy_j", "search_s", "ms_per_query", "cpu_s", "cpu_util_pct", "energy_j",
                              "invol_ctx_switches", "vol_ctx_switches", "minor_faults"});
        for (size_t r = 0; r < reps.size(); r++) {
            const RepeatResult& rr = reps[r];
            summary_csv.add_row({run_id, std::to_string(r + 1), fmt(rr.index_s), fmt(rr.idx.hash_s),
                                 fmt(rr.idx.build_s), fmt(rr.index_cpu_s), fmt_or_na(rr.index_energy_j, 3),
                                 fmt(rr.search_s), fmt(rr.search_s * 1e3 / Q, 4), fmt(rr.cpu_s),
                                 fmt(100 * rr.cpu_s / (rr.search_s * T), 1), fmt_or_na(rr.energy_j, 3),
                                 std::to_string(rr.usage_delta.invol_ctx_switches),
                                 std::to_string(rr.usage_delta.vol_ctx_switches),
                                 std::to_string(rr.usage_delta.minor_faults)});
        }

        // Colunas da busca com os mesmos nomes do runs.csv do brute-force, para a analise juntar os dois.
        CsvTable runs_csv({"run_id", "timestamp", "host", "cpu_model", "algorithm", "dataset", "threads", "n_base", "d",
                           "queries", "k", "repeats", "tables", "hashes", "width", "num_buckets", "seed", "batch",
                           "index_s_mean", "index_s_std", "index_s_min", "index_hash_s_mean", "index_build_s_mean",
                           "index_cpu_s_mean", "index_energy_j_mean", "index_baseline_s", "index_speedup",
                           "index_efficiency", "index_mb", "nonempty_buckets", "max_bucket", "search_s_mean",
                           "search_s_std", "search_s_min", "ms_per_query", "queries_per_s", "candidates_per_query",
                           "candidate_pct", "recall_at_k", "recall_at_k_strict", "cpu_s_mean", "cpu_util_pct",
                           "energy_j_mean", "avg_power_w", "energy_mj_per_query", "baseline_s", "speedup",
                           "efficiency", "max_rss_mb", "omp_proc_bind", "omp_places"});
        runs_csv.add_row({run_id, timestamp_iso(), hostname(), cpu_model(), algorithm, h.dataset, std::to_string(T),
                          std::to_string(n), std::to_string(d), std::to_string(Q), std::to_string(k),
                          std::to_string(opt.repeats), std::to_string(P.tables), std::to_string(P.hashes),
                          fmt(P.width, 3), std::to_string(fam.num_buckets()), std::to_string(P.seed),
                          opt.sequential ? "NA" : std::to_string(opt.batch), fmt(index_mean), fmt(stddev(index_v)),
                          fmt(*std::min_element(index_v.begin(), index_v.end())), fmt(mean(hash_v)),
                          fmt(mean(build_v)), fmt(mean(index_cpu)), fmt_or_na(index_energy_mean, 3),
                          opt.index_baseline_s > 0 ? fmt(opt.index_baseline_s) : "NA", fmt_or_na(index_speedup, 3),
                          fmt_or_na(index_efficiency, 3), fmt(shape.mb, 1), std::to_string(shape.nonempty),
                          std::to_string(shape.max_bucket), fmt(search_mean), fmt(stddev(search)),
                          fmt(*std::min_element(search.begin(), search.end())), fmt(search_mean * 1e3 / Q, 4),
                          fmt(Q / search_mean, 2), fmt(cand_per_query, 1), fmt(100.0 * cand_per_query / n, 4),
                          fmt(recall.tie_aware, 4), fmt(recall.strict, 4), fmt(cpu_mean), fmt(100 * cpu_util, 1),
                          fmt_or_na(energy_mean, 3), fmt_or_na(energy_mean / search_mean, 3),
                          fmt_or_na(energy_mean * 1e3 / Q, 4), opt.baseline_s > 0 ? fmt(opt.baseline_s) : "NA",
                          fmt_or_na(speedup, 3), fmt_or_na(efficiency, 3),
                          fmt(reps.back().usage_delta.max_rss_kb / 1024.0, 1), env_or("OMP_PROC_BIND", "unset"),
                          env_or("OMP_PLACES", "unset")});

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
