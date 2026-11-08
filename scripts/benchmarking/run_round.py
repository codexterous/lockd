"""Run one randomized benchmark round, or resume its saved order after cleanup."""

import argparse
import datetime
import json
import os
import random
import shlex
import sys
import uuid
from pathlib import Path

from . import __main__ as controller
from . import config, vultr

ATTEMPT_FIELDS = (
    'run_id',
    'manifest',
    'status',
    'started_utc',
    'finished_utc',
    'exit_code',
)


def timestamp() -> str:
    """Keep journal timestamps in UTC, independent of the controller's locale."""
    return datetime.datetime.now(datetime.UTC).isoformat()


def new_round() -> dict:
    """Discover matching configurations once and save a shuffled, complete round."""
    directory = config.ROOT / 'scripts/benchmarking'
    configurations = sorted(
        path for path in directory.glob('benchmark-*.json') if path.is_file()
    )
    if not configurations:
        raise ValueError(f"No benchmark-*.json configurations found in {directory}")
    profiles: list[dict] = []
    for path in configurations:
        name = path.stem.removeprefix('benchmark-')
        if not name:
            raise ValueError(f"Configuration needs a profile name: {path}")
        profiles.append(
            {
                'profile': name,
                'config': str(path),
                'config_sha256': None,
                'status': 'pending',
                'started_utc': None,
                'finished_utc': None,
                'exit_code': None,
                'run_id': None,
                'manifest': None,
                'attempts': [],
            }
        )
    random.shuffle(profiles)
    return {
        'round_id': str(uuid.uuid4()),
        'started_utc': timestamp(),
        'finished_utc': None,
        'outcome': 'incomplete',
        'profiles': profiles,
    }


def load_round(path: Path) -> dict:
    """Validate the saved sequence, including journals with fewer profiles."""
    state = config.object_value(json.loads(path.read_text(encoding='utf-8-sig')))
    if not isinstance(state['round_id'], str):
        raise TypeError("Invalid round identifier")
    uuid.UUID(state['round_id'])
    if state['outcome'] not in ('completed', 'failed', 'incomplete'):
        raise ValueError("Invalid round outcome")
    profiles = state['profiles']
    if not isinstance(profiles, list) or not profiles:
        raise ValueError("Invalid round profiles")
    seen: set[str] = set()
    unfinished = False
    pending = False
    attempt = False
    for entry in profiles:
        profile = config.object_value(entry)
        name = profile['profile']
        status = profile['status']
        if (
            not isinstance(name, str)
            or not name
            or name in seen
            or status not in ('completed', 'pending', 'failed', 'incomplete', 'running')
        ):
            raise ValueError("Invalid or duplicate profile in round journal")
        seen.add(name)
        if status == 'completed':
            if (
                unfinished
                or type(profile['exit_code']) is not int
                or profile['exit_code'] != 0
            ):
                raise ValueError(
                    "Completed profiles must form the successful prefix of the round"
                )
        else:
            unfinished = True
            if status == 'pending':
                pending = True
            elif attempt or pending:
                raise ValueError("An unfinished attempt must precede pending profiles")
            else:
                attempt = True
        if not isinstance(profile['config'], str) or not profile['config']:
            raise ValueError("Invalid configuration path in round journal")
        if not isinstance(profile['config_sha256'], str):
            raise TypeError("Missing configuration hash in round journal")

        # Older rounds predate explicit run identities and retry history. Add these
        # fields in memory only; a preview or rejected resume must not rewrite them.
        profile.setdefault('run_id', None)
        profile.setdefault('manifest', None)
        profile.setdefault('attempts', [])
        if not isinstance(profile['attempts'], list):
            raise TypeError("Invalid attempt history in round journal")
    return state


def validate_profiles(state: dict, *, resuming: bool) -> None:
    """Check the entire baseline locally before any profile can create resources."""
    for profile in state['profiles']:
        path = Path(profile['config'])
        digest = config.file_digest(path).upper()
        if resuming and digest != profile['config_sha256'].upper():
            raise ValueError(
                f"Profile changed since this round started: {profile['profile']}. "
                "Start a new round."
            )
        config.load(path)
        profile['config_sha256'] = digest


def check_previous(state: dict, previous_manifest: Path | None) -> None:
    """Associate a legacy attempt only after its identity and cleanup are verified."""
    attempt = next(
        (
            profile
            for profile in state['profiles']
            if profile['status'] not in ('completed', 'pending')
        ),
        None,
    )
    if attempt is None:
        if previous_manifest is not None:
            raise ValueError(
                "There is no previous attempt to associate with this manifest"
            )
        return
    manifest = attempt['manifest']
    if (
        manifest
        and previous_manifest is not None
        and Path(manifest).resolve() != previous_manifest
    ):
        raise ValueError(
            "Previous manifest differs from the one recorded for this attempt"
        )
    if not manifest:
        if previous_manifest is None:
            raise ValueError(
                "This older journal needs --previous-manifest with the failed run's manifest.json"
            )
        manifest = str(previous_manifest)
    previous_id = controller.check_resume(
        Path(manifest),
        Path(attempt['config']),
        attempt['started_utc'],
        attempt['finished_utc'] or '-',
    )
    if attempt['run_id'] and attempt['run_id'] != previous_id:
        raise ValueError("Previous manifest UUID differs from the recorded attempt")
    attempt['manifest'] = manifest
    attempt['run_id'] = previous_id


def execute_round(path: Path, state: dict) -> None:
    """Journal each identity before running; stop at the first failure or interruption."""
    state['outcome'] = 'incomplete'
    state['finished_utc'] = None
    vultr.save(path, state)
    print(f"Round journal: {path}", flush=True)
    current: dict | None = None
    try:
        api = vultr.Api(os.environ.get('VULTR_API_KEY', ''))
        for profile in state['profiles']:
            if profile['status'] == 'completed':
                continue
            configuration = Path(profile['config'])
            if config.file_digest(configuration).upper() != profile['config_sha256']:
                raise ValueError(
                    f"Profile changed after validation: {profile['profile']}. Start a new round."
                )

            # Reload through the same validator as a single run, including its source
            # archive checksum. Do not reuse validated inputs after another long run.
            settings = config.load(configuration)
            next_id = str(uuid.uuid4())
            next_manifest = str(config.output_directory(next_id) / 'manifest.json')
            current = profile
            if profile['status'] != 'pending':
                profile['attempts'].append(
                    {field: profile[field] for field in ATTEMPT_FIELDS}
                )
            profile.update(
                run_id=next_id,
                manifest=next_manifest,
                status='running',
                started_utc=timestamp(),
                finished_utc=None,
                exit_code=None,
            )
            vultr.save(path, state)
            print(f"Running {profile['profile']}", flush=True)
            try:
                # Use the single-run controller directly. Ctrl+C reaches its existing
                # finally-based VM cleanup without a parent process killing it early.
                controller.run(api, settings, next_id)
            except Exception as error:
                profile.update(status='failed', finished_utc=timestamp(), exit_code=1)
                state['outcome'] = 'failed'
                raise RuntimeError(
                    f"Benchmark failed: {profile['profile']}. Manifest: {next_manifest}\n{error}"
                ) from error
            profile.update(status='completed', finished_utc=timestamp(), exit_code=0)
            vultr.save(path, state)
        state['outcome'] = 'completed'
    finally:
        if current is not None and current['status'] == 'running':
            current.update(status='incomplete', finished_utc=timestamp())
        state['finished_utc'] = timestamp()
        vultr.save(path, state)


def resume_command(path: Path, previous_manifest: Path | None) -> str:
    """Print a shell-quoted hint for the current persisted attempt, not an earlier one."""
    arguments = [
        'python',
        '-m',
        'scripts.benchmarking.run_round',
        '--resume',
        str(path),
    ]
    if previous_manifest is not None:
        try:
            state = load_round(path)
            next_profile = next(
                (
                    profile
                    for profile in state['profiles']
                    if profile['status'] != 'completed'
                ),
                None,
            )
            if next_profile is not None and not next_profile['manifest']:
                arguments += ['--previous-manifest', str(previous_manifest)]
        except (OSError, ValueError, KeyError, TypeError):
            pass
    if os.name == 'nt':
        # PowerShell needs the command and switches unquoted, with literal quoting
        # for paths. Quoting "python" itself would create a string, not invoke it.
        return ' '.join(
            value
            if index < 4 or value == '--previous-manifest'
            else "'" + value.replace("'", "''") + "'"
            for index, value in enumerate(arguments)
        )
    return shlex.join(arguments)


def main(arguments: list[str] | None = None) -> int:
    """Expose preview and explicit resumption on Windows, Linux and macOS."""
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument(
        '--plan-only',
        action='store_true',
        help='validate and show order without API calls or journal writes',
    )
    parser.add_argument(
        '--resume',
        type=Path,
        help='resume this round.json without rerunning completed profiles',
    )
    parser.add_argument(
        '--previous-manifest',
        type=Path,
        help='associate the failed VM manifest with an older round journal',
    )
    args = parser.parse_args(arguments)
    if args.previous_manifest is not None and args.resume is None:
        parser.error('--previous-manifest requires --resume')
    round_file: Path | None = None
    previous_manifest: Path | None = None
    try:
        if args.previous_manifest is not None:
            previous_manifest = args.previous_manifest.resolve(strict=True)
        if args.resume is not None:
            round_file = args.resume.resolve(strict=True)
            if args.plan_only:
                state = load_round(round_file)
            else:
                with vultr.journal_lock(round_file):
                    return run_round(
                        round_file,
                        load_round(round_file),
                        previous_manifest,
                        resuming=True,
                    )
        else:
            state = new_round()
        if args.plan_only:
            return run_round(
                round_file,
                state,
                previous_manifest,
                resuming=args.resume is not None,
                plan_only=True,
            )
        validate_profiles(state, resuming=False)
        round_file = config.output_directory(state['round_id']) / 'round.json'
        round_file.parent.mkdir(parents=True, exist_ok=False)
        with vultr.journal_lock(round_file):
            return run_round(round_file, state, None, resuming=False)
    except KeyboardInterrupt:
        print('Round interrupted.', file=sys.stderr)
        code = 130
    except (OSError, ValueError, TypeError, KeyError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        code = 1
    if round_file is not None and round_file.is_file():
        print(
            'Resume with: ' + resume_command(round_file, previous_manifest),
            file=sys.stderr,
        )
    return code


def run_round(
    path: Path | None,
    state: dict,
    previous_manifest: Path | None,
    *,
    resuming: bool,
    plan_only: bool = False,
) -> int:
    """Check a saved baseline and previous cleanup before executing its remaining order."""
    remaining = [
        profile for profile in state['profiles'] if profile['status'] != 'completed'
    ]
    if not remaining:
        print('This round is already complete.')
        return 0
    # New executions validate before creating their journal directory. A resume
    # rechecks the full saved baseline, including already completed profiles.
    if resuming or plan_only:
        validate_profiles(state, resuming=resuming)
    if resuming:
        check_previous(state, previous_manifest)
    print('Remaining order: ' + ', '.join(profile['profile'] for profile in remaining))
    if not plan_only:
        if path is None:
            raise ValueError("Missing round journal path")
        execute_round(path, state)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
