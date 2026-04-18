# DeskPulse PC Backend - Setup Instructions

## Overview
This is the Python Flask REST API server that runs on your Windows PC and accepts requests from the ESP32 display. It will eventually provide system metrics, media control, and system actions.

## Prerequisites
- Python 3.8 or higher
- pip (Python package manager)
- Windows 10/11

## Installation Steps

### 1. Install Python Dependencies
Open PowerShell or Command Prompt and navigate to the `pc_backend` directory:

```powershell
cd "D:\Github\DeskPulse\pc_backend"
pip install -r requirements.txt
```

### 2. Run the Server
```powershell
python app.py
```

You should see output like:
```
============================================================
DeskPulse PC Backend Server Starting
============================================================
Server will be accessible at: http://0.0.0.0:5000
Test endpoint: http://localhost:5000/status
============================================================
```

### 3. Test the Server

#### Option A: Using Browser
Open your browser and navigate to:
```
http://localhost:5000/status
```

You should see:
```json
{
  "status": "ok",
  "timestamp": "2026-04-18T12:34:56.789123"
}
```

#### Option B: Using PowerShell/Command Prompt
```powershell
curl http://localhost:5000/status
```

Or with JSON formatting:
```powershell
curl http://localhost:5000/status | ConvertFrom-Json | ConvertTo-Json
```

#### Option C: Using Command Line `curl` (if installed)
```
curl http://localhost:5000/health
```

## Available Endpoints

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/status` | GET | Basic status check |
| `/api/health` | GET | Health check with version info |

## Configuration

### Change Server Port
Edit `app.py` and modify the `app.run()` call:
```python
app.run(host='0.0.0.0', port=8080)  # Change 5000 to desired port
```

### Access from ESP32
The ESP32 can access the server using:
```
http://<YOUR_PC_IP_ADDRESS>:5000/status
```

To find your PC's IP address on Windows:
```powershell
ipconfig
```
Look for "IPv4 Address" under your network adapter (typically looks like: 192.168.x.x)

## Troubleshooting

### "Address already in use" error
Another application is using port 5000. Either:
1. Close the other application
2. Change the port in `app.py`
3. Kill the process using the port:
   ```powershell
   netstat -ano | findstr :5000
   taskkill /PID <PID> /F
   ```

### Cannot access server from ESP32
1. Ensure Windows Firewall allows Flask on port 5000
2. Verify PC and ESP32 are on the same WiFi network
3. Check your PC's IP address with `ipconfig`
4. Test locally first: `curl http://localhost:5000/status`

### ModuleNotFoundError
Run `pip install -r requirements.txt` again to ensure all dependencies are installed.

## Next Steps
Once testing is successful, the following will be added:
- System metrics collection (CPU, RAM, GPU usage, temperatures)
- Media playback control endpoints
- System control actions (shutdown, restart, lock)
- WebSocket support for real-time updates (optional)

## Development Notes
- Debug mode is enabled by default (auto-reloads on code changes)
- Flask runs on all network interfaces (0.0.0.0) to be accessible from ESP32
- CORS is enabled to allow cross-origin requests
- All responses include proper JSON formatting and HTTP status codes
