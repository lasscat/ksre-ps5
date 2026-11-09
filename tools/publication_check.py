#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Publication check for the public ksre-ps5 repository.

Scans exactly what would be published -- not the working tree -- for
content that must stay local: personal workflow files, console
configuration, logs, game data, build output, credentials, home-directory
paths, private LAN addresses and personal e-mail addresses.

Modes (pick one):
  --staged             every blob in the index (what the next commit contains)
  --tracked            every blob in HEAD
  --outgoing [REF]     every commit in REF..HEAD (default REF: the upstream of
                       the current branch; all of HEAD's history if none),
                       including author/committer e-mail addresses
  --all-history        every commit reachable from HEAD (used by CI)

Exit status: 0 = clean, 1 = findings, 2 = usage/git error.
A line containing the marker "publication-check: allow" is exempt from
content rules (use sparingly, e.g. for documented placeholder examples).
"""

import argparse
import fnmatch
import re
import subprocess
import sys

MAX_BYTES = 2 * 1024 * 1024
ALLOW_MARKER = "publication-check: allow"

# Paths that must never be published (fnmatch patterns, matched against the
# full repository-relative path and against each path component).
FORBIDDEN_PREFIXES = (
    ".local-work/", ".local-config/", "external/", "game-data/", "build/",
    "dist/", "out/", ".claude/projects/", ".claude/todos/",
)
FORBIDDEN_FILES = (
    "CLAUDE.local.md", ".claude/settings.local.json",
)
FORBIDDEN_GLOBS = (
    "*.log", ".env", ".env.*", "*.env", "*.pem", "*.key", "*.p12", "*.pfx",
    "id_rsa*", "id_ed25519*", ".netrc", "*.netrc", "credentials*",
    "*.elf", "*.self", "*.pkg", "*.bin", "*.o", "*.a", "*.so", "*.dylib",
    "*.rpyc", "*.rpymc", "*.rpa", "*.save", "persistent", "traceback.txt",
    "errors.txt", "log.txt", "*.core", "core", "core.*", "*.dmp", "*.crash",
    "klog*.txt",
)
ALLOWED_GLOBS = (".env.example", "config/*.example")

# Game media/fonts are blocked everywhere except small synthetic fixtures.
MEDIA_GLOBS = ("*.mkv", "*.webm", "*.mp4", "*.ogg", "*.opus", "*.mp3", "*.wav",
               "*.flac", "*.ttf", "*.otf", "*.woff", "*.woff2")
IMAGE_GLOBS = ("*.png", "*.jpg", "*.jpeg", "*.webp", "*.avif", "*.gif")
FIXTURE_DIRS = ("fixtures/", "docs/images/")
FIXTURE_MAX_BYTES = 512 * 1024

REQUIRED_IGNORES = ("/.local-work/", "/.local-config/", "/external/",
                    "/game-data/", "/build/", "/dist/")

EMAIL_ALLOW = (
    re.compile(r"^noreply@anthropic\.com$", re.I),
    re.compile(r"^[0-9]+\+[A-Za-z0-9-]+@users\.noreply\.github\.com$", re.I),
    re.compile(r"^[A-Za-z0-9-]+@users\.noreply\.github\.com$", re.I),
    re.compile(r"@example\.(com|org|net)$", re.I),
    re.compile(r"^git@(github\.com|codeberg\.org)$", re.I),
)
HOME_ALLOW = {"you", "user", "USER", "username", "runner", "<user>", "<you>"}

CONTENT_RULES = [
    ("private-key", re.compile(r"-----BEGIN (?:RSA |EC |DSA |OPENSSH |PGP |ENCRYPTED )?PRIVATE KEY-----")),
    ("github-token", re.compile(r"\b(?:gh[pousr]_[A-Za-z0-9]{36,}|github_pat_[A-Za-z0-9_]{40,})\b")),
    ("anthropic-key", re.compile(r"\bsk-ant-[A-Za-z0-9_-]{20,}")),
    ("aws-key", re.compile(r"\bAKIA[0-9A-Z]{16}\b")),
    ("slack-token", re.compile(r"\bxox[abprs]-[A-Za-z0-9-]{10,}")),
    ("google-key", re.compile(r"\bAIza[0-9A-Za-z_-]{35}\b")),
    ("generic-secret", re.compile(
        r"(?i)\b(?:password|passwd|secret|api[_-]?key|access[_-]?token|auth[_-]?token)\b"
        r"\s*[:=]\s*['\"][^'\"\s<>{}$]{8,}['\"]")),
    ("private-ipv4", re.compile(
        r"(?<![\d.])(?:10\.(?:\d{1,3}\.){2}\d{1,3}|192\.168\.\d{1,3}\.\d{1,3}"
        r"|172\.(?:1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3})(?![\d.])")),
]
HOME_RE = re.compile(r"(?:/Users|/home)/([^/\s'\"`)\]]+)/")
EMAIL_RE = re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}")


def git(*args, binary=False, check=True):
    r = subprocess.run(["git", *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if check and r.returncode != 0:
        sys.stderr.write(r.stderr.decode(errors="replace"))
        raise SystemExit(2)
    return r.stdout if binary else r.stdout.decode(errors="replace")


def match_any(path, globs):
    name = path.rsplit("/", 1)[-1]
    return any(fnmatch.fnmatch(path, g) or fnmatch.fnmatch(name, g) for g in globs)


def email_allowed(addr):
    return any(p.search(addr) for p in EMAIL_ALLOW)


class Checker:
    def __init__(self):
        self.findings = []

    def add(self, where, rule, detail):
        self.findings.append((where, rule, detail))

    def check_path(self, path, size, where):
        if match_any(path, ALLOWED_GLOBS):
            return
        for p in FORBIDDEN_PREFIXES:
            if path.startswith(p) or ("/" + p) in ("/" + path):
                self.add(where, "forbidden-path", f"{p} must stay local")
                return
        if path in FORBIDDEN_FILES or match_any(path, FORBIDDEN_GLOBS):
            self.add(where, "forbidden-file", "private, generated or game-runtime file")
            return
        in_fixture = path.startswith(FIXTURE_DIRS)
        if match_any(path, MEDIA_GLOBS) and not (in_fixture and size <= FIXTURE_MAX_BYTES):
            self.add(where, "media", "media/font files only allowed as small original fixtures")
        elif match_any(path, IMAGE_GLOBS) and not (in_fixture and size <= FIXTURE_MAX_BYTES):
            self.add(where, "image", "images only allowed as small original fixtures/docs images")
        if size > MAX_BYTES:
            self.add(where, "too-large", f"{size} bytes > {MAX_BYTES}")

    def check_content(self, path, data, where):
        if b"\0" in data[:8192]:
            if not path.startswith(FIXTURE_DIRS):
                self.add(where, "binary", "binary file outside fixture directories")
            return
        text = data.decode("utf-8", errors="replace")
        for lineno, line in enumerate(text.splitlines(), 1):
            if ALLOW_MARKER in line:
                continue
            loc = f"{where}:{lineno}"
            for rule, rx in CONTENT_RULES:
                m = rx.search(line)
                if m:
                    self.add(loc, rule, redact(m.group(0)))
            for m in HOME_RE.finditer(line):
                if m.group(1) not in HOME_ALLOW:
                    self.add(loc, "home-path", redact(m.group(0)))
            for m in EMAIL_RE.finditer(line):
                if not email_allowed(m.group(0)):
                    self.add(loc, "email", redact(m.group(0)))
        if path == ".gitignore":
            for req in REQUIRED_IGNORES:
                if req not in text.splitlines():
                    self.add(where, "gitignore", f"missing required ignore rule {req}")


def redact(s):
    s = s.strip()
    return s if len(s) <= 8 else s[:4] + "…" + s[-2:]


def blobs_in_tree(treeish):
    out = git("ls-tree", "-r", "-z", "-l", treeish)
    for rec in out.split("\0"):
        if not rec:
            continue
        meta, path = rec.split("\t", 1)
        mode, typ, sha, size = meta.split()
        if typ == "blob":
            yield path, sha, int(size) if size != "-" else 0


def blobs_in_index():
    out = git("ls-files", "-s", "-z")
    for rec in out.split("\0"):
        if not rec:
            continue
        meta, path = rec.split("\t", 1)
        mode, sha, _stage = meta.split()
        size = int(git("cat-file", "-s", sha).strip())
        yield path, sha, size


def scan_blobs(chk, items, label):
    seen = 0
    for path, sha, size in items:
        seen += 1
        where = f"{label}:{path}"
        chk.check_path(path, size, where)
        if size <= MAX_BYTES:
            chk.check_content(path, git("cat-file", "blob", sha, binary=True), where)
    return seen


def scan_commits(chk, rev_range):
    commits = [c for c in git("rev-list", *rev_range).split() if c]
    for c in commits:
        meta = git("show", "-s", "--format=%ae%n%ce%n%B", c).splitlines()
        for addr in meta[:2]:
            if addr and not email_allowed(addr):
                chk.add(f"commit {c[:10]}", "author-email", redact(addr))
        for line in meta[2:]:
            if ALLOW_MARKER in line:
                continue
            for rule, rx in CONTENT_RULES:
                m = rx.search(line)
                if m:
                    chk.add(f"commit {c[:10]} message", rule, redact(m.group(0)))
            for m in EMAIL_RE.finditer(line):
                if not email_allowed(m.group(0)):
                    chk.add(f"commit {c[:10]} message", "email", redact(m.group(0)))
        # Every blob introduced or modified by the commit.
        raw = git("diff-tree", "--root", "-r", "-z", "--no-commit-id", "--diff-filter=AMRC", c)
        parts = [p for p in raw.split("\0") if p != ""]
        i = 0
        while i < len(parts):
            meta = parts[i].split()
            status = meta[-1] if meta else ""
            if status[:1] in ("R", "C"):
                path = parts[i + 2]
                i += 3
            else:
                path = parts[i + 1]
                i += 2
            sha = meta[3]
            size = int(git("cat-file", "-s", sha).strip())
            where = f"commit {c[:10]}:{path}"
            chk.check_path(path, size, where)
            if size <= MAX_BYTES:
                chk.check_content(path, git("cat-file", "blob", sha, binary=True), where)
    return len(commits)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--staged", action="store_true")
    g.add_argument("--tracked", action="store_true")
    g.add_argument("--outgoing", nargs="?", const="@upstream", metavar="REF")
    g.add_argument("--all-history", action="store_true")
    args = ap.parse_args()

    chk = Checker()
    if args.tracked:
        n = scan_blobs(chk, blobs_in_tree("HEAD"), "HEAD")
        summary = f"{n} files in HEAD"
    elif args.outgoing is not None or args.all_history:
        has_head = git("rev-parse", "--verify", "-q", "HEAD", check=False).strip()
        if not has_head:
            print("publication-check: no commits yet")
            return 0
        rng = ["HEAD"]
        if args.outgoing is not None:
            ref = args.outgoing
            resolved = git("rev-parse", "--verify", "-q", ref, check=False).strip()
            if resolved:
                rng = [f"{resolved}..HEAD"]
        n = scan_commits(chk, rng)
        summary = f"{n} commit(s) in {' '.join(rng)}"
    else:
        n = scan_blobs(chk, blobs_in_index(), "index")
        summary = f"{n} files in the index"

    if chk.findings:
        print(f"publication-check: FAILED ({len(chk.findings)} finding(s); scanned {summary})")
        for where, rule, detail in chk.findings:
            print(f"  {where}: [{rule}] {detail}")
        return 1
    print(f"publication-check: OK (scanned {summary})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
