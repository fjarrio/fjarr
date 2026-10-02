#!/usr/bin/env python3
"""CI images by content hash (docs/30-continuous-integration.md#ci-images).

  image.py compose <service>...                    images of docker-compose.yml's services
  image.py build <local-tag> <context> [dockerfile] an image outside compose (the deb builder)
  image.py tag <context> <dockerfile> [arg=value]... print the content tag only (tests, debugging)

Each image is tagged `<registry>:<name>-<hash>`, the hash covering the Dockerfile, the build
arguments and target, every tracked file a COPY or ADD names, the architecture and the ISO week.
It is pulled when it exists and built otherwise; FJARR_CI_PUSH=1 pushes what was built (CI sets it
only for pushes to main, the schedule and manual runs). Either way it ends up under the local name
compose and the Makefile expect. For compose services an override dropping their `build:` is added
to COMPOSE_FILE (through GITHUB_ENV), so a later `up --build` uses the pulled image.
"""
import datetime
import hashlib
import json
import os
import platform
import shlex
import subprocess
import sys

SCHEME = "1"  # bump when what goes into the hash changes
REGISTRY = os.environ.get("FJARR_CI_REGISTRY", "ghcr.io/fjarrio/fjarr-ci")
ROOT = os.path.realpath(os.path.join(os.path.dirname(__file__), "..", ".."))
OVERRIDE = os.path.join(ROOT, "build", "ci", "compose.images.yml")


def run(*cmd, capture=False, check=True):
    r = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=capture)
    if check and r.returncode != 0:
        sys.exit(f"image.py: {' '.join(cmd)} failed ({r.returncode})\n{r.stderr if capture else ''}")
    return r


def instructions(text):
    """The Dockerfile's instructions, continuation lines joined, comments dropped."""
    out, cur = [], ""
    for line in text.splitlines():
        s = line.strip()
        if not cur and (not s or s.startswith("#")):
            continue
        if cur and s.startswith("#"):
            continue
        if s.endswith("\\"):
            cur += s[:-1] + " "
            continue
        out.append(cur + s)
        cur = ""
    if cur:
        out.append(cur)
    return out


def copy_sources(dockerfile_text):
    """Every build-context path a COPY or ADD reads (not --from=, which reads another stage)."""
    sources = []
    for ins in instructions(dockerfile_text):
        word, _, rest = ins.partition(" ")
        if word.upper() not in ("COPY", "ADD"):
            continue
        rest = rest.strip()
        if rest.startswith("["):
            args = json.loads(rest)
        else:
            args = shlex.split(rest)
        flags = [a for a in args if a.startswith("--")]
        if any(f.startswith("--from") for f in flags):
            continue
        paths = [a for a in args if not a.startswith("--")]
        sources += paths[:-1]
    return sources


def tracked(context, sources):
    """`git ls-files -s` for each source under the context; a plain path that matches nothing fails:
    the image would read a file the hash cannot see."""
    lines = []
    for src in sources:
        rel = os.path.relpath(os.path.normpath(os.path.join(context, src)), ROOT)
        out = run("git", "ls-files", "-s", "--", rel, capture=True).stdout.strip()
        if not out and not any(c in src for c in "*?["):
            sys.exit(f"image.py: {src} (COPY in {context}) is not a tracked file; its content cannot be hashed")
        lines.append(f"{src}\n{out}")
    return lines


def content_tag(name, context, dockerfile, args, target=None, week=None):
    with open(dockerfile, encoding="utf-8") as f:
        text = f.read()
    week = week or "%d-W%02d" % datetime.date.today().isocalendar()[:2]
    h = hashlib.sha256()
    for part in [SCHEME, platform.machine(), week, target or "", text,
                 *sorted(f"{k}={v}" for k, v in args.items()),
                 *tracked(context, copy_sources(text))]:
        h.update(part.encode())
        h.update(b"\0")
    return f"{REGISTRY}:{name}-{h.hexdigest()[:16]}"


def ensure(local, context, dockerfile, args, target=None):
    name = local.split("/")[-1].split(":")[0].removeprefix("fjarr-")
    remote = content_tag(name, context, dockerfile, args, target)
    if run("docker", "pull", "-q", remote, capture=True, check=False).returncode == 0:
        print(f"image.py: {local} <- {remote} (pulled)")
    else:
        print(f"image.py: {remote} not in the registry; building {local}")
        cmd = ["docker", "build", "-t", remote, "-f", dockerfile]
        if target:
            cmd += ["--target", target]
        for k, v in sorted(args.items()):
            cmd += ["--build-arg", f"{k}={v}"]
        run(*cmd, context)
        if os.environ.get("FJARR_CI_PUSH") == "1":
            if run("docker", "push", "-q", remote, capture=True, check=False).returncode == 0:
                print(f"image.py: pushed {remote}")
            else:
                print(f"::warning::image.py: could not push {remote}; the next run builds it again")
    run("docker", "tag", remote, local)


def compose_services(names):
    cfg = json.loads(run("docker", "compose", "-f", "docker-compose.yml", "--profile", "*",
                         "config", "--format", "json", capture=True).stdout)
    done = []
    for n in names:
        svc = cfg["services"].get(n)
        if not svc or "build" not in svc:
            sys.exit(f"image.py: compose service {n} has no build section")
        b = svc["build"]
        ensure(svc["image"], b["context"], os.path.join(b["context"], b.get("dockerfile", "Dockerfile")),
               b.get("args") or {}, b.get("target"))
        done.append(n)
    # The override: these services' images are ready, so compose must not build them again.
    os.makedirs(os.path.dirname(OVERRIDE), exist_ok=True)
    have = []
    if os.path.exists(OVERRIDE):
        have = [l.strip()[:-1] for l in open(OVERRIDE) if l.startswith("  ") and not l.startswith("    ")]
    services = sorted(set(have) | set(done))
    with open(OVERRIDE, "w") as f:
        f.write("# Written by tools/ci/image.py (docs/30#ci-images): these images were pulled or built by\n"
                "# content hash, so compose uses them as they are.\nservices:\n")
        for s in services:
            f.write(f"  {s}:\n    build: !reset null\n")
    env = os.environ.get("GITHUB_ENV")
    if env:
        with open(env, "a") as f:
            f.write(f"COMPOSE_FILE=docker-compose.yml:{os.path.relpath(OVERRIDE, ROOT)}\n")


def main(argv):
    sys.stdout.reconfigure(line_buffering=True)  # in order with docker's own output in the log
    if len(argv) >= 2 and argv[0] == "compose":
        compose_services(argv[1:])
    elif len(argv) in (3, 4) and argv[0] == "build":
        context = os.path.join(ROOT, argv[2])
        ensure(argv[1], context, os.path.join(ROOT, argv[3]) if len(argv) == 4 else os.path.join(context, "Dockerfile"), {})
    elif len(argv) >= 3 and argv[0] == "tag":
        args = dict(a.split("=", 1) for a in argv[3:])
        print(content_tag("x", os.path.join(ROOT, argv[1]), os.path.join(ROOT, argv[2]), args, week="test"))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
