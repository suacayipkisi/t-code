#!/usr/bin/env python3
"""Runs the jobs of .github/workflows/ci.yml on this machine, each in the same Fedora container with podman,
on the working tree as it is (uncommitted changes included), before a push.

    tools/ci-local.py                 every job (build-test on each Fedora, asan, asan-gtk, format)
    tools/ci-local.py asan-gtk format only these jobs
    tools/ci-local.py build-test:44   one matrix entry of a job
    tools/ci-local.py -j 2            two jobs at a time (the GTK tests are timing-sensitive: slower then)
    tools/ci-local.py --changed       only the jobs what changed since origin needs (GitHub runs them all)
    tools/ci-local.py --list          the jobs it would run (with --changed: and why)

The workflow is read each time, so a change to ci.yml applies here too. Steps that use an action
(actions/checkout) are left out: the working tree is copied into the container at the job's start instead,
without .git, as the checkout leaves it there. Each job's output goes to build/ci-local/<job>.log; downloaded
packages are kept in podman volumes (tcode-ci-dnf-*) so later runs install faster. A job allowed to fail on
GitHub (continue-on-error: Rawhide) does not fail the run here either.
"""

import argparse
import concurrent.futures
import itertools
import os
import re
import shlex
import subprocess
import sys
import tarfile
import tempfile
import time

import yaml

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORKFLOW = os.path.join(ROOT, ".github", "workflows", "ci.yml")
LOGS = os.path.join(ROOT, "build", "ci-local")


def expand(text, matrix):
    """${{ matrix.x }} put in; other expressions are left as they are."""
    return re.sub(r"\$\{\{\s*matrix\.(\w+)\s*\}\}", lambda m: str(matrix.get(m.group(1), "")), str(text))


def allowed_to_fail(value, matrix):
    """continue-on-error: true/false, or ${{ matrix.x == 'y' }} (the form ci.yml uses)."""
    if isinstance(value, bool):
        return value
    m = re.fullmatch(r"\$\{\{\s*matrix\.(\w+)\s*==\s*'([^']*)'\s*\}\}", str(value or "").strip())
    return bool(m) and str(matrix.get(m.group(1))) == m.group(2)


def jobs_of(workflow):
    """(id, name, image, steps, allowed to fail) for every job and matrix entry."""
    out = []
    for job_id, job in workflow["jobs"].items():
        matrix = (job.get("strategy") or {}).get("matrix") or {}
        keys = [k for k in matrix if isinstance(matrix[k], list)]
        for values in itertools.product(*(matrix[k] for k in keys)) if keys else [()]:
            entry = dict(zip(keys, values))
            container = job.get("container")
            image = container.get("image") if isinstance(container, dict) else container
            if not image:
                continue  # runs on the GitHub runner itself: nothing to copy here
            out.append({
                "id": job_id + "".join(":" + str(v) for v in values),
                "name": expand(job.get("name", job_id), entry),
                "image": expand(image, entry),
                "steps": [(s.get("name", "step"), expand(s["run"], entry)) for s in job.get("steps", []) if "run" in s],
                "may_fail": allowed_to_fail(job.get("continue-on-error"), entry),
            })
    return out


# What a changed path needs: job ids, a matrix job's first entry only (GitHub runs every Fedora). The first
# pattern a path matches wins; a path matching none needs nothing (docs, packaging, tools).
NEEDS = [
    (r"src/", ["build-test", "asan", "asan-gtk", "format"]),
    (r"(tests|plugins)/|Makefile$", ["build-test", "asan", "asan-gtk"]),
    (r"(assets|styles|snippets|desktop)/", ["build-test"]),
]


def changed_paths(base):
    """Paths changed since base (commits not pushed, the working tree, new files)."""
    def git(*args):
        return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True, text=True).stdout.split()
    return sorted(set(git("diff", "--name-only", base) + git("ls-files", "-o", "--exclude-standard")))


def changed_jobs(jobs, base):
    """The jobs the changes since base need, each with why: ci.yml's changed jobs, and NEEDS for the rest."""
    why = {}
    first = {}
    for j in jobs:  # a matrix job: its first entry GitHub does not let fail
        if not j["may_fail"]:
            first.setdefault(j["id"].split(":")[0], j["id"])
    for path in changed_paths(base):
        if path == os.path.relpath(WORKFLOW, ROOT):
            old = subprocess.run(["git", "show", f"{base}:{path}"], cwd=ROOT, capture_output=True, text=True)
            before = {j["id"]: j for j in jobs_of(yaml.safe_load(old.stdout))} if old.returncode == 0 else {}
            for j in jobs:
                if before.get(j["id"]) != j:
                    why.setdefault(j["id"], set()).add(path)
            continue
        for pattern, ids in NEEDS:
            if re.match(pattern, path):
                for job_id in ids:
                    if first.get(job_id):
                        why.setdefault(first[job_id], set()).add(path)
                break
    return [j for j in jobs if j["id"] in why], why


def working_tree_tar(path):
    """The working tree as git sees it (tracked and new files, not the ignored ones), as a tar file."""
    files = subprocess.run(["git", "ls-files", "-z", "-co", "--exclude-standard"], cwd=ROOT, check=True,
                           capture_output=True).stdout.decode().split("\0")
    with tarfile.open(path, "w") as tar:
        for name in files:
            if name and os.path.lexists(os.path.join(ROOT, name)):  # a deleted file still listed: left out
                tar.add(os.path.join(ROOT, name), arcname=name, recursive=False)


def script_of(job):
    """The job's steps one after another as GitHub runs them in a container (bash -e -o pipefail)."""
    lines = ["set -e", "echo keepcache=True >> /etc/dnf/dnf.conf", "mkdir -p /src", "tar -xf /work.tar -C /src",
             "cd /src"]
    for name, run in job["steps"]:
        lines.append("echo " + shlex.quote("::: " + name))
        lines.append("(cd /src && bash --noprofile --norc -e -o pipefail -c " + shlex.quote(run) + ")")
    return "\n".join(lines)


def run_job(job, tar_path):
    log_path = os.path.join(LOGS, job["id"].replace(":", "-") + ".log")
    volume = "tcode-ci-dnf-" + re.sub(r"[^a-z0-9]+", "-", job["image"].split("/")[-1].lower())
    # z, not Z: a private label for one container took the copy away from the job started before it
    command = ["podman", "run", "--rm", "--init", "-v", f"{tar_path}:/work.tar:ro,z",
               "-v", f"{volume}:/var/cache/libdnf5", job["image"], "bash", "-c", script_of(job)]
    start = time.time()
    with open(log_path, "w") as log:
        code = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT).returncode
    return job, code, time.time() - start, log_path


def failed_step(log_path):
    """The last step a job's log reached: where it failed."""
    step = None
    with open(log_path, errors="replace") as log:
        for line in log:
            if line.startswith("::: "):
                step = line[4:].strip()
    return step


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("only", nargs="*", help="jobs to run: a job id (build-test) or one entry (build-test:44)")
    parser.add_argument("-j", "--jobs", type=int, default=1, help="jobs at a time (1 by default)")
    parser.add_argument("--list", action="store_true", help="list the jobs and stop")
    parser.add_argument("--changed", action="store_true",
                        help="only the jobs the changes since the upstream branch (origin/main) need")
    args = parser.parse_args()

    with open(WORKFLOW) as f:
        jobs = jobs_of(yaml.safe_load(f))
    if args.only:
        jobs = [j for j in jobs if j["id"] in args.only or j["id"].split(":")[0] in args.only]
        if not jobs:
            sys.exit("no such job: " + " ".join(args.only))
    why = {}
    if args.changed:
        base = subprocess.run(["git", "rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}"], cwd=ROOT,
                              capture_output=True, text=True).stdout.strip() or "origin/main"
        jobs, why = changed_jobs(jobs, base)
        if not jobs:
            print(f"nothing since {base} needs a job (docs, packaging, tools only)")
            return 0
    if args.list:
        for j in jobs:
            reason = "  <- " + ", ".join(sorted(why[j["id"]])[:4]) if j["id"] in why else ""
            print(f"{j['id']:<20} {j['image']}{'  (may fail)' if j['may_fail'] else ''}{reason}")
        return 0

    os.makedirs(LOGS, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="t-code-ci-") as tmp:
        tar_path = os.path.join(tmp, "work.tar")
        working_tree_tar(tar_path)
        print(f"{len(jobs)} job(s), {args.jobs} at a time; logs in {os.path.relpath(LOGS, ROOT)}/", flush=True)
        results = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(args.jobs, 1)) as pool:
            for job, code, seconds, log_path in pool.map(lambda j: run_job(j, tar_path), jobs):
                ok = code == 0
                where = "" if ok else f"  at: {failed_step(log_path)}"
                note = " (allowed to fail)" if not ok and job["may_fail"] else ""
                print(f"{'PASS' if ok else 'FAIL'}  {job['id']:<20} {seconds / 60:5.1f} min{where}{note}", flush=True)
                results.append((job, ok))

    failed = [j["id"] for j, ok in results if not ok and not j["may_fail"]]
    print("\nall required jobs passed" if not failed else "\nfailed: " + ", ".join(failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
