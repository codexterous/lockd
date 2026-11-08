"""Package the current working tree for a benchmark without publishing through CI."""

import gzip
import hashlib
import json
import os
import subprocess
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def source_files(root: Path) -> dict[str, int]:
    """Select tracked and nonignored new files, using Git's executable modes."""
    indexed = subprocess.check_output(['git', 'ls-files', '--stage', '-z'], cwd=root)
    modes: dict[str, int] = {}
    for record in indexed.split(b'\0'):
        if not record:
            continue
        metadata, name = record.split(b'\t', 1)
        mode, _, stage = metadata.split()
        if stage != b'0':
            raise ValueError("Resolve Git conflicts before packaging the working tree")
        modes[os.fsdecode(name)] = int(mode, 8)
    new = subprocess.check_output(
        ['git', 'ls-files', '--others', '--exclude-standard', '-z'], cwd=root
    )
    for name in new.split(b'\0'):
        if name:
            modes[os.fsdecode(name)] = 0o100644
    return modes


def package(root: Path) -> tuple[Path, str, int]:
    """
    Archive current bytes, including unstaged edits, while omitting deleted files.

    Keep one top-level directory for the benchmark agent. Private files are never
    included, even if accidentally tracked. Reject links and submodules because
    the remote extractor accepts ordinary files only. Replace a previous archive
    only after the new one is complete.
    """
    root = root.resolve()
    destination = root / '.private/lockd-dev.tar.gz'
    temporary = destination.with_suffix('.gz.tmp')
    files = source_files(root)
    destination.parent.mkdir(exist_ok=True)
    count = 0
    try:
        with (
            temporary.open('wb') as output,
            gzip.GzipFile(filename='', mode='wb', fileobj=output, mtime=0) as zipped,
            tarfile.open(fileobj=zipped, mode='w|') as archive,
        ):
            for name, mode in sorted(files.items()):
                if Path(name).parts[0] == '.private':
                    continue
                path = root / name
                if path.is_symlink() or not path.resolve().is_relative_to(root):
                    raise ValueError(f"Cannot package a symbolic link: {name}")
                if not path.exists():
                    continue
                if not path.is_file() or mode not in (0o100644, 0o100755):
                    raise ValueError(f"Cannot package a nonregular Git entry: {name}")
                member = tarfile.TarInfo('lockd-dev/' + Path(name).as_posix())
                member.size = path.stat().st_size
                member.mode = 0o755 if mode == 0o100755 else 0o644
                with path.open('rb') as contents:
                    archive.addfile(member, contents)
                count += 1
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)
    with destination.open('rb') as contents:
        digest = hashlib.file_digest(contents, 'sha256').hexdigest()
    return destination, digest, count


def main() -> None:
    """Write the local archive and print the two benchmark configuration fields."""
    archive, digest, count = package(ROOT)
    print(f"Packaged {count} working-tree files ({archive.stat().st_size} bytes)")
    print(
        json.dumps({'source_archive': str(archive), 'source_sha256': digest}, indent=2)
    )


if __name__ == '__main__':
    main()
