<#
.SYNOPSIS
Builds the Windows package and publishes a tag and GitHub release for each unreleased commit.

.DESCRIPTION
Run this from a working session after pushing master. Tags are named v<version>-build.<commit count>,
so every commit has a stable, ordered name. The newest commit carries the download. Older commits in
the same run get a notes-only release that points at it, because the package is built once per run.
Commits whose subject starts with [skip] are agent housekeeping and are not released. Existing tags
are left untouched, so reruns are safe.

.PARAMETER SkipBuild
Packages the existing build/release output instead of rebuilding it first.
#>
param([switch]$SkipBuild)
$taskRoot = Split-Path $PSScriptRoot -Parent
Set-Location $taskRoot
$taskVersion = [regex]::Match((Get-Content -Raw 'CMakeLists.txt'), 'project\(SourceCubemap VERSION (\d+\.\d+\.\d+)').Groups[1].Value
if (!$taskVersion) { throw 'Cannot read the project version.' }
$taskGh = Join-Path $env:ProgramFiles 'GitHub CLI\gh.exe'
if (!(Test-Path -LiteralPath $taskGh)) { $taskGh = 'gh' }

function Get-ReleaseTag([string]$Commit) { "v$taskVersion-build.$(git rev-list --count $Commit)" }
function Assert-Success([string]$Action) { if ($LASTEXITCODE -ne 0) { throw "$Action failed ($LASTEXITCODE)." } }

if ((git rev-parse --abbrev-ref HEAD) -ne 'master') { throw 'Releases are published from master.' }
if (git status --porcelain) { throw 'Commit local changes before releasing.' }
git fetch --quiet --tags origin master
Assert-Success 'Fetching origin'
if ((git rev-parse HEAD) -ne (git rev-parse origin/master)) { throw 'Push master before releasing.' }

$taskLast = git tag --list 'v*-build.*' --merged HEAD | Sort-Object { [int]($_ -replace '^.*-build\.', '') } | Select-Object -Last 1
$taskCommits = if ($taskLast) { @(git rev-list --reverse --first-parent "$taskLast..HEAD") } else { @(git rev-parse HEAD) }
$taskReleases = @($taskCommits | Where-Object { !(git log -1 --format=%s $_).StartsWith('[skip]') })
if ($taskReleases.Count -eq 0) { Write-Output 'Nothing to release.'; return }
$taskNewest = $taskReleases[-1]
$taskNewestTag = Get-ReleaseTag $taskNewest

if (!$SkipBuild) {
    $taskConfigure = if (Test-Path -LiteralPath 'build/release/build.ninja') { '-SkipConfigure' } else { '' }
    & cmd.exe /d /c "powershell -NoProfile -ExecutionPolicy Bypass -File `"$PSScriptRoot\build.ps1`" $taskConfigure"
    Assert-Success 'Building'
}

$taskWork = Join-Path ([IO.Path]::GetTempPath()) "SourceCubemap-release-$PID"
$taskArchive = Join-Path $taskWork 'SourceCubemap-windows-x64.zip'
$taskNotesFile = Join-Path $taskWork 'notes.md'
try {
    & cmd.exe /d /c "powershell -NoProfile -ExecutionPolicy Bypass -File `"$PSScriptRoot\package.ps1`" -Destination `"$taskWork\SourceCubemap`""
    Assert-Success 'Packaging'
    Compress-Archive -Path (Join-Path $taskWork 'SourceCubemap') -DestinationPath $taskArchive
    foreach ($taskCommit in $taskReleases) {
        $taskTag = Get-ReleaseTag $taskCommit
        if (git tag --list $taskTag) { continue }
        $taskSubject = git log -1 --format=%s $taskCommit
        $taskNotes = (git log -1 --format=%b $taskCommit | Out-String).Trim()
        if ($taskCommit -ne $taskNewest) { $taskNotes = "$taskNotes`n`nThe Windows download for this change is in $taskNewestTag.".Trim() }
        [IO.File]::WriteAllText($taskNotesFile, $taskNotes)
        git tag $taskTag $taskCommit
        Assert-Success "Tagging $taskTag"
        git push --quiet origin "refs/tags/$taskTag"
        Assert-Success "Pushing $taskTag"
        if ($taskCommit -eq $taskNewest) {
            & $taskGh release create $taskTag $taskArchive --title $taskSubject --notes-file $taskNotesFile --verify-tag --latest
        } else {
            & $taskGh release create $taskTag --title $taskSubject --notes-file $taskNotesFile --verify-tag --latest=false
        }
        Assert-Success "Publishing $taskTag"
    }
} finally { Remove-Item -LiteralPath $taskWork -Recurse -Force -ErrorAction SilentlyContinue }
Write-Output "Released $taskNewestTag"
