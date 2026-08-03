#!/usr/bin/env python3
"""NeuralAxe OS — THE canonical build-revision provider.

ONE definition of "what revision is this build", shared by every packaging
helper, so the firmware image and the web image can never disagree.

WHY THIS EXISTS
---------------
A device compares the revision embedded in esp-miner.bin (esp_app_desc_t) with
the revision embedded in www.bin (version.txt) by EXACT STRING EQUALITY. Two
independent `git describe` invocations do not guarantee that:

  * ESP-IDF derives PROJECT_VER through
    tools/cmake/third_party/GetGitRevisionDescription.cmake's git_describe(),
    called from project.cmake with NO extra arguments — so git picks the
    abbreviation length.
  * axe-os/generate-version.js ran `git describe --tags --always --dirty` — so
    git picks the abbreviation length there too.

`core.abbrev` defaults to "auto", which git computes per repository and which
has changed between git releases. Observed on this very repository, same
commit, same working tree:

    host      git 2.54.0  ->  v2.14.2-70-g34a5150     (7 hex)
    container git 2.43.0  ->  v2.14.2-70-g34a51508    (8 hex)

The firmware builds in the ESP-IDF container and the frontend builds on the
host, so the two sides picked different lengths and a real device correctly
reported BOOT PAIR MISMATCH.

THE CONTRACT
------------
1. The full commit is `git rev-parse HEAD` — 40 characters, never abbreviated,
   recorded separately in every manifest.
2. The display revision is `git describe --tags --long --always --abbrev=8`.
   * `--abbrev=8` is EXPLICIT: neither side may fall back on git's default.
   * `--long` means the `-<count>-g<hash>` suffix is always present, so the
     format does not change shape when HEAD happens to sit on a tag.
   * `--always` keeps a tag-less repository working.
3. That exact string is passed to BOTH sides — `-DPROJECT_VER=` for the
   firmware, `NX_CANONICAL_REVISION=` for the frontend generator. Neither is
   allowed to derive its own.
4. Dirtiness is decided by `git status --porcelain`, NOT by `describe --dirty`:
   on this repository the host's `--dirty` did not report a genuinely dirty
   tree (stale index stat data under CRLF normalization) while `status
   --porcelain` did. The porcelain check is the authoritative one.

This module performs no network access, touches no hardware and writes nothing.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

# The ONE describe invocation. Any tool that needs a display revision must use
# these exact arguments — a test asserts the pilot helpers and the release
# exporter cannot drift from them.
CANONICAL_ABBREV = 8
CANONICAL_DESCRIBE_ARGS = (
    "describe", "--tags", "--long", "--always", f"--abbrev={CANONICAL_ABBREV}",
)

# The environment variable through which the canonical revision reaches the
# frontend version generator.
REVISION_ENV = "NX_CANONICAL_REVISION"

# A canonical revision: a tag, then a commit count, then exactly
# CANONICAL_ABBREV lowercase hex characters. No "-dirty" is ever canonical.
CANONICAL_REVISION_RE = re.compile(
    rf"^v\d+\.\d+\.\d+-\d+-g[0-9a-f]{{{CANONICAL_ABBREV}}}$")
# A tag-less repository degrades to the bare abbreviated commit.
CANONICAL_FALLBACK_RE = re.compile(rf"^[0-9a-f]{{{CANONICAL_ABBREV}}}$")

FULL_COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")


class RevisionError(Exception):
    """The repository cannot supply a trustworthy canonical revision."""


def _git(repo: Path, *args: str) -> str:
    out = subprocess.run(["git", *args], cwd=str(repo), text=True, capture_output=True)
    if out.returncode != 0:
        raise RevisionError(f"git {' '.join(args)} failed: {out.stderr.strip()}")
    return out.stdout.strip()


def full_commit(repo: Path) -> str:
    """The complete 40-character commit. Never abbreviated, never truncated."""
    commit = _git(repo, "rev-parse", "HEAD")
    if not FULL_COMMIT_RE.match(commit):
        raise RevisionError(f"'{commit}' is not a full 40-character commit")
    return commit


def working_tree_dirty(repo: Path) -> bool:
    """Authoritative dirtiness. `git describe --dirty` is NOT trusted here."""
    return bool(_git(repo, "status", "--porcelain"))


def canonical_revision(repo: Path) -> str:
    """THE display revision. Deterministic across git versions and hosts."""
    rev = _git(repo, *CANONICAL_DESCRIBE_ARGS)
    if not rev:
        raise RevisionError("git describe produced an empty revision")
    if "-dirty" in rev:
        # Cannot happen with these arguments; refuse rather than assume.
        raise RevisionError(f"the canonical revision must never carry -dirty ({rev})")
    validate_canonical(rev)
    return rev


def validate_canonical(rev: str) -> None:
    """Reject anything that is not exactly the canonical shape.

    In particular this rejects the 7-character form git produces by default on
    some hosts, which is the mismatch this contract exists to prevent.
    """
    if CANONICAL_REVISION_RE.match(rev) or CANONICAL_FALLBACK_RE.match(rev):
        return
    raise RevisionError(
        f"'{rev}' is not a canonical revision: expected a tag plus "
        f"-<count>-g<{CANONICAL_ABBREV} hex>, produced by "
        f"`git {' '.join(CANONICAL_DESCRIBE_ARGS)}`")


def identity(repo: Path) -> dict:
    """The complete build identity: full commit plus canonical revision."""
    return {
        "commit": full_commit(repo),
        "revision": canonical_revision(repo),
        "dirty": working_tree_dirty(repo),
    }


def abbreviated_commit(commit: str) -> str:
    """The canonical abbreviation of a full commit, for cross-checking."""
    if not FULL_COMMIT_RE.match(commit):
        raise RevisionError("a full 40-character commit is required")
    return commit[:CANONICAL_ABBREV]


def revision_matches_commit(rev: str, commit: str) -> bool:
    """True when a canonical revision's hash is this commit's abbreviation."""
    validate_canonical(rev)
    return rev.endswith("g" + abbreviated_commit(commit)) or \
        rev == abbreviated_commit(commit)


if __name__ == "__main__":
    import json
    import sys
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(
        subprocess.check_output(["git", "rev-parse", "--show-toplevel"],
                                text=True).strip())
    print(json.dumps(identity(root), indent=2))
