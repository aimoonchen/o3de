<#
.SYNOPSIS
C1 guardrail: assert CrossEngineEditor does not depend on the O3DE Atom renderer
(Plan.md A6 C1 / B12 E1). NOTE: this script is deliberately ASCII-only so that
PowerShell 5.1 never misreads it under any system codepage (UTF-8 vs GBK).

.DESCRIPTION
Three layers of assertion; any failure exits with code 1:
  1. Source layer: .h/.cpp under Code/Source must not contain "Atom" after
     stripping comments (mentioning Atom inside comments is allowed).
  2. Declaration layer: gem.json "dependencies" and Code/CMakeLists.txt must
     not contain "Atom".
  3. Build layer (requires a configured build tree, default build/windows):
     a. Link inputs (AdditionalDependencies of the generated
        CrossEngineEditor.vcxproj) must not contain Atom_*.lib.
     b. Any Atom project reference must have ReferenceOutputAssembly=false
        (build-order-only). Such references are propagated by O3DE's runtime
        dependency walker from AzToolsFramework's own build deps - a framework
        fact the vision rules allow; a real link reference (flag missing or
        not "false") fails the check.
     Layer 3 is skipped with a warning when no vcxproj is found (build tree
     not configured yet); layers 1-2 still run.

.EXAMPLE
.\Scripts\check_no_atom.ps1
.\Scripts\check_no_atom.ps1 -BuildDir build\vs2022
#>
param(
    [string]$BuildDir = "build/windows"
)

$ErrorActionPreference = "Stop"
$gemRoot = Split-Path -Parent $PSScriptRoot
$failures = New-Object System.Collections.Generic.List[string]

# ------------------------------------------------------------ 1. Source layer
Get-ChildItem -Path (Join-Path $gemRoot "Code/Source") -Recurse -Include *.h, *.cpp |
    ForEach-Object {
        $content = Get-Content -Raw -LiteralPath $_.FullName
        # Strip comments; "Atom" may survive only inside comments. -cmatch keeps
        # this case-sensitive (plain -match would false-positive on "std::atomic").
        $stripped = $content -replace '/\*[\s\S]*?\*/', ''
        $stripped = $stripped -replace '//[^\r\n]*', ''
        if ($stripped -cmatch 'Atom') {
            $failures.Add("source references Atom: $($_.FullName)")
        }
    }

# ------------------------------------------------------- 2. Declaration layer
$gemJson = Get-Content -Raw -LiteralPath (Join-Path $gemRoot "gem.json") | ConvertFrom-Json
if (@($gemJson.dependencies) | Where-Object { $_ -match 'Atom' }) {
    $failures.Add("gem.json dependencies declare an Atom dependency")
}
$cmakeText = Get-Content -Raw -LiteralPath (Join-Path $gemRoot "Code/CMakeLists.txt")
if ($cmakeText -match 'Atom') {
    $failures.Add("Code/CMakeLists.txt references Atom")
}

# -------------------------------------------------------------- 3. Build layer
$buildAbs = Join-Path (Split-Path -Parent (Split-Path -Parent $gemRoot)) $BuildDir
$vcxproj = Get-ChildItem -Path (Join-Path $buildAbs "External") -Recurse -Filter "CrossEngineEditor.vcxproj" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -like "*External\CrossEngineEditor-*\Code\CrossEngineEditor.vcxproj" } |
    Select-Object -First 1

if (-not $vcxproj) {
    Write-Warning "CrossEngineEditor.vcxproj not found (build not configured?); skipping layer 3"
}
else {
    $projText = Get-Content -Raw -LiteralPath $vcxproj.FullName

    # 3a. Link inputs must not contain Atom_*.lib (any configuration).
    foreach ($m in [regex]::Matches($projText, '<AdditionalDependencies[^>]*>([\s\S]*?)</AdditionalDependencies>')) {
        if ($m.Groups[1].Value -match 'Atom[A-Za-z0-9_]*\.lib') {
            $failures.Add("link input contains an Atom library: $($Matches[0])")
        }
    }

    # 3b. Atom project references must be build-order-only.
    $projXml = [xml]$projText
    foreach ($ref in @($projXml.Project.ItemGroup.ProjectReference | Where-Object { $_.Include -match 'Atom' })) {
        if ($ref.ReferenceOutputAssembly -ne 'false') {
            $failures.Add("Atom project reference is not build-order-only (ReferenceOutputAssembly=$($ref.ReferenceOutputAssembly)): $($ref.Include)")
        }
    }
}

# --------------------------------------------------------------------- Summary
if ($failures.Count -eq 0) {
    Write-Host "check_no_atom: PASS (C1 holds)" -ForegroundColor Green
    exit 0
}
foreach ($f in $failures) {
    Write-Host "check_no_atom: FAIL - $f" -ForegroundColor Red
}
exit 1
