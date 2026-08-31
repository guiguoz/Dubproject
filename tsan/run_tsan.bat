@echo off
REM ──────────────────────────────────────────────────────────────────────────
REM  run_tsan.bat — Build & execute TSan via Docker
REM
REM  Prérequis :
REM    - Docker Desktop lancé
REM    - Git submodules checkoutés (git submodule update --init --recursive)
REM ──────────────────────────────────────────────────────────────────────────

setlocal

set PROJECT_DIR=%~dp0..
set IMAGE_NAME=dubengine-tsan

echo [1/3] Building Docker image %IMAGE_DIR% ...
docker build -t %IMAGE_NAME% -f "%PROJECT_DIR%\tsan\Dockerfile" "%PROJECT_DIR%"
if %ERRORLEVEL% neq 0 (
    echo ERREUR: build Docker echoue
    exit /b 1
)

echo.
echo [2/3] Running TSan (timeout 60s) ...
echo ──────────────────────────────────────────────────────────────────────
docker run --rm --isolation=process %IMAGE_NAME% "[tsan]" --duration 200
set EXIT_CODE=%ERRORLEVEL%

echo.
echo ──────────────────────────────────────────────────────────────────────
if %EXIT_CODE% equ 0 (
    echo [3/3] TSan: AUCUNE race detectee.
) else (
    echo [3/3] TSan a rapporte des races (voir ci-dessus).
)

exit /b %EXIT_CODE%
