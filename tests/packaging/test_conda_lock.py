#!/usr/bin/env python3
"""Validate the committed, single-platform explicit Conda lock without network access."""
from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parents[2]
ENV_SPEC = ROOT / "packaging" / "linux" / "environment.yml"
LOCK = ROOT / "packaging" / "linux" / "conda-linux-64.lock"
CHECKSUM = ROOT / "packaging" / "linux" / "conda-linux-64.lock.sha256"
METADATA = ROOT / "packaging" / "linux" / "conda-lock-meta.json"
MD5_FRAGMENT = re.compile(r"#[0-9a-fA-F]{32}$")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    metadata = json.loads(METADATA.read_text(encoding="utf-8"))
    lock_lines = LOCK.read_text(encoding="utf-8").splitlines()
    urls = [line for line in lock_lines if line.startswith("https://")]

    assert metadata["platform"] == "linux-64"
    assert "# platform: linux-64" in lock_lines
    assert "@EXPLICIT" in lock_lines
    assert metadata["source_spec_sha256"] == sha256(ENV_SPEC), (
        "environment.yml changed; regenerate and review the explicit lock"
    )
    actual_lock_sha = sha256(LOCK)
    assert metadata["lock_sha256"] == actual_lock_sha, "lock metadata hash mismatch"
    checksum_parts = CHECKSUM.read_text(encoding="utf-8").strip().split()
    assert checksum_parts == [actual_lock_sha, "packaging/linux/conda-linux-64.lock"], (
        "lock checksum file does not match the committed lock"
    )
    assert metadata["explicit_package_count"] == len(urls) > 0
    assert len(urls) == len(set(urls)), "duplicate package URL in lock"

    for url in urls:
        parsed = urlparse(url)
        assert parsed.scheme == "https" and parsed.hostname == "conda.anaconda.org"
        assert parsed.username is None and parsed.password is None
        assert "/linux-64/" in parsed.path or "/noarch/" in parsed.path, (
            f"non-Linux lock entry: {url}"
        )
        assert MD5_FRAGMENT.search(url), f"explicit package URL lacks MD5: {url}"

    print(
        "Conda lock valid: "
        f"platform=linux-64 packages={len(urls)} sha256={actual_lock_sha}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
