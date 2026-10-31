# Agent instructions

## Releases

GitHub Actions does not build or publish this project, and no workflow should be added for it. The agent working in a session builds and publishes every release itself.

After pushing commits to `master`, and only once they have built and passed the local tests, run this from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/release.ps1
```

The script rebuilds, packages, and creates a tag and GitHub release for each unreleased commit. The newest commit carries the Windows download. Commits starting with `[skip]` are not released. Report the release tag and link at the end of the session, or say plainly why no release was made.

Raise the version in `CMakeLists.txt` and `vcpkg.json` when a change is large enough to deserve a new number. Tags are `v<version>-build.<commit count>`.
