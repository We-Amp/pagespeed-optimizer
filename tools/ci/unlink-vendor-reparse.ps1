# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# unlink-vendor-reparse.ps1 - remove the reparse points (junctions and
# symlinks) a previous build left in a Windows runner workspace.
#
# A Windows build leaves vendor\bazel-external as a junction to a Bazel output
# base, which itself holds many nested junctions that point back into
# vendor\. The checkout step's clean walks that loop on every run. This script
# runs before the checkout and removes only the links, so the walk has nothing
# to follow.
#
# It looks at the direct children of <Workspace>\vendor and, when
# <Workspace>\vendor\bazel-external is a real directory, at its direct
# children. Each reparse point goes with [IO.Directory]::Delete($path, $false),
# which removes the link and never its target. It never recurses into a
# reparse point. A workspace without vendor\ is not an error.
#
# Usage: unlink-vendor-reparse.ps1 -Workspace <dir>
# Prints: unlinked <n> reparse point(s)
param(
  [Parameter(Mandatory = $true)][string]$Workspace
)
$ErrorActionPreference = 'Stop'

$reparse = [IO.FileAttributes]::ReparsePoint
$count = 0

function Remove-Links {
  param([string]$Dir)
  $di = New-Object System.IO.DirectoryInfo($Dir)
  foreach ($e in $di.GetFileSystemInfos()) {
    if (($e.Attributes -band $reparse) -ne 0) {
      [IO.Directory]::Delete($e.FullName, $false)
      $script:count += 1
    }
  }
}

$vendor = Join-Path $Workspace 'vendor'
if (-not (Test-Path -LiteralPath $vendor)) {
  Write-Host 'unlinked 0 reparse point(s)'
  exit 0
}

Remove-Links $vendor

# A real (non-link) bazel-external directory: unlink the links inside it.
$bx = Join-Path $vendor 'bazel-external'
if (Test-Path -LiteralPath $bx) {
  $item = Get-Item -LiteralPath $bx -Force
  if (($item.Attributes -band $reparse) -eq 0) { Remove-Links $bx }
}

Write-Host ("unlinked " + $count + " reparse point(s)")
exit 0
