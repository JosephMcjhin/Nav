@echo off
echo Starting Voice Navigation PC Backend...
cd /d "%~dp0"
python web_app.py --host 0.0.0.0 --port 8090
pause
