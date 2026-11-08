"""Create the publication tag without moving an existing version tag."""

import json
import os
import subprocess

from build_version import validate_version


def ensure_tag(repository: str, tag: str, commit: str) -> None:
    """Accept lightweight and annotated tags only at the tested source commit."""
    if not tag.startswith('v'):
        raise ValueError(f'Invalid release tag: {tag!r}')
    validate_version(tag[1:])
    endpoint: str = f'repos/{repository}/git'
    references: list[dict] = json.loads(
        subprocess.check_output(
            ['gh', 'api', f'{endpoint}/matching-refs/tags/{tag}'], text=True
        )
    )
    reference: dict | None = next(
        (ref for ref in references if ref['ref'] == f'refs/tags/{tag}'), None
    )

    if reference is not None:
        target: dict = reference['object']

        # Annotated tags refer to a tag object, possibly another annotated tag.
        while target['type'] == 'tag':
            target = json.loads(
                subprocess.check_output(
                    ['gh', 'api', f"{endpoint}/tags/{target['sha']}"], text=True
                )
            )['object']
        if target['type'] != 'commit' or target['sha'] != commit:
            raise ValueError(
                f'Release tag {tag} does not point to tested commit {commit}'
            )
        return

    # Creation is atomic. A concurrent creator must not cause us to move its tag.
    subprocess.run(
        [
            'gh',
            'api',
            '--method',
            'POST',
            f'{endpoint}/refs',
            '-f',
            f'ref=refs/tags/{tag}',
            '-f',
            f'sha={commit}',
        ],
        check=True,
    )


if __name__ == '__main__':
    ensure_tag(
        os.environ['GH_REPO'], os.environ['RELEASE_TAG'], os.environ['RELEASE_COMMIT']
    )
