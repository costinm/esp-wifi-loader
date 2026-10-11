#!/usr/bin/env python3
"""Sign a tree of unsigned firmware with one application key.

Input: <input>/<cpu>/main.bin (or main-app.bin), and optional
<input>/<cpu>/modules/mod_<name>.dmod. Output: <output>/<cpu>.wlpkg,
<output>/<cpu>-<name>.wlpkg, and SHA256SUMS. Container paths default to
/input, /key/key.pem and /output; native callers pass paths explicitly.
"""
import argparse
import hashlib
from pathlib import Path
import sys

import sign
import wl_format as wf


def bundle(source: Path, key: Path, output: Path, version: int, fqdn: str,
           expected_pubkey: Path | None = None) -> list[Path]:
    if not source.is_dir():
        raise ValueError(f"input directory is missing: {source}")
    if not key.is_file():
        raise ValueError(f"private key is missing: {key}")
    if expected_pubkey and sign.public_key(str(key)).hex() != expected_pubkey.read_text().strip():
        raise ValueError(f"private key does not match {expected_pubkey}")
    jobs = []
    for chip in sorted(source.iterdir()):
        if not chip.is_dir():
            continue
        mains = [chip / name for name in ("main.bin", "main-app.bin") if (chip / name).is_file()]
        if not mains:
            continue
        if chip.name not in wf.CHIPS:
            raise ValueError(f"unknown chip directory: {chip.name}")
        if len(mains) != 1:
            raise ValueError(f"expected one main.bin or main-app.bin in {chip}")
        jobs.append((mains[0], f"{chip.name}.wlpkg", chip.name, "main", fqdn))
        for module in sorted((chip / "modules").glob("*.dmod")):
            name = module.stem.removeprefix("mod_")
            if not name:
                raise ValueError(f"bad module name: {module}")
            jobs.append((module, f"{chip.name}-{name}.wlpkg", chip.name, "module", name))
    if not jobs:
        raise ValueError(f"no <cpu>/main.bin or main-app.bin under {source}")
    output.mkdir(parents=True, exist_ok=True)
    if any(output.glob("*.wlpkg")) or (output / "SHA256SUMS").exists():
        raise ValueError(f"output already contains signed packages: {output}")
    results = []
    for image, filename, cpu, target, name in jobs:
        package = sign.make_package(str(key), cpu, version, name, image.read_bytes(), target)
        path = output / filename
        path.write_bytes(package)
        print(f"wrote {path} (target {target}, {len(package)} bytes)")
        results.append(path)
    (output / "SHA256SUMS").write_text("".join(
        f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n" for path in results))
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=Path("/input"))
    parser.add_argument("--key", type=Path, default=Path("/key/key.pem"))
    parser.add_argument("--output", type=Path, default=Path("/output"))
    parser.add_argument("--version", type=int, required=True)
    parser.add_argument("--fqdn", required=True, help="Main's release source, e.g. owner/repo:latest")
    parser.add_argument("--pubkey", type=Path, help="expected public key in hex")
    args = parser.parse_args()
    try:
        bundle(args.input, args.key, args.output, args.version, args.fqdn, args.pubkey)
    except (ValueError, OSError) as exc:
        parser.exit(1, f"esp-wifi-sign: {exc}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
