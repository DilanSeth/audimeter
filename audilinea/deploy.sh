#!/bin/bash
# deploy.sh - Script de despliegue completo del sistema de monitoreo

set -e

echo "=== Sistema de Monitoreo de Audiencias ==="
echo "Iniciando despliegue completo..."

# Colores para output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Verificar que se ejecuta como root
if [[ $EUID -ne 0 ]]; then
   log_error "Este script debe ejecutarse como root"
   exit 1
fi

# Verificar dependencias del sistema
check_dependencies() {
    log_info "Verificando dependencias del sistema..."
    
    # Verificar Docker
    if ! command -v docker &> /dev/null; then
        log_error "Docker no está instalado. Instalando..."
        curl -fsSL https://get.docker.com -o get-docker.sh
        sh get-docker.sh
        systemctl enable docker
        systemctl start docker
        usermod -aG docker $USER
        log_info "Docker instalado correctamente"
    fi
    
    # Verificar Docker Compose
    if ! command -v docker-compose &> /dev/null; then
        log_error "Docker Compose no está instalado. Instalando..."
        curl -L "https://github.com/docker/compose/releases/latest/download/docker-compose-$(uname -s)-$(uname -m)" -o /usr/local/bin/docker-compose
        chmod +x /usr/local/bin/docker-compose
        log_info "Docker Compose instalado correctamente"
    fi
    
    # Verificar LXD para OpenWrt (opcional)
    if ! command -v lxd &> /dev/null; then
        log_warning "LXD no está instalado. Si planeas usar OpenWrt virtualizado, instálalo manualmente"
    fi
}

# Detectar interfaz USB-Ethernet automáticamente
detect_usb_interface() {
    log_info "Detectando interfaz USB-Ethernet..."
    
    USB_INTERFACE=$(ip -o link show | grep -E "(enx|usb)" | grep -v "lo\|eno1\|eth0\|wlan" | head -n1 | cut -d: -f2 | xargs)
    
    if [ -z "$USB_INTERFACE" ]; then
        log_warning "No se detectó interfaz USB-Ethernet automáticamente"
        echo "Interfaces disponibles:"
        ip -c link show
        read -p "Introduce el nombre de la interfaz USB-Ethernet manualmente: " USB_INTERFACE
    fi
    
    if [ -n "$USB_INTERFACE" ]; then
        log_info "Interfaz USB-Ethernet detectada: $USB_INTERFACE"
        echo "USB_INTERFACE=$USB_INTERFACE" >> .env
        
        # Activar interfaz si no está activa
        if ! ip link show $USB_INTERFACE | grep -q "state UP"; then
            log_info "Activando interfaz $USB_INTERFACE..."
            ip link set $USB_INTERFACE up
        fi
    else
        log_error "No se pudo configurar la interfaz USB-Ethernet"
        exit 1
    fi
}

# Configurar red bridge para routing
setup_network_bridge() {
    log_info "Configurando bridge de red..."
    
    # Instalar bridge-utils si no existe
    if ! command -v brctl &> /dev/null; then
        apt-get update
        apt-get install -y bridge-utils
    fi
    
    # Crear bridge para la red LAN si no existe
    if ! brctl show | grep -q "br-lan"; then
        brctl addbr br-lan
        brctl addif br-lan $USB_INTERFACE
        ip link set br-lan up
        
        # Configurar IP del bridge
        ip addr add 192.168.1.1/24 dev br-lan
        
        log_info "Bridge br-lan creado y configurado"
    fi
}

# Configurar iptables para routing
setup_iptables() {
    log_info "Configurando iptables para routing..."
    
    # Habilitar IP forwarding
    echo 1 > /proc/sys/net/ipv4/ip_forward
    echo "net.ipv4.ip_forward=1" >> /etc/sysctl.conf
    
    # Configurar NAT
    iptables -t nat -A POSTROUTING -o eno1 -j MASQUERADE
    iptables -A FORWARD -i br-lan -o eno1 -j ACCEPT
    iptables -A FORWARD -i eno1 -o br-lan -m state --state RELATED,ESTABLISHED -j ACCEPT
    
    # Guardar reglas iptables
    iptables-save > /etc/iptables/rules.v4
    
    log_info "Iptables configurado para routing"
}

# Crear estructura de directorios
create_directory_structure() {
    log_info "Creando estructura de directorios..."
    
    mkdir -p {ntopng/{config,data},scripts,extractor,cleaner,zerotier,db_data,db_init,logs,openwrt/{config,init}}
    chmod 755 scripts/*
    chmod +x scripts/detect_interface.sh
    
    log_info "Estructura de directorios creada"
}

# Crear Dockerfiles necesarios
create_dockerfiles() {
    log_info "Creando Dockerfiles..."
    
    # Dockerfile para extractor
    cat > extractor/Dockerfile << 'EOF'
FROM python:3.11-slim

WORKDIR /app

# Instalar dependencias del sistema
RUN apt-get update && apt-get install -y \
    gcc \
    && rm -rf /var/lib/apt/lists/*

# Copiar requirements
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copiar código
COPY . .

# Crear directorio de logs
RUN mkdir -p logs

CMD ["python", "main.py"]
EOF

    # Requirements para extractor
    cat > extractor/requirements.txt << 'EOF'
aiohttp==3.8.6
psycopg2-binary==2.9.7
requests==2.31.0
asyncio-timeout==4.0.3
EOF

    # Dockerfile para data cleaner
    cat > cleaner/Dockerfile << 'EOF'
FROM python:3.11-slim

WORKDIR /app

RUN apt-get update && apt-get install -y \
    gcc \
    && rm -rf /var/lib/apt/lists/*

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY . .

CMD ["python", "cleaner.py"]
EOF

    # Requirements para cleaner
    cat > cleaner/requirements.txt << 'EOF'
psycopg2-binary==2.9.7
schedule==1.2.0
EOF

    log_info "Dockerfiles creados"
}

# Crear script de limpieza de datos
create_data_cleaner() {
    log_info "Creando script de limpieza de datos..."
    
    cat > cleaner/cleaner.py << 'EOF'
#!/usr/bin/env python3
import os
import time
import logging
import psycopg2
from datetime import datetime, timedelta
import schedule

class DataCleaner:
    def __init__(self):
        self.setup_logging()
        self.load_config()
        self.setup_database()
    
    def setup_logging(self):
        logging.basicConfig(
            level=logging.INFO,
            format='%(asctime)s - %(levelname)s - %(message)s'
        )
        self.logger = logging.getLogger(__name__)
    
    def load_config(self):
        self.config = {
            'retention_days': int(os.getenv('RETENTION_DAYS', 7)),
            'db_config': {
                'host': os.getenv('DB_HOST', 'localhost'),
                'port': int(os.getenv('DB_PORT', 5432)),
                'database': os.getenv('DB_NAME', 'audience_metrics'),
                'user': os.getenv('DB_USER', 'postgres'),
                'password': os.getenv('DB_PASSWORD', 'postgres123')
            }
        }
    
    def setup_database(self):
        try:
            self.db_conn = psycopg2.connect(**self.config['db_config'])
            self.db_conn.autocommit = True
            self.logger.info("Conexión a base de datos establecida para limpieza")
        except Exception as e:
            self.logger.error(f"Error conectando a la base de datos: {e}")
            raise
    
    def clean_old_data(self):
        """Limpiar datos antiguos según el período de retención"""
        try:
            cutoff_date = datetime.utcnow() - timedelta(days=self.config['retention_days'])
            
            with self.db_conn.cursor() as cursor:
                # Limpiar métricas de tráfico antiguas que ya fueron enviadas
                cursor.execute("""
                    DELETE FROM traffic_metrics 
                    WHERE timestamp < %s AND sent_to_remote = TRUE
                """, (cutoff_date,))
                
                deleted_metrics = cursor.rowcount
                
                # Limpiar resúmenes antiguos
                cursor.execute("""
                    DELETE FROM application_summary 
                    WHERE timestamp < %s AND sent_to_remote = TRUE
                """, (cutoff_date,))
                
                deleted_summaries = cursor.rowcount
                
                self.logger.info(f"Limpieza completada: {deleted_metrics} métricas y {deleted_summaries} resúmenes eliminados")
                
        except Exception as e:
            self.logger.error(f"Error durante la limpieza: {e}")
    
    def run(self):
        """Ejecutar limpieza programada"""
        schedule.every().day.at("02:00").do(self.clean_old_data)
        schedule.every().hour.do(self.clean_old_data)  # Limpieza cada hora para datos muy antiguos
        
        self.logger.info("Iniciando servicio de limpieza de datos...")
        
        while True:
            schedule.run_pending()
            time.sleep(60)

if __name__ == "__main__":
    cleaner = DataCleaner()
    cleaner.run()
EOF

    log_info "Script de limpieza creado"
}

# Crear script de inicialización de base de datos
create_db_init() {
    log_info "Creando script de inicialización de base de datos..."
    
    cat > db_init/01-init.sql << 'EOF'
-- Inicialización de base de datos para métricas de audiencia

-- Crear extensiones necesarias
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";

-- Crear tabla de métricas de tráfico
CREATE TABLE IF NOT EXISTS traffic_metrics (
    id SERIAL PRIMARY KEY,
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    host_ip VARCHAR(45) NOT NULL,
    application VARCHAR(100) NOT NULL,
    category VARCHAR(100),
    bytes_sent BIGINT DEFAULT 0,
    bytes_received BIGINT DEFAULT 0,
    packets_sent BIGINT DEFAULT 0,
    packets_received BIGINT DEFAULT 0,
    duration INTEGER DEFAULT 0,
    flow_info JSONB,
    sent_to_remote BOOLEAN DEFAULT FALSE,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Crear índices para optimización
CREATE INDEX IF NOT EXISTS idx_timestamp ON traffic_metrics(timestamp);
CREATE INDEX IF NOT EXISTS idx_host_ip ON traffic_metrics(host_ip);
CREATE INDEX IF NOT EXISTS idx_application ON traffic_metrics(application);
CREATE INDEX IF NOT EXISTS idx_category ON traffic_metrics(category);
CREATE INDEX IF NOT EXISTS idx_sent_to_remote ON traffic_metrics(sent_to_remote);
CREATE INDEX IF NOT EXISTS idx_created_at ON traffic_metrics(created_at);

-- Crear tabla de resúmenes de aplicaciones
CREATE TABLE IF NOT EXISTS application_summary (
    id SERIAL PRIMARY KEY,
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    application VARCHAR(100) NOT NULL,
    category VARCHAR(100),
    total_bytes BIGINT DEFAULT 0,
    total_flows INTEGER DEFAULT 0,
    unique_hosts INTEGER DEFAULT 0,
    avg_duration FLOAT DEFAULT 0,
    summary_period VARCHAR(20),
    sent_to_remote BOOLEAN DEFAULT FALSE,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Índices para resúmenes
CREATE INDEX IF NOT EXISTS idx_app_timestamp ON application_summary(timestamp);
CREATE INDEX IF NOT EXISTS idx_app_name ON application_summary(application);
CREATE INDEX IF NOT EXISTS idx_app_category ON application_summary(category);
CREATE INDEX IF NOT EXISTS idx_app_sent ON application_summary(sent_to_remote);

-- Crear tabla de configuración del sistema
CREATE TABLE IF NOT EXISTS system_config (
    id SERIAL PRIMARY KEY,
    key VARCHAR(100) UNIQUE NOT NULL,
    value TEXT,
    description TEXT,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Insertar configuración inicial
INSERT INTO system_config (key, value, description) VALUES 
('system_version', '1.0.0', 'Versión del sistema de monitoreo'),
('last_cleanup', CURRENT_TIMESTAMP::TEXT, 'Última limpieza de datos'),
('device_id', gen_random_uuid()::TEXT, 'ID único del dispositivo')
ON CONFLICT (key) DO NOTHING;

-- Crear vistas útiles
CREATE OR REPLACE VIEW traffic_summary_hourly AS
SELECT 
    date_trunc('hour', timestamp) as hour,
    application,
    category,
    COUNT(*) as flow_count,
    SUM(bytes_sent + bytes_received) as total_bytes,
    COUNT(DISTINCT host_ip) as unique_hosts,
    AVG(duration) as avg_duration
FROM traffic_metrics 
GROUP BY date_trunc('hour', timestamp), application, category
ORDER BY hour DESC, total_bytes DESC;

CREATE OR REPLACE VIEW top_applications_today AS
SELECT 
    application,
    category,
    SUM(bytes_sent + bytes_received) as total_bytes,
    COUNT(*) as flow_count,
    COUNT(DISTINCT host_ip) as unique_hosts
FROM traffic_metrics 
WHERE timestamp >= CURRENT_DATE
GROUP BY application, category
ORDER BY total_bytes DESC
LIMIT 50;
EOF

    log_info "Script de inicialización de BD creado"
}

# Configurar OpenWrt virtualizado (alternativa)
setup_openwrt_alternative() {
    log_info "Configurando alternativa a OpenWrt con herramientas nativas..."
    
    # Instalar dnsmasq para DHCP
    apt-get update
    apt-get install -y dnsmasq hostapd

    # Configurar dnsmasq
    cat > /etc/dnsmasq.d/audience-monitor.conf << EOF
interface=br-lan
dhcp-range=192.168.1.100,192.168.1.200,255.255.255.0,12h
dhcp-option=3,192.168.1.1
dhcp-option=6,8.8.8.8,8.8.4.4
server=8.8.8.8
server=8.8.4.4
EOF

    # Reiniciar dnsmasq
    systemctl enable dnsmasq
    systemctl restart dnsmasq
    
    log_info "Configuración de red completada"
}

# Función principal de despliegue
deploy_system() {
    log_info "Iniciando despliegue del sistema completo..."
    
    check_dependencies
    detect_usb_interface
    setup_network_bridge
    setup_iptables
    create_directory_structure
    create_dockerfiles
    create_data_cleaner
    create_db_init
    setup_openwrt_alternative
    
    # Verificar que existe el archivo .env
    if [ ! -f .env ]; then
        log_error "Archivo .env no encontrado. Créalo usando el template proporcionado."
        exit 1
    fi
    
    # Construir y levantar servicios
    log_info "Construyendo y levantando servicios Docker..."
    docker-compose build
    docker-compose up -d
    
    # Verificar que los servicios están corriendo
    sleep 10
    log_info "Verificando estado de los servicios..."
    docker-compose ps
    
    # Mostrar logs iniciales
    log_info "Logs iniciales de ntopng:"
    docker-compose logs --tail=20 ntopng
    
    log_info "Logs iniciales del extractor:"
    docker-compose logs --tail=20 data_extractor
    
    log_info "=== DESPLIEGUE COMPLETADO ==="
    log_info "Servicios disponibles:"
    log_info "- ntopng Web Interface: http://localhost:3000"
    log_info "- Métricas en base de datos local"
    log_info "- Extractor enviando datos cada ${POLL_INTERVAL:-10} segundos"
    log_info ""
    log_info "Para ver logs en tiempo real:"
    log_info "  docker-compose logs -f data_extractor"
    log_info ""
    log_info "Para detener el sistema:"
    log_info "  docker-compose down"
}

# Función de cleanup en caso de error
cleanup_on_error() {
    log_error "Error durante el despliegue. Limpiando..."
    docker-compose down 2>/dev/null || true
    exit 1
}

# Configurar trap para cleanup
trap cleanup_on_error ERR

# Verificar argumentos
case "${1:-deploy}" in
    "deploy")
        deploy_system
        ;;
    "stop")
        log_info "Deteniendo sistema..."
        docker-compose down
        ;;
    "restart")
        log_info "Reiniciando sistema..."
        docker-compose restart
        ;;
    "logs")
        docker-compose logs -f
        ;;
    "status")
        docker-compose ps
        ;;
    *)
        echo "Uso: $0 [deploy|stop|restart|logs|status]"
        echo "  deploy  - Desplegar sistema completo (por defecto)"
        echo "  stop    - Detener todos los servicios"
        echo "  restart - Reiniciar servicios"
        echo "  logs    - Ver logs en tiempo real"
        echo "  status  - Ver estado de servicios"
        exit 1
        ;;
esac