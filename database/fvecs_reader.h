#pragma once

// Parser dos formatos .fvecs (float), .bvecs (uint8) e .ivecs (int) do corpus TEXMEX / ann-benchmarks.
// Cada vetor no arquivo e [int32 d][d x valores], repetido; o prefixo de dimensao e conferido e
// descartado — so os valores vao para o array achatado (row-major, n * d).
//
// .bvecs e o formato do SIFT1B/BIGANN (de onde sai o SIFT10M): mesmos descritores SIFT, mas com 1 byte por
// valor. O leitor converte para float na hora, entao o banco tem o mesmo layout float32 para qualquer dataset.

#include <cstdint>
#include <fstream>
#include <string>

#include "vector_dataset.h"

enum class VecsType { Float, Byte, Int };

// Leitor em streaming: le blocos de linhas por vez, para datasets que nao cabem inteiros na RAM
// (o SIFT10M em float ocupa 5,1 GB). Lanca std::runtime_error se o arquivo nao existir, tiver tamanho
// inconsistente ou algum vetor com dimensao diferente do primeiro.
class VecsReader {
public:
    VecsReader(const std::string& path, VecsType type);

    int n() const { return n_; }
    int d() const { return d_; }
    int rows_read() const { return next_; }

    // Le as proximas `rows` linhas. Float/Byte -> out_f (rows * d floats); Int -> out_i, mantendo so as
    // primeiras `keep` colunas de cada linha (rows * keep ints; keep <= 0 = todas).
    void read_rows(int rows, float* out_f);
    void read_rows(int rows, int* out_i, int keep = 0);

private:
    void read_prefix();
    void read_values(void* dst, uint64_t bytes);

    std::string path_;
    VecsType type_;
    std::ifstream in_;
    int n_ = 0, d_ = 0, next_ = 0;
    int elem_ = 4; // bytes por valor no arquivo
    std::string buf_;
};

// Leitura do arquivo inteiro de uma vez (datasets pequenos).
VectorDataset read_fvecs(const std::string& path);
IntDataset read_ivecs(const std::string& path);
