# SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
# SPDX-License-Identifier: MIT

# Builds MessageHeightTwitch and installs it next to the local Chatterino build
# Requires .NET SDK 10, Rust and native build tools
[CmdletBinding()]
param(
    [string] $Source = (Join-Path $PSScriptRoot '../../.local-tools/message-height/upstream'),
    [string] $Output = (Join-Path $PSScriptRoot '../../build-ninja/bin/message-height')
)
$ErrorActionPreference = 'Stop'
$revision = 'd4ac1cbd3b4fec54223a6cc8ccf5ca78ddb0338e'
if ($IsWindows -and [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'X64') {
    $runtime = 'win-x64'
} elseif ($IsLinux -and [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'X64') {
    $runtime = 'linux-x64'
} else {
    throw 'Build MessageHeightTwitch on Windows x64 or Linux x64'
}
if (-not (Test-Path (Join-Path $Source '.git'))) {
    git init $Source
    if ($LASTEXITCODE) { throw 'Could not create upstream checkout' }
    git -C $Source remote add origin https://github.com/TETYYS/MessageHeightTwitch.git
    if ($LASTEXITCODE) { throw 'Could not configure upstream remote' }
    git -C $Source fetch --depth 1 origin $revision
    if ($LASTEXITCODE) { throw 'Could not fetch upstream revision' }
    git -C $Source checkout --detach FETCH_HEAD
    if ($LASTEXITCODE) { throw 'Clone failed' }
}
if ((git -C $Source rev-parse HEAD) -ne $revision) { throw "Expected upstream revision $revision" }
if (git -C $Source status --porcelain --untracked-files=no) { throw 'The upstream source must be unmodified' }
$Source = (Resolve-Path $Source).Path
$Output = [IO.Path]::GetFullPath($Output)
if ($IsWindows -and -not (Test-Path (Join-Path $Source 'Native/icu78/win-x64/icuuc78.dll'))) {
    & (Join-Path $Source 'Tools/IcuData/Build-IcuData.ps1')
}
dotnet publish (Join-Path $Source 'MessageHeightTwitch.csproj') -c Release -r $runtime `
    "-p:CustomAfterMicrosoftCommonTargets=$(Join-Path $PSScriptRoot 'Chatterino.targets')" -o $Output
if ($LASTEXITCODE) { throw 'MessageHeightTwitch build failed' }
# Download the emoji list when building so we don't need to fetch it at runtime
$catalog = Invoke-RestMethod 'https://cdn.frankerfacez.com/static/emoji/v2-.json'
$emojis = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
function Add-Emoji([string] $sequence) {
    if (-not $sequence) { return }
    $value = -join ($sequence.Split('-') | ForEach-Object { [char]::ConvertFromUtf32([Convert]::ToInt32($_, 16)) })
    [void] $emojis.Add($value)
}
foreach ($entry in $catalog.e) {
    if ($entry.Count -ge 5) { Add-Emoji $entry[4] }
    if ($entry.Count -ge 8 -and $entry[7] -is [array]) {
        foreach ($alternate in $entry[7]) { if ($alternate.Count) { Add-Emoji $alternate[0] } }
    }
}
[IO.File]::WriteAllText((Join-Path $Output 'ffz-emojis.json'), (ConvertTo-Json @($emojis) -Compress))
[IO.File]::WriteAllText((Join-Path $Output 'upstream-revision'), $revision)
Copy-Item (Join-Path $Source 'LICENSE') (Join-Path $Output 'LICENSE.MessageHeightTwitch') -Force
Copy-Item (Join-Path $Source 'Native/icu78/LICENSE') (Join-Path $Output 'LICENSE.ICU') -Force
$python = if ($IsWindows) { 'python' } else { 'python3' }
& $python (Join-Path $PSScriptRoot 'smoke-test.py') $Output
if ($LASTEXITCODE) { throw 'MessageHeightTwitch smoke test failed' }
Write-Host "Built MessageHeightTwitch $revision in $Output"
