#include "metrics.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

double wall_time() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

double unix_time() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

double thread_cpu_time() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

ProcessUsage process_usage() {
    rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    ProcessUsage u;
    u.user_s = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6;
    u.sys_s = ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6;
    u.max_rss_kb = ru.ru_maxrss;
    u.vol_ctx_switches = ru.ru_nvcsw;
    u.invol_ctx_switches = ru.ru_nivcsw;
    u.minor_faults = ru.ru_minflt;
    u.major_faults = ru.ru_majflt;
    return u;
}

// ---------------------------------------------------------------------------------------------
// RAPL
// ---------------------------------------------------------------------------------------------

namespace {

bool read_u64(const std::string& path, uint64_t& out) {
    std::ifstream f(path);
    return bool(f >> out);
}

} // namespace

EnergyMeter::EnergyMeter() {
    // Um dominio "package-N" por soquete: intel-rapl:0, intel-rapl:1, ...
    for (int i = 0; i < 16; i++) {
        std::string dir = "/sys/class/powercap/intel-rapl:" + std::to_string(i);
        struct stat st;
        if (stat(dir.c_str(), &st) != 0) break;
        Domain d;
        d.path = dir + "/energy_uj";
        uint64_t probe;
        if (!read_u64(d.path, probe)) {
            reason_ = d.path + " existe mas nao e legivel (em kernels recentes precisa de root: "
                               "`sudo chmod o+r " + d.path + "`)";
            domains_.clear();
            return;
        }
        read_u64(dir + "/max_energy_range_uj", d.max_range_uj);
        domains_.push_back(d);
    }
    if (domains_.empty() && reason_.empty())
        reason_ = "RAPL indisponivel (/sys/class/powercap/intel-rapl:* nao existe — comum em WSL/VMs)";
}

void EnergyMeter::start() {
    for (auto& d : domains_) read_u64(d.path, d.start_uj);
}

double EnergyMeter::stop_joules() {
    double total_uj = 0;
    for (auto& d : domains_) {
        uint64_t now = 0;
        read_u64(d.path, now);
        // O contador e cumulativo e "da a volta" ao atingir max_energy_range_uj.
        uint64_t delta = now >= d.start_uj ? now - d.start_uj : now + d.max_range_uj - d.start_uj;
        total_uj += double(delta);
    }
    return total_uj * 1e-6;
}

// ---------------------------------------------------------------------------------------------
// Identificadores
// ---------------------------------------------------------------------------------------------

namespace {

std::string strftime_now(const char* format) {
    std::time_t t = std::time(nullptr);
    std::tm tm;
    localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), format, &tm);
    return buf;
}

} // namespace

std::string timestamp_id() { return strftime_now("%Y%m%d-%H%M%S"); }
std::string timestamp_iso() { return strftime_now("%Y-%m-%dT%H:%M:%S"); }

std::string hostname() {
    char buf[256] = {0};
    gethostname(buf, sizeof(buf) - 1);
    return buf;
}

std::string cpu_model() {
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("model name", 0) == 0) {
            auto pos = line.find(':');
            return pos == std::string::npos ? line : line.substr(pos + 2);
        }
    return "desconhecido";
}

// ---------------------------------------------------------------------------------------------
// CSV
// ---------------------------------------------------------------------------------------------

namespace {

std::string csv_escape(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) out += (c == '"') ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

void write_rows(std::ostream& out, const std::vector<std::vector<std::string>>& rows) {
    for (const auto& row : rows) {
        for (size_t i = 0; i < row.size(); i++) out << (i ? "," : "") << csv_escape(row[i]);
        out << "\n";
    }
}

} // namespace

void CsvTable::write(const std::string& path) const {
    std::ofstream f(path, std::ios::trunc);
    if (!f) throw std::runtime_error("nao consegui criar " + path);
    write_rows(f, {header_});
    write_rows(f, rows_);
}

void CsvTable::append(const std::string& path) const {
    struct stat st;
    bool exists = stat(path.c_str(), &st) == 0 && st.st_size > 0;
    std::ofstream f(path, std::ios::app);
    if (!f) throw std::runtime_error("nao consegui abrir " + path);
    if (!exists) write_rows(f, {header_});
    write_rows(f, rows_);
}

std::string fmt(double v, int precision) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", precision, v);
    return buf;
}

void make_dirs(const std::string& path) {
    std::string cur;
    std::stringstream ss(path);
    std::string part;
    if (!path.empty() && path[0] == '/') cur = "/";
    while (std::getline(ss, part, '/')) {
        if (part.empty()) continue;
        cur += part + "/";
        if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
            throw std::runtime_error("mkdir " + cur + ": " + std::strerror(errno));
    }
}

std::string fmt_or_na(double v, int precision) { return std::isnan(v) ? "NA" : fmt(v, precision); }

std::string unique_run_dir(const std::string& base) {
    std::string dir = base;
    struct stat st;
    for (int i = 2; stat(dir.c_str(), &st) == 0; i++) dir = base + "-" + std::to_string(i);
    return dir;
}

std::string env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v ? v : fallback;
}

double mean(const std::vector<double>& v) {
    double s = 0;
    for (double x : v) s += x;
    return s / v.size();
}

double stddev(const std::vector<double>& v) {
    if (v.size() < 2) return 0;
    double m = mean(v), s = 0;
    for (double x : v) s += (x - m) * (x - m);
    return std::sqrt(s / (v.size() - 1));
}
