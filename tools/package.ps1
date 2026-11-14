# Packages a release zip from a build folder:
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1 -BuildDir build -Version v0.2.0
# Produces dist\VC3Recompiled-<version>-Windows-x64.zip with the launcher, the runtime, the SDK the
# launcher compiles against, and recomplauncher.json at the top (next to VC3Launcher.exe).
# No game files are involved: players recompile from their own disc image.
param(
    [string]$BuildDir = "build",
    [string]$Version = "dev",
    [string]$OutDir = "dist"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path (Resolve-Path (Join-Path $root $BuildDir)) "bin"

$files = @("VC3Launcher.exe", "vc3runtime.dll", "recomplauncher.json",
           "sdk\vc3runtime.lib", "sdk\psp_recomp.h", "sdk\vc2_runtime.h")
foreach ($f in $files) {
    if (-not (Test-Path (Join-Path $bin $f))) { throw "missing $f in $bin (build the VC3Launcher target first)" }
}

$name = "VC3Recompiled-$Version-Windows-x64"
$stage = Join-Path ([IO.Path]::GetTempPath()) $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force (Join-Path $stage "sdk") | Out-Null
foreach ($f in $files) { Copy-Item (Join-Path $bin $f) (Join-Path $stage $f) }
Copy-Item (Join-Path $root "README.md") $stage
Copy-Item (Join-Path $root "src\runtime\media\atrac3plus\LICENSE-NOTICE.md") (Join-Path $stage "LICENSE-ATRAC3PLUS.md")

$dist = if ([IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $root $OutDir }
New-Item -ItemType Directory -Force $dist | Out-Null
$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
# (Compress-Archive in Windows PowerShell 5.1 writes "\" separators, which other unzippers mishandle)
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    Get-ChildItem -Recurse -File $stage | ForEach-Object {
        $entry = $_.FullName.Substring($stage.Length).TrimStart('\').Replace('\', '/')
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $_.FullName, $entry,
            [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally {
    $archive.Dispose()
}
Remove-Item -Recurse -Force $stage
Write-Output "wrote $zip"
