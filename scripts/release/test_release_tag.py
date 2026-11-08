"""Check publication tag safety without changing GitHub or local Git refs."""

import json
import subprocess
import unittest
from unittest.mock import patch

import release_tag


class ReleaseTagTests(unittest.TestCase):
    """Bind the release version to the exact tested commit, including annotated tags."""

    def test_missing_tag_is_created_at_tested_commit(self) -> None:
        """A matching prefix is not the exact version tag."""
        references: list[dict] = [{'ref': 'refs/tags/v1.0.10'}]
        with (
            patch.object(
                subprocess, 'check_output', return_value=json.dumps(references)
            ),
            patch.object(subprocess, 'run') as create,
        ):
            release_tag.ensure_tag('owner/repo', 'v1.0.1', 'tested')
        create.assert_called_once_with(
            [
                'gh',
                'api',
                '--method',
                'POST',
                'repos/owner/repo/git/refs',
                '-f',
                'ref=refs/tags/v1.0.1',
                '-f',
                'sha=tested',
            ],
            check=True,
        )

    def test_existing_tags_are_peeled_and_never_moved(self) -> None:
        """Accept matching tags; reject a different commit or non-commit target."""
        for annotated in (False, True):
            for kind, sha in (
                ('commit', 'tested'),
                ('commit', 'other'),
                ('tree', 'tested'),
            ):
                with self.subTest(annotated=annotated, kind=kind, sha=sha):
                    target: dict = {'type': kind, 'sha': sha}
                    references: list[dict] = [
                        {
                            'ref': 'refs/tags/v1.0.1',
                            'object': {'type': 'tag', 'sha': 'annotation'}
                            if annotated
                            else target,
                        }
                    ]
                    replies: list[str] = [json.dumps(references)]
                    if annotated:
                        replies.append(json.dumps({'object': target}))
                    with (
                        patch.object(subprocess, 'check_output', side_effect=replies),
                        patch.object(subprocess, 'run') as create,
                    ):
                        if kind == 'commit' and sha == 'tested':
                            release_tag.ensure_tag('owner/repo', 'v1.0.1', 'tested')
                        else:
                            with self.assertRaises(ValueError):
                                release_tag.ensure_tag('owner/repo', 'v1.0.1', 'tested')
                        create.assert_not_called()

    def test_api_failure_does_not_create_a_tag(self) -> None:
        """A failed lookup must not be mistaken for an absent tag."""
        with (
            patch.object(
                subprocess,
                'check_output',
                side_effect=subprocess.CalledProcessError(1, 'gh'),
            ),
            patch.object(subprocess, 'run') as create,
        ):
            with self.assertRaises(subprocess.CalledProcessError):
                release_tag.ensure_tag('owner/repo', 'v1.0.1', 'tested')
            create.assert_not_called()

    def test_development_version_cannot_be_published(self) -> None:
        """Only the validated assigned release version may become a tag."""
        with patch.object(subprocess, 'check_output') as lookup:
            with self.assertRaises(ValueError):
                release_tag.ensure_tag('owner/repo', 'v1.0.1.dev0', 'tested')
            lookup.assert_not_called()
