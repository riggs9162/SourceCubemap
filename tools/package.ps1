param(
    [string]$BuildDirectory = 'build/release',
    [Parameter(Mandatory)][string]$Destination
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
$taskBuild = if ([IO.Path]::IsPathRooted($BuildDirectory)) { [IO.Path]::GetFullPath($BuildDirectory) } else { [IO.Path]::GetFullPath((Join-Path $taskRoot $BuildDirectory)) }
$taskOutput = [IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $taskOutput) { throw 'The package destination must be a new directory.' }
$taskFiles = @('sourcecubemap.exe', 'sourcecubemap-gui.exe', 'shaders/scene.vert.spv', 'shaders/scene.frag.spv')
foreach ($taskFile in $taskFiles) {
    if (!(Test-Path -LiteralPath (Join-Path $taskBuild $taskFile))) { throw "Build output is missing: $taskFile" }
}
$taskLicenses = [ordered]@{
    'sourcepp.txt' = '_deps/sourcepp-src/LICENSE'
    'bufferstream.txt' = '_deps/bufferstream-src/LICENSE'
    'bcdec.txt' = '_deps/bcdec-src/LICENSE'
    'half.txt' = '_deps/sourcepp-src/ext/half/LICENSE.txt'
    'stb.txt' = '_deps/sourcepp-src/ext/stb/LICENSE'
    'hat-trie.txt' = '_deps/tsl_hat_trie-src/LICENSE'
    'miniz.txt' = '_deps/miniz-src/LICENSE'
    'minizip-ng.txt' = '_deps/minizip-ng-src/LICENSE'
    'tomcrypt.txt' = '_deps/libtomcrypt-src/LICENSE'
    'tommath.txt' = '_deps/libtommath-src/LICENSE'
    'bzip2.txt' = '_deps/minizip-ng-build/_deps/bzip2-src/LICENSE'
    'liblzma.txt' = '_deps/minizip-ng-build/_deps/liblzma-src/COPYING.0BSD'
    'liblzma-scope.txt' = '_deps/minizip-ng-build/_deps/liblzma-src/COPYING'
    'zlib-ng.txt' = '_deps/minizip-ng-build/_deps/zlib-src/LICENSE.md'
    'zstd.txt' = '_deps/minizip-ng-build/_deps/zstd-src/LICENSE'
    'compressonator.txt' = '_deps/cmp_compressonator-src/license/license.txt'
    'compressonator-core.txt' = '_deps/cmp_compressonator-src/license/corelicense.txt'
    'compressonator-sdk.txt' = '_deps/cmp_compressonator-src/license/sdklicense.txt'
    'compressonator-framework.txt' = '_deps/cmp_compressonator-src/license/frameworklicense.txt'
}
foreach ($taskPackage in @('cli11','fmt','glm','nlohmann-json','spdlog','volk','vulkan-headers','vulkan-memory-allocator','xxhash')) {
    $taskLicenses["$taskPackage.txt"] = "vcpkg_installed/x64-windows-static/share/$taskPackage/copyright"
}
foreach ($taskLicense in $taskLicenses.Values) {
    if (!(Test-Path -LiteralPath (Join-Path $taskBuild $taskLicense))) { throw "Dependency notice is missing: $taskLicense" }
}
New-Item -ItemType Directory -Path $taskOutput | Out-Null
New-Item -ItemType Directory -Path (Join-Path $taskOutput 'shaders'), (Join-Path $taskOutput 'licenses') | Out-Null
foreach ($taskFile in $taskFiles) { Copy-Item -LiteralPath (Join-Path $taskBuild $taskFile) -Destination (Join-Path $taskOutput $taskFile) }
foreach ($taskFile in @('LICENSE','README.md','THIRD_PARTY.md')) { Copy-Item -LiteralPath (Join-Path $taskRoot $taskFile) -Destination (Join-Path $taskOutput $taskFile) }
Copy-Item -LiteralPath (Join-Path $taskRoot 'docs') -Destination (Join-Path $taskOutput 'docs') -Recurse
foreach ($taskEntry in $taskLicenses.GetEnumerator()) { Copy-Item -LiteralPath (Join-Path $taskBuild $taskEntry.Value) -Destination (Join-Path $taskOutput ('licenses/' + $taskEntry.Key)) }
@'
PPMdI codec: Igor Pavlov, public domain.
Based on PPMd var.I (2002): Dmitry Shkarin, public domain.
Only the C PPMd codec is linked; 7-Zip command-line and RAR components are not included.
Source: the Ppmd8.c, Ppmd8.h, and Ppmd.h headers in the pinned minizip PPMd dependency.
'@ | Set-Content -LiteralPath (Join-Path $taskOutput 'licenses/ppmd.txt') -Encoding utf8
$taskHashes = [ordered]@{}
foreach ($taskFile in $taskFiles) { $taskHashes[$taskFile] = (Get-FileHash -LiteralPath (Join-Path $taskOutput $taskFile) -Algorithm SHA256).Hash.ToLowerInvariant() }
$taskCommit = & git -C $taskRoot rev-parse HEAD
$taskDirty = @(& git -C $taskRoot status --porcelain).Count -gt 0
$taskVersion = (& (Join-Path $taskBuild 'sourcecubemap.exe') --version) -replace '^SourceCubemap ', ''
if ($LASTEXITCODE -ne 0 -or $taskVersion -notmatch '^\d+\.\d+\.\d+$') { throw 'Cannot determine the packaged executable version.' }
[ordered]@{ version = $taskVersion; status = 'experimental'; source_commit = $taskCommit; source_dirty = $taskDirty; packaged_utc = [DateTime]::UtcNow.ToString('o'); sha256 = $taskHashes } |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $taskOutput 'manifest.json') -Encoding utf8
Write-Output $taskOutput
