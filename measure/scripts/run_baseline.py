"""KaxWav measurement harness - one command for docs/MEASUREMENT-PROTOCOL.md (M1-M8).

    python measure/scripts/run_baseline.py                 # everything, asks for the physical setup
    python measure/scripts/run_baseline.py --steps m2,m5   # subset (results merge into the same run)
    python measure/scripts/run_baseline.py --steps assemble # only rebuild the baseline JSON
    python measure/scripts/run_baseline.py --cond gain_ch1="12h" --yes   # non-interactive

Each step writes measure/out/<date>-<commit>/<step>.json; after every invocation all step files of
the run are merged into measure/baselines/<date>-<commit>.json. Registry values the driver reads
(HKCU\\Software\\BehringerUM2\\ASIO DriverMode/BufferSize) are restored at the end.
Requires the Release build (build/Release): BehringerUM2ASIODriver.dll, um2_asio_probe.exe,
um2_drift.exe, raw_capture_test.exe.
"""
import argparse
import datetime as dt
import hashlib
import json
import platform
import re
import subprocess
import sys
import time
import winreg
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import um2_analysis as A  # noqa: E402
import um2_ipc  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
BIN = REPO / "build" / "Release"
DLL = BIN / "BehringerUM2ASIODriver.dll"
PROBE = BIN / "um2_asio_probe.exe"
DRIFT = BIN / "um2_drift.exe"
RAWCAP = BIN / "raw_capture_test.exe"
REG = r"Software\BehringerUM2\ASIO"
ALL_STEPS = ["m8", "m2", "m5", "m7", "m1", "m3", "m6"]
MODES = {1: "SharedFlex", 0: "ExclusivePro"}
BUFFERS = [64, 128, 256]

CONDITION_PROMPTS = {
    "gain_ch1": "Posicao do knob GAIN do canal 1 (ex.: 'minimo', '9h', '12h')",
    "gain_ch2": "Posicao do knob GAIN do canal 2 / INST",
    "output_knob": "Posicao do knob OUTPUT/PHONES",
    "monitor_switch": "Chave DIRECT MONITOR (on/off)",
}


def sh(cmd, timeout=None):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=REPO, errors="replace")
    return r.returncode, r.stdout, r.stderr


def git(*a):
    return sh(["git", *a])[1].strip()


def md5(p):
    return hashlib.md5(Path(p).read_bytes()).hexdigest().upper() if Path(p).exists() else None


def ask(msg, args):
    if args.yes:
        print(f"[auto] {msg}")
        return ""
    return input(f"\n>>> {msg}\n    [Enter quando pronto] ")


def reg_read():
    out = {}
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REG) as k:
            for name in ("DriverMode", "BufferSize"):
                try:
                    out[name] = winreg.QueryValueEx(k, name)[0]
                except FileNotFoundError:
                    out[name] = None
    except FileNotFoundError:
        pass
    return out


def reg_restore(vals):
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, REG) as k:
        for name, v in vals.items():
            if v is None:
                try:
                    winreg.DeleteValue(k, name)
                except FileNotFoundError:
                    pass
            else:
                winreg.SetValueEx(k, name, 0, winreg.REG_DWORD, int(v))


def probe(mode, buffer, seconds, rundir, tag, stim=None, stim_at=0, taps=False, poll_telemetry=False):
    rec = rundir / f"{tag}.i16"
    cmd = [str(PROBE), "--dll", str(DLL), "--mode", str(mode), "--buffer", str(buffer),
           "--seconds", str(seconds), "--rec", str(rec)]
    if stim is not None:
        sp = rundir / f"{tag}.stim.i16"
        A.write_i16(sp, stim)
        cmd += ["--stim", str(sp), "--stim-at", str(stim_at)]
    if taps:
        cmd += ["--tap-capture", str(rundir / f"{tag}.tapcap.f32"), "--tap-loopback", str(rundir / f"{tag}.taploop.f32"),
                "--tap-raw", "1" if taps == "raw" else "0"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=REPO, errors="replace")
    last = None
    if poll_telemetry:
        while p.poll() is None:
            t = um2_ipc.read_telemetry()
            if t is not None and t.ownerPid == p.pid:
                last = t
            time.sleep(0.5)
    out, err = p.communicate(timeout=seconds + 60)
    try:
        j = json.loads(out.strip().splitlines()[-1])
    except Exception:
        j = {"ok": False, "error": "probe output", "stdout": out[-2000:], "stderr": err[-2000:]}
    j["_rec"] = str(rec)
    if poll_telemetry:
        j["_telemetry"] = um2_ipc.summarize_telemetry(last)
    (rundir / f"{tag}.probe.json").write_text(json.dumps(j, indent=1, default=str), encoding="utf-8")
    return j


def endpoint_capture_level():
    """Capture endpoint volume as raw_capture_test reports it (protocol: must be 0.0 dB)."""
    rc, out, _ = sh([str(RAWCAP), "1", "x", "info"], timeout=60)
    m = re.search(r"atual\s+(-?[\d.]+) dB \(scalar ([\d.]+)\)", out)
    return {"db": float(m.group(1)), "scalar": float(m.group(2))} if m else {"raw": out.strip()[-200:]}


def rec_channel(j, ch):
    x = A.load_i16(j["_rec"], 2)
    return x[:, ch].astype(np.float64) / 32768.0


# ---------------------------------------------------------------- steps
def step_m8(args, rundir, cond):
    _, prov, _ = sh(["logman", "query", "providers"])
    providers = [l.strip() for l in prov.splitlines() if "audio" in l.lower()]
    _, prof, _ = sh(["wpr", "-profiles"])
    profiles = [l.strip() for l in prof.splitlines() if re.match(r"^\s+\w+\s{2,}", l)]
    audio_profiles = [p for p in profiles if "audio" in p.lower()]
    return {"how": "logman query providers | findstr /i audio ; wpr -profiles (descoberto nesta maquina)",
            "audio_providers": providers, "audio_profiles": audio_profiles, "all_profiles_count": len(profiles),
            "trace_run": None,
            "note": "Descoberta apenas. Trace com o perfil acima fica para a rodada que medir glitches (M8 completo)."}


def step_m2(args, rundir, cond):
    ask("M2: desconecte TUDO das duas entradas (XLR/P10 e INST). Deixe os knobs GAIN onde ficam no uso normal "
        "e informe a posicao (--cond gain_ch1=..., gain_ch2=...).", args)
    secs = args.m2_seconds
    prefix = rundir / "m2"
    reuse = args.m2_reuse and Path(f"{prefix}_apo.wav").exists() and Path(f"{prefix}_raw.wav").exists()
    if reuse:  # analyse captures already taken in this run; endpoint info only (3rd arg skips capture)
        rc, out, err = sh([str(RAWCAP), "1", str(prefix), "info"], timeout=60)
    else:
        rc, out, err = sh([str(RAWCAP), str(secs), str(prefix)], timeout=secs * 2 + 60)
    res = {"tool": "tools/raw_capture_test.exe (shared default/APO e shared RAW, float WAV)",
           "seconds": secs, "input": cond.get("input_connection", "nada ligado"),
           "reused_captures": bool(reuse), "raw_capture_test_stdout": out.strip().splitlines()}
    for kind in ("apo", "raw"):
        wav = Path(f"{prefix}_{kind}.wav")
        if not wav.exists():
            res[kind] = {"error": "wav ausente"}
            continue
        sr, x = A.read_wav_float(wav)
        x = x[int(0.3 * sr):]
        if x.ndim == 1:
            x = x[:, None]
        r = {"sr": sr, "channels": int(x.shape[1]), "ch1_mic": A.noise_floor(x[:, 0], sr)}
        if x.shape[1] > 1:
            r["ch2_inst"] = A.noise_floor(x[:, 1], sr)
        else:
            r["note"] = "mix format do stream tem 1 canal: so o canal 1 aparece nesta captura"
        res[kind] = r
    return res


def cpu_burners(n):
    """N busy-loop processes at normal priority (synthetic load for M5)."""
    return [subprocess.Popen([sys.executable, "-c", "while True: pass"]) for _ in range(n)]


def step_m5(args, rundir, cond):
    secs = args.m5_seconds
    runs = []
    burners = cpu_burners(args.cpu_load)
    try:
        return _m5_runs(args, rundir, cond, secs, runs)
    finally:
        for b in burners:
            b.kill()


def _m5_runs(args, rundir, cond, secs, runs):
    for mode in [int(m) for m in args.m5_modes.split(",")]:
        for buf in BUFFERS:
            before = (um2_ipc.read_shared_state(["xruns"]) or {}).get("xruns")
            j = probe(mode, buf, secs, rundir, f"m5_{MODES[mode]}_{buf}", poll_telemetry=True)
            after = (um2_ipc.read_shared_state(["xruns"]) or {}).get("xruns")
            r = {"mode_requested": MODES[mode], "mode_active": j.get("mode_active"), "buffer": buf, "seconds": secs,
                 "ok": j.get("ok"), "pdc_in": j.get("input_latency"), "pdc_out": j.get("output_latency"),
                 "callbacks": j.get("blocks"), "expected_callbacks": round(secs * 48000 / buf),
                 "telemetry": j.get("_telemetry"),
                 "shared_state_xruns_delta": (after - before) if None not in (before, after) else None,
                 "time_info": j.get("time_info"), "driver_log": j.get("driver_log")}
            # the same recording doubles as ASIO-path noise spectrum (7 kHz step detector)
            try:
                x = rec_channel(j, 0)[48000:]
                f, p = A.psd(x)
                r["asio_input_ch1_noise"] = {"rms_dbfs": round(A.dbfs(x - x.mean()), 2), "step_7k": A.step_7k(f, p)}
            except Exception as e:  # noqa: BLE001
                r["asio_input_ch1_noise"] = {"error": str(e)}
            runs.append(r)
            print(f"  M5 {MODES[mode]}@{buf}: active={r['mode_active']} misses="
                  f"{(r['telemetry'] or {}).get('deadline_misses')} p99={(r['telemetry'] or {}).get('block_us_p99')}us")
    return {"load": (f"{args.cpu_load} processos em busy-loop (prioridade normal)" if args.cpu_load
                     else cond.get("cpu_load", "nenhuma (ociosa)")),
            "definition": "duracao do bloco = QPC na entrada de processOneAsioBlock ate o fim do estagio de saida; "
                          "miss = duracao > bufferSize/fs. Contador xruns do UM2SharedState e incrementado por amostra "
                          "faltante no FIFO de captura (nao por evento).",
            "runs": runs}


def step_m7(args, rundir, cond):
    stim = A.to_i16(A.pseudo_noise(3.0, -20.0))
    at = 24000
    saved = rundir / "m7_SharedFlex_256.probe.json"
    if args.m7_reuse and saved.exists():  # reanalyse the capture already taken in this run
        j = json.loads(saved.read_text(encoding="utf-8"))
    else:
        j = probe(1, 256, 4.5, rundir, "m7_SharedFlex_256", stim=stim, stim_at=at, taps="raw" if args.m7_tap_raw else True)
    res = {"mode_requested": "SharedFlex", "mode_active": j.get("mode_active"), "buffer": 256,
           "tap_capture_raw": bool(args.m7_tap_raw),
           "stimulus": "ruido pseudo-aleatorio -20 dBFS, 3 s, int16, nas duas saidas",
           "taps": {"capture": j.get("tap_capture"), "loopback": j.get("tap_loopback")}}
    # output path: int16 the host wrote vs what the engine got (WASAPI loopback of the UM2 render endpoint)
    lp = Path(rundir / "m7_SharedFlex_256.taploop.f32")
    tl = j.get("tap_loopback") or {}
    if lp.exists() and tl.get("channels"):
        loop = A.load_f32(lp, tl["channels"])
        loop = loop if loop.ndim == 1 else loop[:, 0]
        res["output_null"] = A.null_test(stim, loop, scale=32768.0)
    # input path: what the engine delivered (shared capture tap) vs what the DAW received (ASIO in 1)
    cp = Path(rundir / "m7_SharedFlex_256.tapcap.f32")
    tc = j.get("tap_capture") or {}
    if cp.exists() and tc.get("channels"):
        caps = A.load_f32(cp, tc["channels"])
        if caps.ndim == 1:  # RAW capture of a mono device format
            caps = caps[:, None]
        asio = A.load_i16(j["_rec"], 2)
        res["input_null"] = {}
        for c in range(min(2, caps.shape[1])):
            cap = caps[:, c]
            r = A.null_test(asio[24000:, c], cap, scale=32767.0)
            r["note"] = (f"referencia = ASIO in {c + 1} (int16 entregue a DAW); captura = tap shared canal {c + 1} "
                         "x 32767 (escala do Quantize16)")
            r["input_level_dbfs"] = round(A.dbfs(cap), 1)
            r["asio_level_dbfs"] = round(A.dbfs(asio[24000:, c] / 32768.0), 1)
            res["input_null"][f"ch{c + 1}"] = r
    res["exclusive_pro"] = ("nao medivel: em exclusive nao existe tap digital do lado WASAPI (loopback e captura "
                            "compartilhada nao enxergam stream exclusivo)")
    res["driver_log"] = j.get("driver_log")
    return res


def level_check(args, rundir):
    j = probe(1, 256, 3.0, rundir, "m1_level", stim=A.to_i16(A.tone(1000, 2.0, -12)), stim_at=24000)
    x = A.load_i16(j["_rec"], 2).astype(np.float64) / 32768
    peaks = [round(20 * np.log10(np.max(np.abs(x[30000:, c])) + 1e-12), 1) for c in (0, 1)]
    ch = int(np.argmax(peaks))
    return ch, peaks


def step_m1(args, rundir, cond):
    ask("M1: ligue o loopback (saida da UM2 -> entrada) e DESLIGUE o DIRECT MONITOR para nao realimentar.", args)
    ch, peaks = level_check(args, rundir)
    res = {"loopback": cond.get("loopback", "nao informado"), "input_channel": ch + 1, "level_check_peak_dbfs": peaks}
    if peaks[ch] < -50:
        res["error"] = "sinal de loopback nao detectado (pico < -50 dBFS)"
        return res
    burst = A.log_chirp(0.04, 200, 12000, -12)
    stim = np.zeros(int(0.9 * 48000))
    offs = [0, 12000, 24000]
    for o in offs:
        stim[o:o + len(burst)] += burst
    stim_i16 = A.to_i16(stim)
    at = 28800  # 0.6 s
    table = []
    for mode in (1, 0):
        for buf in BUFFERS:
            lags, quality, pdc, active = [], [], None, None
            for i in range(args.m1_sessions):
                j = probe(mode, buf, 1.8, rundir, f"m1_{MODES[mode]}_{buf}_{i:02d}", stim=stim_i16, stim_at=at)
                active = j.get("mode_active")
                pdc = (j.get("input_latency"), j.get("output_latency"))
                x = A.load_i16(j["_rec"], 2)[:, ch].astype(np.float64)
                lag, q, inv = A.xcorr_lag(x[at:], stim_i16.astype(np.float64), max_lag=20000)
                if q > 8:
                    lags.append(lag)
                    quality.append(q)
            row = {"mode_requested": MODES[mode], "mode_active": active, "buffer": buf, "sessions": args.m1_sessions,
                   "valid_sessions": len(lags), "pdc_reported_in": pdc[0] if pdc else None,
                   "pdc_reported_out": pdc[1] if pdc else None}
            if lags:
                L = np.array(lags)
                row.update({"rtl_mean_samples": round(float(L.mean()), 2), "rtl_std_samples": round(float(L.std(ddof=1)) if len(L) > 1 else 0.0, 2),
                            "rtl_mean_ms": round(float(L.mean()) / 48.0, 3), "rtl_std_ms": round(float(L.std(ddof=1)) / 48.0 if len(L) > 1 else 0.0, 3),
                            "rtl_min_samples": round(float(L.min()), 2), "rtl_max_samples": round(float(L.max()), 2),
                            "pdc_total_samples": (pdc[0] + pdc[1]) if pdc and None not in pdc else None})
                if row["pdc_total_samples"]:
                    row["measured_minus_reported_samples"] = round(row["rtl_mean_samples"] - row["pdc_total_samples"], 2)
            table.append(row)
            print(f"  M1 {MODES[mode]}@{buf} ({active}): {row.get('rtl_mean_ms')} ms +/- {row.get('rtl_std_ms')} | PDC {pdc}")
    res["method"] = ("3 chirps log 200 Hz-12 kHz (-12 dBFS) por sessao, correlacao cruzada na linha de tempo de amostras ASIO; "
                     f"{args.m1_sessions} sessoes independentes (start/stop) por modo e buffer")
    res["results"] = table
    return res


def step_m3(args, rundir, cond):
    ask("M3/M4: mantenha o loopback ligado e o DIRECT MONITOR desligado.", args)
    ch, peaks = level_check(args, rundir)
    if peaks[ch] < -50:
        return {"error": "sinal de loopback nao detectado", "level_check_peak_dbfs": peaks}
    sweep, inv = A.ess(5.0, 20, 20000, -12)
    sw16 = A.to_i16(sweep)
    tn16 = A.to_i16(A.tone(1000, 3.0, -12))
    out = {"input_channel": ch + 1, "level_check_peak_dbfs": peaks, "runs": []}
    for mode in (1, 0):
        j = probe(mode, 256, 6.5, rundir, f"m3_{MODES[mode]}_256", stim=sw16, stim_at=24000)
        x = A.load_i16(j["_rec"], 2)[:, ch].astype(np.float64) / 32768
        resp = A.sweep_response(x, sweep, inv)
        jt = probe(mode, 256, 4.0, rundir, f"m4_{MODES[mode]}_256", stim=tn16, stim_at=24000)
        xt = A.load_i16(jt["_rec"], 2)[:, ch].astype(np.float64) / 32768
        lag, _, _ = A.xcorr_lag(xt[24000:], tn16.astype(np.float64), max_lag=20000)
        k = 24000 + int(lag)
        out["runs"].append({"mode_requested": MODES[mode], "mode_active": j.get("mode_active"), "buffer": 256,
                            "M3_response": resp, "M4_thd_n_1khz": A.thd_n(xt[k:k + len(tn16)])})
    return out


def step_m6(args, rundir, cond):
    if args.drift_csv:
        csvp = Path(args.drift_csv)
        meta = {"imported_from": str(csvp)}
        jp = Path(str(csvp).replace(".csv", ".json"))
        if jp.exists():
            try:
                meta["tool_summary"] = json.loads(jp.read_text().strip().splitlines()[-1])
            except Exception:  # noqa: BLE001
                pass
    else:
        ask(f"M6: a UM2 fica em exclusivo (entrada+saida) por {args.drift_seconds} s. Feche apps que usam a UM2.", args)
        csvp = rundir / "m6_drift.csv"
        rc, out, err = sh([str(DRIFT), "--seconds", str(args.drift_seconds), "--csv", str(csvp)],
                          timeout=args.drift_seconds + 120)
        meta = {"tool_summary": json.loads(out.strip().splitlines()[-1]) if out.strip() else {"rc": rc, "stderr": err}}
    prog = Path(str(csvp) + ".progress.log")
    if prog.exists():
        meta["progress_log_tail"] = prog.read_text(errors="replace").strip().splitlines()[-3:]
    return {"tool": "measure/tools/um2_drift.exe (WASAPI exclusive event-driven, captura+render simultaneos, 48k/16/2, render em silencio)",
            **meta, "analysis": A.drift_from_csv(csvp)}


STEP_FUNCS = {"m8": step_m8, "m2": step_m2, "m5": step_m5, "m7": step_m7, "m1": step_m1, "m3": step_m3, "m6": step_m6}


def assemble(rundir, run_id, header):
    doc = dict(header)
    doc["analysis_code"] = git("describe", "--tags", "--always", "--dirty")  # code that produced the analyses
    for s in ALL_STEPS:
        p = rundir / f"{s}.json"
        doc[s.upper()] = json.loads(p.read_text(encoding="utf-8")) if p.exists() else {"status": "nao executado"}
    if "M3" in doc and isinstance(doc["M3"], dict):
        doc["M4"] = ({"see": "M3.runs[].M4_thd_n_1khz"} if doc["M3"].get("runs") else
                     {"status": doc["M3"].get("status", "nao executado"), "reason": doc["M3"].get("reason"), "see": "M3"})
    out = REPO / "measure" / "baselines" / f"{run_id}.json"
    out.write_text(json.dumps(doc, indent=1, ensure_ascii=False), encoding="utf-8")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--steps", default=",".join(ALL_STEPS))
    ap.add_argument("--cond", action="append", default=[], help="key=value (gain_ch1, gain_ch2, output_knob, "
                    "monitor_switch, loopback, input_connection, cpu_load, ...)")
    ap.add_argument("--yes", action="store_true", help="nao perguntar (condicoes ausentes ficam 'nao informado')")
    ap.add_argument("--run-id", help="default: <data>-<commit>")
    ap.add_argument("--m1-sessions", type=int, default=20)
    ap.add_argument("--m2-seconds", type=int, default=60)
    ap.add_argument("--m7-tap-raw", type=int, default=1,
                    help="M7: tap de captura com AUDCLNT_STREAMOPTIONS_RAW (1, igual ao driver desde o passo 3) ou padrao (0)")
    ap.add_argument("--m7-reuse", action="store_true", help="reanalisa a captura M7 ja feita nesta corrida")
    ap.add_argument("--m2-reuse", action="store_true", help="reanalisa m2_apo.wav/m2_raw.wav ja capturados nesta corrida")
    ap.add_argument("--m5-seconds", type=int, default=60)
    ap.add_argument("--cpu-load", type=int, default=0, help="M5: N processos em busy-loop durante a medicao")
    ap.add_argument("--m5-modes", default="1,0", help="M5: modos (1=SharedFlex, 0=ExclusivePro)")
    ap.add_argument("--drift-seconds", type=int, default=3600)
    ap.add_argument("--drift-csv", help="importa uma corrida de um2_drift ja feita em vez de rodar de novo")
    args = ap.parse_args()

    for exe in (DLL, PROBE, DRIFT, RAWCAP):
        if not exe.exists():
            sys.exit(f"falta {exe} - compile Release x64 antes (ver CLAUDE.md)")
    commit = git("rev-parse", "--short", "HEAD")
    run_id = args.run_id or f"{dt.date.today().isoformat()}-{commit}"
    rundir = REPO / "measure" / "out" / run_id
    rundir.mkdir(parents=True, exist_ok=True)

    condfile = rundir / "conditions.json"
    cond = json.loads(condfile.read_text(encoding="utf-8")) if condfile.exists() else {}
    for kv in args.cond:
        k, _, v = kv.partition("=")
        cond[k.strip()] = v.strip()
    for k, msg in CONDITION_PROMPTS.items():
        if k not in cond:
            cond[k] = "nao informado" if args.yes else (input(f">>> {msg}: ").strip() or "nao informado")
    condfile.write_text(json.dumps(cond, indent=1, ensure_ascii=False), encoding="utf-8")

    reg_before = reg_read()
    header = {
        "schema": 1, "run_id": run_id, "date": dt.datetime.now().isoformat(timespec="seconds"),
        "commit": commit, "describe": git("describe", "--tags", "--always", "--dirty"),
        "driver_dll": {"path": str(DLL), "md5": md5(DLL), "bytes": DLL.stat().st_size},
        "machine": {"os": platform.platform(), "cpu": platform.processor()},
        "conditions": cond, "registry_before_run": reg_before,
        "dsp_state_shared_memory": um2_ipc.read_shared_state(),
        "fixed": {"sample_rate": 48000, "buffers": BUFFERS, "modes": list(MODES.values())},
        "protocol": "docs/MEASUREMENT-PROTOCOL.md",
    }
    hp = rundir / "header.json"
    if hp.exists():  # keep the first header of the run, refresh dsp/conditions
        old = json.loads(hp.read_text(encoding="utf-8"))
        old.update({"conditions": cond, "dsp_state_shared_memory": header["dsp_state_shared_memory"]})
        header = old
    hp.write_text(json.dumps(header, indent=1, ensure_ascii=False), encoding="utf-8")

    try:
        steps = [x.strip().lower() for x in args.steps.split(",") if x.strip()]
        for bad in [x for x in steps if x not in STEP_FUNCS and x != "assemble"]:
            print(f"passo desconhecido ignorado: {bad}")
        for s in [x for x in steps if x in STEP_FUNCS]:
            print(f"== {s.upper()} ==")
            ep_before = endpoint_capture_level()
            if ep_before.get("db") not in (None, 0.0):
                print(f"  AVISO: volume do endpoint de captura = {ep_before.get('db')} dB (protocolo pede 0,0 dB)")
            t0 = time.time()
            try:
                r = STEP_FUNCS[s](args, rundir, cond)
            except Exception as e:  # noqa: BLE001 - one failing step must not lose the others
                r = {"error": f"{type(e).__name__}: {e}"}
            r["_step_seconds"] = round(time.time() - t0, 1)
            r["_dsp_state"] = um2_ipc.read_shared_state()
            r["_endpoint_capture_before"] = ep_before
            r["_endpoint_capture_after"] = endpoint_capture_level()
            r["_conditions"] = dict(cond)
            r["_measured_at"] = dt.datetime.now().isoformat(timespec="seconds")
            (rundir / f"{s}.json").write_text(json.dumps(r, indent=1, ensure_ascii=False, default=str), encoding="utf-8")
    finally:
        reg_restore(reg_before)
    out = assemble(rundir, run_id, header)
    print(f"baseline: {out}")


if __name__ == "__main__":
    main()
