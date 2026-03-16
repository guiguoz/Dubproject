# setup.ps1 — Vérification des prérequis pour SaxFX Live (Windows)
# Usage : .\scripts\setup.ps1

$ErrorActionPreference = "Continue"
$allOk = $true

Write-Host ""
Write-Host "=== SaxFX Live — Vérification des prérequis ===" -ForegroundColor Cyan
Write-Host ""

# ─────────────────────────────────────────────────────────────────────────────
# 1. CMake >= 3.22
# ─────────────────────────────────────────────────────────────────────────────
Write-Host "[ CMake ]" -NoNewline
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmake) {
    $cmakeVersion = (cmake --version 2>&1) | Select-String "cmake version (\d+\.\d+)" |
        ForEach-Object { $_.Matches[0].Groups[1].Value }
    $major, $minor = $cmakeVersion -split '\.' | Select-Object -First 2 |
        ForEach-Object { [int]$_ }
    if ($major -gt 3 -or ($major -eq 3 -and $minor -ge 22)) {
        Write-Host "  OK — $cmakeVersion" -ForegroundColor Green
    } else {
        Write-Host "  ATTENTION — version $cmakeVersion < 3.22 requise" -ForegroundColor Yellow
        Write-Host "  → winget install Kitware.CMake" -ForegroundColor Gray
        $allOk = $false
    }
} else {
    Write-Host "  MANQUANT" -ForegroundColor Red
    Write-Host "  → winget install Kitware.CMake" -ForegroundColor Gray
    $allOk = $false
}

# ─────────────────────────────────────────────────────────────────────────────
# 2. MSVC 2022 (vswhere)
# ─────────────────────────────────────────────────────────────────────────────
Write-Host "[ MSVC 2022 ]" -NoNewline
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vsInstalls = & $vswhere -version "[17.0,18.0)" -format json 2>$null | ConvertFrom-Json
    if ($vsInstalls -and $vsInstalls.Count -gt 0) {
        Write-Host "  OK — $($vsInstalls[0].displayName)" -ForegroundColor Green
    } else {
        Write-Host "  MANQUANT (Visual Studio 2022 non trouvé)" -ForegroundColor Red
        Write-Host "  → https://aka.ms/vs/17/release/vs_BuildTools.exe" -ForegroundColor Gray
        $allOk = $false
    }
} else {
    Write-Host "  MANQUANT (vswhere introuvable)" -ForegroundColor Red
    Write-Host "  → https://aka.ms/vs/17/release/vs_BuildTools.exe" -ForegroundColor Gray
    $allOk = $false
}

# ─────────────────────────────────────────────────────────────────────────────
# 3. Git
# ─────────────────────────────────────────────────────────────────────────────
Write-Host "[ Git ]" -NoNewline
$git = Get-Command git -ErrorAction SilentlyContinue
if ($git) {
    $gitVersion = (git --version 2>&1) -replace "git version ", ""
    Write-Host "  OK — $gitVersion" -ForegroundColor Green
} else {
    Write-Host "  MANQUANT" -ForegroundColor Red
    Write-Host "  → winget install Git.Git" -ForegroundColor Gray
    $allOk = $false
}

# ─────────────────────────────────────────────────────────────────────────────
# 4. Submodule JUCE
# ─────────────────────────────────────────────────────────────────────────────
Write-Host "[ JUCE submodule ]" -NoNewline
$juceDir = Join-Path $PSScriptRoot "..\third_party\JUCE\CMakeLists.txt"
if (Test-Path $juceDir) {
    Write-Host "  OK" -ForegroundColor Green
} else {
    Write-Host "  MANQUANT" -ForegroundColor Yellow
    Write-Host "  → git submodule update --init --recursive" -ForegroundColor Gray
    $allOk = $false
}

# ─────────────────────────────────────────────────────────────────────────────
# 5. ASIO SDK (optionnel)
# ─────────────────────────────────────────────────────────────────────────────
Write-Host "[ ASIO SDK ]" -NoNewline
$asioDir = Join-Path $PSScriptRoot "..\third_party\ASIO\common\asio.h"
if (Test-Path $asioDir) {
    Write-Host "  OK (latence ASIO disponible)" -ForegroundColor Green
} else {
    Write-Host "  ABSENT — fallback WASAPI (latence ~10-20 ms)" -ForegroundColor Yellow
    Write-Host "  → Télécharger sur https://www.steinberg.net/developers/" -ForegroundColor Gray
    Write-Host "  → Extraire dans third_party/ASIO/" -ForegroundColor Gray
    Write-Host "  → Recompiler avec: cmake -B build -DJUCE_ASIO_SDK_PATH=third_party/ASIO" -ForegroundColor Gray
}

# ─────────────────────────────────────────────────────────────────────────────
# 6. Python + PyAudio (optionnel — pour test latence)
# ─────────────────────────────────────────────────────────────────────────────
Write-Host "[ Python + PyAudio ]" -NoNewline
$python = Get-Command python -ErrorAction SilentlyContinue
if ($python) {
    $pyaudio = python -c "import pyaudio; print('ok')" 2>$null
    if ($pyaudio -eq "ok") {
        Write-Host "  OK" -ForegroundColor Green
    } else {
        Write-Host "  Python OK mais PyAudio manquant" -ForegroundColor Yellow
        Write-Host "  → pip install pyaudio" -ForegroundColor Gray
    }
} else {
    Write-Host "  ABSENT (optionnel — pour scripts/latency_test.py)" -ForegroundColor Gray
}

# ─────────────────────────────────────────────────────────────────────────────
# Résumé
# ─────────────────────────────────────────────────────────────────────────────
Write-Host ""
if ($allOk) {
    Write-Host "✓ Tous les prérequis obligatoires sont installés." -ForegroundColor Green
    Write-Host ""
    Write-Host "Prochaines étapes :" -ForegroundColor Cyan
    Write-Host "  git submodule update --init --recursive"
    Write-Host "  cmake -B build -DCMAKE_BUILD_TYPE=Release"
    Write-Host "  cmake --build build --config Release --parallel"
} else {
    Write-Host "✗ Certains prérequis sont manquants. Installe-les puis relance ce script." -ForegroundColor Red
}
Write-Host ""
