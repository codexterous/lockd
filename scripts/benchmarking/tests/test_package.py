"""Verify development archives use current files without private or ignored data."""

import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

from scripts.benchmarking import agent
from scripts.benchmarking import package as development
from scripts.benchmarking.config import file_digest, output_directory


class PackageTests(unittest.TestCase):
    """Exercise real Git file selection and the production remote extractor."""

    def test_working_tree_archive_matches_remote_extraction_contract(self) -> None:
        """Include edits and new source, preserve modes, and omit ignored secrets."""
        base = output_directory('package-tests')
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as directory:
            root = Path(directory)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            (root / '.gitignore').write_text('.private/\nignored/\n', encoding='utf-8')
            (root / 'CMakeLists.txt').write_text('project(lockd)', encoding='utf-8')
            (root / 'changed.cpp').write_text('old', encoding='utf-8')
            (root / 'deleted.cpp').write_text('deleted', encoding='utf-8')
            subprocess.run(['git', 'add', '.'], cwd=root, check=True)
            subprocess.run(
                ['git', 'update-index', '--chmod=+x', 'changed.cpp'],
                cwd=root,
                check=True,
            )
            (root / 'changed.cpp').write_text('current', encoding='utf-8')
            (root / 'deleted.cpp').unlink()
            (root / 'new source.cpp').write_text('new', encoding='utf-8')
            (root / '.private').mkdir()
            (root / '.private/key').write_text('secret', encoding='utf-8')
            (root / 'ignored').mkdir()
            (root / 'ignored/object').write_text('object', encoding='utf-8')
            archive, digest, count = development.package(root)
            self.assertEqual(count, 4)
            self.assertEqual(digest, file_digest(archive))
            with tarfile.open(archive) as contents:
                self.assertEqual(
                    contents.getmember('lockd-dev/changed.cpp').mode, 0o755
                )
                self.assertEqual(len(contents.getnames()), 4)
            extracted = root / '.private/extracted'
            agent.extract(archive, extracted)
            self.assertEqual(
                (extracted / 'lockd-dev/changed.cpp').read_text(), 'current'
            )
            self.assertEqual(
                (extracted / 'lockd-dev/new source.cpp').read_text(), 'new'
            )
            self.assertFalse((extracted / 'lockd-dev/.private').exists())
            self.assertFalse((extracted / 'lockd-dev/ignored').exists())
            self.assertEqual(development.package(root)[1], digest)


if __name__ == '__main__':
    unittest.main()
