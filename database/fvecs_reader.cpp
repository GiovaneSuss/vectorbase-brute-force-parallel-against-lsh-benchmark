#include "fvecs_reader.h"

#include <cstring>
#include <stdexcept>
#include <vector>

VecsReader::VecsReader(const std::string& path, VecsType type)
    : path_(path), type_(type), in_(path, std::ios::binary | std::ios::ate), elem_(type == VecsType::Byte ? 1 : 4) {
    if (!in_) throw std::runtime_error("nao consegui abrir " + path + " (rode `make download`)");
    const uint64_t file_size = in_.tellg();
    in_.seekg(0);

    int32_t dim;
    if (!in_.read(reinterpret_cast<char*>(&dim), sizeof(dim)) || dim <= 0)
        throw std::runtime_error(path + ": dimensao do primeiro vetor invalida");

    const uint64_t record = sizeof(int32_t) + uint64_t(dim) * elem_;
    if (file_size % record != 0)
        throw std::runtime_error(path + ": tamanho " + std::to_string(file_size) +
                                 " nao e multiplo do registro de " + std::to_string(record) + " bytes");
    n_ = int(file_size / record);
    d_ = dim;
    in_.seekg(0);
}

void VecsReader::read_prefix() {
    int32_t this_dim;
    in_.read(reinterpret_cast<char*>(&this_dim), sizeof(this_dim));
    if (!in_ || this_dim != d_)
        throw std::runtime_error(path_ + ": vetor " + std::to_string(next_) + " tem dimensao " +
                                 std::to_string(this_dim) + ", esperado " + std::to_string(d_));
}

void VecsReader::read_values(void* dst, uint64_t bytes) {
    in_.read(static_cast<char*>(dst), bytes);
    if (!in_) throw std::runtime_error(path_ + ": arquivo truncado no vetor " + std::to_string(next_));
}

void VecsReader::read_rows(int rows, float* out) {
    if (type_ == VecsType::Int) throw std::logic_error(path_ + ": .ivecs lido como float");
    if (rows > n_ - next_) throw std::logic_error(path_ + ": leitura alem do fim do arquivo");
    buf_.resize(size_t(d_) * elem_);
    for (int r = 0; r < rows; r++, next_++, out += d_) {
        read_prefix();
        if (type_ == VecsType::Float) {
            read_values(out, uint64_t(d_) * sizeof(float));
        } else {
            read_values(buf_.data(), d_);
            const unsigned char* b = reinterpret_cast<const unsigned char*>(buf_.data());
            for (int j = 0; j < d_; j++) out[j] = float(b[j]);
        }
    }
}

void VecsReader::read_rows(int rows, int* out, int keep) {
    if (type_ != VecsType::Int) throw std::logic_error(path_ + ": .fvecs/.bvecs lido como int");
    if (rows > n_ - next_) throw std::logic_error(path_ + ": leitura alem do fim do arquivo");
    if (keep <= 0 || keep > d_) keep = d_;
    buf_.resize(size_t(d_) * sizeof(int));
    for (int r = 0; r < rows; r++, next_++, out += keep) {
        read_prefix();
        read_values(buf_.data(), uint64_t(d_) * sizeof(int));
        std::memcpy(out, buf_.data(), size_t(keep) * sizeof(int));
    }
}

VectorDataset read_fvecs(const std::string& path) {
    VecsReader r(path, VecsType::Float);
    VectorDataset ds;
    ds.n = r.n();
    ds.d = r.d();
    ds.data.resize(uint64_t(ds.n) * ds.d); // uma unica alocacao para o dataset inteiro
    r.read_rows(ds.n, ds.data.data());
    return ds;
}

IntDataset read_ivecs(const std::string& path) {
    VecsReader r(path, VecsType::Int);
    IntDataset ds;
    ds.n = r.n();
    ds.d = r.d();
    ds.data.resize(uint64_t(ds.n) * ds.d);
    r.read_rows(ds.n, ds.data.data());
    return ds;
}
