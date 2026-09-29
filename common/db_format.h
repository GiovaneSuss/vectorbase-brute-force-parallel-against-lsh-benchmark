#pragma once

// Contrato binario do banco: o arquivo data/<dataset>.db e o segmento de memoria compartilhada
// /dev/shm/pcd_<dataset> tem EXATAMENTE o mesmo layout, byte a byte:
//
//   [DbHeader][padding ate 4096] [base: n_base*d floats] [pad] [queries: n_query*d floats] [pad]
//   [ground truth: n_query*gt_k ints] [pad]
//
// Cada secao comeca num offset alinhado a DB_ALIGN (pagina de 4 KiB) e e contigua em row-major,
// igual ao VectorDataset da especificacao: vetor i, dimensao j = base[i * d + j].

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

inline constexpr char DB_MAGIC[8] = {'P', 'C', 'D', 'V', 'D', 'B', '0', '1'};
inline constexpr uint32_t DB_VERSION = 1;
inline constexpr uint64_t DB_ALIGN = 4096;

struct DbHeader {
    char magic[8];         // DB_MAGIC; no segmento e escrito por ULTIMO (sinaliza "banco pronto")
    uint32_t version;      // DB_VERSION
    uint32_t reserved;
    char dataset[32];      // ex.: "sift1m"
    int32_t n_base;        // 1000000 no SIFT1M
    int32_t d;             // 128
    int32_t n_query;       // 10000
    int32_t gt_k;          // 100 vizinhos por query no ground truth
    uint64_t off_base;     // offsets em bytes a partir do inicio do arquivo/segmento
    uint64_t off_query;
    uint64_t off_gt;
    uint64_t total_size;   // tamanho total do arquivo/segmento
    uint64_t checksum;     // fnv1a64 sobre [off_base, total_size)
};
static_assert(sizeof(DbHeader) <= DB_ALIGN, "DbHeader precisa caber na primeira pagina");

inline uint64_t db_align_up(uint64_t x) { return (x + DB_ALIGN - 1) / DB_ALIGN * DB_ALIGN; }

// Nome do segmento POSIX (shm_open): "/pcd_sift1m" -> arquivo /dev/shm/pcd_sift1m
inline std::string db_segment_name(const std::string& dataset) { return "/pcd_" + dataset; }

// FNV-1a 64 bits aplicado a palavras de 64 bits (8x mais rapido que byte a byte; o payload tem
// tamanho multiplo de DB_ALIGN, entao e sempre multiplo de 8). Serve para detectar corrupcao.
inline uint64_t fnv1a64(const void* ptr, size_t len) {
    const unsigned char* p = static_cast<const unsigned char*>(ptr);
    uint64_t h = 1469598103934665603ULL;
    size_t words = len / 8;
    for (size_t i = 0; i < words; i++) {
        uint64_t w;
        std::memcpy(&w, p + i * 8, 8);
        h ^= w;
        h *= 1099511628211ULL;
    }
    for (size_t i = words * 8; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// Mesmo fnv1a64, mas alimentado em pedacos (o construct grava o banco em streaming, sem ter a imagem
// inteira na memoria). update(a); update(b); digest() == fnv1a64(a ++ b), para qualquer divisao em pedacos.
class Fnv1a64Stream {
public:
    void update(const void* ptr, size_t len) {
        const unsigned char* p = static_cast<const unsigned char*>(ptr);
        while (len > 0 && pending_ > 0) { // completa a palavra que ficou pela metade no pedaco anterior
            buf_[pending_++] = *p++;
            len--;
            if (pending_ == 8) mix_word(buf_), pending_ = 0;
        }
        for (; len >= 8; p += 8, len -= 8) mix_word(p);
        while (len > 0) buf_[pending_++] = *p++, len--;
    }
    uint64_t digest() const {
        uint64_t h = h_;
        for (size_t i = 0; i < pending_; i++) {
            h ^= buf_[i];
            h *= 1099511628211ULL;
        }
        return h;
    }

private:
    void mix_word(const unsigned char* p) {
        uint64_t w;
        std::memcpy(&w, p, 8);
        h_ ^= w;
        h_ *= 1099511628211ULL;
    }
    uint64_t h_ = 1469598103934665603ULL;
    unsigned char buf_[8];
    size_t pending_ = 0;
};

// Confere se o cabecalho e coerente com um arquivo/segmento de mapped_size bytes.
// Retorna string vazia se OK, ou a descricao do problema.
inline std::string validate_header(const DbHeader& h, uint64_t mapped_size) {
    if (std::memcmp(h.magic, DB_MAGIC, sizeof(DB_MAGIC)) != 0) return "magic invalido (nao e um banco PCD, ou ainda carregando)";
    if (h.version != DB_VERSION) return "versao do formato incompativel: " + std::to_string(h.version);
    if (h.n_base <= 0 || h.d <= 0 || h.n_query <= 0 || h.gt_k <= 0) return "dimensoes invalidas no cabecalho";
    if (h.total_size != mapped_size) return "tamanho no cabecalho (" + std::to_string(h.total_size) +
                                            ") difere do tamanho real (" + std::to_string(mapped_size) + ")";
    uint64_t base_bytes = uint64_t(h.n_base) * h.d * sizeof(float);
    uint64_t query_bytes = uint64_t(h.n_query) * h.d * sizeof(float);
    uint64_t gt_bytes = uint64_t(h.n_query) * h.gt_k * sizeof(int32_t);
    if (h.off_base != DB_ALIGN ||
        h.off_query != db_align_up(h.off_base + base_bytes) ||
        h.off_gt != db_align_up(h.off_query + query_bytes) ||
        h.total_size != db_align_up(h.off_gt + gt_bytes))
        return "offsets das secoes inconsistentes";
    return "";
}
