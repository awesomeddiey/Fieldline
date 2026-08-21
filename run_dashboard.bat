@echo off
title Fieldline dashboard
cd /d "%~dp0"
start "" http://localhost:8123
python scripts\serve_local.py
pause
