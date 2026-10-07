param([switch]$BuildHost,[int]$CaptureSeconds=360)
$ErrorActionPreference="Stop"
$repo=(Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Push-Location $repo
try {
  if($BuildHost){ & .\md.bat build host; if($LASTEXITCODE -ne 0){throw "host build failed"} }
  & cmake -S .\pico -B .\build-pico\out; if($LASTEXITCODE -ne 0){throw "cmake failed"}
  & cmake --build .\build-pico\out --target microdos_pico_m29a_intprofile; if($LASTEXITCODE -ne 0){throw "build failed"}
  & .\scripts\md_engine_splitbench.ps1 -Uf2 .\build-pico\out\microdos_pico_m29a_intprofile.uf2 -Label "M29a-v2-int-profile" -CaptureSeconds $CaptureSeconds
  if($LASTEXITCODE -ne 0){throw "splitbench failed"}
  $log=Get-ChildItem .\logs\engine-splitbench-M29a-v2-int-profile-*.txt | Sort-Object LastWriteTime -Descending | Select-Object -First 1
  $raw=Get-Content $log.FullName -Raw
  if($raw -notmatch 'm25:\s+translator ON'){throw "M29a v2 invalid: translator did not initialize"}
  Write-Host ""; Write-Host "=== M29a v2 INT PROFILE RESULT ==="
  Get-Content $log.FullName | Select-String 'm25:\s+translator ON','^=== ENGINE INTERVAL SUMMARY:','^BOOT\s','^DOS2TEST #','^MDSTRESS AUTO','^\[intprof\]','^\[m25\] top steps:'
  Write-Host "saved: $($log.FullName)"
} finally { Pop-Location }
