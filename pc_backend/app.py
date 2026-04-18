"""
DeskPulse PC Backend - Flask REST API Server
============================================
This is the main Flask server that provides system metrics and controls
to the ESP32 touchscreen display. The server collects system information
and exposes it via HTTP REST API endpoints in JSON format.

Author: DeskPulse Project
Version: 1.0.0
"""

from flask import Flask, jsonify
from flask_cors import CORS
import logging
from datetime import datetime
import psutil
import platform

# Initialize Flask application
app = Flask(__name__)

# Enable CORS (Cross-Origin Resource Sharing) to allow requests from ESP32
CORS(app)

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

# Optional imports for temperature and GPU monitoring
try:
    import wmi
    WMI_AVAILABLE = True
except ImportError:
    WMI_AVAILABLE = False
    logger.warning("WMI not available - CPU temperature monitoring disabled")

try:
    import pynvml
    pynvml.nvmlInit()
    GPU_AVAILABLE = True
except Exception as e:
    GPU_AVAILABLE = False
    logger.warning(f"NVIDIA GPU monitoring not available: {e}")
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)


# ==================== SYSTEM MONITORING FUNCTIONS ====================

def get_cpu_usage():
    """
    Get current CPU usage percentage.
    
    Returns:
        float: CPU usage as percentage (0-100)
    """
    return psutil.cpu_percent(interval=0.1)


def get_ram_usage():
    """
    Get current RAM usage percentage.
    
    Returns:
        dict: {
            "used_percent": float (0-100),
            "used_gb": float,
            "available_gb": float,
            "total_gb": float
        }
    """
    ram = psutil.virtual_memory()
    return {
        "used_percent": ram.percent,
        "used_gb": round(ram.used / (1024**3), 2),
        "available_gb": round(ram.available / (1024**3), 2),
        "total_gb": round(ram.total / (1024**3), 2)
    }


def get_disk_usage():
    """
    Get disk usage for the system drive.
    
    Returns:
        dict: {
            "used_percent": float (0-100),
            "used_gb": float,
            "free_gb": float,
            "total_gb": float
        }
    """
    disk = psutil.disk_usage('/')
    return {
        "used_percent": disk.percent,
        "used_gb": round(disk.used / (1024**3), 2),
        "free_gb": round(disk.free / (1024**3), 2),
        "total_gb": round(disk.total / (1024**3), 2)
    }


def get_network_usage():
    """
    Get network I/O statistics (upload/download speeds).
    Note: Returns cumulative bytes sent/received since last boot.
    To calculate speed, caller should sample this at intervals.
    
    Returns:
        dict: {
            "bytes_sent": int,
            "bytes_received": int,
            "packets_sent": int,
            "packets_received": int
        }
    """
    net_io = psutil.net_io_counters()
    return {
        "bytes_sent": net_io.bytes_sent,
        "bytes_received": net_io.bytes_recv,
        "packets_sent": net_io.packets_sent,
        "packets_received": net_io.packets_recv
    }


def get_system_stats():
    """
    Collect all system statistics in a lightweight format.
    
    Returns:
        dict: Aggregated system stats
    """
    return {
        "cpu_percent": round(get_cpu_usage(), 1),
        "ram": get_ram_usage(),
        "disk": get_disk_usage(),
        "network": get_network_usage()
    }




@app.route('/status', methods=['GET'])
def status():
    """
    Basic status endpoint to check if server is running.
    
    Returns:
        JSON: {"status": "ok", "timestamp": ISO8601 timestamp}
    """
    return jsonify({
        "status": "ok",
        "timestamp": datetime.utcnow().isoformat()
    }), 200


@app.route('/api/health', methods=['GET'])
def health_check():
    """
    Health check endpoint for monitoring server availability.
    
    Returns:
        JSON: {"healthy": true, "version": "1.0.0"}
    """
    return jsonify({
        "healthy": True,
        "version": "1.0.0",
        "timestamp": datetime.utcnow().isoformat()
    }), 200


@app.route('/stats', methods=['GET'])
def get_stats():
    """
    Get real-time system statistics including CPU, RAM, disk, and network usage.
    Lightweight endpoint optimized for frequent polling from ESP32.
    
    Returns:
        JSON: {
            "cpu_percent": float,
            "ram": {
                "used_percent": float,
                "used_gb": float,
                "available_gb": float,
                "total_gb": float
            },
            "disk": {
                "used_percent": float,
                "used_gb": float,
                "free_gb": float,
                "total_gb": float
            },
            "network": {
                "bytes_sent": int,
                "bytes_received": int,
                "packets_sent": int,
                "packets_received": int
            }
        }
    """
    try:
        stats = get_system_stats()
        return jsonify({
            "status": "ok",
            "data": stats,
            "timestamp": datetime.utcnow().isoformat()
        }), 200
    except Exception as e:
        logger.error(f"Error collecting system stats: {e}")
        return jsonify({
            "status": "error",
            "message": "Failed to collect system statistics"
        }), 500


# ==================== ERROR HANDLERS ====================

@app.errorhandler(404)
def not_found(error):
    """Handle 404 errors gracefully."""
    return jsonify({
        "error": "Endpoint not found",
        "status": "error"
    }), 404


@app.errorhandler(500)
def internal_error(error):
    """Handle 500 errors gracefully."""
    logger.error(f"Internal server error: {error}")
    return jsonify({
        "error": "Internal server error",
        "status": "error"
    }), 500


# ==================== MAIN ENTRY POINT ====================

if __name__ == '__main__':
    logger.info("=" * 60)
    logger.info("DeskPulse PC Backend Server Starting")
    logger.info("=" * 60)
    logger.info("Server will be accessible at: http://0.0.0.0:5000")
    logger.info("Test endpoint: http://localhost:5000/status")
    logger.info("=" * 60)
    
    # Run Flask server on all available network interfaces
    # debug=True enables auto-reload and better error messages (disable in production)
    app.run(
        host='0.0.0.0',      # Listen on all network interfaces
        port=5000,            # Port to listen on
        debug=True,           # Enable debug mode for development
        use_reloader=True     # Auto-reload on code changes
    )
