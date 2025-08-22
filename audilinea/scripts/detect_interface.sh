#!/bin/bash
# scripts/detect_interface.sh
# Detecta automáticamente la interfaz USB-Ethernet

echo "Detectando interfaz USB-Ethernet..."

# Buscar interfaces que NO sean loopback ni la interfaz principal (eno1, eth0)
USB_INTERFACE=$(ip -o link show | grep -E "(enx|usb)" | grep -v "lo\|eno1\|eth0\|wlan" | head -n1 | cut -d: -f2 | xargs)

if [ -z "$USB_INTERFACE" ]; then
    echo "No se encontró interfaz USB-Ethernet. Buscando alternativas..."
    
    # Buscar por fabricantes comunes de adaptadores USB-Ethernet
    USB_INTERFACE=$(ip -o link show | grep -E "(enp[0-9]+s[0-9]+u|enx)" | head -n1 | cut -d: -f2 | xargs)
fi

if [ -z "$USB_INTERFACE" ]; then
    echo "Error: No se pudo detectar la interfaz USB-Ethernet"
    echo "Interfaces disponibles:"
    ip -c a s
    exit 1
fi

echo "Interfaz USB-Ethernet detectada: $USB_INTERFACE"
echo $USB_INTERFACE > /tmp/target_interface

# Verificar que la interfaz esté UP
if ! ip link show $USB_INTERFACE | grep -q "state UP"; then
    echo "Activando interfaz $USB_INTERFACE..."
    ip link set $USB_INTERFACE up
fi

echo "Interfaz $USB_INTERFACE lista para monitoreo"