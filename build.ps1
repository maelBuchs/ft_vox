#!/usr/bin/env pwsh
# Script de compilation pour ft_vox
# Usage: .\build.ps1 [clean|debug|release|run|help]

param(
    [Parameter(Position = 0)]
    [ValidateSet('clean', 'debug', 'release', 'run', 'run-release', 'help')]
    [string]$Action = 'debug'
)

function Show-Help {
    Write-Host @"
ft_vox - Script de compilation
==============================

Usage: .\build.ps1 [action]

Actions disponibles:
  clean        - Nettoie le dossier build
  debug        - Compile en mode Debug (par défaut)
  release      - Compile en mode Release
  run          - Compile et exécute en mode Debug (par défaut)
  run-release  - Compile et exécute en mode Release
  help         - Affiche cette aide

Exemples:
  .\build.ps1                # Compile en Debug
  .\build.ps1 release        # Compile en Release
  .\build.ps1 run            # Compile et lance en Debug
  .\build.ps1 clean          # Nettoie le build

"@
}

function Clean-Build {
    Write-Host "Nettoyage du dossier build..." -ForegroundColor Cyan
    if (Test-Path "build") {
        Remove-Item -Path "build\*" -Recurse -Force
        Write-Host "[OK] Build nettoye" -ForegroundColor Green
    }
    else {
        Write-Host "[INFO] Aucun dossier build a nettoyer" -ForegroundColor Yellow
    }
}

function Configure-CMake {
    param([string]$Config)

    Write-Host "Configuration CMake avec Ninja Multi-Config et clang-cl (mode $Config)..." -ForegroundColor Cyan

    # Force l'utilisation de clang-cl comme compilateur
    $env:CC = "clang-cl"
    $env:CXX = "clang-cl"

    cmake -S . -B build -G "Ninja Multi-Config" `
        -DCMAKE_C_COMPILER="clang-cl" `
        -DCMAKE_CXX_COMPILER="clang-cl"

    if ($LASTEXITCODE -ne 0) {
        Write-Host "[ERREUR] Erreur lors de la configuration CMake" -ForegroundColor Red
        Write-Host "[INFO] Assurez-vous que Ninja et clang-cl sont installes" -ForegroundColor Yellow
        exit 1
    }
    Write-Host "[OK] Configuration reussie" -ForegroundColor Green
}

function Should-Reconfigure {
    param([string]$Config)

    # With Ninja, we need to reconfigure if cache doesn't exist
    # or if build type has changed
    if (-not (Test-Path "build\CMakeCache.txt")) {
        return $true
    }

    # Check if build type has changed
    $cacheContent = Get-Content "build\CMakeCache.txt" -Raw
    if ($cacheContent -match "CMAKE_BUILD_TYPE:STRING=(.+)") {
        $currentConfig = $matches[1].Trim()
        if ($currentConfig -ne $Config) {
            Write-Host "[INFO] Changement de configuration: $currentConfig -> $Config" -ForegroundColor Yellow
            return $true
        }
    }

    return $false
}

function Build-Project {
    param([string]$Config)

    Write-Host "Compilation en mode $Config..." -ForegroundColor Cyan

    if (Should-Reconfigure -Config $Config) {
        Configure-CMake -Config $Config
    }
    else {
        Write-Host "[INFO] Utilisation de la configuration existante" -ForegroundColor Yellow
    }

    # Use all available CPU cores for faster compilation
    # With Ninja Multi-Config, we need to specify --config
    cmake --build build --config $Config --parallel
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[ERREUR] Erreur lors de la compilation" -ForegroundColor Red
        exit 1
    }

    # Copy compile_commands.json to root for LSP tools
    if (Test-Path "build\compile_commands.json") {
        Copy-Item "build\compile_commands.json" "." -Force
        Write-Host "[OK] compile_commands.json copie a la racine" -ForegroundColor Green
    }

    Write-Host "[OK] Compilation reussie" -ForegroundColor Green
}

function Run-Project {
    param([string]$Config)

    # Always build before running - Ninja is smart and only recompiles what changed
    # This ensures we always run the latest version
    Build-Project -Config $Config

    # With Ninja Multi-Config, executables are in build/<Config> directory
    $exePath = "build\$Config\ft_vox.exe"

    Write-Host "Lancement de ft_vox ($Config)..." -ForegroundColor Cyan
    & $exePath
}# Exécution selon l'action
switch ($Action) {
    'clean' {
        Clean-Build
    }
    'debug' {
        Build-Project -Config 'Debug'
    }
    'release' {
        Build-Project -Config 'Release'
    }
    'run' {
        Run-Project -Config 'Debug'
    }
    'run-release' {
        Run-Project -Config 'Release'
    }
    'help' {
        Show-Help
    }
}

Write-Host ""
Write-Host "[OK] Termine!" -ForegroundColor Green
