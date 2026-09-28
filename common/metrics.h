#pragma once

// Medicoes do protocolo experimental: relogio de parede, tempo de CPU (processo e por thread), energia via
// Intel RAPL (quando o sistema expoe) e escrita de CSV.

#include <cstdint>
#include <string>
#include <vector>

// Relogio de parede monotonico, em segundos.
double wall_time();
// Tempo de CPU consumido pela thread que chama (user + sys), em segundos.
double thread_cpu_time();

// Uso de recursos do processo inteiro (getrusage), para calcular deltas antes/depois da busca.
struct ProcessUsage {
    double user_s = 0, sys_s = 0;
    long max_rss_kb = 0;
    long vol_ctx_switches = 0, invol_ctx_switches = 0;
    long minor_faults = 0, major_faults = 0;
};
ProcessUsage process_usage();

// Energia do pacote da CPU via Intel RAPL (/sys/class/powercap/intel-rapl:N/energy_uj). Nao existe em WSL
// nem na maioria das VMs, e em kernels recentes o arquivo so e legivel por root — nesse caso available()
// retorna false e a energia vai como NA no CSV (o tempo de CPU fica como proxy, como a especificacao permite).
class EnergyMeter {
public:
    EnergyMeter();
    bool available() const { return !domains_.empty(); }
    std::string unavailable_reason() const { return reason_; }
    void start();
    double stop_joules(); // energia desde start(), somando todos os pacotes; trata o "wrap" do contador

private:
    struct Domain {
        std::string path;
        uint64_t max_range_uj = 0;
        uint64_t start_uj = 0;
    };
    std::vector<Domain> domains_;
    std::string reason_;
};

// Identificadores da execucao.
std::string timestamp_id();   // "20260928-161503" (hora local)
std::string timestamp_iso();  // "2026-09-28T16:15:03"
std::string hostname();
std::string cpu_model();

// Uma tabela CSV simples: header fixo + linhas de strings. write() cria o arquivo; append() cria com header
// se ainda nao existir, senao so acrescenta as linhas.
class CsvTable {
public:
    explicit CsvTable(std::vector<std::string> header) : header_(std::move(header)) {}
    void add_row(std::vector<std::string> row) { rows_.push_back(std::move(row)); }
    void write(const std::string& path) const;
    void append(const std::string& path) const;

private:
    std::vector<std::string> header_;
    std::vector<std::vector<std::string>> rows_;
};

std::string fmt(double v, int precision = 6);
void make_dirs(const std::string& path);
