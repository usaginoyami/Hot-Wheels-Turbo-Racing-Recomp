@echo off
chcp 65001 >nul
cd /d "%~dp0"
title Hot Wheels Turbo Racing recomp
"ferramentas\python\python.exe" jogar.py %*
if errorlevel 1 (
  echo.
  echo O programa terminou com erro. Veja a mensagem acima.
  pause
)
