#pragma once

// API de cliente do banco em memoria compartilhada. Os programas de busca (brute-force, LSH) usam
// attach_db() para "se plugar" no banco que o `make up-db` deixou no ar, sem recarregar nada do disco.
//
// O mapeamento e somente leitura (PROT_READ sobre um fd aberto com O_RDONLY): os ponteiros sao const,
// e qualquer tentativa de escrita (mesmo via const_cast) mata o processo com SIGSEGV em vez de
// corromper o banco.

#include <cstddef>
#include <string>

#include "db_format.h"

struct DbView {
    const DbHeader* hdr = nullptr;
    const float* base = nullptr;     // n_base * d, row-major
    const float* queries = nullptr;  // n_query * d, row-major
    const int* gt = nullptr;         // n_query * gt_k (ids dos vizinhos corretos, em ordem)
    size_t size = 0;                 // bytes mapeados
};

// Lanca std::runtime_error com mensagem clara se o banco nao estiver no ar ou for invalido.
DbView attach_db(const std::string& dataset);
void detach_db(DbView& view);
