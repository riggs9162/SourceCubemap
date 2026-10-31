<#
.SYNOPSIS
Creates a tag and GitHub release for each commit in a pushed range.

.DESCRIPTION
Tags are named v<version>-build.<commit count>, so every commit has a stable, ordered name.
Commits older than the push head get a notes-only release that points at the head release, because
packages are built once per push. A first push, or a manual run, releases only the head commit.
Commits whose subject starts with [skip] are ignored. Existing tags are left untouched, so reruns are safe.

.PARAMETER Before
The commit that preceded the push. Empty or all zeros releases only the head commit.

.PARAMETER Head
The newest commit of the push.

.PARAMETER Package
The Windows package attached to the head release.
#>
param(
    [string]$Before,
    [Parameter(Mandatory)][string]$Head,
    [Parameter(Mandatory)][string]$Package
)
$ErrorActionPreference = 'Stop'
$taskVersion = [regex]::Match((Get-Content -Raw (Join-Path $PSScriptRoot '../CMakeLists.txt')), 'project\(SourceCubemap VERSION (\d+\.\d+\.\d+)').Groups[1].Value
if (!$taskVersion) { throw 'Cannot read the project version.' }

function Get-ReleaseTag([string]$Commit) { "v$taskVersion-build.$(git rev-list --count $Commit)" }
function Test-Tag([string]$Tag) { [bool](git tag --list $Tag) }

$taskCommits = @($Head)
if ($Before -and $Before -notmatch '^0+$') {
    git cat-file -e "$Before^{commit}" 2>$null
    if ($LASTEXITCODE -eq 0) { $taskCommits = @(git rev-list --reverse --first-parent "$Before..$Head") }
}
$taskHeadTag = Get-ReleaseTag $Head
foreach ($taskCommit in $taskCommits) {
    $taskSubject = git log -1 --format=%s $taskCommit
    if ($taskSubject.StartsWith('[skip]')) { continue }
    $taskTag = Get-ReleaseTag $taskCommit
    if (Test-Tag $taskTag) { continue }
    $taskNotes = git log -1 --format=%b $taskCommit | Out-String
    git tag $taskTag $taskCommit
    git push origin "refs/tags/$taskTag"
    if ($LASTEXITCODE -ne 0) { throw "Cannot push $taskTag." }
    if ($taskCommit -eq $Head) {
        gh release create $taskTag $Package --title $taskSubject --notes "$taskNotes" --verify-tag --latest
    } else {
        gh release create $taskTag --title $taskSubject --notes "$taskNotes`n`nThe Windows download for this change is in $taskHeadTag." --verify-tag --latest=false
    }
    if ($LASTEXITCODE -ne 0) { throw "Cannot publish $taskTag." }
}
