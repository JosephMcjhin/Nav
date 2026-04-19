@echo off
echo Starting Voice Navigation PC Backend...
cd /d "%~dp0"
uv run python web_app.py --host 0.0.0.0 --port 8090
pause
