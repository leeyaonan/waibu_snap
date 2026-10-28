"""用真实编译器检查 macOS 平台源文件的 ObjC 块，防止异步捕获 C++ 引用。"""

import argparse
import re
import shlex
import subprocess
import sys
from pathlib import Path

DEFAULT_SOURCES = (
    "src/platform/macos/capture_services.mm",
    "src/platform/macos/recording_services.mm",
    "src/platform/macos/recording_audio_pipeline.mm",
)


def compile_command(build_dir: Path, source: Path) -> list[str]:
    cache = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8")
    ninja = re.search(r"^CMAKE_MAKE_PROGRAM:FILEPATH=(.+)$", cache, re.MULTILINE)
    if not ninja or "CMAKE_GENERATOR:INTERNAL=Ninja\n" not in cache:
        raise RuntimeError("请提供已配置的 macOS Ninja 构建目录。")
    object_path = (
        f"src/platform/CMakeFiles/waibusnap_platform.dir/macos/{source.name}.o"
    )
    commands = subprocess.check_output(
        [ninja.group(1), "-C", str(build_dir), "-t", "commands", object_path],
        text=True,
    )
    candidates = [shlex.split(line) for line in commands.splitlines()]
    candidates = [
        command
        for command in candidates
        if "-c" in command and str(source) in command
    ]
    if len(candidates) != 1:
        raise RuntimeError(f"无法唯一定位 {source.name} 的生产编译命令。")
    command = []
    pending = iter(candidates[0])
    for flag in pending:
        if flag in ("-o", "-MT", "-MF", "-MQ"):
            next(pending)
        elif flag not in ("-c", "-MD", "-MMD"):
            command.append(flag)
    command += [
        "-fsyntax-only",
        "-Xclang",
        "-ast-dump",
        "-Xclang",
        "-ast-dump-filter",
        "-Xclang",
        "waibusnap",
    ]
    return command


def check_source(build_dir: Path, repo: Path, relative: str) -> int:
    source = repo / relative
    if not source.is_file():
        raise RuntimeError(f"源文件不存在：{source}")
    command = compile_command(build_dir, source)
    result = subprocess.run(command, cwd=build_dir, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stderr)
    blocks = result.stdout.count("-BlockDecl ")
    captures = re.findall(
        r"\bcapture (?:nested )?(?:ParmVar|Var) \S+ '([^']+)' '([^']+)'",
        result.stdout,
    )
    # 防止编译器输出变化导致零断言假通过；引用类型不增加被引用对象的持有计数。
    if not blocks or not captures:
        raise RuntimeError(
            f"{relative} 的编译器输出没有 ObjC 块或捕获信息，不能判定通过。"
        )
    references = [
        (name, kind) for name, kind in captures if kind.rstrip().endswith("&")
    ]
    if references:
        for name, kind in references:
            print(
                f"失败：{relative} 的 ObjC 块捕获引用 {name}（{kind}），"
                "异步执行可能访问悬垂存储。",
                file=sys.stderr,
            )
        return 1
    print(
        f"通过：{relative} 检查 {blocks} 个 ObjC 块、"
        f"{len(captures)} 个捕获，无 C++ 引用捕获。"
    )
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument(
        "--source",
        action="append",
        dest="sources",
        help="相对仓库根的源文件；可重复，默认检查采集与录制两个实现。",
    )
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    build_dir = args.build_dir.resolve()
    sources = args.sources or DEFAULT_SOURCES
    failures = 0
    for relative in sources:
        failures += check_source(build_dir, repo, relative)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
