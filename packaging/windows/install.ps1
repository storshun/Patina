# Installs Patina for the current user (no administrator rights needed).
#   Right-click > Run with PowerShell, or:
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-AddToPath] [-Uninstall]
param(
  [switch]$AddToPath,
  [switch]$Uninstall,
  [string]$Destination = "$env:LOCALAPPDATA\Programs\Patina"
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$startMenu = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs"
$shortcut = Join-Path $startMenu "Patina.lnk"

function Remove-FromUserPath($dir) {
  $p = [Environment]::GetEnvironmentVariable("Path", "User")
  if ($p) {
    $parts = $p.Split(";") | Where-Object { $_ -and ($_.TrimEnd("\") -ne $dir.TrimEnd("\")) }
    [Environment]::SetEnvironmentVariable("Path", ($parts -join ";"), "User")
  }
}

if ($Uninstall) {
  if (Test-Path $shortcut) { Remove-Item $shortcut -Force }
  Remove-FromUserPath $Destination
  if (Test-Path $Destination) { Remove-Item $Destination -Recurse -Force }
  Write-Host "Patina removed. Your materials and settings in $env:LOCALAPPDATA\Patina and $env:APPDATA\Patina are kept."
  exit 0
}

Write-Host "Installing Patina to $Destination"
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
foreach ($item in @("patina.exe", "patina-cli.exe", "materials", "blender", "docs", "web", "samples",
                    "patina_bridge.zip", "README.md", "INSTALL-WINDOWS.txt", "install.ps1", "LICENSES")) {
  $src = Join-Path $here $item
  if (Test-Path $src) {
    $dst = Join-Path $Destination $item
    if (Test-Path $dst) { Remove-Item $dst -Recurse -Force }
    Copy-Item $src $dst -Recurse -Force
  }
}

# Start-menu shortcut
$ws = New-Object -ComObject WScript.Shell
$lnk = $ws.CreateShortcut($shortcut)
$lnk.TargetPath = Join-Path $Destination "patina.exe"
$lnk.WorkingDirectory = $Destination
$lnk.Description = "Patina texture painter"
$lnk.Save()

if ($AddToPath) {
  Remove-FromUserPath $Destination
  $p = [Environment]::GetEnvironmentVariable("Path", "User")
  [Environment]::SetEnvironmentVariable("Path", (($p, $Destination) -join ";").Trim(";"), "User")
  Write-Host "Added $Destination to your PATH (open a new terminal to use 'patina-cli')."
}

$cli = Join-Path $Destination "patina-cli.exe"
Write-Host ""
Write-Host "Done."
Write-Host "  Start:          Start menu > Patina   (or $Destination\patina.exe)"
Write-Host "  Manual:         $Destination\docs\manual\index.html"
Write-Host "  Patina Web:     $Destination\web\index.html"
Write-Host "  Blender add-on: $Destination\patina_bridge.zip  (Edit > Preferences > Get Extensions > Install from Disk)"
Write-Host "  Claude Code:    claude mcp add patina -- `"$cli`" --mcp"
Write-Host "  Uninstall:      powershell -ExecutionPolicy Bypass -File `"$Destination\install.ps1`" -Uninstall"
