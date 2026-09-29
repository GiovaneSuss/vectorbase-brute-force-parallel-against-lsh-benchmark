#include "shm_db.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

std::vector<std::string> list_online_dbs() {
    std::vector<std::string> out;
    const std::string prefix = db_segment_name("").substr(1); // "pcd_"
    if (DIR* dir = opendir("/dev/shm")) {
        while (const dirent* e = readdir(dir)) {
            const std::string name = e->d_name;
            if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0)
                out.push_back(name.substr(prefix.size()));
        }
        closedir(dir);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string resolve_dataset(const std::string& requested) {
    if (requested != "auto") return requested;
    const std::vector<std::string> online = list_online_dbs();
    if (online.empty())
        throw std::runtime_error("nenhum banco no ar — rode `make up-db-1m` ou `make up-db-10m` em outro terminal");
    if (online.size() > 1) {
        std::string names;
        for (const auto& n : online) names += (names.empty() ? "" : ", ") + n;
        throw std::runtime_error("ha mais de um banco no ar (" + names + ") — escolha com DATASET=<nome>, ex.: "
                                 "`make brute-force DATASET=" + online[0] + "`");
    }
    return online[0];
}

DbView attach_db(const std::string& dataset) {
    const std::string name = db_segment_name(dataset);
    int fd = shm_open(name.c_str(), O_RDONLY, 0);
    if (fd < 0) {
        if (errno == ENOENT)
            throw std::runtime_error("banco '" + dataset + "' nao esta no ar — rode `make up-db DATASET=" + dataset +
                                     "` em outro terminal");
        throw std::runtime_error("shm_open(" + name + "): " + std::strerror(errno));
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        close(fd);
        throw std::runtime_error("fstat(" + name + "): " + std::strerror(e));
    }
    if (size_t(st.st_size) < sizeof(DbHeader)) {
        close(fd);
        throw std::runtime_error("banco '" + dataset + "' ainda esta sendo carregado — tente de novo em instantes");
    }

    // MAP_POPULATE ja monta as tabelas de pagina de todo o segmento aqui, para que os page faults do primeiro
    // acesso nao caiam dentro do tempo medido das buscas.
    void* p = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED | MAP_POPULATE, fd, 0);
    int e = errno;
    close(fd); // o mapeamento continua valido sem o fd
    if (p == MAP_FAILED) throw std::runtime_error("mmap(" + name + "): " + std::strerror(e));

    const DbHeader* h = static_cast<const DbHeader*>(p);
    if (std::memcmp(h->magic, DB_MAGIC, sizeof(DB_MAGIC)) != 0) {
        munmap(p, st.st_size);
        throw std::runtime_error("banco '" + dataset + "' ainda esta sendo carregado — tente de novo em instantes");
    }
    std::string err = validate_header(*h, st.st_size);
    if (!err.empty()) {
        munmap(p, st.st_size);
        throw std::runtime_error("segmento " + name + " invalido: " + err);
    }

    const char* bytes = static_cast<const char*>(p);
    DbView v;
    v.hdr = h;
    v.base = reinterpret_cast<const float*>(bytes + h->off_base);
    v.queries = reinterpret_cast<const float*>(bytes + h->off_query);
    v.gt = reinterpret_cast<const int*>(bytes + h->off_gt);
    v.size = st.st_size;
    return v;
}

void detach_db(DbView& view) {
    if (view.hdr) munmap(const_cast<DbHeader*>(view.hdr), view.size);
    view = DbView{};
}
