<#
.SYNOPSIS
C1 guardrail: assert CrossEngineEditor does not depend on the O3DE Atom renderer
(Plan.md A6 C1 / B12 E1). NOTE: this script is deliberately ASCII-only so that
PowerShell 5.1 never misreads it under any system codepage (UTF-8 vs GBK).

.DESCRIPTION
Five layers of assertion; any failure exits with code 1:
  1. Source layer: .h/.cpp under Code/Source must not reference Atom rendering
     modules (Atom/RPI, Atom/RHI, Atom/Feature, etc.) after stripping comments.
     AtomToolsFramework headers are allowed (vendored per SS11.9); only rendering-
     module includes and namespace references are forbidden.
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
  4. Contract assertion: IMaterialSource must have exactly 8 pure virtual
     methods ("= 0;"). Sentinel defaults (body provided) are not counted.
  5. Vendor diff whitelist: files under Vendor/AtomToolsFramework/ must only
     differ from upstream by S1-S9 modifications (informational; a full diff
     requires the upstream checkout).

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
$exitCode = 0

# ------------------------------------------------------------ 1. Source layer
# Forbid Atom RENDERING modules, not AtomToolsFramework which is legitimately
# vendored (material_migration_final.md SS11.9, vendor copy per SS3.5/SS11.9).
$forbidden = @(
    'Atom/RPI', 'Atom/RHI', 'Atom/Feature', 'Atom/Bootstrap',
    'Atom/ImageProcessing', 'Atom/Component', 'AtomLyIntegration',
    'AZ::RPI', 'AZ::RHI', 'AZ::Render'
)
# Whitelist: MaterialEditor/Vendor/AtomToolsFramework (vendor copy per SS11.9)
# The vendor path is allowed; only rendering-module references are forbidden.

Get-ChildItem -Path (Join-Path $gemRoot "Code/Source") -Recurse -Include *.h, *.cpp |
    ForEach-Object {
        $content = Get-Content -Raw -LiteralPath $_.FullName
        # Strip comments; forbidden patterns may survive only inside comments.
        # -cmatch keeps this case-sensitive (plain -match would false-positive
        # on "std::atomic" or "AtomToolsFramework").
        $stripped = $content -replace '/\*[\s\S]*?\*/', ''
        $stripped = $stripped -replace '//[^\r\n]*', ''

        foreach ($pattern in $forbidden) {
            if ($stripped -cmatch [regex]::Escape($pattern)) {
                $failures.Add("source references Atom rendering module '$pattern': $($_.FullName)")
            }
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

# ------------------------------------------------------ 4. Contract assertion
# IMaterialSource pure virtual count must be exactly 8 (sentinels are not counted).
# "virtual ~IMaterialSource() = default;" does NOT match "= 0;" so it is excluded.
$materialSourceFile = Join-Path $gemRoot "Code/Source/BackendAPI/IMaterialSource.h"
if (Test-Path $materialSourceFile) {
    $virtualCount = (Select-String -Path $materialSourceFile -Pattern '= 0;' | Measure-Object).Count
    if ($virtualCount -ne 8) {
        Write-Error "IMaterialSource has $virtualCount pure virtuals (expected 8)"
        $exitCode = 1
    }
    else {
        Write-Host "check_no_atom: IMaterialSource pure virtual count = 8 (contract OK)" -ForegroundColor Green
    }
}
else {
    Write-Warning "IMaterialSource.h not found; skipping contract assertion"
}

# --------------------------------------------------- 5. Vendor diff whitelist
# Files under MaterialEditor/Vendor/AtomToolsFramework/ should only differ from
# upstream AtomToolsFramework by the S1-S9 modifications documented in
# material_migration_final.md SS4. This is an informational check; a full diff
# requires the upstream checkout to be available.
$vendorDir = Join-Path $gemRoot "Code/Source/MaterialEditor/Vendor/AtomToolsFramework"
if (Test-Path $vendorDir) {
    $vendorFiles = Get-ChildItem -Path $vendorDir -Recurse -Include *.h, *.cpp
    if ($vendorFiles.Count -gt 0) {
        Write-Host "check_no_atom: $($vendorFiles.Count) vendor files under AtomToolsFramework/ (diff whitelist applies)" -ForegroundColor Yellow
    }
}
# No hard failure here -- this is informational. The actual diff whitelist
# assertion requires the upstream checkout and is enforced manually during
# sync reviews.

# --------------------------------------------------------------------- Summary
if ($failures.Count -eq 0 -and $exitCode -eq 0) {
    Write-Host "check_no_atom: PASS (C1 holds, contract OK)" -ForegroundColor Green
    exit 0
}
foreach ($f in $failures) {
    Write-Host "check_no_atom: FAIL - $f" -ForegroundColor Red
}
if ($exitCode -ne 0) {
    Write-Host "check_no_atom: FAIL - contract assertion failed" -ForegroundColor Red
}
exit 1
