#!/bin/bash

# VNC Control Script for LoRa Gateway
# Compatible with existing gateway script structure

VNC_SERVICE="x11vnc.service"
LOG_FILE="/var/log/x11vnc.log"

log_message() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1"
}

start_vnc() {
    log_message "Starting VNC server for gateway..."
    sudo systemctl start $VNC_SERVICE
    
    if sudo systemctl is-active --quiet $VNC_SERVICE; then
        log_message "VNC server started successfully"
        log_message "Connect via SSH tunnel: ssh -L 5900:localhost:5900 pi@gateway_ip"
    else
        log_message "Failed to start VNC server"
        return 1
    fi
}

stop_vnc() {
    log_message "Stopping VNC server..."
    sudo systemctl stop $VNC_SERVICE
    log_message "VNC server stopped"
}

status_vnc() {
    if sudo systemctl is-active --quiet $VNC_SERVICE; then
        log_message "VNC server is running"
        log_message "Port: 5900"
        log_message "Log: $LOG_FILE"
        
        # Show process info
        VNC_PID=$(pgrep x11vnc)
        if [ -n "$VNC_PID" ]; then
            log_message "Process ID: $VNC_PID"
        fi
    else
        log_message "VNC server is not running"
        return 1
    fi
}

restart_vnc() {
    log_message "Restarting VNC server..."
    sudo systemctl restart $VNC_SERVICE
    status_vnc
}

case "$1" in
    start)
        start_vnc
        ;;
    stop)
        stop_vnc
        ;;
    status)
        status_vnc
        ;;
    restart)
        restart_vnc
        ;;
    *)
        echo "Usage: $0 {start|stop|status|restart}"
        exit 1
        ;;
esac
