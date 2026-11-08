@echo off
rem Assistente de configuracao do Alerta de Sons. De dois cliques neste arquivo.
chcp 65001 >nul
cd /d "%~dp0"

where node >nul 2>nul
if errorlevel 1 (
  echo.
  echo  O Node.js nao foi encontrado neste computador.
  echo  Instale a versao 22 ou mais nova em https://nodejs.org e rode este arquivo de novo.
  echo.
  pause
  exit /b 1
)

node setup.mjs %*
echo.
pause
