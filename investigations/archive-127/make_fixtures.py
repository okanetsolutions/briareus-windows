"""Test-only stdlib generator/oracle; Python/zlib is NOT a proposed app dependency."""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import tarfile
import tempfile
import zlib

ROOT = "owner-project-c0ffee"
ZERO = b"\0" * 512


def entry(path, data=b"", kind=b"0", link="", declared=None):
    info = tarfile.TarInfo(path)
    info.type = kind
    info.linkname = link
    info.size = len(data) if declared is None else declared
    return info.tobuf(format=tarfile.USTAR_FORMAT) + data + b"\0" * (-len(data) % 512)


def pax_record(key, value):
    tail = f" {key}={value}\n".encode()
    size = len(tail) + 1
    while len(str(size)) + len(tail) != size:
        size = len(str(size)) + len(tail)
    return str(size).encode() + tail


def strict_gzip(path, cap=512 * 1024 * 1024):
    """Independent streaming oracle: requires zlib EOF, no unused input, bounded output."""
    dec = zlib.decompressobj(31)
    total = 0
    with path.open("rb") as source:
        while chunk := source.read(65536):
            if dec.eof:
                raise ValueError("trailing gzip input")
            while chunk:
                out = dec.decompress(chunk, min(65536, cap - total + 1))
                total += len(out)
                if total > cap:
                    raise ValueError("raw limit")
                if dec.unused_data:
                    raise ValueError("trailing gzip input")
                chunk = dec.unconsumed_tail
    if not dec.eof:
        raise ValueError("gzip missing EOF")
    return total


def generate(out, large):
    out.mkdir(parents=True, exist_ok=True)
    cases = []
    good_file = entry(ROOT + "/src/a.cs", b"class A {}\n")
    base = entry(ROOT + "/", kind=b"5")
    good = base + good_file + ZERO * 2

    def save(name, raw=None, compressed=None, result="REJECT", paths=None, **limits):
        path = out / (name + ".tgz")
        path.write_bytes(gzip.compress(raw, mtime=0) if compressed is None else compressed)
        cases.append(dict(name=name, file=path.name, result=result,
                          selected=["src/a.cs"] if paths is None else paths, **limits))

    save("valid-root", good, result="ACCEPT")
    save("valid-empty", base + ZERO * 2, paths=[], result="ACCEPT")
    global_header = entry("pax_global_header", pax_record("comment", "c" * 40), b"g")
    save("valid-github-pax", global_header + good, result="ACCEPT")
    long_path = "src/" + "a" * 95 + "/file.cs"
    pax = entry(ROOT + "/paxheader", pax_record("path", ROOT + "/" + long_path), b"x")
    save("valid-long-pax", base + pax + entry(ROOT + "/placeholder", b"class Long {}") + ZERO * 2,
         paths=[long_path], result="ACCEPT")
    save("missing-root", entry("a.cs", b"ok") + ZERO * 2)
    save("multiple-roots", good[:-1024] + entry("other/file.cs", b"bad") + ZERO * 2)
    save("missing-tree-file", base + ZERO * 2)
    for name, path in {
        "traversal": ROOT + "/../../escape.cs",
        "absolute": "/tmp/escape.cs",
        "drive": "C:/escape.cs",
        "unc": "//server/share/escape.cs",
        "backslash": ROOT + "/..\\escape.cs",
        "ads": ROOT + "/src/a.cs:stream",
        "device": ROOT + "/CON.cs",
        "superscript-device": ROOT + "/COM\u00b9.cs",
        "dot": ROOT + "/./a.cs",
        "empty-component": ROOT + "//a.cs",
        "trailing-dot": ROOT + "/dir./a.cs",
        "trailing-space": ROOT + "/dir /a.cs",
        "newline": ROOT + "/src/a\nb.cs",
    }.items():
        # Safe file FIRST: reject a later unselected entry before any candidate write.
        save(name, good[:-1024] + entry(path, b"bad") + ZERO * 2)
    for name, kind in {"symlink": b"2", "hardlink": b"1", "fifo": b"6", "char-device": b"3",
                       "block-device": b"4", "gnu-sparse": b"S", "gnu-longname": b"L"}.items():
        save(name, good[:-1024] + entry(ROOT + "/special", kind=kind, link="../../escape" if kind in (b"1", b"2") else "") + ZERO * 2)
    save("regular-link-target", base + entry(ROOT + "/src/a.cs", b"ok", link="elsewhere") + ZERO * 2)
    save("duplicate", good[:-1024] + good_file + ZERO * 2)
    save("case-collision", good[:-1024] + entry(ROOT + "/src/A.cs", b"bad") + ZERO * 2)
    save("file-as-parent", good[:-1024] + entry(ROOT + "/src/a.cs/child", b"bad") + ZERO * 2)
    save("parent-as-file", good[:-1024] + entry(ROOT + "/src", b"bad") + ZERO * 2)
    save("pax-traversal", base + entry(ROOT + "/pax", pax_record("path", ROOT + "/../escape"), b"x") + good_file + ZERO * 2)
    save("pax-link-override", base + entry(ROOT + "/pax", pax_record("linkpath", "../../escape"), b"x") + good_file + ZERO * 2)
    save("pax-sparse", base + entry(ROOT + "/pax", pax_record("GNU.sparse.size", "999999"), b"x") + good_file + ZERO * 2)
    save("pax-oversized", base + entry(ROOT + "/pax", b"a" * 65537, b"x") + good_file + ZERO * 2)
    save("pax-invalid-record", base + entry(ROOT + "/pax", b"999 path=x\n", b"x") + good_file + ZERO * 2)
    save("pax-orphan", base + entry(ROOT + "/pax", pax_record("path", ROOT + "/src/a.cs"), b"x") + ZERO * 2)
    save("directory-data", entry(ROOT + "/", b"bad", b"5") + good_file + ZERO * 2)
    broken = bytearray(good)
    broken[0] ^= 1
    save("bad-tar-checksum", bytes(broken))
    save("tar-no-end", good[:-1024])
    save("tar-one-end", good[:-512])
    save("tar-truncated-payload", base + good_file[:513])
    save("tar-trailing-junk", good + b"junk")
    gz = gzip.compress(good, mtime=0)
    save("gzip-missing-footer", compressed=gz[:-8])
    save("gzip-short-footer", compressed=gz[:-3])
    save("gzip-truncated-deflate", compressed=gz[:len(gz) // 2])
    save("gzip-bad-crc", compressed=gz[:-8] + bytes([gz[-8] ^ 1]) + gz[-7:])
    save("gzip-bad-isize", compressed=gz[:-4] + struct.pack("<I", len(good) + 1))
    save("gzip-concatenated", compressed=gz + gz, result="SECURITY_PROBE")
    save("gzip-trailing-junk", compressed=gz + b"junk")
    # The managed decoder may stop at the first member and read ahead through junk;
    # checking only the final 8 bytes against output does not establish consumption.
    save("gzip-forged-trailing-footer", compressed=gz + b"NOT-A-GZIP-MEMBER" + gz[-8:], result="SECURITY_PROBE")
    save("truncated-body-known-length", compressed=gz[:-8], declaredLength=len(gz))
    save("truncated-body-unknown-length", compressed=gz[:-8], unknownLength=True)
    save("compressed-streaming-limit-scaled", good, compressedLimit=len(gz) - 1, unknownLength=True)
    save("exact-boundaries-scaled", good, compressedLimit=len(gz), rawLimit=len(good),
         fileLimit=11, indexLimit=11, fileCountLimit=1, result="ACCEPT")
    save("compressed-limit-scaled", good, compressedLimit=len(gz) - 1)
    save("raw-limit-scaled", good, rawLimit=len(good) - 1)
    save("file-limit-real", base + entry(ROOT + "/src/a.cs", b"x" * (1024 * 1024 + 1)) + ZERO * 2)
    save("index-limit-scaled", good, indexLimit=10)
    save("file-count-scaled", good, fileCountLimit=0)
    paths = [f"src/f{i}.cs" for i in range(5001)]
    save("file-count-real", base + b"".join(entry(ROOT + "/" + p, b"x") for p in paths) + ZERO * 2, paths=paths)
    save("cancel-cooperative", base + entry(ROOT + "/src/a.cs", b"x" * 1048576) + ZERO * 2,
         result="CANCEL", delayMs=30, cancelAfterReadyMs=60)
    save("cancel-forced", good, result="KILL", hang=True, cancelAfterReadyMs=60)
    if large:
        # Sparse compressed-input file: preflight must reject without reading/allocating it.
        huge = out / "compressed-limit-real.tgz"
        with huge.open("wb") as f:
            f.truncate(300 * 1024 * 1024 + 1)
        cases.append(dict(name="compressed-limit-real", file=huge.name, result="REJECT", selected=[]))
        cases.append(dict(name="compressed-streaming-limit-real", file=huge.name, result="REJECT", selected=[], unknownLength=True))
        # Streaming generator: the raw-limit case should stop without persisting >512 MiB.
        with (out / "raw-limit-real.tgz").open("wb") as compressed_file, gzip.GzipFile(filename="", mode="wb", fileobj=compressed_file, mtime=0) as f:
            block = b"\0" * 1048576
            for _ in range(513):
                f.write(block)
        cases.append(dict(name="raw-limit-real", file="raw-limit-real.tgz", result="REJECT", selected=[]))
        paths = [f"src/f{i}.cs" for i in range(193)]
        with (out / "index-limit-real.tgz").open("wb") as compressed_file, gzip.GzipFile(filename="", mode="wb", fileobj=compressed_file, mtime=0) as f:
            f.write(base)
            for p in paths:
                f.write(entry(ROOT + "/" + p, b"x" * 1048576))
            f.write(ZERO * 2)
        cases.append(dict(name="index-limit-real", file="index-limit-real.tgz", result="REJECT", selected=paths))
    (out / "cases.json").write_text(json.dumps(cases, indent=2) + "\n")
    oracle = []
    for case in cases:
        if case["name"].startswith("gzip-") or case["name"] == "valid-root":
            try:
                oracle.append(dict(name=case["name"], result="ACCEPT", rawBytes=strict_gzip(out / case["file"])))
            except (ValueError, zlib.error) as exc:
                oracle.append(dict(name=case["name"], result="REJECT", reason=str(exc)))
    (out / "oracle.json").write_text(json.dumps(oracle, indent=2) + "\n")


def export_fixture(out):
    with tempfile.TemporaryDirectory(prefix="archive-127-git-") as tmp:
        repo = Path(tmp)
        def git(*args):
            return subprocess.check_output(["git", "-C", str(repo), *args])
        git("init", "-q")
        git("config", "user.name", "Archive feasibility fixture")
        git("config", "user.email", "fixture@example.invalid")
        # Git for Windows runner defaults must not introduce an unrelated EOL conversion.
        git("config", "core.autocrlf", "false")
        git("config", "core.eol", "lf")
        (repo / "src").mkdir()
        (repo / ".gitattributes").write_bytes(b"src/hidden.cs export-ignore\nsrc/version.cs export-subst\n.gitattributes export-ignore\n")
        (repo / "src/hidden.cs").write_bytes(b"class Hidden {}\n")
        (repo / "src/version.cs").write_bytes(b'// $Format:%H$\nclass Version {}\n')
        (repo / "src/plain.cs").write_bytes(b"class Plain {}\n")
        git("add", ".")
        git("commit", "-qm", "Export fixture")
        sha = git("rev-parse", "HEAD").decode().strip()
        raw = git("archive", "--format=tar", "--prefix=" + ROOT + "/", sha)
        (out / "export-attributes.tgz").write_bytes(gzip.compress(raw, mtime=0))
        with tarfile.open(fileobj=io.BytesIO(raw)) as archive:
            paths = archive.getnames()
            changed = archive.extractfile(ROOT + "/src/version.cs").read()
        original = git("show", sha + ":src/version.cs")
        plain = git("show", sha + ":src/plain.cs")
        assert ROOT + "/src/hidden.cs" not in paths
        assert ROOT + "/.gitattributes" not in paths
        assert original != changed and sha.encode() in changed
        with tarfile.open(fileobj=io.BytesIO(raw)) as archive:
            assert archive.extractfile(ROOT + "/src/plain.cs").read() == plain
        evidence = dict(sha=sha, tree=git("ls-tree", "-r", "--name-only", sha).decode().splitlines(),
                        archive=paths, canonicalVersion=original.decode(), archiveVersion=changed.decode(),
                        canonicalSha256=hashlib.sha256(original).hexdigest(), archiveSha256=hashlib.sha256(changed).hexdigest(),
                        fallback="Whole index uses canonical per-file reads because the TREE contains .gitattributes, even though the archive omits it.")
        (out / "export-results.json").write_text(json.dumps(evidence, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("out", type=Path)
    parser.add_argument("--large", action="store_true")
    args = parser.parse_args()
    generate(args.out, args.large)
    export_fixture(args.out)
    print(f"Fixtures and strict-gzip/export evidence: {args.out}")
