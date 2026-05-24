# 监视源码变更并自动上传（无需 VS Code 扩展）
# 用法：在项目根目录执行  powershell -File tools/watch_upload.ps1

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$debounceSec = 2
$pending = $false
$lastUpload = [datetime]::MinValue

function Invoke-Upload {
  Write-Host "[$(Get-Date -Format 'HH:mm:ss')] 正在上传..." -ForegroundColor Cyan
  & pio run -t upload
  if ($LASTEXITCODE -ne 0) {
    Write-Host "上传失败 (exit $LASTEXITCODE)" -ForegroundColor Red
  } else {
    Write-Host "上传完成" -ForegroundColor Green
  }
}

$watcher = New-Object System.IO.FileSystemWatcher
$watcher.Path = Join-Path $root "src"
$watcher.Filter = "*.*"
$watcher.IncludeSubdirectories = $true
$watcher.EnableRaisingEvents = $true

$onChange = {
  $script:pending = $true
}

Register-ObjectEvent $watcher Changed -Action $onChange | Out-Null
Register-ObjectEvent $watcher Created -Action $onChange | Out-Null
Register-ObjectEvent $watcher Renamed -Action $onChange | Out-Null

$iniWatcher = New-Object System.IO.FileSystemWatcher
$iniWatcher.Path = $root
$iniWatcher.Filter = "platformio.ini"
$iniWatcher.EnableRaisingEvents = $true
Register-ObjectEvent $iniWatcher Changed -Action $onChange | Out-Null

Write-Host "监视 $root\src 与 platformio.ini，保存后 ${debounceSec}s 自动上传。Ctrl+C 退出。" -ForegroundColor Yellow

while ($true) {
  Start-Sleep -Milliseconds 400
  if (-not $pending) { continue }
  $pending = $false
  Start-Sleep -Seconds $debounceSec
  if ($pending) { continue }
  $now = [datetime]::UtcNow
  if (($now - $lastUpload).TotalSeconds -lt 1) { continue }
  $lastUpload = $now
  Invoke-Upload
}
