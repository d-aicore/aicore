# AICore-C: puts a build together as a program that runs on its own.
#   1. compiled\<version>\  = AICoreC.exe + aicore-render.exe + supported-plugins.json + services\ (+ VERSION.txt)
#   2. gate on that copy   = AICoreC --self-test, aicore-render hello, one grab of EDIT > TRACKS
#   3. -Promote            = the files go to D:\AICore\Main (what was there before goes to historico\Main-<time>)
#   4. -Shortcut           = "AICORE (novo)" on the Desktop -> Main\AICoreC.exe (the old AICORE shortcut is not touched)
# Nothing here reads or writes the Core. Run it at low priority (start /LOW) after scripts\build-s5.cmd.
param(
    [string]$Build = (Join-Path $PSScriptRoot "..\build"),
    [string]$Root = "D:\AICore",
    [switch]$Promote,
    [switch]$Shortcut
)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$exe = Join-Path $Build "app\AICoreC_artefacts\Release\AICoreC.exe"
$render = Join-Path $Build "app\aicore-render_artefacts\Release\aicore-render.exe"
$plugins = Join-Path $Build "app\aicore-render_artefacts\Release\supported-plugins.json"
foreach ($f in $exe, $render, $plugins) { if (-not (Test-Path $f)) { throw "missing build output: $f" } }

$version = (& $exe --version | Select-Object -First 1).Trim()
if (-not $version) { throw "AICoreC --version printed nothing" }
$safe = ($version -replace '[^0-9A-Za-z._+-]', '_') -replace '\+', '-'
$dest = Join-Path $Root "compiled\$safe"
if (Test-Path $dest) { Move-Item $dest (Join-Path $Root ("historico\compiled-" + $safe + "-" + (Get-Date -Format yyyyMMdd-HHmmss))) }
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item $exe, $render, $plugins $dest
robocopy (Join-Path $repo "services") (Join-Path $dest "services") /E /XD __pycache__ eval /NFL /NDL /NJH /NJS /NP | Out-Null
# the services validate requests with the contract's schemas and validator (services\<x>\..\..\contracts)
robocopy (Join-Path $repo "contracts") (Join-Path $dest "contracts") /E /XD __pycache__ evidence examples /NFL /NDL /NJH /NJS /NP | Out-Null
foreach ($f in "contracts\schemas\v1\service-task-request.schema.json", "contracts\sim\jsonschema_mini.py") {
    if (-not (Test-Path (Join-Path $dest $f))) { throw "package is missing $f" }
}
@("version: $version", "built: " + (Get-Item $exe).LastWriteTime.ToString("yyyy-MM-dd HH:mm"),
  "commit: " + (git -C $repo rev-parse --short HEAD), "branch: " + (git -C $repo rev-parse --abbrev-ref HEAD)) |
    Set-Content -Encoding utf8 (Join-Path $dest "VERSION.txt")

# gate on the copy, not on the build folder
$fail = @()
$st = & (Join-Path $dest "AICoreC.exe") --self-test
if ($LASTEXITCODE -ne 0 -or -not ($st -match '^ok:')) { $fail += "self-test: $st" }
$hello = & (Join-Path $dest "aicore-render.exe") hello 2>$null
if ($LASTEXITCODE -ne 0) { $fail += "aicore-render hello exit $LASTEXITCODE" }
$png = Join-Path $dest "gate-tracks.png"
$grab = & (Join-Path $dest "AICoreC.exe") --area 0 --section 0 --grab $png
if (-not ($grab -match '^ok: grab') -or -not (Test-Path $png)) { $fail += "grab: $grab" }
if ($fail.Count) { $fail | ForEach-Object { Write-Output "RED $_" }; exit 2 }
Write-Output "GREEN $dest ($version)"

if ($Promote) {
    $main = Join-Path $Root "Main"
    New-Item -ItemType Directory -Force $main | Out-Null
    $old = Get-ChildItem $main | Where-Object Name -ne "README.md"
    if ($old) {
        $keep = Join-Path $Root ("historico\Main-" + (Get-Date -Format yyyyMMdd-HHmmss))
        New-Item -ItemType Directory -Force $keep | Out-Null
        $old | Move-Item -Destination $keep
        Write-Output "previous Main -> $keep"
    }
    Get-ChildItem $dest | Where-Object Name -ne "gate-tracks.png" | Copy-Item -Destination $main -Recurse
    Write-Output "Main = $version"
}

if ($Shortcut) {
    $lnk = Join-Path ([Environment]::GetFolderPath("Desktop")) "AICORE (novo).lnk"
    $s = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk)
    $s.TargetPath = Join-Path $Root "Main\AICoreC.exe"
    $s.WorkingDirectory = Join-Path $Root "Main"
    $s.Description = "AICORE nativo (C++/JUCE)"
    $s.Save()
    Write-Output "shortcut: $lnk"
}
