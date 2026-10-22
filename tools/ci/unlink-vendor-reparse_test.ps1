# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Hermetic test for unlink-vendor-reparse.ps1. No Pester dependency.
# Builds a temp workspace whose vendor\bazel-external is a junction to a temp
# target holding sentinel.txt, plus a normal vendor\keep\a.txt, and checks:
#   * the junction is removed;
#   * the junction's target (and its sentinel) survives;
#   * normal vendor files are untouched;
#   * a workspace without vendor\ exits 0;
#   * the output reports exactly 1 reparse point.
#
# Run:   powershell -NoProfile -ExecutionPolicy Bypass -File tools\ci\unlink-vendor-reparse_test.ps1
$ErrorActionPreference = 'Stop'

$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $here 'unlink-vendor-reparse.ps1'

$fails = 0
function Pass { param([string]$Name) Write-Host ("PASS: " + $Name) }
function Fail { param([string]$Name) Write-Host ("FAIL: " + $Name); $script:fails += 1 }

$roots = New-Object System.Collections.Generic.List[string]
function New-TempRoot {
  $p = Join-Path ([System.IO.Path]::GetTempPath()) ("unlink-vendor-reparse-test-" + [guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Path $p | Out-Null
  $roots.Add($p)
  return $p
}

try {
  $root   = New-TempRoot
  $ws     = Join-Path $root 'ws'
  $target = Join-Path $root 'target'
  $vendor = Join-Path $ws 'vendor'
  $link   = Join-Path $vendor 'bazel-external'
  $keep   = Join-Path $vendor 'keep'
  New-Item -ItemType Directory -Path $target, $keep | Out-Null
  [System.IO.File]::WriteAllText((Join-Path $target 'sentinel.txt'), 'x')
  [System.IO.File]::WriteAllText((Join-Path $keep 'a.txt'), 'x')
  New-Item -ItemType Junction -Path $link -Target $target | Out-Null

  $out = $null
  $code = 0
  if (Test-Path -LiteralPath $script) {
    $out = (& powershell -NoProfile -ExecutionPolicy Bypass -File $script -Workspace $ws 2>&1 | Out-String)
    $code = $LASTEXITCODE
  } else {
    Write-Host ("script missing: " + $script)
    $code = 127
  }

  if (-not (Test-Path -LiteralPath $link)) { Pass 'bazel-external junction removed' } else { Fail 'bazel-external junction removed' }
  if (Test-Path -LiteralPath (Join-Path $target 'sentinel.txt')) { Pass 'target sentinel survives' } else { Fail 'target sentinel survives' }
  if (Test-Path -LiteralPath (Join-Path $keep 'a.txt')) { Pass 'normal vendor files untouched' } else { Fail 'normal vendor files untouched' }

  $empty = Join-Path $root 'empty-ws'
  New-Item -ItemType Directory -Path $empty | Out-Null
  $emptyCode = 127
  if (Test-Path -LiteralPath $script) {
    $null = (& powershell -NoProfile -ExecutionPolicy Bypass -File $script -Workspace $empty 2>&1 | Out-String)
    $emptyCode = $LASTEXITCODE
  }
  if ($emptyCode -eq 0) { Pass 'absent vendor dir exits 0' } else { Fail 'absent vendor dir exits 0' }

  if ($code -eq 0 -and $out -match 'unlinked 1 reparse point\(s\)') { Pass 'output reports 1 reparse point' } else { Fail ('output reports 1 reparse point (exit ' + $code + ', output: ' + $out + ')') }
}
finally {
  foreach ($r in $roots) {
    # Drop any junction first so cleanup never follows it into a target.
    foreach ($j in (Get-ChildItem -LiteralPath $r -Recurse -Force -ErrorAction SilentlyContinue | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint })) {
      try { [IO.Directory]::Delete($j.FullName, $false) } catch { }
    }
    Remove-Item -LiteralPath $r -Recurse -Force -ErrorAction SilentlyContinue
  }
}

if ($fails -gt 0) { Write-Host ("FAILED: " + $fails); exit 1 }
Write-Host 'ALL PASS'
exit 0
