# Builds the ARM binary into app\Overclock and packs dist\Overclock-<version>.zip (extracts to SD root as App/Overclock/...).
# Needs Zig 0.14 (https://ziglang.org/download/) on PATH or passed with -Zig.
#   .\build.ps1 -Version 1.0.0
#   .\build.ps1 -Preview          # also builds overclock_preview.exe (renders PNG frames on a PC)
param([string]$Version = '1.0.0', [string]$Zig = 'zig', [switch]$Preview)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

& $Zig cc -target arm-linux-musleabihf -mcpu=cortex_a7 -O2 -static -s src/overclock.c -o app/Overclock/overclock -lm
if ($LASTEXITCODE) { throw 'ARM build failed' }
if ($Preview) {
    & $Zig cc -target x86_64-windows-gnu -DPREVIEW -O2 src/overclock.c -o overclock_preview.exe
    if ($LASTEXITCODE) { throw 'preview build failed' }
}

# Zip with forward-slash entry names (Compress-Archive on Windows PowerShell writes backslashes).
# Never ship a card's own state: settings, backups, preview frames.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
New-Item -ItemType Directory -Force dist | Out-Null
$zip = Join-Path $PSScriptRoot "dist\Overclock-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip }
$root = Join-Path $PSScriptRoot 'app'
$archive = [IO.Compression.ZipFile]::Open($zip, 'Create')
try {
    Get-ChildItem $root -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($root.Length + 1).Replace('\', '/')
        if ($rel -match '^Overclock/(settings\.txt|backup/|frames/)') { return }
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $_.FullName, "App/$rel", 'Optimal') | Out-Null
    }
}
finally { $archive.Dispose() }
"built $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
