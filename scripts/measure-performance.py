#!/usr/bin/env python3
"""NF01/NF04 外部测量，仅使用 Python 标准库和本机系统工具。"""
import argparse
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import signal
import statistics
import subprocess
import sys
import time

TOOL_VERSION = "2"
MIB = 1024 * 1024
REPO = Path(__file__).resolve().parent.parent


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def command(args):
    result = subprocess.run(args, capture_output=True, text=True, timeout=30)
    return {"exit_code": result.returncode, "stdout": result.stdout.strip(),
            "stderr": result.stderr.strip()}


def probe_json(probe, *args):
    result = subprocess.run([str(probe), *args], capture_output=True, text=True, timeout=5)
    if result.returncode:
        raise RuntimeError(f"测量探针失败：{result.stderr.strip()}")
    return json.loads(result.stdout)


def metadata(args, build, probe):
    build_info = json.loads((build / "build-metadata-Release.json").read_text())
    if build_info["configuration"] != "Release":
        raise RuntimeError("必须使用 Release 构建。")
    calibration = probe_json(probe, "--self-test")
    if not calibration.get("valid"):
        raise RuntimeError("CPU 探针与 getrusage 校准不一致，请修复工具后采集。")
    current_commit = command(["git", "-C", str(REPO), "rev-parse", "HEAD"])["stdout"]
    dirty = bool(command(["git", "-C", str(REPO), "status", "--porcelain"])["stdout"])
    if not args.rehearsal and (dirty or build_info["dirty"] or current_commit != build_info["commit"]):
        raise RuntimeError("正式测量要求源码和 Release 构建均来自同一干净 commit。")
    return {"classification": "非正式彩排" if args.rehearsal else "正式采集（不自动宣告 NF 通过）",
            "recorded_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "commit": current_commit, "source_dirty": dirty, "build": build_info,
            "tool_version": TOOL_VERSION, "python": sys.version, "cpu_calibration": calibration,
            "tool_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "macos": command(["sw_vers"]), "architecture": command(["uname", "-m"]),
            "power_settings": command(["pmset", "-g", "custom"]),
            "power_source": command(["pmset", "-g", "batt"]),
            "sdk_version": command(["xcrun", "--show-sdk-version"]),
            "sdk_path": command(["xcrun", "--show-sdk-path"]),
            "compiler": command([build_info["compiler_path"], "--version"]),
            "hardware_displays_input": probe_json(probe, "--metadata"),
            "parameters": vars(args),
            "application_sha256": hashlib.sha256((build / "bin" / "WaibuSnap.app" / "Contents" / "MacOS" / "WaibuSnap").read_bytes()).hexdigest(),
            "probe_sha256": hashlib.sha256(probe.read_bytes()).hexdigest(),
            "clock": "应用 std::chrono::steady_clock 纳秒；外部 time.monotonic_ns",
            "nf01_endpoint": "两次显示刷新后的可见代理 + 窗口激活；需外部呈现校验",
            "stability_criterion": "至少 10 秒等待；末 5 次 RSS 波动 ≤ 5 MiB、两半 CPU 均值差 ≤ 0.5 个百分点",
            "memory_accounting": "全部根进程及子孙进程 RSS 同步总和；另报 physical footprint；共享页可能重复",
            "sampling_limit": "0.8 秒目标间隔（1.25Hz）可能遗漏短寿命进程；RSS 峰值是观察下限；WindowServer 是共享系统进程",
            "nf_thresholds": {"nf01_p95_ms": 300, "nf04_rss_mib": 150, "nf04_cpu_percent": 1}}


def ps_window_server(pid):
    """权限不允许 libproc 时，保留 ps 的 10ms CPU / KiB RSS 低精度旁证。"""
    result = command(["ps", "-p", str(pid), "-o", "rss=", "-o", "time="])
    fields = result["stdout"].split()
    if result["exit_code"] or len(fields) != 2:
        return None
    pieces = fields[1].split(":")
    cpu_seconds = 0.0
    for piece in pieces:
        cpu_seconds = cpu_seconds * 60 + float(piece)
    return {"pid": pid, "birth": "ps-unavailable", "cpu_ns": str(round(cpu_seconds * 1e9)),
            "rss_bytes": int(fields[0]) * 1024, "footprint_bytes": None,
            "error": 0, "source": "ps-fallback-10ms"}


def snapshot(probe, process):
    before = time.monotonic_ns()
    window_server = command(["pgrep", "-x", "WindowServer"])
    ws_pids = window_server["stdout"].split()
    ws_pid = ws_pids[0] if ws_pids else "0"
    result = probe_json(probe, "--sample", str(process.pid), ws_pid)
    after = time.monotonic_ns()
    if process.poll() is not None or not result["app"]:
        raise RuntimeError("应用提前退出，不能记录为空闲零占用。")
    if any(row["error"] for row in result["app"]):
        raise RuntimeError("无法读取全部应用进程的 RSS / physical footprint / CPU。")
    result["monotonic_ns"] = (before + after) // 2
    result["probe_duration_ms"] = (after - before) / 1e6
    result["app_rss_bytes"] = sum(row["rss_bytes"] for row in result["app"])
    result["app_footprint_bytes"] = sum(row["footprint_bytes"] for row in result["app"])
    for index, row in enumerate(result["window_server"]):
        if row["error"]:
            fallback = ps_window_server(row["pid"])
            if fallback:
                fallback["libproc_error"] = row["error"]
                result["window_server"][index] = fallback
    result["shared_capture_services"] = []
    replay = command(["pgrep", "-x", "replayd"])
    for candidate in replay["stdout"].split():
        service = probe_json(probe, "--sample", candidate, "0")["app"]
        if not service:
            result["shared_capture_services"].append({"pid": int(candidate), "error": 1})
        for entry in service:
            if entry["error"]:
                fallback = ps_window_server(entry["pid"])
                if fallback:
                    fallback["libproc_error"] = entry["error"]
                    entry = fallback
            result["shared_capture_services"].append(entry)
    candidates = result["shared_capture_services"]
    result["shared_service_rss_upper_bytes"] = (sum(row["rss_bytes"] for row in candidates)
        if candidates and not any(row["error"] for row in candidates) else None)
    result["app_rss_lower_bytes"] = result["app_rss_bytes"]
    result["app_plus_shared_rss_upper_bytes"] = (result["app_rss_bytes"] + result["shared_service_rss_upper_bytes"]
        if result["shared_service_rss_upper_bytes"] is not None else None)
    completed = time.monotonic_ns()
    result["full_probe_duration_ms"] = (completed - before) / 1e6
    return result


def cpu_delta(previous, current, group):
    old = {(row["pid"], row.get("birth")): row for row in previous[group] if not row["error"]}
    total = 0
    incomplete = False
    current_keys = set()
    for row in current[group]:
        key = (row["pid"], row.get("birth"))
        current_keys.add(key)
        if row["error"]:
            incomplete = True
            continue
        if key not in old:
            # 新进程从创建到当前的全部 CPU 纳入，误差由采样边界明确报告。
            total += int(row["cpu_ns"])
            incomplete = True
        else:
            total += max(0, int(row["cpu_ns"]) - int(old[key]["cpu_ns"]))
    if old.keys() - current_keys:
        incomplete = True
    if not current[group]:
        incomplete = True
    return total, incomplete


def add_cpu(previous, current):
    wall_ns = current["monotonic_ns"] - previous["monotonic_ns"]
    current["interval_ms"] = wall_ns / 1e6
    for group in ("app", "window_server", "shared_capture_services"):
        delta, incomplete = cpu_delta(previous, current, group)
        current[f"{group}_cpu_delta_ns"] = delta
        current[f"{group}_cpu_percent"] = delta / wall_ns * 100
        current[f"{group}_cpu_incomplete"] = incomplete


def collect_stability(probe, process, seconds):
    if seconds < 10:
        raise RuntimeError("稳定等待至少 10 秒。")
    rows = []
    start = time.monotonic()
    deadline = start
    # 预留 1 秒，避免外部探针与 NF01 捕获同时执行。
    while time.monotonic() < start + seconds - 1:
        time.sleep(max(0, deadline - time.monotonic()))
        row = snapshot(probe, process)
        if rows:
            add_cpu(rows[-1], row)
        rows.append(row)
        deadline += 1
    tail = rows[-5:]
    rss_spread = max(row["app_rss_bytes"] for row in tail) - min(row["app_rss_bytes"] for row in tail)
    cpus = [row["app_cpu_percent"] for row in tail if "app_cpu_percent" in row]
    cpu_change = abs(statistics.mean(cpus[:2]) - statistics.mean(cpus[2:])) if len(cpus) == 5 else math.inf
    stable = len(tail) == 5 and rss_spread <= 5 * MIB and cpu_change <= 0.5
    return {"stable": stable, "rss_spread_mib": rss_spread / MIB,
            "cpu_half_change_percentage_points": cpu_change, "samples": rows}


def stop(process):
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def nearest_rank(values, percentile=0.95):
    ordered = sorted(values)
    if not ordered:
        return None
    return ordered[math.ceil(percentile * len(ordered)) - 1]


def nf01_summary(rows):
    result = {"presentation_validated": False, "nf01_pass": None,
              "note": "代理时间未通过外部呈现校验；不能宣布正式 NF01 达标。"}
    for cold, name in ((True, "cold"), (False, "warm")):
        samples = [row for row in rows if row.get("cold") is cold and row.get("interactive_ns")
                   and row.get("outcome") in (1, 2, 10)]
        times = [row["nf01_proxy_ms"] for row in samples]
        result[name] = {"count": len(times), "p95_proxy_ms": nearest_rank(times),
                        "maximum_proxy_ms": max(times) if times else None,
                        "nearest_rank_index": math.ceil(0.95 * len(times))}
    return result


def measure_nf01(args, app, probe, output):
    collected = []
    runs = [("cold", i, 1) for i in range(args.cold)]
    # 热路径另起一个进程，保留第一次的冷样本作诊断，不混入 30 次冷汇总。
    if args.warm:
        runs.append(("warm", 0, args.warm + 1))
    for kind, index, count in runs:
        log = output / f"{kind}-{index:02d}.jsonl"
        stderr = output / f"{kind}-{index:02d}-stderr.log"
        with stderr.open("w", encoding="utf-8") as diagnostics:
            process = subprocess.Popen([str(app), "--test-mode", "--test-count", str(count),
                "--test-stable-ms", str(round(args.stable_seconds * 1000)),
                "--test-interval-ms", str(round(args.interval_seconds * 1000)),
                "--metrics-file", str(log)], stdout=diagnostics, stderr=diagnostics)
            try:
                stability = collect_stability(probe, process, args.stable_seconds)
                write_json(output / f"{kind}-{index:02d}-stability.json", stability)
                if not stability["stable"]:
                    raise RuntimeError("启动后尚未稳定，请增加等待时间再采集。")
                remaining_timeout = count * (args.interval_seconds + 20) + 30
                if process.wait(timeout=remaining_timeout):
                    raise RuntimeError(f"截图未完成，请查看 {stderr.name} 和授权状态。")
            finally:
                stop(process)
        rows = [json.loads(line) for line in log.read_text().splitlines()]
        if len(rows) != count or any(not row.get("interactive_ns") for row in rows):
            raise RuntimeError("会话记录数量或可交互终点缺失。")
        for row in rows:
            row["collection"] = kind
        collected.extend(rows if kind == "cold" else rows[1:])
        print(f"{kind} 已完成 {count} 次会话", flush=True)
    (output / "sessions.jsonl").write_text("".join(json.dumps(row, ensure_ascii=False) + "\n" for row in collected))
    write_json(output / "summary.json", nf01_summary(collected))


def graphics_report(pid, output, name):
    result = command(["vmmap", "-summary", str(pid)])
    write_json(output / f"{name}-graphics.json", result)
    return {"available": result["exit_code"] == 0,
            "file": f"{name}-graphics.json", "note": "系统图形 / IOSurface 记账旁证，不能分配共享 WindowServer 成本"}


def measure_idle(args, app, probe, output):
    # 普通启动：无 --test-mode，无自动触发和受控定时器。
    with (output / "app-stderr.log").open("w", encoding="utf-8") as diagnostics:
        process = subprocess.Popen([str(app), "--metrics-file", str(output / "idle-sessions.jsonl")], stdout=diagnostics, stderr=diagnostics)
        try:
            stability = collect_stability(probe, process, args.stable_seconds)
            write_json(output / "stability.json", stability)
            if not stability["stable"]:
                raise RuntimeError("空闲基线尚未稳定，请增加等待时间。")
            time.sleep(1)
            rows = [snapshot(probe, process)]
            graphics = {"application": graphics_report(process.pid, output, "app")}
            for row in rows[0]["window_server"]:
                graphics[f"window_server_{row['pid']}"] = graphics_report(row["pid"], output, "window-server")
            # 图形旁证在时序起点之前完成，避免其工具开销混入平均 CPU。
            rows = [snapshot(probe, process)]
            start = time.monotonic()
            sample_interval = 0.8
            intervals = math.ceil(args.duration_seconds / sample_interval)
            with (output / "idle.jsonl").open("w", encoding="utf-8") as timeline:
                timeline.write(json.dumps(rows[0]) + "\n")
                for index in range(1, intervals + 1):
                    time.sleep(max(0, start + index * sample_interval - time.monotonic()))
                    row = snapshot(probe, process)
                    add_cpu(rows[-1], row)
                    timeline.write(json.dumps(row) + "\n")
                    timeline.flush()
                    rows.append(row)
            wall_ns = rows[-1]["monotonic_ns"] - rows[0]["monotonic_ns"]
            app_cpu = sum(row["app_cpu_delta_ns"] for row in rows[1:]) / wall_ns * 100
            window_cpu = sum(row["window_server_cpu_delta_ns"] for row in rows[1:]) / wall_ns * 100
            app_incomplete = any(row["app_cpu_incomplete"] for row in rows[1:])
            window_incomplete = any(row["window_server_cpu_incomplete"] for row in rows[1:])
            ws_rss = [[row["rss_bytes"] for row in sample["window_server"] if not row["error"]] for sample in rows]
            summary = {"classification": "非正式彩排" if args.rehearsal else "正式采集",
                "samples": len(rows), "duration_seconds": wall_ns / 1e9,
                "max_rss_mib": max(row["app_rss_bytes"] for row in rows) / MIB,
                "max_footprint_mib": max(row["app_footprint_bytes"] for row in rows) / MIB,
                "average_cpu_percent_one_core": app_cpu,
                "app_process_accounting_incomplete": app_incomplete,
                "maximum_sample_interval_ms": max(row["interval_ms"] for row in rows[1:]),
                "maximum_probe_duration_ms": max(row["full_probe_duration_ms"] for row in rows),
                "window_server_cpu_percent_one_core": None if window_incomplete else window_cpu,
                "window_server_rss_delta_mib": (sum(ws_rss[-1]) - sum(ws_rss[0])) / MIB if all(ws_rss) else None,
                "window_server_accounting_incomplete": window_incomplete,
                "graphics": graphics, "nf04_pass": None,
                "note": "不自动判通过；需审查采样误差、共享服务、图形记账、正式 5 分钟与设备资格。"}
            bounds = [row["app_plus_shared_rss_upper_bytes"] for row in rows]
            summary["app_plus_replayd_rss_upper_mib"] = max(bounds) / MIB if all(value is not None for value in bounds) else None
            summary["shared_service_note"] = "replayd 为共享系统捕获服务；归属下界为 0、上界为其全部 RSS。未知服务未精确归属，不用下界宣布通过。"
            summary["shared_service_cpu_percent_upper"] = (sum(row["shared_capture_services_cpu_delta_ns"] for row in rows[1:]) / wall_ns * 100
                if not any(row["shared_capture_services_cpu_incomplete"] for row in rows[1:]) else None)
            session_log = output / "idle-sessions.jsonl"
            if session_log.exists() and session_log.stat().st_size:
                raise RuntimeError("空闲期间发生截图会话，数据无效，请重新采集。")
            write_json(output / "summary.json", summary)
        finally:
            stop(process)


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("mode", choices=["nf01", "idle", "summarize"])
    result.add_argument("--build-dir", default=str(REPO / "build" / "macos"))
    result.add_argument("--output", required=True)
    result.add_argument("--input", help="汇总已有会话 JSONL")
    result.add_argument("--rehearsal", action="store_true", help="明确标记非正式，允许少量样本")
    result.add_argument("--cold", type=int, default=30)
    result.add_argument("--warm", type=int, default=30)
    result.add_argument("--stable-seconds", type=float, default=15)
    result.add_argument("--interval-seconds", type=float, default=2)
    result.add_argument("--duration-seconds", type=float, default=300)
    return result


def main():
    args = parser().parse_args()
    output = Path(args.output).expanduser().resolve()
    if output.exists():
        raise RuntimeError("输出目录已存在，请使用新目录以保护已有数据。")
    output.mkdir(parents=True)
    if args.mode == "summarize":
        if not args.input:
            raise RuntimeError("汇总需要 --input。")
        rows = [json.loads(line) for line in Path(args.input).read_text().splitlines()]
        write_json(output / "summary.json", nf01_summary(rows))
        return
    if sys.platform != "darwin":
        raise RuntimeError("此采集工具仅实现 macOS，Windows 待补。")
    if args.cold < 0 or args.warm < 0 or args.duration_seconds <= 0 or args.interval_seconds < 0:
        raise RuntimeError("次数与时长参数无效。")
    if not args.rehearsal and ((args.mode == "nf01" and (args.cold != 30 or args.warm != 30)) or
                              (args.mode == "idle" and args.duration_seconds < 300)):
        raise RuntimeError("少量试点必须加 --rehearsal；正式采集需 30 冷 / 30 热和至少 300 秒空闲。")
    build = Path(args.build_dir).expanduser().resolve()
    app = build / "bin" / "WaibuSnap.app" / "Contents" / "MacOS" / "WaibuSnap"
    probe = build / "bin" / "waibusnap_measure_probe"
    if not app.is_file() or not probe.is_file():
        raise RuntimeError("请先构建 Release 应用及探针。")
    # 拒绝同时运行其他实例，避免 F1 冲突以及遗漏同产品进程。
    other = command(["pgrep", "-x", "WaibuSnap"])
    if other["exit_code"] == 0:
        raise RuntimeError("请先从托盘退出已有 WaibuSnap，采集工具将独占应用实例。")
    try:
        write_json(output / "metadata.json", metadata(args, build, probe))
        if args.mode == "nf01":
            measure_nf01(args, app, probe, output)
        else:
            measure_idle(args, app, probe, output)
    except Exception as error:
        write_json(output / "failure.json", {"error": str(error), "classification": "采集未完成"})
        raise
    print(f"结果已保存：{output}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"测量失败：{error}", file=sys.stderr)
        sys.exit(1)
