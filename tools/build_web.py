#!/usr/bin/env python3
"""Builds Warlords II for the web using Emscripten.

Compiles all C++ game sources and Abseil into WebAssembly, packaging original/
into the virtual filesystem so C++ serves as the single source of truth for
both desktop and web releases.

Usage:
    python3 tools/build_web.py [--out DIR] [--clean] [--debug]
"""

import argparse
import concurrent.futures
import hashlib
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def find_emsdk() -> None:
    """Ensures em++ is available on PATH, checking ~/.emsdk if needed."""
    if shutil.which("em++") and shutil.which("emcc"):
        return

    home_emsdk = Path.home() / ".emsdk"
    emscripten_dir = home_emsdk / "upstream" / "emscripten"
    if (emscripten_dir / "em++").exists():
        os.environ["PATH"] = f"{home_emsdk}:{emscripten_dir}:{os.environ.get('PATH', '')}"
        os.environ["EMSDK"] = str(home_emsdk)
        return

    sys.exit(
        "error: em++ not found. Please install emsdk or run:\n"
        "  git clone https://github.com/emscripten-core/emsdk.git ~/.emsdk\n"
        "  ~/.emsdk/emsdk install latest && ~/.emsdk/emsdk activate latest\n"
        "  source ~/.emsdk/emsdk_env.sh"
    )


def resolve_dependencies() -> tuple[Path, Path]:
    """Finds Abseil and nlohmann/json include paths, fetching via Bazel if needed."""
    absl_dir = None
    json_dir = None

    # Check Bazel cache
    try:
        res = subprocess.run(
            ["bazel", "info", "output_base"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
        )
        base = Path(res.stdout.strip())
        candidate_absl = base / "external" / "abseil-cpp+"
        candidate_json = base / "external" / "nlohmann_json+" / "include"
        if candidate_absl.exists() and candidate_json.exists():
            absl_dir = candidate_absl
            json_dir = candidate_json
    except Exception:
        pass

    if not absl_dir or not json_dir:
        # Fall back to running bazel fetch
        print("Fetching abseil-cpp and nlohmann_json dependencies via Bazel...")
        subprocess.run(
            ["bazel", "fetch", "@abseil-cpp//...", "@nlohmann_json//..."],
            cwd=ROOT,
            check=True,
        )
        res = subprocess.run(
            ["bazel", "info", "output_base"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
        )
        base = Path(res.stdout.strip())
        absl_dir = base / "external" / "abseil-cpp+"
        json_dir = base / "external" / "nlohmann_json+" / "include"

    if not absl_dir.exists() or not json_dir.exists():
        sys.exit(f"error: Could not locate Abseil ({absl_dir}) or JSON ({json_dir})")

    return absl_dir, json_dir


def collect_sources(absl_dir: Path) -> tuple[list[Path], list[Path]]:
    """Gathers all engine .cpp files and required Abseil .cc files."""
    game_srcs = sorted(ROOT.glob("cpp/src/**/*.cpp"))

    absl_srcs = []
    excluded_keywords = [
        "_test.", "test_", "_testing.", "testing_", "benchmark", "fuzz", "gentables", "print_hash_of",
        "mock", "status_matchers"
    ]
    for root, _, files in os.walk(absl_dir / "absl"):
        if any(k in root for k in ["test", "benchmark", "fuzz", "sample"]):
            continue
        for f in sorted(files):
            if f.endswith(".cc") and not any(k in f for k in excluded_keywords):
                absl_srcs.append(Path(root) / f)

    return game_srcs, absl_srcs


def file_hash(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()[:16]


def compile_file(
    src: Path,
    obj_path: Path,
    cxx_flags: list[str],
    hash_file: Path,
    current_hash: str,
) -> tuple[Path, bool, str]:
    """Compiles a single translation unit if changed."""
    if hash_file.exists() and obj_path.exists():
        try:
            cached_hash = hash_file.read_text().strip()
            if cached_hash == current_hash:
                return src, True, ""
        except Exception:
            pass

    cmd = ["em++"] + cxx_flags + ["-c", str(src), "-o", str(obj_path)]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        return src, False, res.stderr

    try:
        hash_file.write_text(current_hash)
    except Exception:
        pass
    return src, True, ""


def build(out_dir: Path, clean: bool = False, debug: bool = False) -> None:
    find_emsdk()
    absl_dir, json_dir = resolve_dependencies()
    game_srcs, absl_srcs = collect_sources(absl_dir)

    cache_dir = ROOT / "build" / "cache"
    obj_dir = cache_dir / "obj"
    hashes_dir = cache_dir / "hashes"

    if clean and cache_dir.exists():
        shutil.rmtree(cache_dir)

    obj_dir.mkdir(parents=True, exist_ok=True)
    hashes_dir.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)

    opt_flag = "-O0" if debug else "-O2"
    base_flags = [
        "-std=c++23",
        opt_flag,
        f"-I{ROOT / 'cpp' / 'src'}",
        f"-I{absl_dir}",
        f"-I{json_dir}",
        "-sUSE_SDL=2",
        "-Wno-deprecated-pragma",
        "-Wno-deprecated-attributes",
        "-DJSON_NOEXCEPTION=1",
    ]
    if debug:
        base_flags += ["-g", "-sASSERTIONS=1"]

    all_tasks = []
    all_objs = []

    for src in game_srcs:
        rel = src.relative_to(ROOT / "cpp" / "src")
        name = str(rel).replace("/", "_").replace("\\", "_").replace(".cpp", ".o")
        obj = obj_dir / name
        hash_path = hashes_dir / (name + ".hash")
        all_objs.append(obj)
        all_tasks.append((src, obj, base_flags, hash_path, file_hash(src)))

    for src in absl_srcs:
        rel = src.relative_to(absl_dir)
        name = "absl_" + str(rel).replace("/", "_").replace("\\", "_").replace(".cc", ".o")
        obj = obj_dir / name
        hash_path = hashes_dir / (name + ".hash")
        all_objs.append(obj)
        all_tasks.append((src, obj, base_flags, hash_path, file_hash(src)))

    print(f"Compiling {len(all_tasks)} units ({len(game_srcs)} game + {len(absl_srcs)} Abseil)...")
    start = time.time()
    compiled_count = 0

    with concurrent.futures.ThreadPoolExecutor() as ex:
        futures = {
            ex.submit(compile_file, s, o, f, h, cur): s
            for s, o, f, h, cur in all_tasks
        }
        for fut in concurrent.futures.as_completed(futures):
            src, success, err = fut.result()
            if not success:
                sys.exit(f"\nCompilation failed for {src}:\n{err}")
            compiled_count += 1

    elapsed = time.time() - start
    print(f"Compilation finished in {elapsed:.2f}s.")

    # Link step
    output_js = out_dir / "warlords2.js"
    print(f"Linking {output_js.name}...")
    link_cmd = [
        "em++",
        "-std=c++23",
        opt_flag,
        "-sUSE_SDL=2",
        "-sALLOW_MEMORY_GROWTH=1",
        "-sINITIAL_MEMORY=67108864",
        "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','FS']",
        f"--preload-file={ROOT / 'original'}@/original",
        "-o",
        str(output_js),
    ] + [str(o) for o in all_objs]

    res = subprocess.run(link_cmd, capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit(f"Linking failed:\n{res.stderr}")

    # Copy HTML shell and _headers if needed
    for name in ["index.html", "_headers"]:
        src_file = ROOT / "web" / name
        if src_file.exists() and (out_dir != ROOT / "web"):
            shutil.copy2(src_file, out_dir / name)

    wasm_file = out_dir / "warlords2.wasm"
    data_file = out_dir / "warlords2.data"
    print("Web build complete!")
    for f in [output_js, wasm_file, data_file]:
        if f.exists():
            size_mb = f.stat().st_size / (1024 * 1024)
            print(f"  {f.name}: {size_mb:.2f} MB")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--out",
        type=Path,
        default=ROOT / "build" / "web",
        help="Destination directory for web artifacts (default: build/web)",
    )
    parser.add_argument("--clean", action="store_true", help="Clean build directory before compiling")
    parser.add_argument("--debug", action="store_true", help="Build with debug assertions and symbols")
    parser.add_argument("--serve", action="store_true", help="Start local web server after building")
    parser.add_argument("--port", type=int, default=8765, help="Port to serve on with --serve (default: 8765)")
    args = parser.parse_args()
    out_dir = args.out.resolve()
    build(out_dir, clean=args.clean, debug=args.debug)

    if args.serve:
        import http.server
        import socketserver

        class Handler(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *a, **kw):
                super().__init__(*a, directory=str(out_dir), **kw)

        url = f"http://localhost:{args.port}/"
        print(f"\nServing Warlords II Web at {url} (Press Ctrl+C to stop)...")
        socketserver.TCPServer.allow_reuse_address = True
        with socketserver.TCPServer(("", args.port), Handler) as httpd:
            try:
                httpd.serve_forever()
            except KeyboardInterrupt:
                print("\nServer stopped.")


if __name__ == "__main__":
    main()
