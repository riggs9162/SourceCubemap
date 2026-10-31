# Agent instructions

## Releases

GitHub Actions does not build or publish this project, and no workflow should be added for it. The agent working in a session builds and publishes every release itself.

After pushing commits to `master`, and only once they have built and passed the local tests, run this from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/release.ps1
```

The script builds each unreleased commit on its own in `build/publish`, then creates a tag and a GitHub release with that commit's Windows download attached. Every release must carry its own build. Commits starting with `[skip]` are not released. Report the release number and link at the end of the session, or say plainly why no release was made.

Release numbers are always `AA.BB.CC`, two digits in each part with no prefix, and the tag, release title, download, and program version all use the same number. `AA.BB` comes from the version in `CMakeLists.txt` and `vcpkg.json`; raise it there when a change deserves a new number. `CC` counts releases and is set by the script. After 99 it wraps to 00 and `BB` moves up.
