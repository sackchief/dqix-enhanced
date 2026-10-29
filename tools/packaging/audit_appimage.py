#!/usr/bin/env python3
"""Audit AppImage/toolchain ELF dependencies, package sizes, and Conda SBOM.

This runs only in CI against the newly built public builder. It never opens a ROM.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import uuid
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import quote, urlparse

HOST_ELF_ALLOWLIST = {
    # Core Linux ABI and dynamic loader.
    "ld-linux-x86-64.so.2", "libc.so.6", "libm.so.6", "libdl.so.2",
    "libpthread.so.0", "librt.so.1", "libutil.so.1", "libgcc_s.so.1",
    "libresolv.so.2", "libnss_files.so.2", "libnss_dns.so.2",
    # Linux desktop/display/audio APIs that remain host-provided when not bundled.
    "libfuse.so.2", "libX11.so.6", "libXext.so.6", "libXrender.so.1",
    "libXrandr.so.2", "libXcursor.so.1", "libXi.so.6", "libXfixes.so.3",
    "libXinerama.so.1", "libXss.so.1", "libXtst.so.6", "libxcb.so.1",
    "libxkbcommon.so.0", "libwayland-client.so.0", "libwayland-cursor.so.0",
    "libwayland-egl.so.1", "libdrm.so.2", "libgbm.so.1", "libGL.so.1",
    "libGLX.so.0", "libEGL.so.1", "libGLdispatch.so.0", "libvulkan.so.1",
    "libasound.so.2", "libpulse.so.0", "libpulse-simple.so.0", "libudev.so.1",
    "libsystemd.so.0", "libfontconfig.so.1", "libfreetype.so.6", "libdbus-1.so.3",
    "libexpat.so.1", "libz.so.1", "libzstd.so.1", "libselinux.so.1",
    "libmount.so.1", "libuuid.so.1", "libtirpc.so.3", "libnsl.so.1",
    "libcrypt.so.1", "libgomp.so.1", "libtinfo.so.6", "libncursesw.so.6",
}
NEEDED_RE = re.compile(r"\(NEEDED\).*Shared library: \[([^\]]+)\]")
LD_NOT_FOUND_RE = re.compile(r"^\s*(\S+)\s+=>\s+not found\s*$")
LD_RESOLVED_RE = re.compile(r"^\s*(\S+)\s+=>\s+(/\S+)(?:\s+\(0x[0-9a-fA-F]+\))?\s*$")
LD_DIRECT_RE = re.compile(r"^\s*(\S+)\s+(/\S+)\s+\(0x[0-9a-fA-F]+\)\s*$")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def walk_files(root: Path) -> list[Path]:
    result: list[Path] = []
    for current, dirs, names in os.walk(root, followlinks=False):
        dirs.sort()
        names.sort()
        for name in names:
            path = Path(current) / name
            try:
                if path.is_file():
                    result.append(path)
            except OSError:
                continue
    return result


def file_stats(root: Path, limit: int = 20) -> dict:
    files = walk_files(root)
    entries = []
    total = 0
    for path in files:
        try:
            size = path.stat().st_size
        except OSError:
            continue
        total += size
        entries.append({"path": path.relative_to(root).as_posix(), "bytes": size})
    entries.sort(key=lambda row: (-row["bytes"], row["path"]))
    return {"root": str(root), "file_count": len(entries), "total_bytes": total, "largest_files": entries[:limit]}


def readelf(path: Path, option: str) -> str:
    result = subprocess.run(
        ["readelf", option, str(path)],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"readelf {option} failed for {path}: {result.stderr.strip()}")
    return result.stdout


def ldd_for(path: Path, library_root: Path | None, bundle_root: Path | None) -> dict:
    dynamic = readelf(path, "-d")
    needed = NEEDED_RE.findall(dynamic)
    if not needed:
        return {"needed": [], "resolved": [], "host_dependencies": [], "unresolved": [], "unexpected_host": []}

    env = os.environ.copy()
    if library_root is not None:
        env["LD_LIBRARY_PATH"] = str(library_root) + (
            os.pathsep + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else ""
        )
    result = subprocess.run(
        ["ldd", str(path)],
        check=False,
        capture_output=True,
        text=True,
        env=env,
    )
    output = result.stdout + "\n" + result.stderr
    unresolved: list[str] = []
    resolved: list[dict] = []
    host_dependencies: list[str] = []
    unexpected_host: list[str] = []
    # The toolchain contains both normal runtime libraries and a separate GCC sysroot.
    # Bundle membership must be tested against the entire relocated environment, not only lib/.
    resolved_root = bundle_root.resolve() if bundle_root else None

    for line in output.splitlines():
        missing_match = LD_NOT_FOUND_RE.match(line)
        if missing_match:
            unresolved.append(missing_match.group(1))
            continue
        match = LD_RESOLVED_RE.match(line)
        if not match:
            match = LD_DIRECT_RE.match(line)
        if not match:
            continue
        soname, raw_path = match.groups()
        dependency_path = Path(raw_path)
        try:
            resolved_path = dependency_path.resolve(strict=True)
        except OSError:
            unresolved.append(f"{soname} (reported {raw_path}, path unavailable)")
            continue
        if resolved_root is not None and resolved_path.is_relative_to(resolved_root):
            resolved.append({"soname": soname, "path": str(resolved_path), "origin": "bundled-toolchain"})
            continue

        # Anything outside the bundled environment must have a known host ABI role.
        host_dependencies.append(soname)
        if soname not in HOST_ELF_ALLOWLIST:
            unexpected_host.append(f"{soname} => {resolved_path}")
        else:
            resolved.append({"soname": soname, "path": str(resolved_path), "origin": "host-allowlist"})

    if result.returncode != 0 and not unresolved:
        unresolved.append(f"ldd exited {result.returncode}: {output.strip()[:300]}")
    return {
        "needed": needed,
        "resolved": resolved,
        "host_dependencies": sorted(set(host_dependencies)),
        "unresolved": sorted(set(unresolved)),
        "unexpected_host": sorted(set(unexpected_host)),
    }


def inspect_elf(
    path: Path,
    display_path: str,
    library_root: Path | None,
    bundle_root: Path | None,
    link_time_sysroot: bool,
) -> dict:
    header = readelf(path, "-h")
    machine_match = re.search(r"^\s*Machine:\s*(.+)$", header, flags=re.MULTILINE)
    machine = machine_match.group(1).strip() if machine_match else "unknown"
    if link_time_sysroot:
        # This is the cross compiler's target sysroot, not a runtime component:
        # its SONAMEs describe the libc ABI that output executables link against.
        # Keep the ELF and NEEDED inventory but do not resolve those target libraries
        # with the runner's host dynamic loader.
        needed = NEEDED_RE.findall(readelf(path, "-d"))
        dependencies = {
            "needed": needed,
            "resolved": [],
            "host_dependencies": [],
            "unresolved": [],
            "unexpected_host": [],
            "resolution_scope": "link-time-sysroot",
        }
    else:
        dependencies = ldd_for(path, library_root, bundle_root)
        dependencies["resolution_scope"] = "runtime"
    return {
        "path": display_path,
        "machine": machine,
        **dependencies,
    }


def conda_component(url: str) -> dict:
    parsed = urlparse(url)
    filename = Path(parsed.path).name
    if filename.endswith(".tar.bz2"):
        stem = filename[:-8]
    elif filename.endswith(".conda"):
        stem = filename[:-6]
    else:
        raise ValueError(f"unsupported Conda package filename: {filename}")
    parts = stem.rsplit("-", 2)
    if len(parts) != 3:
        raise ValueError(f"cannot parse package name/version/build: {filename}")
    name, version, build = parts
    md5 = parsed.fragment
    component = {
        "type": "library",
        "name": name,
        "version": version,
        "bom-ref": f"pkg:conda/{quote(name, safe='._-')}@{quote(version, safe='._-')}?build={quote(build, safe='._-')}",
        "purl": f"pkg:conda/{quote(name, safe='._-')}@{quote(version, safe='._-')}?build={quote(build, safe='._-')}",
        "externalReferences": [{"type": "distribution", "url": url.split("#", 1)[0]}],
        "properties": [
            {"name": "conda:build", "value": build},
            {"name": "conda:channel", "value": f"{parsed.scheme}://{parsed.netloc}"},
        ],
    }
    if re.fullmatch(r"[0-9a-fA-F]{32}", md5):
        component["hashes"] = [{"alg": "MD5", "content": md5.lower()}]
    return component


def write_reports(args: argparse.Namespace, elf_rows: list[dict], failures: list[str]) -> None:
    args.out_dir.mkdir(parents=True, exist_ok=True)
    lock_sha = sha256_file(args.lock)
    appimage_sha = sha256_file(args.appimage)
    archive_sha = sha256_file(args.toolchain_archive)
    appdir_stats = file_stats(args.appdir)
    toolchain_stats = file_stats(args.toolchain_dir)
    elf_report = {
        "schema_version": 1,
        "source_commit": args.source_sha,
        "target": "linux-64",
        "lock_sha256": lock_sha,
        "appimage_sha256": appimage_sha,
        "elf_count": len(elf_rows),
        "host_elf_allowlist": sorted(HOST_ELF_ALLOWLIST),
        "failures": failures,
        "files": elf_rows,
        "status": "passed" if not failures else "failed",
    }
    size_report = {
        "schema_version": 1,
        "source_commit": args.source_sha,
        "target": "linux-64",
        "appimage": {"path": args.appimage.name, "bytes": args.appimage.stat().st_size, "sha256": appimage_sha},
        "toolchain_archive": {"path": args.toolchain_archive.name, "bytes": args.toolchain_archive.stat().st_size, "sha256": archive_sha},
        "lock": {"path": str(args.lock), "bytes": args.lock.stat().st_size, "sha256": lock_sha},
        "appdir": appdir_stats,
        "relocated_toolchain": toolchain_stats,
    }
    urls = [line.strip() for line in args.lock.read_text(encoding="utf-8").splitlines() if line.startswith("https://")]
    packages = [conda_component(url) for url in urls]
    component = {
        "type": "application",
        "name": "DQIX Enhanced Builder",
        "version": args.version,
        "bom-ref": f"pkg:generic/dqix-enhanced-builder@{quote(args.version, safe='._-')}",
        "hashes": [{"alg": "SHA-256", "content": appimage_sha}],
        "properties": [
            {"name": "source:git:commit", "value": args.source_sha},
            {"name": "conda:lock:sha256", "value": lock_sha},
            {"name": "toolchain:archive:sha256", "value": archive_sha},
        ],
    }
    timestamp = datetime.fromisoformat(args.timestamp.replace("Z", "+00:00")).astimezone(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")
    sbom = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "serialNumber": "urn:uuid:" + str(uuid.uuid5(uuid.NAMESPACE_URL, f"{args.source_sha}:{lock_sha}:{appimage_sha}")),
        "version": 1,
        "metadata": {
            "timestamp": timestamp,
            "tools": [{"vendor": "DQIX Enhanced fork", "name": "audit_appimage.py", "version": "1"}],
            "component": component,
            "properties": [
                {"name": "scope", "value": "bundled Conda toolchain packages; excludes host libraries and project source files"},
                {"name": "source:git:commit", "value": args.source_sha},
                {"name": "conda:lock:sha256", "value": lock_sha},
            ],
        },
        "components": packages,
    }
    (args.out_dir / "elf-audit.json").write_text(json.dumps(elf_report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (args.out_dir / "size-report.json").write_text(json.dumps(size_report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (args.out_dir / "sbom.cdx.json").write_text(json.dumps(sbom, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    diagnostics = [
        args.out_dir / "elf-audit.json",
        args.out_dir / "size-report.json",
        args.out_dir / "sbom.cdx.json",
    ]
    checksum_lines = [f"{sha256_file(path)}  {path.name}" for path in diagnostics]
    (args.out_dir / "DIAGNOSTICS-SHA256SUMS").write_text(
        "\n".join(checksum_lines) + "\n", encoding="utf-8"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--appdir", type=Path, required=True)
    parser.add_argument("--appimage", type=Path, required=True)
    parser.add_argument("--toolchain-dir", type=Path, required=True)
    parser.add_argument("--toolchain-archive", type=Path, required=True)
    parser.add_argument("--lock", type=Path, required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--timestamp", required=True, help="ISO 8601 source commit timestamp")
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()

    for path in (args.appdir, args.toolchain_dir, args.appimage, args.toolchain_archive, args.lock):
        if not path.exists():
            parser.error(f"required audit input does not exist: {path}")
    if not re.fullmatch(r"[0-9a-f]{40}", args.source_sha):
        parser.error("--source-sha must be a full lowercase Git SHA")
    if not shutil_which("readelf") or not shutil_which("ldd"):
        parser.error("readelf and ldd must be available on the CI runner")

    candidates: list[tuple[Path, str, Path | None, Path | None, bool]] = []
    candidates.append((args.appimage, args.appimage.name, None, None, False))
    seen: set[Path] = {args.appimage.resolve()}

    # Scan the actual AppDir separately from the relocated Conda tree. This catches
    # ELF payloads hidden under source paths or given unusual file extensions.
    for path in walk_files(args.appdir):
        try:
            with path.open("rb") as stream:
                if stream.read(4) != b"\x7fELF":
                    continue
            real = path.resolve()
            if real in seen:
                continue
            seen.add(real)
            candidates.append((
                path,
                f"AppDir/{path.relative_to(args.appdir).as_posix()}",
                # AppRun exports the relocated Conda environment's lib/ in
                # LD_LIBRARY_PATH before it launches this binary. Audit against
                # that same library root instead of the CI host defaults.
                args.toolchain_dir / "lib",
                args.toolchain_dir,
                False,
            ))
        except OSError:
            continue

    # Then scan every ELF in the extracted and relocated Conda toolchain, including
    # the compiler's link-time sysroot (reported separately, not host-loader tested).
    for path in walk_files(args.toolchain_dir):
        try:
            with path.open("rb") as stream:
                if stream.read(4) != b"\x7fELF":
                    continue
            real = path.resolve()
            if real in seen:
                continue
            seen.add(real)
            relative = path.relative_to(args.toolchain_dir).as_posix()
            is_link_time_sysroot = "/sysroot/" in "/" + relative + "/"
            candidates.append((
                path,
                relative,
                args.toolchain_dir / "lib",
                args.toolchain_dir,
                is_link_time_sysroot,
            ))
        except OSError:
            continue

    elf_rows = []
    failures: list[str] = []
    for path, display, lib_root, bundle_root, link_time_sysroot in candidates:
        try:
            row = inspect_elf(path, display, lib_root, bundle_root, link_time_sysroot)
        except RuntimeError as exc:
            failures.append(str(exc))
            continue
        if row["machine"] != "Advanced Micro Devices X86-64":
            failures.append(f"{display}: unexpected ELF machine {row['machine']}")
        if row["unresolved"]:
            failures.append(f"{display}: unresolved dependencies: {', '.join(row['unresolved'])}")
        if row["unexpected_host"]:
            failures.append(f"{display}: unexpected host dependencies: {', '.join(row['unexpected_host'])}")
        elf_rows.append(row)

    if not elf_rows:
        failures.append("No ELF files were discovered in the AppImage runtime/toolchain")
    write_reports(args, elf_rows, failures)
    print(
        f"ELF audit: {len(elf_rows)} ELF files, {len(failures)} finding(s); "
        f"reports written under {args.out_dir}"
    )
    print(f"Size report: AppImage={args.appimage.stat().st_size} bytes; "
          f"toolchain archive={args.toolchain_archive.stat().st_size} bytes; "
          f"relocated toolchain={file_stats(args.toolchain_dir)['total_bytes']} bytes")
    print(f"Conda SBOM: {sum(1 for x in args.lock.read_text(encoding='utf-8').splitlines() if x.startswith('https://'))} components")
    if failures:
        for failure in failures[:50]:
            print(f"ELF AUDIT FAILURE: {failure}", file=sys.stderr)
        return 1
    return 0


def shutil_which(name: str) -> str | None:
    # Keep import set small and make the required external tools explicit.
    from shutil import which
    return which(name)


if __name__ == "__main__":
    raise SystemExit(main())
