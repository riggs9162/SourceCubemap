<#
.SYNOPSIS
Builds and publishes one GitHub release, with its own Windows download, for each unreleased commit.

.DESCRIPTION
Run this from a working session after pushing master. Versions are always A.B.cc, such as 0.4.07, with A
for major changes that break compatibility, B for backwards-compatible changes, and cc for bug fixes and
other small corrections. Raising one part resets the parts after it. Commits count as fixes unless listed
in -Minor or -Major. The tag, the download, and the version the program reports all use the same number.
Release titles pair that number with the commit subject, and the commit body's bullets become the notes.
With -Together, all unreleased commits ship as one release, built from the last of them, under -Title.

Each commit is built on its own in build/publish, a separate checkout of this repository, so a release
cannot pick up later changes and an open copy of the program cannot block the build. The first run takes
several minutes because it compiles every dependency. Commits whose subject starts with [skip] are
agent housekeeping and are not released.

.PARAMETER Minor
Hashes of commits that add or improve behavior without breaking compatibility. Each raises B and resets cc.

.PARAMETER Major
Hashes of commits that break compatibility. Each raises A and resets B and cc.

.PARAMETER Together
Publishes every unreleased commit as one release. The largest kind listed for any of them sets its number.

.PARAMETER Title
The release title after the number. Required with -Together, because no single commit subject fits.
#>
param([string[]]$Minor = @(), [string[]]$Major = @(), [switch]$Together, [string]$Title)
$taskRoot = Split-Path $PSScriptRoot -Parent
Set-Location $taskRoot
$taskGh = Join-Path $env:ProgramFiles 'GitHub CLI\gh.exe'
if (!(Test-Path -LiteralPath $taskGh)) { $taskGh = 'gh' }

function Assert-Success([string]$Action) { if ($LASTEXITCODE -ne 0) { throw "$Action failed ($LASTEXITCODE)." } }
function Test-Listed([string[]]$Commits, [string[]]$Hashes) {
    foreach ($taskHash in $Hashes) { if (@($Commits | Where-Object { $_.StartsWith($taskHash) }).Count -gt 0) { return $true } }
    $false
}
function Get-Changes([string]$Commit) {
    $taskBullets = @(git log -1 --format=%b $Commit | Where-Object { $_.StartsWith('- ') })
    if ($taskBullets.Count -gt 0) { $taskBullets } else { "- $(git log -1 --format=%s $Commit)" }
}
function Get-ReleaseNotes([string]$Version, [string]$Previous, [string[]]$Commits) {
    $taskChanges = @($Commits | ForEach-Object { Get-Changes $_ })
    $taskRepository = 'https://github.com/riggs9162/SourceCubemap'
    @(
        '## What changed', '', ($taskChanges -join "`n"), '',
        '## Install', '',
        "1. Download ``SourceCubemap-$Version-windows-x64.zip`` below and unzip it anywhere.",
        '2. Run `sourcecubemap-gui.exe`, or use `sourcecubemap.exe` from a terminal.', '',
        "You need 64-bit Windows 10 or 11 and a graphics driver with Vulkan 1.3. The [README]($taskRepository/blob/$Version/README.md) has a full walkthrough.", '',
        "[All changes since $Previous]($taskRepository/compare/$Previous...$Version)"
    ) -join "`n"
}
function Get-NextVersion([string]$Previous, [string[]]$Commits) {
    if (!$Previous) {
        $taskProject = [regex]::Match((Get-Content -Raw 'CMakeLists.txt'), 'project\(SourceCubemap VERSION (\d+)\.(\d+)\.\d+')
        if (!$taskProject.Success) { throw 'Cannot read the project version.' }
        return '{0}.{1}.00' -f $taskProject.Groups[1].Value, $taskProject.Groups[2].Value
    }
    $taskParts = @($Previous.Split('.') | ForEach-Object { [int]$_ })
    if (Test-Listed $Commits $Major) { $taskParts = @(($taskParts[0] + 1), 0, 0) }
    elseif (Test-Listed $Commits $Minor) { $taskParts = @($taskParts[0], ($taskParts[1] + 1), 0) }
    elseif ($taskParts[2] -ge 99) { $taskParts = @($taskParts[0], ($taskParts[1] + 1), 0) }
    else { $taskParts = @($taskParts[0], $taskParts[1], ($taskParts[2] + 1)) }
    '{0}.{1}.{2:00}' -f $taskParts[0], $taskParts[1], $taskParts[2]
}

if ($Together -and !$Title) { throw 'Give a -Together release a -Title.' }
if ((git rev-parse --abbrev-ref HEAD) -ne 'master') { throw 'Releases are published from master.' }
if (git status --porcelain) { throw 'Commit local changes before releasing.' }
git fetch --quiet --tags origin master
Assert-Success 'Fetching origin'
if ((git rev-parse HEAD) -ne (git rev-parse origin/master)) { throw 'Push master before releasing.' }

$taskLast = git tag --list '[0-9]*.[0-9]*.[0-9][0-9]' --merged HEAD | Sort-Object { [version]$_ } | Select-Object -Last 1
$taskCommits = if ($taskLast) { @(git rev-list --reverse --first-parent "$taskLast..HEAD") } else { @(git rev-parse HEAD) }
$taskReleases = @($taskCommits | Where-Object { !(git log -1 --format=%s $_).StartsWith('[skip]') })
foreach ($taskHash in @($Minor) + @($Major)) {
    if (@($taskReleases | Where-Object { $_.StartsWith($taskHash) }).Count -ne 1) { throw "$taskHash is not one unreleased commit." }
}
if ($taskReleases.Count -eq 0) { Write-Output 'Nothing to release.'; return }
$taskBatches = @(if ($Together) { , $taskReleases } else { $taskReleases | ForEach-Object { , @($_) } })

$taskPublish = Join-Path $taskRoot 'build\publish'
if (!(Test-Path -LiteralPath (Join-Path $taskPublish '.git'))) {
    git worktree add --quiet --detach $taskPublish HEAD
    Assert-Success 'Creating the release checkout'
}
$taskWork = Join-Path ([IO.Path]::GetTempPath()) "SourceCubemap-release-$PID"
$taskNotesFile = Join-Path $taskWork 'notes.md'
New-Item -ItemType Directory -Path $taskWork | Out-Null
try {
    foreach ($taskBatch in $taskBatches) {
        $taskCommit = $taskBatch[-1]
        $taskVersion = Get-NextVersion $taskLast $taskBatch
        $taskFolder = Join-Path $taskWork $taskVersion
        $taskArchive = Join-Path $taskWork "SourceCubemap-$taskVersion-windows-x64.zip"
        git -C $taskPublish checkout --quiet --detach $taskCommit
        Assert-Success "Checking out $taskCommit"
        & cmd.exe /d /c "powershell -NoProfile -ExecutionPolicy Bypass -File `"$taskPublish\tools\build.ps1`" -Version $taskVersion"
        Assert-Success "Building $taskVersion"
        & cmd.exe /d /c "powershell -NoProfile -ExecutionPolicy Bypass -File `"$taskPublish\tools\package.ps1`" -BuildDirectory `"$taskPublish\build\release`" -Destination `"$taskFolder\SourceCubemap`""
        Assert-Success "Packaging $taskVersion"
        Compress-Archive -Path (Join-Path $taskFolder 'SourceCubemap') -DestinationPath $taskArchive
        $taskSubject = if ($Title) { $Title } else { git log -1 --format=%s $taskCommit }
        [IO.File]::WriteAllText($taskNotesFile, (Get-ReleaseNotes $taskVersion $taskLast $taskBatch))
        git tag $taskVersion $taskCommit
        Assert-Success "Tagging $taskVersion"
        git push --quiet origin "refs/tags/$taskVersion"
        Assert-Success "Pushing $taskVersion"
        & $taskGh release create $taskVersion $taskArchive --title "${taskVersion}: $taskSubject" --notes-file $taskNotesFile --verify-tag --latest
        Assert-Success "Publishing $taskVersion"
        $taskLast = $taskVersion
    }
} finally { Remove-Item -LiteralPath $taskWork -Recurse -Force -ErrorAction SilentlyContinue }
Write-Output "Released $taskLast"
