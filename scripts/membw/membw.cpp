// membw — banda de memoria da maquina por numero de threads (no estilo do STREAM, McCalpin).
//
//   membw [--threads N] [--mb MB] [--repeats R] [--out results/membw]
//
// Mede o teto de banda da RAM independentemente das buscas, para comparar com a banda efetiva do brute-force
// (coluna effective_gbps do runs.csv): se o brute-force encosta nesse teto, o gargalo e a memoria, nao as
// threads. Dois testes sobre vetores bem maiores que a cache L3:
//
//   read  — soma todos os elementos de um vetor: so leitura sequencial, o mesmo padrao de acesso do brute-force
//           (cada query varre a base inteira). E o numero que importa para a comparacao.
//   triad — a[i] = b[i] + s * c[i] do STREAM: 2 leituras + 1 escrita, para comparar com numeros publicados.
//
// Cada teste roda 1 vez de aquecimento + R repeticoes; o STREAM reporta a melhor (a menos perturbada). Grava uma
// linha por teste em <out>/runs.csv.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include <omp.h>

#include "metrics.h"

namespace {

struct Options {
    int threads = 1;
    int mb = 2048; // memoria total usada por teste (read: 1 vetor; triad: 3 vetores de mb/3)
    int repeats = 10;
    std::string out_dir = "results/membw";
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("faltou o valor de " + a);
            return argv[++i];
        };
        if (a == "--threads") o.threads = std::stoi(next());
        else if (a == "--mb") o.mb = std::stoi(next());
        else if (a == "--repeats") o.repeats = std::stoi(next());
        else if (a == "--out") o.out_dir = next();
        else throw std::runtime_error("opcao desconhecida: " + a);
    }
    if (o.threads < 1 || o.mb < 64 || o.repeats < 1)
        throw std::runtime_error("--threads >= 1, --mb >= 64 e --repeats >= 1");
    return o;
}

// Leitura: 8 acumuladores independentes para a CPU nao ficar presa na dependencia da soma — o limite passa a
// ser a memoria. Inteiros: a soma nao depende da ordem, entao o compilador pode vetorizar sem -ffast-math.
uint64_t read_kernel(const uint64_t* a, int64_t n, int T) {
    uint64_t total = 0;
#pragma omp parallel num_threads(T) reduction(+ : total)
    {
        uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0;
#pragma omp for schedule(static)
        for (int64_t i = 0; i < n; i += 8) {
            s0 += a[i];
            s1 += a[i + 1];
            s2 += a[i + 2];
            s3 += a[i + 3];
            s4 += a[i + 4];
            s5 += a[i + 5];
            s6 += a[i + 6];
            s7 += a[i + 7];
        }
        total += s0 + s1 + s2 + s3 + s4 + s5 + s6 + s7;
    }
    return total;
}

void triad_kernel(double* a, const double* b, const double* c, int64_t n, double s, int T) {
#pragma omp parallel for num_threads(T) schedule(static)
    for (int64_t i = 0; i < n; i++) a[i] = b[i] + s * c[i];
}

struct Result {
    std::vector<double> times;
    double bytes = 0;
};

} // namespace

int main(int argc, char** argv) {
    Options opt;
    try {
        opt = parse(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "erro: %s\nuso: membw [--threads N] [--mb MB] [--repeats R] [--out DIR]\n", e.what());
        return 2;
    }
    try {
        const int T = opt.threads;
        const int64_t total_bytes = int64_t(opt.mb) << 20;
        std::printf("[membw] %d thread(s) | %d MiB por teste | %d repeticoes (+1 aquecimento)\n", T, opt.mb,
                    opt.repeats);

        // read: um vetor de uint64 (n multiplo de 8). Inicializado em paralelo com o mesmo schedule do teste,
        // para cada pagina ser alocada perto da thread que a le (first touch).
        const int64_t n_read = (total_bytes / 8) & ~int64_t(7);
        std::vector<uint64_t> a(n_read);
#pragma omp parallel for num_threads(T) schedule(static)
        for (int64_t i = 0; i < n_read; i++) a[i] = uint64_t(i);

        Result read;
        read.bytes = double(n_read) * 8;
        uint64_t sink = 0;
        for (int r = 0; r <= opt.repeats; r++) {
            const double t0 = wall_time();
            sink += read_kernel(a.data(), n_read, T);
            const double dt = wall_time() - t0;
            if (r > 0) read.times.push_back(dt);
        }
        std::vector<uint64_t>().swap(a);

        const int64_t n_triad = total_bytes / 3 / 8;
        std::vector<double> ta(n_triad), tb(n_triad), tc(n_triad);
#pragma omp parallel for num_threads(T) schedule(static)
        for (int64_t i = 0; i < n_triad; i++) {
            ta[i] = 0;
            tb[i] = 1;
            tc[i] = 2;
        }
        Result triad;
        triad.bytes = double(n_triad) * 8 * 3;
        for (int r = 0; r <= opt.repeats; r++) {
            const double t0 = wall_time();
            triad_kernel(ta.data(), tb.data(), tc.data(), n_triad, 3.0, T);
            const double dt = wall_time() - t0;
            if (r > 0) triad.times.push_back(dt);
        }
        if (ta[n_triad / 2] != 7.0 || sink == 42) std::printf("  (aviso: resultado inesperado do triad)\n");

        const std::string run_id = timestamp_id() + "_t" + std::to_string(T);
        CsvTable runs({"run_id", "timestamp", "host", "cpu_model", "test", "threads", "bytes", "repeats", "best_s",
                       "mean_s", "best_gbps", "mean_gbps", "omp_proc_bind", "omp_places"});
        for (auto [name, res] : {std::pair<const char*, Result*>{"read", &read}, {"triad", &triad}}) {
            const double best = *std::min_element(res->times.begin(), res->times.end());
            const double avg = mean(res->times);
            std::printf("  %-5s %8.1f GB/s (melhor) | %8.1f GB/s (media)\n", name, res->bytes / best / 1e9,
                        res->bytes / avg / 1e9);
            runs.add_row({run_id, timestamp_iso(), hostname(), cpu_model(), name, std::to_string(T),
                          fmt(res->bytes, 0), std::to_string(opt.repeats), fmt(best), fmt(avg),
                          fmt(res->bytes / best / 1e9, 2), fmt(res->bytes / avg / 1e9, 2),
                          env_or("OMP_PROC_BIND", "unset"), env_or("OMP_PLACES", "unset")});
        }
        make_dirs(opt.out_dir);
        runs.append(opt.out_dir + "/runs.csv");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "erro: %s\n", e.what());
        return 1;
    }
}
