"""Check platform scheduling and publication gates without starting CI jobs."""

import json
import re
import unittest
from pathlib import Path

WORKFLOWS = Path(__file__).resolve().parents[2] / '.github/workflows'


def job_blocks(filename: str) -> dict[str, str]:
    """Read the repository's two-space job blocks without interpreting step scripts."""
    source: str = (
        (WORKFLOWS / filename).read_text(encoding='utf-8').split('jobs:\n', 1)[1]
    )
    return dict(
        re.findall(
            r'^  (\w+):\n(.*?)(?=^  \w+:\n|\Z)', source, re.MULTILINE | re.DOTALL
        )
    )


def dependencies(block: str) -> set[str]:
    """Read a job's scalar or inline-list prerequisite names."""
    match = re.search(r'^    needs: (.+)$', block, re.MULTILINE)
    return set(match[1].strip('[]').replace(' ', '').split(',')) if match else set()


class WorkflowDependenciesTests(unittest.TestCase):
    """Linux work must proceed independently, but publishing still requires Windows."""

    def test_windows_native_architectures_have_complete_release_coverage(self) -> None:
        """Build both linkages natively and keep installers, services, and wheels gated."""
        native = job_blocks('release_windows.yml')['tests']
        services = job_blocks('windows_services.yml')['tests']
        wheels = job_blocks('release_python_windows.yml')['wheels']
        for block in (native, services):
            self.assertIn('arch: [x86, x86_64, arm64]', block)
            self.assertIn(
                "runs-on: ${{ matrix.arch == 'arm64' && 'windows-11-arm' || 'windows-2022' }}",
                block,
            )
        self.assertIn('linkage: [static, shared]', native)
        self.assertNotIn('-DVCPKG_MANIFEST_MODE=OFF', native)
        self.assertNotIn('-DVCPKG_INSTALL_OPTIONS=', native)
        self.assertNotIn('-DVCPKG_INSTALLED_DIR=', native)
        for command in (
            "Invoke-Checked ctest",
            'package.py windows --architecture "$env:BUILD_ARCH"',
            'build_windows_installer.ps1 -Architecture $env:BUILD_ARCH',
            'test_windows_installer.ps1 -Architecture $env:BUILD_ARCH',
        ):
            self.assertIn(command, native)
        self.assertIn(
            "architecture: ${{ matrix.arch == 'arm64' && 'arm64' || 'x64' }}", native
        )
        self.assertIn("matrix.arch == 'arm64' && 'release_windows_arm64'", wheels)
        self.assertIn("matrix.arch == 'arm64' && 'windows_arm64'", wheels)
        self.assertIn("'release_windows_x86*'", wheels)
        publish = job_blocks('release.yml')['publish']
        for suffix in ('portable.zip', 'setup.exe'):
            self.assertIn(f'*-windows-arm64-{suffix})', publish)

    def test_native_presets_cover_each_architecture_profile_and_linkage(self) -> None:
        """Keep Debug and Release configure/test presets for every native architecture."""
        presets = json.loads((WORKFLOWS.parents[1] / 'CMakePresets.json').read_text())
        configure = {preset['name']: preset for preset in presets['configurePresets']}
        tests = {preset['name']: preset for preset in presets['testPresets']}
        for architecture, compiler in (
            ('x86', 'Win32'),
            ('x86_64', 'x64'),
            ('arm64', 'ARM64'),
        ):
            for profile, build_type in (('dbg', 'Debug'), ('opt', 'Release')):
                for linkage in ('static', 'shared'):
                    name = f'{architecture}-{profile}-{linkage}'
                    with self.subTest(preset=name):
                        preset = configure[name]
                        self.assertEqual(
                            preset['architecture'],
                            {'value': compiler, 'strategy': 'external'},
                        )
                        variables = preset['cacheVariables']
                        self.assertEqual(variables['LOCKD_TARGET_ARCH'], architecture)
                        self.assertEqual(variables['CMAKE_BUILD_TYPE'], build_type)
                        self.assertEqual(
                            variables['LOCKD_STATIC_RUNTIME'], linkage == 'static'
                        )
                        self.assertEqual(
                            variables['BUILD_SHARED_LIBS'], linkage == 'shared'
                        )
                        self.assertEqual(
                            preset['binaryDir'],
                            f'${{sourceDir}}/output/obj/{architecture}/{profile}/{linkage}',
                        )
                        self.assertEqual(tests[name]['configurePreset'], name)

    def test_all_release_wheel_architectures_are_built_and_preserved(self) -> None:
        """New ABIs must have independent jobs, cache keys and tested wheel artifacts."""
        windows = job_blocks('release_python_windows.yml')['wheels']
        linux = job_blocks('release_python.yml')['wheels']
        self.assertIn('arch: [x86_64, x86, arm64]', windows)
        self.assertIn("'windows-11-arm'", windows)
        self.assertIn('architecture: ${{ matrix.arch', windows)
        self.assertIn('-Architecture $env:WHEEL_ARCH', windows)
        self.assertIn(
            "LOCKD_MSVC_TOOLSET_VERSION: ${{ matrix.arch == 'arm64' && '14.44' || '' }}",
            windows,
        )
        self.assertIn(
            'artifact: release_wheel_${{ matrix.arch }}_${{ matrix.python }}', windows
        )
        self.assertIn(
            'name: release_wheel_${{ matrix.arch }}_${{ matrix.python }}', windows
        )
        self.assertIn('boost-wheel-windows-v1-${{ matrix.arch }}-', windows)
        self.assertIn('lib/pylockd/output/${{ matrix.arch }}/opt/static/*.whl', windows)
        self.assertIn('arch: [x86_64, i686, aarch64]', linux)
        self.assertIn('i686) platform=linux/386', linux)
        self.assertIn('docker run --rm --platform', linux)

    def test_recommended_installer_uses_an_asset_label(self) -> None:
        """Highlight the x64 installer without changing filenames used by consumers."""
        publish = job_blocks('release.yml')['publish']
        self.assertIn(
            "*-windows-x86_64-setup.exe) label='Windows x86_64 - Installer (Recommended)'",
            publish,
        )

    def test_release_checks_active_runs_before_packaging(self) -> None:
        """Don't spend time packaging or recover a version still being assigned."""
        source: str = job_blocks('release.yml')['source']
        guard: int = source.index('reuse_checks.py --check-idle')
        self.assertLess(guard, source.index('- name: Test packaging'))
        self.assertLess(guard, source.index('build_version.py'))
        self.assertIn('name: compatibility_reuse\n', source)
        self.assertNotIn('name: release_reuse\n', source)

    def test_manual_release_publishes_only_after_assembly(self) -> None:
        """Manual and tag releases use the assigned version and checked-out commit."""
        jobs: dict[str, str] = job_blocks('release.yml')
        block: str = jobs['publish']
        self.assertEqual(dependencies(block), {'source', 'assemble', 'sign_windows'})
        expression: str = block.split('    if: >-\n', 1)[1].split('    runs-on:', 1)[0]
        expression = ' '.join(
            expression.replace('&&', 'and').replace('||', 'or').split()
        )
        for event, ref, eligible in (
            ('workflow_dispatch', 'refs/heads/main', True),
            ('push', 'refs/tags/v1.0.1', True),
            ('push', 'refs/heads/main', False),
        ):
            for result in ('success', 'failure', 'skipped'):
                for cancelled in (False, True):
                    condition: str = expression.replace(
                        '!cancelled()', str(not cancelled)
                    )
                    condition = condition.replace('needs.assemble.result', repr(result))
                    condition = condition.replace(
                        'needs.sign_windows.result', repr(result)
                    )
                    condition = condition.replace('github.event_name', repr(event))
                    condition = condition.replace(
                        "startsWith(github.ref, 'refs/tags/v')",
                        str(ref.startswith('refs/tags/v')),
                    )
                    self.assertEqual(
                        bool(eval(condition, {'__builtins__': {}})),
                        eligible and result == 'success' and not cancelled,
                    )
        for name in ('publish', 'release_branch'):
            self.assertIn(
                'RELEASE_TAG: v${{ needs.source.outputs.version }}', jobs[name]
            )
            self.assertIn(
                'RELEASE_COMMIT: ${{ needs.source.outputs.commit }}', jobs[name]
            )
        self.assertEqual(dependencies(jobs['release_branch']), {'source', 'publish'})
        self.assertLess(block.index('sha256sum --check'), block.index('release_tag.py'))
        self.assertLess(block.index('release_tag.py'), block.index('gh release create'))

    def test_linux_reuse_input_is_boolean_on_push(self) -> None:
        """Push events have no workflow_call inputs; don't forward an empty string."""
        block: str = job_blocks('compatibility.yml')['linux']
        self.assertIn('      reuse_native: ${{ inputs.reuse_native == true }}\n', block)
        self.assertEqual(dependencies(block), {'source', 'wheels_linux'})

    def test_signing_failure_blocks_both_new_release_publishers(self) -> None:
        """Explicit cancellation handling must not bypass the signature success gate."""
        jobs = job_blocks('release.yml')
        for name in ('publish', 'publish_python'):
            self.assertIn("needs.sign_windows.result == 'success'", jobs[name])
            self.assertIn('sign_windows', dependencies(jobs[name]))
        self.assertIn('name: signed_release', jobs['publish'])
        signing = jobs['sign_windows']
        self.assertIn('    environment: lockd\n', signing)
        self.assertIn('      id-token: write\n', signing)
        self.assertNotIn('contents: write', signing)
        self.assertIn('    runs-on: windows-2022\n', signing)
        self.assertEqual(signing.count('timestamp-digest: SHA256'), 2)
        self.assertLess(
            signing.index('-Operation Rebuild'),
            signing.index('name: Sign and timestamp installers'),
        )
        self.assertLess(
            signing.index('-Operation Collect'),
            signing.index('name: Generate checksums after signing'),
        )

    def test_pypi_upload_uses_validated_wheels_after_all_release_checks(
        self,
    ) -> None:
        """Publish the tested artifact only on release events, with a scoped API token."""
        release: dict[str, str] = job_blocks('release.yml')
        block: str = release['publish_python']
        self.assertIn('    name: Publish wheels to PyPI\n', block)
        self.assertIn('name: Check PyPI credentials\n', block)
        self.assertIn('name: Upload validated wheels to PyPI\n', block)
        self.assertEqual(dependencies(block), {'source', 'assemble', 'sign_windows'})

        # Both destinations use the already-tested manual/tag release policy.
        # Assembly depends on every compatibility category and tested_python.
        def condition(job: str) -> str:
            return ' '.join(
                job.split('    if: >-\n', 1)[1].split('    runs-on:', 1)[0].split()
            )

        self.assertEqual(condition(block), condition(release['publish']))
        self.assertNotIn('publish_python', job_blocks('compatibility.yml'))
        self.assertNotIn(
            'PYPI_API_TOKEN', (WORKFLOWS / 'compatibility.yml').read_text()
        )
        self.assertIn('name: tested_python\n', block)
        self.assertNotIn('actions/checkout@', block)
        self.assertNotIn('cmake --build', block)
        self.assertNotIn('-m build', block)
        self.assertNotIn('id-token: write', block)
        self.assertIn('if [[ -z "$PYPI_API_TOKEN" ]]', block)
        self.assertIn('password: ${{ secrets.PYPI_API_TOKEN }}', block)
        self.assertIn('user: __token__', block)
        self.assertIn('repository-url: https://upload.pypi.org/legacy/', block)
        self.assertIn('attestations: false', block)
        self.assertIn('skip-existing: true', block)
        self.assertRegex(block, r'pypa/gh-action-pypi-publish@[a-f0-9]{40}')

        downloaded: str = re.search(r'^          path: (.+)$', block, re.MULTILINE)[1]
        uploaded: str = re.search(
            r'^          packages-dir: (.+)$', block, re.MULTILINE
        )[1]
        self.assertEqual(downloaded.rstrip('/'), uploaded.rstrip('/'))
        self.assertIn('lib/pylockd/output/', downloaded)
        self.assertLess(
            block.index('name: tested_python'),
            block.index('uses: pypa/gh-action-pypi-publish@'),
        )

    def test_platform_groups_call_matrices_directly(self) -> None:
        """Keep platform names visible without relying on deeper sidebar nesting."""
        groups = (
            ('wheels_linux', 'Python - Linux', 'release_python.yml', 'wheels'),
            (
                'wheels_windows',
                'Python - Windows',
                'release_python_windows.yml',
                'wheels',
            ),
            (
                'windows',
                'Compatibility Testing - Windows',
                'release_windows.yml',
                'tests',
            ),
            (
                'linux',
                'Compatibility Testing - Linux',
                'compatibility_linux.yml',
                'tests',
            ),
            ('freebsd', 'Compatibility Testing - FreeBSD', 'freebsd.yml', 'tests'),
            (
                'windows_services',
                'Service Testing - Windows',
                'windows_services.yml',
                'tests',
            ),
        )
        for caller in ('compatibility.yml', 'release.yml'):
            jobs: dict[str, str] = job_blocks(caller)
            for job, heading, matrix_file, matrix_job in groups:
                with self.subTest(caller=caller, heading=heading):
                    block: str = jobs[job]
                    self.assertIn(f'    name: {heading}\n', block)
                    self.assertIn(
                        f'    uses: ./.github/workflows/{matrix_file}\n', block
                    )
                    matrix_jobs: dict[str, str] = job_blocks(matrix_file)
                    self.assertEqual(set(matrix_jobs), {matrix_job})
                    self.assertIn('    strategy:\n', matrix_jobs[matrix_job])

    def test_clang_tidy_headings_identify_each_platform(self) -> None:
        """Keep the requested Linux and Windows headings inside the clang-tidy group."""
        for platform in ('Linux', 'Windows'):
            filename: str = f'clang_tidy_{platform.lower()}.yml'
            with self.subTest(workflow=filename):
                jobs: dict[str, str] = job_blocks(filename)
                self.assertEqual(len(jobs), 1)
                block: str = next(iter(jobs.values()))
                self.assertIn(f'    name: {platform}\n', block)

            for caller in ('compatibility.yml', 'release.yml'):
                with self.subTest(workflow=caller, platform=platform):
                    block = job_blocks(caller)[f'analysis_{platform.lower()}']
                    self.assertIn('    name: clang-tidy\n', block)
                    self.assertIn(f'    uses: ./.github/workflows/{filename}\n', block)

    def test_service_checks_use_packages_and_keep_the_same_order(self) -> None:
        """Service checks follow native Windows builds in both entry points."""
        for filename in ('compatibility.yml', 'release.yml'):
            jobs: dict[str, str] = job_blocks(filename)
            self.assertEqual(
                dependencies(jobs['windows_services']), {'source', 'windows'}
            )
            order: list[str] = list(jobs)
            self.assertLess(order.index('windows'), order.index('windows_services'))
            self.assertEqual(order.index('windows_services') + 1, order.index('linux'))
        worker: str = job_blocks('windows_services.yml')['tests']
        self.assertIn('arch: [x86, x86_64, arm64]', worker)
        self.assertIn('dependencies: windows_${{ matrix.arch }}', worker)
        self.assertIn('name: release_windows_${{ matrix.arch }}', worker)
        self.assertIn('job: Service Testing - Windows / ${{ matrix.arch }}', worker)
        self.assertNotIn('cmake --build', worker)

    def test_linux_path_has_no_windows_prerequisites(self) -> None:
        """Both entry points schedule Linux wheels and distro tests independently."""
        for filename in ('compatibility.yml', 'release.yml'):
            with self.subTest(workflow=filename):
                jobs: dict[str, str] = job_blocks(filename)

                def ancestors(name: str, graph: dict[str, str] = jobs) -> set[str]:
                    """Follow the actual workflow graph, including indirect barriers."""
                    parents: set[str] = dependencies(graph[name])
                    return parents | set().union(*(ancestors(p) for p in parents))

                distro: str = 'linux'
                self.assertEqual(ancestors('wheels_linux'), {'source'})
                self.assertEqual(ancestors(distro), {'source', 'wheels_linux'})
                self.assertEqual(
                    ancestors('wheels_windows'),
                    {'source', 'windows'},
                )
                gate: str = (
                    'tested_python' if filename == 'compatibility.yml' else 'assemble'
                )
                self.assertTrue(
                    {'wheels_linux', 'wheels_windows'}.issubset(ancestors(gate))
                )

    def test_analysis_runs_alongside_builds(self) -> None:
        """Check both workflow graphs for parallel analysis and mandatory final gates."""
        # Inspect prerequisites and failure handling so analysis cannot serialize builds
        # or become optional when the workflows preserve wheels or assemble a release.
        for filename in ('compatibility.yml', 'release.yml'):
            with self.subTest(workflow=filename):
                jobs: dict[str, str] = job_blocks(filename)
                analysis: set[str] = {'analysis_linux', 'analysis_windows'}
                for name in analysis | {'windows', 'wheels_linux'}:
                    self.assertEqual(dependencies(jobs[name]), {'source'})
                for name, block in jobs.items():
                    if name not in {'assemble', 'tested_python'}:
                        self.assertFalse(dependencies(block) & analysis)
                for name in analysis:
                    self.assertNotIn('continue-on-error:', jobs[name])
                if filename == 'release.yml':
                    self.assertTrue(analysis <= dependencies(jobs['assemble']))

    def test_wheel_preservation_requires_all_validation(self) -> None:
        """Check that both wheel-preservation jobs require every validation category."""
        # Inspect the actual needs lists and success conditions. A failed, cancelled,
        # or skipped category must prevent these jobs from retaining validated wheels.
        required: set[str] = set(job_blocks('compatibility.yml')) - {'tested_python'}
        for filename in ('compatibility.yml', 'release.yml'):
            with self.subTest(workflow=filename):
                block: str = job_blocks(filename)['tested_python']
                self.assertEqual(dependencies(block), required)

                # GitHub's default success gate rejects failed, cancelled and skipped needs.
                # Compatibility also restricts this job to its standalone invocation.
                conditions: list[str] = re.findall(
                    r'^    if: (.+)$', block, re.MULTILINE
                )
                self.assertEqual(
                    conditions,
                    ["success() && inputs.source_version == ''"]
                    if filename == 'compatibility.yml'
                    else [],
                )

    def test_release_requires_every_compatibility_category(self) -> None:
        """Successful reused jobs and rerun jobs pass the same publication gate."""
        release: dict[str, str] = job_blocks('release.yml')
        compatibility: dict[str, str] = job_blocks('compatibility.yml')
        parts: set[str] = set(compatibility) - {'source', 'tested_python'}
        self.assertEqual(
            dependencies(release['assemble']), parts | {'source', 'tested_python'}
        )
        self.assertNotIn('    if:', release['assemble'])
        for part in parts:
            with self.subTest(part=part):
                self.assertEqual(
                    dependencies(release[part]), dependencies(compatibility[part])
                )
                self.assertIn('      reuse_checks: true', release[part])
                self.assertNotIn('    if:', release[part])
                workflow: str = re.search(
                    r'uses: ./\.github/workflows/(.+)', release[part]
                )[1]
                worker: str = next(iter(job_blocks(workflow).values()))
                self.assertIn('uses: ./.github/actions/reuse_compatibility', worker)
                steps: list[str] = worker.split('      - name: ')[1:]
                found: bool = False
                for step in steps:
                    if 'id: reuse' in step:
                        found = True
                    elif found:
                        self.assertIn("if: steps.reuse.outputs.reused != 'true'", step)
