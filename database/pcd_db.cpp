// pcd_db — ferramenta de gerenciamento do banco vetorial em memoria.
//
//   pcd_db construct <dataset>      .fvecs/.ivecs (data/) -> data/<dataset>.db, validando tudo
//   pcd_db up <dataset>             sobe o .db para /dev/shm/pcd_<dataset> (read-only) e fica no ar ate Ctrl+C
//   pcd_db down <dataset>           remove o segmento (se o `up` morreu sem limpar)
//   pcd_db status <dataset>         conecta read-only, confere checksum e mostra amostras
//   pcd_db test-readonly <dataset>  prova que o segmento nao aceita escrita
//
// Datasets: sift1m (data/sift/sift_*), sift10m (data/bigann/: 10M primeiros do SIFT1B), siftsmall
// (data/siftsmall/siftsmall_*).
// Diretorio de dados: data/ (ou variavel de ambiente PCD_DATA_DIR).

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include "db_format.h"
#include "fvecs_reader.h"
#include "shm_db.h"

namespace {

struct DatasetSpec {
    std::string name;       // nome do banco: "sift1m"
    std::string base_file;  // arquivos brutos, relativos a data/
    std::string query_file;
    std::string gt_file;
    VecsType base_type;     // .fvecs (float) ou .bvecs (uint8, BIGANN) — as queries usam o mesmo formato
    int n_base, d, n_query;
    int gt_k;               // vizinhos por query guardados no banco (o arquivo pode ter mais: sao truncados)
};

const DatasetSpec DATASETS[] = {
    {"sift1m", "sift/sift_base.fvecs", "sift/sift_query.fvecs", "sift/sift_groundtruth.ivecs", VecsType::Float,
     1000000, 128, 10000, 100},
    {"siftsmall", "siftsmall/siftsmall_base.fvecs", "siftsmall/siftsmall_query.fvecs",
     "siftsmall/siftsmall_groundtruth.ivecs", VecsType::Float, 10000, 128, 100, 100},
    // SIFT10M = 10M primeiros vetores do SIFT1B (BIGANN); o idx_10M.ivecs traz 1000 vizinhos/query, guardamos 100.
    {"sift10m", "bigann/bigann_base_10M.bvecs", "bigann/bigann_query.bvecs", "bigann/idx_10M.ivecs", VecsType::Byte,
     10000000, 128, 10000, 100},
};

const DatasetSpec& find_dataset(const std::string& name) {
    for (const auto& s : DATASETS)
        if (s.name == name) return s;
    throw std::runtime_error("dataset desconhecido '" + name + "' (use sift1m, sift10m ou siftsmall)");
}

std::string data_dir() {
    const char* env = std::getenv("PCD_DATA_DIR");
    return env ? env : "data";
}

std::string db_path(const DatasetSpec& s) { return data_dir() + "/" + s.name + ".db"; }

double mib(uint64_t bytes) { return bytes / (1024.0 * 1024.0); }

std::string sys_error(const std::string& what) { return what + ": " + std::strerror(errno); }

void print_samples(const DbHeader& h, const float* base_first, const float* base_last, const float* query0,
                   const int* gt0) {
    auto row = [&](const char* label, const float* v) {
        std::printf("  %-22s", label);
        for (int j = 0; j < 8; j++) std::printf(" %5.0f", v[j]);
        std::printf(" ...\n");
    };
    row("base[0][0..7]:", base_first);
    std::string last = "base[" + std::to_string(h.n_base - 1) + "][0..7]:";
    row(last.c_str(), base_last);
    row("query[0][0..7]:", query0);
    std::printf("  %-22s", "gt[0][0..7]:");
    for (int j = 0; j < 8; j++) std::printf(" %d", gt0[j]);
    std::printf(" ...\n");
}

// ---------------------------------------------------------------------------------------------
// construct
// ---------------------------------------------------------------------------------------------

// Descritores SIFT sao inteiros em [0, 255] guardados como float: qualquer coisa fora disso indica erro de
// parsing (ex.: prefixo de dimensao lido como dado). `first_row` e o indice do primeiro vetor do bloco.
void check_values(const float* v, int rows, int d, int first_row, const std::string& label) {
    for (uint64_t i = 0; i < uint64_t(rows) * d; i++) {
        float x = v[i];
        if (!std::isfinite(x) || x < 0.0f || x > 255.0f || x != std::floor(x))
            throw std::runtime_error(label + ": valor invalido " + std::to_string(x) + " no vetor " +
                                     std::to_string(first_row + i / d) + ", dimensao " + std::to_string(i % d));
    }
}

void expect(const std::string& label, int got_n, int got_d, int want_n, int want_d, bool at_least_d = false) {
    std::printf("  %-14s %8d x %-5d", label.c_str(), got_n, got_d);
    if (got_n != want_n || (at_least_d ? got_d < want_d : got_d != want_d))
        throw std::runtime_error(label + ": esperado " + std::to_string(want_n) + " x " +
                                 (at_least_d ? ">= " : "") + std::to_string(want_d));
    if (got_d != want_d) std::printf("OK (guardando so os %d primeiros)\n", want_d);
    else std::printf("OK\n");
}

// Grava o banco em streaming, bloco a bloco: le do arquivo bruto, valida, escreve no .db e alimenta o checksum.
// Assim a memoria usada e so a de um bloco (~32 MiB), e nao o banco inteiro — o SIFT10M tem 5,1 GB.
int cmd_construct(const DatasetSpec& s) {
    const std::string dir = data_dir() + "/";
    std::printf("[construct] lendo %s%s, %s%s e %s%s\n", dir.c_str(), s.base_file.c_str(), dir.c_str(),
                s.query_file.c_str(), dir.c_str(), s.gt_file.c_str());

    VecsReader base(dir + s.base_file, s.base_type);
    VecsReader query(dir + s.query_file, s.base_type);
    VecsReader gt(dir + s.gt_file, VecsType::Int);

    std::printf("[construct] validando dimensoes\n");
    expect("base", base.n(), base.d(), s.n_base, s.d);
    expect("queries", query.n(), query.d(), s.n_query, s.d);
    expect("ground truth", gt.n(), gt.d(), s.n_query, s.gt_k, true);

    DbHeader h{};
    std::memcpy(h.magic, DB_MAGIC, sizeof(DB_MAGIC));
    h.version = DB_VERSION;
    std::snprintf(h.dataset, sizeof(h.dataset), "%s", s.name.c_str());
    h.n_base = s.n_base;
    h.d = s.d;
    h.n_query = s.n_query;
    h.gt_k = s.gt_k;
    h.off_base = DB_ALIGN;
    h.off_query = db_align_up(h.off_base + uint64_t(h.n_base) * h.d * sizeof(float));
    h.off_gt = db_align_up(h.off_query + uint64_t(h.n_query) * h.d * sizeof(float));
    h.total_size = db_align_up(h.off_gt + uint64_t(h.n_query) * h.gt_k * sizeof(int));

    const std::string out = db_path(s);
    const std::string tmp = out + ".tmp";
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("nao consegui criar " + tmp);

    Fnv1a64Stream sum;
    uint64_t pos = 0;
    auto emit = [&](const void* p, uint64_t len) {
        f.write(static_cast<const char*>(p), len);
        if (!f) throw std::runtime_error("erro escrevendo " + tmp + " (disco cheio?)");
        if (pos >= h.off_base) sum.update(p, len);
        pos += len;
    };
    const std::vector<char> zeros(DB_ALIGN, 0); // padding zerado = checksum deterministico
    auto pad_to = [&](uint64_t off) {
        while (pos < off) emit(zeros.data(), std::min<uint64_t>(off - pos, zeros.size()));
    };
    pad_to(h.off_base); // cabecalho provisorio (zerado); o verdadeiro e escrito no fim, com o checksum

    const int CHUNK = 65536; // vetores por bloco: 65536 * 128 * 4 B = 32 MiB
    std::vector<float> fbuf(size_t(CHUNK) * s.d);
    std::vector<float> base_first(s.d), base_last(s.d), query0(s.d);
    std::vector<int> gt0(s.gt_k);

    auto copy_vectors = [&](VecsReader& r, const std::string& label, bool progress) {
        for (int done = 0; done < r.n();) {
            const int rows = std::min(CHUNK, r.n() - done);
            r.read_rows(rows, fbuf.data());
            check_values(fbuf.data(), rows, s.d, done, label);
            emit(fbuf.data(), uint64_t(rows) * s.d * sizeof(float));
            if (done == 0) std::copy_n(fbuf.data(), s.d, (label == "base" ? base_first : query0).data());
            done += rows;
            if (done == r.n() && label == "base")
                std::copy_n(fbuf.data() + size_t(rows - 1) * s.d, s.d, base_last.data());
            if (progress && (done / CHUNK) % 16 == 0 && done < r.n())
                std::printf("  %s: %d / %d vetores\n", label.c_str(), done, r.n()), std::fflush(stdout);
        }
    };

    std::printf("[construct] convertendo e validando a base (valores inteiros em [0,255])\n");
    copy_vectors(base, "base", s.n_base > 2000000);
    pad_to(h.off_query);
    copy_vectors(query, "queries", false);
    pad_to(h.off_gt);

    std::vector<int> ibuf(size_t(CHUNK) * s.gt_k);
    for (int done = 0; done < gt.n();) {
        const int rows = std::min(CHUNK, gt.n() - done);
        gt.read_rows(rows, ibuf.data(), s.gt_k);
        for (uint64_t i = 0; i < uint64_t(rows) * s.gt_k; i++)
            if (ibuf[i] < 0 || ibuf[i] >= s.n_base)
                throw std::runtime_error("ground truth: id " + std::to_string(ibuf[i]) + " fora de [0, " +
                                         std::to_string(s.n_base) + ") na query " + std::to_string(done + i / s.gt_k));
        if (done == 0) std::copy_n(ibuf.data(), s.gt_k, gt0.data());
        emit(ibuf.data(), uint64_t(rows) * s.gt_k * sizeof(int));
        done += rows;
    }
    pad_to(h.total_size);
    std::printf("  base/queries inteiros em [0,255], ids do ground truth em [0,%d)  OK\n", s.n_base);

    h.checksum = sum.digest();
    f.seekp(0);
    f.write(reinterpret_cast<const char*>(&h), sizeof(h));
    f.close();
    if (!f) throw std::runtime_error("erro finalizando " + tmp);
    if (std::rename(tmp.c_str(), out.c_str()) != 0) throw std::runtime_error(sys_error("rename " + tmp));

    std::printf("[construct] banco gravado em %s (%.1f MiB, checksum %016llx)\n", out.c_str(), mib(h.total_size),
                (unsigned long long)h.checksum);
    print_samples(h, base_first.data(), base_last.data(), query0.data(), gt0.data());
    return 0;
}

// ---------------------------------------------------------------------------------------------
// up / down
// ---------------------------------------------------------------------------------------------

int cmd_up(const DatasetSpec& s) {
    // Bloqueia os sinais de encerramento ANTES de criar o segmento: eles ficam pendentes e sao
    // consumidos pelo sigwait no fim, garantindo que o shm_unlink sempre roda.
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGHUP);
    sigprocmask(SIG_BLOCK, &sigs, nullptr);

    const std::string path = db_path(s);
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("nao encontrei " + path + " — rode `make construct-db DATASET=" + s.name + "`");
    DbHeader h;
    if (!in.read(reinterpret_cast<char*>(&h), sizeof(h))) throw std::runtime_error(path + ": cabecalho truncado");
    in.seekg(0, std::ios::end);
    const uint64_t file_size = in.tellg();
    std::string err = validate_header(h, file_size);
    if (!err.empty()) throw std::runtime_error(path + ": " + err + " — refaca com `make construct-db`");

    struct statvfs vfs;
    if (statvfs("/dev/shm", &vfs) == 0) {
        uint64_t avail = uint64_t(vfs.f_bavail) * vfs.f_frsize;
        if (avail < h.total_size)
            throw std::runtime_error("/dev/shm tem so " + std::to_string(uint64_t(mib(avail))) +
                                     " MiB livres, o banco precisa de " + std::to_string(uint64_t(mib(h.total_size))) +
                                     " MiB. Se houver RAM sobrando, aumente o tmpfs: "
                                     "`sudo mount -o remount,size=<N>G /dev/shm` (ou derrube outros bancos no ar)");
    }

    const std::string name = db_segment_name(s.name);
    int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        if (errno == EEXIST)
            throw std::runtime_error("o banco '" + s.name + "' ja esta no ar (/dev/shm" + name +
                                     "). Se nenhum `make up-db` estiver rodando, e resto de uma execucao anterior: "
                                     "rode `make down-db DATASET=" + s.name + "`");
        throw std::runtime_error(sys_error("shm_open " + name));
    }

    char* seg = nullptr;
    try {
        if (ftruncate(fd, h.total_size) != 0) throw std::runtime_error(sys_error("ftruncate"));
        void* p = mmap(nullptr, h.total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) throw std::runtime_error(sys_error("mmap"));
        seg = static_cast<char*>(p);

        std::printf("[up] carregando %s -> /dev/shm%s (%.1f MiB)\n", path.c_str(), name.c_str(), mib(h.total_size));
        // Le tudo exceto o cabecalho direto para o segmento; o cabecalho (com o magic) vai por ultimo,
        // entao um cliente que conectar no meio da carga ve "ainda carregando" em vez de dados parciais.
        in.seekg(sizeof(DbHeader));
        if (!in.read(seg + sizeof(DbHeader), h.total_size - sizeof(DbHeader)))
            throw std::runtime_error(path + ": erro lendo o payload");

        uint64_t sum = fnv1a64(seg + h.off_base, h.total_size - h.off_base);
        if (sum != h.checksum)
            throw std::runtime_error(path + ": checksum nao confere (arquivo corrompido?) — refaca com `make construct-db`");

        DbHeader no_magic = h;
        std::memset(no_magic.magic, 0, sizeof(no_magic.magic));
        std::memcpy(seg, &no_magic, sizeof(no_magic));
        __sync_synchronize();
        std::memcpy(seg, h.magic, sizeof(h.magic));

        // Sela como ROM: nem este processo nem nenhum outro (nao-root) consegue escrever daqui pra frente.
        if (mprotect(seg, h.total_size, PROT_READ) != 0) throw std::runtime_error(sys_error("mprotect"));
        if (fchmod(fd, 0444) != 0) throw std::runtime_error(sys_error("fchmod"));
    } catch (...) {
        if (seg) munmap(seg, h.total_size);
        close(fd);
        shm_unlink(name.c_str());
        throw;
    }
    close(fd);

    std::printf("[up] banco '%s' no ar (somente leitura)\n", s.name.c_str());
    std::printf("  base %d x %d | queries %d | ground truth k=%d | %.1f MiB | checksum %016llx\n", h.n_base, h.d,
                h.n_query, h.gt_k, mib(h.total_size), (unsigned long long)h.checksum);
    std::printf("  segmento: /dev/shm%s (permissao 0444)\n", name.c_str());
    std::printf("  abra outro terminal para rodar as buscas / `make status-db`. Ctrl+C derruba o banco.\n");
    std::fflush(stdout);

    int sig = 0;
    sigwait(&sigs, &sig);
    munmap(seg, h.total_size);
    shm_unlink(name.c_str());
    std::printf("\n[up] sinal %s recebido — banco '%s' derrubado\n", strsignal(sig), s.name.c_str());
    return 0;
}

int cmd_down(const DatasetSpec& s) {
    const std::string name = db_segment_name(s.name);
    if (shm_unlink(name.c_str()) != 0) {
        if (errno == ENOENT) {
            std::printf("[down] banco '%s' nao estava no ar\n", s.name.c_str());
            return 0;
        }
        throw std::runtime_error(sys_error("shm_unlink " + name));
    }
    std::printf("[down] segmento /dev/shm%s removido\n", name.c_str());
    std::printf("  (se um `make up-db` ainda estiver rodando, encerre-o com Ctrl+C)\n");
    return 0;
}

// ---------------------------------------------------------------------------------------------
// status / test-readonly
// ---------------------------------------------------------------------------------------------

bool verify_checksum(const DbView& v) {
    const char* bytes = reinterpret_cast<const char*>(v.hdr);
    return fnv1a64(bytes + v.hdr->off_base, v.size - v.hdr->off_base) == v.hdr->checksum;
}

int cmd_status(const DatasetSpec& s) {
    DbView v = attach_db(s.name);
    const DbHeader& h = *v.hdr;
    bool ok = verify_checksum(v);
    std::printf("[status] banco '%s' no ar em /dev/shm%s\n", h.dataset, db_segment_name(s.name).c_str());
    std::printf("  base %d x %d | queries %d | ground truth k=%d | %.1f MiB\n", h.n_base, h.d, h.n_query, h.gt_k,
                mib(h.total_size));
    std::printf("  checksum %016llx: %s\n", (unsigned long long)h.checksum, ok ? "OK" : "DIVERGENTE — banco corrompido!");
    print_samples(h, v.base, v.base + uint64_t(h.n_base - 1) * h.d, v.queries, v.gt);
    detach_db(v);
    return ok ? 0 : 1;
}

int cmd_test_readonly(const DatasetSpec& s) {
    const std::string name = db_segment_name(s.name);
    DbView v = attach_db(s.name);
    int failures = 0;
    auto report = [&](bool pass, const std::string& what) {
        std::printf("  [%s] %s\n", pass ? "PASS" : "FAIL", what.c_str());
        if (!pass) failures++;
    };
    std::printf("[test-readonly] banco '%s'\n", s.name.c_str());

    // 1) Ninguem consegue abrir o segmento para escrita (permissao 0444).
    errno = 0;
    int fd = shm_open(name.c_str(), O_RDWR, 0);
    if (fd >= 0) close(fd);
    report(fd < 0 && errno == EACCES, "shm_open(O_RDWR) recusado com EACCES" +
                                          std::string(fd >= 0 ? " (abriu! rodando como root?)" : ""));

    // 2) Um mapeamento read-only nao pode ser promovido para escrita.
    void* page = const_cast<DbHeader*>(v.hdr);
    errno = 0;
    int rc = mprotect(page, DB_ALIGN, PROT_READ | PROT_WRITE);
    report(rc != 0 && errno == EACCES, "mprotect(PROT_WRITE) no mapeamento recusado com EACCES");

    // 3) Escrita direta (burlando o const) derruba o processo com SIGSEGV em vez de corromper o banco.
    pid_t pid = fork();
    if (pid == 0) {
        float* evil = const_cast<float*>(v.base);
        evil[0] = 42.0f;
        _exit(0); // se chegou aqui, a escrita passou
    }
    int status = 0;
    waitpid(pid, &status, 0);
    report(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "escrita via const_cast morre com SIGSEGV");

    // 4) Depois de tudo isso, o conteudo continua intacto.
    report(verify_checksum(v), "checksum continua igual ao do construct-db");

    detach_db(v);
    std::printf("[test-readonly] %s\n", failures ? "FALHOU" : "banco protegido contra escrita");
    return failures ? 1 : 0;
}

int usage() {
    std::fprintf(stderr, "uso: pcd_db {construct|up|down|status|test-readonly} <sift1m|sift10m|siftsmall>\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) return usage();
    const std::string cmd = argv[1];
    try {
        const std::string dataset = argv[2];
        const DatasetSpec& s = find_dataset(dataset);
        if (cmd == "construct") return cmd_construct(s);
        if (cmd == "up") return cmd_up(s);
        if (cmd == "down") return cmd_down(s);
        if (cmd == "status") return cmd_status(s);
        if (cmd == "test-readonly") return cmd_test_readonly(s);
        return usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "erro: %s\n", e.what());
        return 1;
    }
}
