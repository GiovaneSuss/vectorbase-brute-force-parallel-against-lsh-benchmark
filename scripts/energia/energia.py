#!/usr/bin/env python3
"""Energia das buscas no WSL, que nao tem RAPL em /sys/class/powercap.

O `make experimento` grava o inicio/fim de cada repeticao (summary.csv, colunas *_start_unix/*_end_unix),
de cada fase (fases.csv) e a diferenca de relogio WSL x Windows (relogio.csv). A energia vem do Windows, por
uma de duas fontes:

  1. RAPL pelo Windows (padrao): energia acumulada do pacote da CPU gravada pelo scripts/energia/amostrador.ps1
     em energia/windows_rapl.csv durante o experimento. A energia de um intervalo e a diferenca do contador
     acumulado (interpolado) entre o fim e o inicio.
  2. Log do HWiNFO64 (--hwinfo), se o Windows nao tiver o contador "Energy Meter": integra a coluna
     "CPU Package Power" no intervalo (regra do trapezio).

Nos dois casos desconta a potencia ociosa medida na janela "ocioso" do inicio do experimento.

Uso: python3 scripts/energia/energia.py --results results/<maquina> [--hwinfo <log.CSV>]
     [--coluna "CPU Package Power [W]"] [--formato-data %d.%m.%Y]

Saidas em results/<maquina>/energia/:
  repeticoes.csv  uma linha por repeticao (busca e, no LSH, indexacao)
  execucoes.csv   media por execucao (run_id), com J/query e potencia media, junto dos dados da execucao
"""
import argparse, bisect, csv, glob, io, os, statistics, sys
from datetime import datetime, timedelta, timezone

DATE_FORMATS = ["%d.%m.%Y", "%m/%d/%Y", "%d/%m/%Y", "%Y-%m-%d", "%m.%d.%Y"]
TIME_FORMATS = ["%H:%M:%S.%f", "%H:%M:%S"]


def read_text(path):
    raw = open(path, "rb").read()
    for enc in ("utf-8-sig", "utf-16", "cp1252", "latin-1"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    raise SystemExit(f"nao consegui ler {path}")


def parse_stamp(date, time, date_fmt):
    fmts = [date_fmt] if date_fmt else DATE_FORMATS
    for df in fmts:
        for tf in TIME_FORMATS:
            try:
                return datetime.strptime(f"{date.strip()} {time.strip()}", f"{df} {tf}"), df
            except ValueError:
                pass
    return None, None


def load_hwinfo(path, column, date_fmt, utc_offset_min, skew_s):
    """Retorna (tempos em unix do WSL, potencias em W), ordenados."""
    rows = list(csv.reader(io.StringIO(read_text(path))))
    header = rows[0]
    try:
        di, ti = header.index("Date"), header.index("Time")
    except ValueError:
        raise SystemExit("o log nao tem as colunas Date/Time — e um CSV de log do HWiNFO?")
    if column:
        cands = [i for i, h in enumerate(header) if h.strip() == column]
    else:
        cands = [i for i, h in enumerate(header) if "package power" in h.lower() and "[w]" in h.lower()]
    if not cands:
        power_cols = [h for h in header if "[W]" in h]
        raise SystemExit("nao achei a coluna de potencia do pacote da CPU. Colunas em W no log:\n  " +
                         "\n  ".join(power_cols) + "\nescolha uma com --coluna")
    pi = cands[0]
    print(f"coluna de potencia: {header[pi]!r}")
    ts, ws, fmt = [], [], date_fmt
    for r in rows[1:]:
        if len(r) <= max(di, ti, pi):
            continue
        stamp, used = parse_stamp(r[di], r[ti], fmt)
        if stamp is None:
            continue  # as ultimas linhas do HWiNFO repetem o cabecalho
        fmt = used
        try:
            w = float(r[pi].replace(",", "."))
        except ValueError:
            continue
        # hora local do Windows -> UTC -> relogio do WSL (o do Windows adiantado em skew_s)
        utc = stamp.replace(tzinfo=timezone(timedelta(minutes=utc_offset_min)))
        ts.append(utc.timestamp() - skew_s)
        ws.append(w)
    if len(ts) < 2:
        raise SystemExit("log do HWiNFO sem amostras legiveis (confira --formato-data)")
    order = sorted(range(len(ts)), key=ts.__getitem__)
    return [ts[i] for i in order], [ws[i] for i in order]


def load_windows_rapl(path, skew_s):
    """Retorna (tempos em unix do WSL, energia acumulada do pacote em J)."""
    ts, es = [], []
    for r in read_csv(path):
        try:
            ts.append(float(r["windows_unix_ms"]) / 1000 - skew_s)
            es.append(float(r["pkg_energy_pwh"]) * 3.6e-9)  # picowatt-hora -> joule
        except (KeyError, ValueError):
            continue
    if len(ts) < 2:
        raise SystemExit(f"{path} sem amostras")
    return ts, es


def power_at(ts, ws, t):
    i = bisect.bisect_left(ts, t)
    if i == 0:
        return ws[0]
    if i >= len(ts):
        return ws[-1]
    t0, t1 = ts[i - 1], ts[i]
    return ws[i - 1] + (ws[i] - ws[i - 1]) * (t - t0) / (t1 - t0)


def integrate(ts, ws, a, b):
    """Potencia (W) -> energia (J) entre a e b, interpolando nas bordas; (joules, amostras no intervalo)."""
    if b <= a or a < ts[0] or b > ts[-1]:
        return None, 0
    lo, hi = bisect.bisect_right(ts, a), bisect.bisect_left(ts, b)
    pts = [(a, power_at(ts, ws, a))] + [(ts[i], ws[i]) for i in range(lo, hi)] + [(b, power_at(ts, ws, b))]
    e = sum((t1 - t0) * (p0 + p1) / 2 for (t0, p0), (t1, p1) in zip(pts, pts[1:]))
    return e, hi - lo


def cumulative(ts, es, a, b):
    """Energia acumulada (J) -> energia entre a e b; (joules, amostras no intervalo)."""
    if b <= a or a < ts[0] or b > ts[-1]:
        return None, 0
    return power_at(ts, es, b) - power_at(ts, es, a), bisect.bisect_left(ts, b) - bisect.bisect_right(ts, a)


def read_csv(path):
    with open(path) as f:
        return list(csv.DictReader(f))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", required=True)
    ap.add_argument("--hwinfo", default=None, help="log do HWiNFO (so se nao houver energia/windows_rapl.csv)")
    ap.add_argument("--coluna", default=None, help="nome exato da coluna de potencia no log")
    ap.add_argument("--formato-data", default=None, help="formato strptime da coluna Date (ex.: %%d.%%m.%%Y)")
    a = ap.parse_args()
    rapl_csv = os.path.join(a.results, "energia", "windows_rapl.csv")
    if not a.hwinfo and not os.path.exists(rapl_csv):
        raise SystemExit(f"nao ha {rapl_csv} (o Windows nao tinha o contador Energy Meter?). Passe o log do "
                         "HWiNFO: make energia MAQUINA=<maquina> HWINFO=/mnt/c/caminho/log.CSV")
    if a.hwinfo and not os.path.exists(a.hwinfo):
        raise SystemExit(f"nao encontrei {a.hwinfo}")

    offset_min, skew_s = None, 0.0
    rel = os.path.join(a.results, "relogio.csv")
    if os.path.exists(rel):
        r = read_csv(rel)
        try:
            offset_min = float(r[0]["windows_utc_offset_min"])
            skews = [(float(x["windows_unix_ms"]) - float(x["wsl_unix_ms"])) / 1000 for x in r]
            skew_s = statistics.mean(skews)
            print(f"relogio: Windows UTC{offset_min / 60:+.1f}h, Windows - WSL = {skew_s * 1000:.0f} ms "
                  f"(variou {1000 * (max(skews) - min(skews)):.0f} ms durante o experimento)")
        except (KeyError, ValueError, IndexError):
            pass
    if offset_min is None:
        offset_min = datetime.now().astimezone().utcoffset().total_seconds() / 60
        print(f"relogio.csv ausente: usando o fuso local do Linux (UTC{offset_min / 60:+.1f}h) e sem correcao de relogio")

    if a.hwinfo:
        print(f"fonte: log do HWiNFO {a.hwinfo}")
        ts, ws = load_hwinfo(a.hwinfo, a.coluna, a.formato_data, offset_min, skew_s)
        energy = lambda s, e: integrate(ts, ws, s, e)
    else:
        print(f"fonte: RAPL pelo Windows ({rapl_csv})")
        ts, ws = load_windows_rapl(rapl_csv, skew_s)
        energy = lambda s, e: cumulative(ts, ws, s, e)
    dt = statistics.median(b - a for a, b in zip(ts, ts[1:]))
    print(f"log: {len(ts)} amostras, intervalo mediano {dt * 1000:.0f} ms, "
          f"{datetime.fromtimestamp(ts[0]):%H:%M:%S} a {datetime.fromtimestamp(ts[-1]):%H:%M:%S} (hora do Linux)")

    idle_w = None
    for f in read_csv(os.path.join(a.results, "fases.csv")) if os.path.exists(os.path.join(a.results, "fases.csv")) else []:
        if f["fase"] == "ocioso":
            s, e = float(f["start_unix"]), float(f["end_unix"])
            m = min(2.0, 0.1 * (e - s))  # margem nas bordas (a carga de antes/depois vaza na amostragem)
            s, e = s + m, e - m
            j, n = energy(s, e)
            if j is not None and n >= 2:
                idle_w = j / (e - s)
    print(f"potencia ociosa: {idle_w:.2f} W" if idle_w is not None else
          "potencia ociosa: a janela 'ocioso' nao esta coberta pelo log — energia dinamica fica NA")

    out_rows, per_run = [], {}
    for algo in ("brute-force", "lsh"):
        runs = {}
        rp = os.path.join(a.results, algo, "runs.csv")
        if os.path.exists(rp):
            runs = {r["run_id"]: r for r in read_csv(rp)}
        for summ in sorted(glob.glob(os.path.join(a.results, algo, "*", "summary.csv"))):
            for r in read_csv(summ):
                run = runs.get(r["run_id"], {})
                phases = [("busca", "search_start_unix", "search_end_unix")]
                if algo == "lsh":
                    phases.append(("indexacao", "index_start_unix", "index_end_unix"))
                for phase, sk, ek in phases:
                    if sk not in r:
                        continue
                    s, e = float(r[sk]), float(r[ek])
                    j, n = energy(s, e)
                    dyn = None if j is None or idle_w is None else j - idle_w * (e - s)
                    row = {"run_id": r["run_id"], "algorithm": run.get("algorithm", algo),
                           "dataset": run.get("dataset", ""), "threads": run.get("threads", ""),
                           "config": f'{run["tables"]}:{run["hashes"]}:{float(run["width"]):g}' if "tables" in run else "",
                           "fase": phase, "repeat": r["repeat"], "duracao_s": f"{e - s:.3f}", "amostras": n,
                           "energia_j": "NA" if j is None else f"{j:.3f}",
                           "potencia_media_w": "NA" if j is None else f"{j / (e - s):.3f}",
                           "potencia_ociosa_w": "NA" if idle_w is None else f"{idle_w:.3f}",
                           "energia_dinamica_j": "NA" if dyn is None else f"{dyn:.3f}"}
                    out_rows.append(row)
                    per_run.setdefault((r["run_id"], phase), []).append((row, run))

    if not out_rows:
        raise SystemExit("nenhum summary.csv com horarios encontrado em " + a.results)
    od = os.path.join(a.results, "energia")
    os.makedirs(od, exist_ok=True)
    with open(os.path.join(od, "repeticoes.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(out_rows[0]))
        w.writeheader()
        w.writerows(out_rows)

    agg, poucas = [], 0
    for (run_id, phase), items in per_run.items():
        run = items[0][1]
        vals = [float(x["energia_j"]) for x, _ in items if x["energia_j"] != "NA"]
        dyn = [float(x["energia_dinamica_j"]) for x, _ in items if x["energia_dinamica_j"] != "NA"]
        dur = statistics.mean(float(x["duracao_s"]) for x, _ in items)
        amostras = min(int(x["amostras"]) for x, _ in items)
        poucas += amostras < 5
        q = float(run.get("queries") or 0)
        agg.append({"run_id": run_id, "algorithm": items[0][0]["algorithm"], "dataset": items[0][0]["dataset"],
                    "threads": items[0][0]["threads"], "config": items[0][0]["config"], "fase": phase,
                    "repeticoes": len(items), "min_amostras": amostras, "duracao_s_mean": f"{dur:.3f}",
                    "energia_j_mean": f"{statistics.mean(vals):.3f}" if vals else "NA",
                    "energia_j_std": f"{statistics.stdev(vals):.3f}" if len(vals) > 1 else "NA",
                    "potencia_media_w": f"{statistics.mean(vals) / dur:.3f}" if vals else "NA",
                    "energia_dinamica_j_mean": f"{statistics.mean(dyn):.3f}" if dyn else "NA",
                    "mj_por_query": f"{statistics.mean(vals) * 1e3 / q:.4f}" if vals and q and phase == "busca" else "NA"})
    agg.sort(key=lambda r: (r["dataset"], r["algorithm"], r["config"], int(r["threads"] or 0), r["fase"]))
    with open(os.path.join(od, "execucoes.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(agg[0]))
        w.writeheader()
        w.writerows(agg)
    fora = sum(1 for r in out_rows if r["energia_j"] == "NA")
    print(f"\n{len(out_rows)} intervalos ({fora} fora do log) -> {od}/repeticoes.csv e execucoes.csv")
    if poucas:
        print(f"aviso: {poucas} execucao(oes) com menos de 5 amostras de energia dentro do intervalo — a borda "
              "pesa mais nessas (no HWiNFO, diminua o intervalo de log para 100-250 ms)")


if __name__ == "__main__":
    main()
