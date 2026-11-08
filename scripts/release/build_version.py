"""Assign push versions with durable Git tags, without changing commits."""

import json
import os
import re
import subprocess
from pathlib import Path

import output_layout

VERSION_PATTERN = r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]?)'


def validate_version(version: str, *, development: bool = False) -> str:
    """Accept three numeric components with a build number below 100."""
    suffix: str = r'(?:\.dev0)?' if development else ''
    if not re.fullmatch(VERSION_PATTERN + suffix, version):
        raise ValueError(f'Invalid build version: {version!r}')
    return version


def next_version(versions: list[str]) -> str:
    """Start at 1.0.1 and carry build 99 into the next minor version."""
    if not versions:
        return '1.0.1'
    major, minor, build = max(
        tuple(map(int, validate_version(version).split('.'))) for version in versions
    )
    if build == 99:
        minor += 1
        build = 0
    else:
        build += 1
    return f'{major}.{minor}.{build}'


def read_assignments(repository: str) -> dict[str, str]:
    """Read the lightweight build tags; malformed assignments require attention."""
    result = subprocess.run(
        ['gh', 'api', f'repos/{repository}/git/matching-refs/tags/build/v'],
        check=True,
        capture_output=True,
        text=True,
    )
    assignments: dict[str, str] = {}
    for ref in json.loads(result.stdout):
        version: str = validate_version(ref['ref'].removeprefix('refs/tags/build/v'))
        if ref['object']['type'] != 'commit':
            raise ValueError(f'Build tag {version} must point directly to a commit')
        assignments[version] = ref['object']['sha']
    return assignments


def resolve_version(repository: str, commit: str, *, allocate: bool) -> str:
    """Reuse a commit's assignment, or atomically reserve the next push version."""
    for _attempt in range(5):
        assignments: dict[str, str] = read_assignments(repository)
        matches: list[str] = [
            version for version, sha in assignments.items() if sha == commit
        ]
        if len(matches) > 1:
            raise ValueError(f'Multiple build versions point to {commit}: {matches}')
        if matches:
            return matches[0]
        if not allocate:
            return '1.0.1.dev0'

        version: str = next_version(list(assignments))
        result = subprocess.run(
            [
                'gh',
                'api',
                '--method',
                'POST',
                f'repos/{repository}/git/refs',
                '-f',
                f'ref=refs/tags/build/v{version}',
                '-f',
                f'sha={commit}',
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode == 0:
            return version

        # Ref creation is atomic. Retry only if another push reserved this number;
        # permission/network failures must not silently produce development artifacts.
        if version not in read_assignments(repository):
            raise RuntimeError(f'Could not assign build version: {result.stderr}')
    raise RuntimeError(
        'Concurrent pushes repeatedly reserved the next build version; retry the job'
    )


def main() -> None:
    """Expose one assignment to every subsequent packaging step in this job."""
    commit: str = subprocess.check_output(
        ['git', 'rev-parse', 'HEAD'], text=True
    ).strip()
    version: str = resolve_version(
        os.environ['GITHUB_REPOSITORY'],
        commit,
        allocate=os.environ.get('ALLOCATE_VERSION') == 'true',
    )
    if os.environ.get('REQUIRE_ASSIGNED_VERSION') == 'true' and version.endswith(
        '.dev0'
    ):
        raise ValueError('Push this commit to the default branch before releasing it')
    record: Path = (
        output_layout.directories(Path.cwd(), output_layout.architecture())['staging']
        / 'build_version.txt'
    )
    record.parent.mkdir(parents=True, exist_ok=True)
    record.write_text(version + '\n', encoding='utf-8')
    with Path(os.environ['GITHUB_ENV']).open('a', encoding='utf-8') as environment:
        environment.write(f'LOCKD_RELEASE_VERSION={version}\n')
    with Path(os.environ['GITHUB_STEP_SUMMARY']).open('a', encoding='utf-8') as summary:
        summary.write(f'Build version: **{version}** for commit `{commit}`.\n')
    print(f'Build version: {version}')


if __name__ == '__main__':
    main()
