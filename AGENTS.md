# Agent instructions

## Releases

GitHub Actions does not build or publish this project, and no workflow should be added for it. The agent working in a session builds and publishes every release itself.

After pushing commits to `master`, and only once they have built and passed the local tests, run this from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/release.ps1 -Minor <hash>,<hash> -Major <hash>
```

Decide the kind of each unreleased commit yourself and list it. Commits that are not listed count as fixes, so leave out `-Minor` and `-Major` when there are none.

The script builds each unreleased commit on its own in `build/publish`, then creates a tag and a GitHub release with that commit's Windows download attached. Every release must carry its own build. Commits starting with `[skip]` are not released. Report the release number and link at the end of the session, or say plainly why no release was made.

Release numbers are always `A.B.cc`, such as `0.4.07`, with no prefix, and the tag, release title, download, and program version all use the same number.

- `A`, major: changes that are not backwards-compatible. Raising it gives `1.0.00`.
- `B`, minor: backwards-compatible changes, such as new features or speedups. Raising it gives `0.5.00`.
- `cc`, patch: bug fixes, security updates, documentation, and release tooling. Raising it gives `0.4.08`.

Raising one part resets the parts after it, and only one part moves per release. `cc` is always two digits, and a release after `.99` raises `B` instead. Keep the version in `CMakeLists.txt` and `vcpkg.json` at the latest `A.B`; it is only the number local builds report.
