"""Delete older failed or cancelled Compatibility and Release runs after success."""

import json
import os
import subprocess
from pathlib import Path
from urllib.parse import urlencode

WORKFLOW_PATHS = {
    '.github/workflows/compatibility.yml',
    '.github/workflows/release.yml',
}
CLEANUP_PATH = '.github/workflows/cleanup_workflow_runs.yml'


def api(endpoint: str, *, paginate: bool = False, delete: bool = False) -> object:
    """Use the runner's GitHub CLI authentication without exposing its token."""
    arguments: list[str] = ['gh', 'api', endpoint]
    if paginate:
        arguments.extend(['--paginate', '--slurp'])
    if delete:
        arguments.extend(['--method', 'DELETE'])
    result: subprocess.CompletedProcess[str] = subprocess.run(
        arguments, check=True, capture_output=True, text=True
    )
    return json.loads(result.stdout) if result.stdout.strip() else None


def same_scope(run: dict, successful: dict, repository: str) -> bool:
    """Match workflow and branch, allowing manual runs to supersede push runs."""
    # A manual retry and a push validate the same branch. Pull-request runs test
    # a merge context, so they must remain separate from branch validation.
    branch_events = {'push', 'workflow_dispatch'}
    same_event = run['event'] == successful['event'] or (
        run['event'] in branch_events and successful['event'] in branch_events
    )
    return (
        run['workflow_id'] == successful['workflow_id']
        and run['path'] == successful['path']
        and run['head_branch'] == successful['head_branch']
        and (run.get('head_repository') or {}).get('full_name') == repository
        and same_event
    )


def superseded(run: dict, successful: dict, repository: str) -> bool:
    """Never delete a newer run or one retried after this success."""
    return (
        same_scope(run, successful, repository)
        and run['id'] != successful['id']
        and run['run_number'] < successful['run_number']
        and run['status'] == 'completed'
        and run['conclusion'] in ('failure', 'cancelled')
        and run['updated_at'] <= successful['updated_at']
    )


def cleanup(repository: str, run_id: int) -> list[int]:
    """Validate the successful run and recheck candidates before deleting them."""
    successful: dict = api(f'repos/{repository}/actions/runs/{run_id}')
    if (
        successful['path'] not in WORKFLOW_PATHS
        or successful['status'] != 'completed'
        or successful['conclusion'] != 'success'
        or (successful.get('head_repository') or {}).get('full_name') != repository
        or not successful['head_branch']
    ):
        raise ValueError(
            'Cleanup requires a successful Compatibility or Release run in this repository.'
        )

    query: str = urlencode(
        {'branch': successful['head_branch'], 'status': 'completed', 'per_page': 100}
    )
    pages: list[dict] = api(
        f'repos/{repository}/actions/workflows/{successful["workflow_id"]}/runs?{query}',
        paginate=True,
    )

    # Collect all pages before deleting anything, or pagination could skip records.
    candidates: list[dict] = [run for page in pages for run in page['workflow_runs']]
    deleted: list[int] = []
    for candidate in candidates:
        if not superseded(candidate, successful, repository):
            continue
        endpoint: str = f'repos/{repository}/actions/runs/{candidate["id"]}'
        current: dict = api(endpoint)
        if not superseded(current, successful, repository):
            continue
        api(endpoint, delete=True)
        deleted.append(current['id'])
        print(
            f'Deleted superseded {current["conclusion"]} workflow run {current["id"]}.'
        )
    print(f'Deleted {len(deleted)} failed or cancelled run(s).')
    return deleted


def cleanup_skipped(repository: str, run_id: int) -> list[int]:
    """Remove older no-op cleanup runs after the validation cleanup succeeds."""
    current: dict = api(f'repos/{repository}/actions/runs/{run_id}')
    if (
        current['path'] != CLEANUP_PATH
        or current['event'] != 'workflow_run'
        or (current.get('head_repository') or {}).get('full_name') != repository
    ):
        raise ValueError(
            'Skipped-run cleanup requires this repository cleanup workflow.'
        )

    def eligible(run: dict) -> bool:
        # Failed validation creates skipped cleanup runs. They contain no cleanup
        # work, but accumulate in Actions unless a later successful cleanup removes them.
        return (
            same_scope(run, current, repository)
            and run['id'] < current['id']
            and run['run_number'] < current['run_number']
            and run['status'] == 'completed'
            and run['conclusion'] == 'skipped'
            and run['updated_at'] <= current['created_at']
        )

    query = urlencode({'status': 'skipped', 'per_page': 100})
    pages = api(
        f'repos/{repository}/actions/workflows/{current["workflow_id"]}/runs?{query}',
        paginate=True,
    )
    deleted: list[int] = []
    for candidate in [run for page in pages for run in page['workflow_runs']]:
        if not eligible(candidate):
            continue
        endpoint = f'repos/{repository}/actions/runs/{candidate["id"]}'
        if not eligible(api(endpoint)):
            continue
        api(endpoint, delete=True)
        deleted.append(candidate['id'])
        print(f'Deleted skipped cleanup run {candidate["id"]}.')
    print(f'Deleted {len(deleted)} skipped cleanup run(s).')
    return deleted


if __name__ == '__main__':
    event: dict = json.loads(
        Path(os.environ['GITHUB_EVENT_PATH']).read_text(encoding='utf-8')
    )
    cleanup(os.environ['GITHUB_REPOSITORY'], event['workflow_run']['id'])
    cleanup_skipped(os.environ['GITHUB_REPOSITORY'], int(os.environ['GITHUB_RUN_ID']))
