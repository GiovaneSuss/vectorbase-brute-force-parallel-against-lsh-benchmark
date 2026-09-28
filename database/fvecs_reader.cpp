#include "fvecs_reader.h"

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {

// T = float (.fvecs) ou int (.ivecs); ambos tem 4 bytes, entao o layout no arquivo e identico.
template <class T>
std::vector<T> read_vecs(const std::string& path, int& n, int& d) {
    static_assert(sizeof(T) == 4, "fvecs/ivecs usam valores de 4 bytes");

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("nao consegui abrir " + path + " (rode `make download`)");
    const uint64_t file_size = in.tellg();
    in.seekg(0);

    int32_t dim;
    if (!in.read(reinterpret_cast<char*>(&dim), sizeof(dim)) || dim <= 0)
        throw std::runtime_error(path + ": dimensao do primeiro vetor invalida");

    const uint64_t record = sizeof(int32_t) + uint64_t(dim) * sizeof(T);
    if (file_size % record != 0)
        throw std::runtime_error(path + ": tamanho " + std::to_string(file_size) +
                                 " nao e multiplo do registro de " + std::to_string(record) + " bytes");
    n = int(file_size / record);
    d = dim;

    std::vector<T> out(uint64_t(n) * d); // uma unica alocacao para o dataset inteiro
    in.seekg(0);
    for (int i = 0; i < n; i++) {
        int32_t this_dim;
        in.read(reinterpret_cast<char*>(&this_dim), sizeof(this_dim));
        if (!in || this_dim != d)
            throw std::runtime_error(path + ": vetor " + std::to_string(i) + " tem dimensao " +
                                     std::to_string(this_dim) + ", esperado " + std::to_string(d));
        in.read(reinterpret_cast<char*>(&out[uint64_t(i) * d]), uint64_t(d) * sizeof(T));
        if (!in) throw std::runtime_error(path + ": arquivo truncado no vetor " + std::to_string(i));
    }
    return out;
}

} // namespace

VectorDataset read_fvecs(const std::string& path) {
    VectorDataset ds;
    ds.data = read_vecs<float>(path, ds.n, ds.d);
    return ds;
}

IntDataset read_ivecs(const std::string& path) {
    IntDataset ds;
    ds.data = read_vecs<int>(path, ds.n, ds.d);
    return ds;
}
