"""Find completed push checks for the exact release commit."""

import argparse
import json
import os
import subprocess
from pathlib import Path
from urllib.parse import urlencode

import output_layout
from package import LINUX_WHEEL_ARCHITECTURES, PYTHON_VERSIONS, WINDOWS_WHEEL_PLATFORMS


def api_pages(endpoint: str) -> list[dict]:
    """Read all result pages through the runner's authenticated GitHub CLI."""
    result: subprocess.CompletedProcess[str] = subprocess.run(
        ['gh', 'api', '--paginate', '--slurp', endpoint],
        check=True,
        capture_output=True,
        text=True,
    )
    return json.loads(result.stdout)


def compatibility_runs(repository: str, commit: str) -> list[dict]:
    """Read this repository's compatibility runs for the selected source commit."""
    query: str = urlencode({'head_sha': commit, 'per_page': 100})
    pages: list[dict] = api_pages(
        f'repos/{repository}/actions/workflows/compatibility.yml/runs?{query}'
    )
    return [
        run
        for page in pages
        for run in page['workflow_runs']
        if run['head_sha'] == commit
        and run['head_repository']['full_name'] == repository
    ]


def require_idle(runs: list[dict], repository: str) -> None:
    """Stop before duplicating queued, running, or approval-pending checks."""
    for run in runs:
        if run['status'] != 'completed':
            raise RuntimeError(
                f"Compatibility checks are still {run['status']}: "
                f"https://github.com/{repository}/actions/runs/{run['id']}. "
                'Wait for them to finish, then run Release again.'
            )


def reuse_plan(repository: str, commit: str, version: str) -> dict:
    """Keep each job's newest result; failures in other jobs do not discard it."""
    runs: list[dict] = compatibility_runs(repository, commit)
    require_idle(runs, repository)
    packages: dict[str, str] = {
        f'Compatibility Testing - Windows / {arch} (opt, static)': f'release_windows_{arch}'
        for arch in WINDOWS_WHEEL_PLATFORMS
    }
    linux: dict[str, str] = {
        f'Python - Linux / Python {python} wheel ({arch})': f'release_linux_{arch}_{python}'
        for python in PYTHON_VERSIONS
        for arch in LINUX_WHEEL_ARCHITECTURES
    }
    windows: dict[str, str] = {
        f'Python - Windows / Python {python} wheel ({arch})': f'release_wheel_{arch}_{python}'
        for python in PYTHON_VERSIONS
        for arch in WINDOWS_WHEEL_PLATFORMS
    }
    expected: dict[str, str] = packages | linux | windows
    results: dict[str, dict] = {}
    for run in sorted(runs, key=lambda item: item['id'], reverse=True):
        if run['event'] != 'push':
            continue
        run_id: str = str(run['id'])
        pages: list[dict] = api_pages(
            f'repos/{repository}/actions/runs/{run_id}/artifacts'
        )
        artifacts: list[str] = [
            artifact['name']
            for page in pages
            for artifact in page['artifacts']
            if not artifact['expired']
        ]
        if f'build_version_{version}' not in artifacts:
            continue
        pages = api_pages(
            f'repos/{repository}/actions/runs/{run_id}/jobs?filter=latest'
        )
        for page in pages:
            for job in page['jobs']:
                name: str = job['name']
                if name in results:
                    continue
                results[name] = {
                    'run': run_id if job['conclusion'] == 'success' else '',
                    'artifacts': artifacts,
                    'package_available': expected.get(name, '') in artifacts,
                }
    return {
        'commit': commit,
        'version': version,
        'jobs': results,
        'dependencies': {
            'linux_wheels': list(linux),
            'windows_x86_packages': [
                f'Compatibility Testing - Windows / {arch} (opt, static)'
                for arch in ('x86', 'x86_64')
            ],
            **{
                f'windows_{arch}': [
                    f'Compatibility Testing - Windows / {arch} (opt, static)'
                ]
                for arch in WINDOWS_WHEEL_PLATFORMS
            },
        },
    }


def main() -> None:
    """Write explicit reuse decisions for the release workflow."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '--check-idle',
        action='store_true',
        help='Stop if compatibility checks are active for this commit',
    )
    parser.add_argument(
        '--plan',
        action='store_true',
        help='Record independent reuse decisions for each compatibility job',
    )
    arguments = parser.parse_args()
    commit: str = subprocess.check_output(
        ['git', 'rev-parse', 'HEAD'],
        text=True,
    ).strip()
    if arguments.check_idle:
        repository: str = os.environ['GITHUB_REPOSITORY']
        require_idle(compatibility_runs(repository, commit), repository)
        return
    if arguments.plan:
        plan: dict = reuse_plan(
            os.environ['GITHUB_REPOSITORY'], commit, os.environ['LOCKD_RELEASE_VERSION']
        )
        path = (
            output_layout.directories(Path.cwd(), output_layout.architecture())[
                'staging'
            ]
            / 'reuse_plan.json'
        )
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(plan), encoding='utf-8')
        return
    parser.error('Choose --check-idle or --plan')


if __name__ == '__main__':
    try:
        main()
    except RuntimeError as error:
        message: str = str(error)
        with Path(os.environ['GITHUB_STEP_SUMMARY']).open(
            'a', encoding='utf-8'
        ) as summary:
            summary.write(f'Release stopped: {message}\n')
        print(f'::error::{message}')
        raise SystemExit(1) from error
