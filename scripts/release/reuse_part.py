"""Select a tested compatibility job and its original output for release."""

import json
import os
import subprocess
from pathlib import Path

import output_layout


def reusable_run(plan: dict, job: str, artifact: str, dependencies: list[str]) -> str:
    """Reuse only successful jobs whose required package inputs are also reused."""
    result: dict = plan['jobs'].get(job, {})
    run: str = result.get('run', '')
    if not run or (artifact and artifact not in result['artifacts']):
        return ''
    for dependency in dependencies:
        previous: dict = plan['jobs'].get(dependency, {})
        if previous.get('run') != run:
            return ''
        if not previous.get('package_available', False):
            return ''
    return run


def main() -> None:
    """Write a job's reuse decision for the shared workflow action."""
    # Match the composite action's download directory. The controller Python can
    # remain 64-bit while the job produces i686 wheels in the x86 output tree.
    output_arch: str = os.environ.get('OUTPUT_ARCH') or output_layout.architecture()
    plan: dict = json.loads(
        (
            output_layout.directories(Path.cwd(), output_arch)['build']
            / 'reuse/reuse_plan.json'
        ).read_text(encoding='utf-8')
    )
    commit: str = subprocess.check_output(
        ['git', 'rev-parse', 'HEAD'], text=True
    ).strip()
    if plan['commit'] != commit or plan['version'] != os.environ['REUSE_VERSION']:
        raise ValueError(
            'Compatibility reuse plan does not match this source and version'
        )
    job: str = os.environ['REUSE_JOB']
    artifact: str = os.environ.get('REUSE_ARTIFACT', '')
    group: str = os.environ.get('REUSE_DEPENDENCIES', '')
    dependencies: list[str] = plan['dependencies'][group] if group else []
    run: str = reusable_run(plan, job, artifact, dependencies)
    with Path(os.environ['GITHUB_OUTPUT']).open('a', encoding='utf-8') as output:
        output.write(f'run={run}\nreused={str(bool(run)).lower()}\n')
    with Path(os.environ['GITHUB_STEP_SUMMARY']).open('a', encoding='utf-8') as summary:
        if run:
            summary.write(
                f"Reuse **{job}** from [compatibility run {run}]"
                f"({os.environ['GITHUB_SERVER_URL']}/"
                f"{os.environ['GITHUB_REPOSITORY']}/actions/runs/{run}).\n"
            )
        else:
            summary.write(
                f'Run **{job}**: no reusable successful result with its artifacts.\n'
            )


if __name__ == '__main__':
    main()
