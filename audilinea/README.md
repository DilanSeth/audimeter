# Sistema de Monitoreo de Audiencias en Línea

Sistema completo para monitoreo y medición de audiencias basado en análisis de tráfico de red usando ntopng, containerizado con Docker.

## 🏗️ Arquitectura del Sistema

```
Internet ─── [eno1] Mini PC [USB-Eth/enx*] ─── Red LAN ─── Dispositivos
                    │
                    ├── ntopng (análisis de tráfico)
                    ├── Data Extractor (recolección)
                    ├── PostgreSQL (almacenamiento local)
                    ├── ZeroTier (conectividad remota)
                    └── Data Cleaner (limpieza automática)
```

## 📋 Características Principales

- **Monitoreo en Tiempo Real**: Análisis de tráfico cada 10 segundos (configurable)
- **Detección Automática**: Identificación automática de interfaz USB-Ethernet
- **Clasificación de Aplicaciones**: ntopng clasifica tráfico por aplicación y categoría
- **Almacenamiento Dual**: Local (PostgreSQL) + Remoto (vía ZeroTier)
- **Limpieza Automática**: Eliminación de datos antiguos según política de retención
- **Containerización Completa**: Todo el sistema ejecuta en Docker
- **Red Privada**: Conectividad segura vía ZeroTier

## 🚀 Instalación Rápida

### 1. Clonar y Configurar

```bash
git clone <repository>
cd audience-monitoring-system

# Copiar y configurar variables de entorno
cp .env.example .env
nano .env  # Configurar tus credenciales
```

### 2. Configurar Variables de Entorno

Edita `.env` con tus valores:

```bash
# Credenciales ntopng
NTOPNG_USER=admin
NTOPNG_PASSWORD=tu_password_seguro

# Red ZeroTier
ZEROTIER_NETWORK_ID=tu_network_id
ZEROTIER_API_TOKEN=tu_api_token

# Servidor remoto
REMOTE_ENDPOINT=http://servidor-central.zerotier-ip:8080/api/metrics
REMOTE_API_KEY=tu_api_key

# WiFi (opcional)
WIFI_SSID=AudienceMonitor_AP
WIFI_PASSWORD=wifi_password_seguro
```

### 3. Desplegar Sistema

```bash
# Hacer ejecutable el script de despliegue
chmod +x deploy.sh

# Ejecutar despliegue completo
sudo ./deploy.sh deploy
```

## 📊 Mejoras para Granularidad de ntopng

El sistema incluye configuración optimizada para máxima granularidad:

### ntopng.conf Optimizado

```conf
# Máxima capacidad de flujos y hosts
--max-num-flows=200000
--max-num-hosts=50000

# Análisis detallado
--enable-application-analysis
--enable-category-analysis
--enable-ndpi-protocols
--enable-flow-device-port-rrd-creation

# Configuración de tiempo real
--housekeeping-frequency=60
--intf-rrd-raw-days=1
--intf-rrd-1min-days=1
```

### API Endpoints Utilizados

```python
# Endpoints principales para extracción
/lua/rest/get/host/active.json       # Hosts activos
/lua/rest/get/flow/active.json       # Flujos activos  
/lua/rest/get/application/data.json  # Datos de aplicaciones
/lua/rest/get/host/data.json?host=X  # Detalles por host
```

## 🔧 Comandos de Administración

```bash
# Ver estado del sistema
./deploy.sh status

# Ver logs en tiempo real
./deploy.sh logs

# Reiniciar servicios
./deploy.sh restart

# Detener sistema
./deploy.sh stop

# Ver logs específicos
docker-compose logs -f data_extractor
docker-compose logs -f ntopng
```

## 📈 Estructura de Datos

### Métricas de Tráfico
```sql
traffic_metrics {
    host_ip: IP del dispositivo
    application: Aplicación detectada (YouTube, Netflix, etc.)
    category: Categoría (Video, Social, etc.)
    bytes_sent/received: Bytes transferidos
    packets_sent/received: Paquetes transferidos
    duration: Duración de la sesión
    flow_info: Información adicional del flujo
}
```

### Resúmenes de Aplicación
```sql
application_summary {
    application: Nombre de la aplicación
    total_bytes: Total de bytes en el período
    total_flows: Número de flujos
    unique_hosts: Hosts únicos que usaron la app
    avg_duration: Duración promedio de sesiones
}
```

## 🌐 Detección Automática de Interfaz

El sistema detecta automáticamente la interfaz USB-Ethernet:

```bash
# Busca interfaces USB
USB_INTERFACE=$(ip -o link show | grep -E "(enx|usb)" | grep -v "lo|eno1|eth0|wlan")

# Ejemplos detectados:
# enx00e04c68087d (Realtek USB Ethernet)
# enxf8e43b5b1234 (ASIX USB Ethernet)
# usb0 (Algunos adaptadores genéricos)
```

## 🔒 Seguridad

### Configuración de Red
- Contenedor ntopng en modo `host` (acceso a interfaces)
- Otros contenedores en red bridge aislada
- Comunicación remota cifrada vía ZeroTier
- Credenciales via variables de entorno

### Limpieza de Datos
- Retención configurable (7 días por defecto)
- Eliminación automática de datos enviados al servidor remoto
- Optimización de almacenamiento local

## 🚨 Solución de Problemas

### ntopng no inicia
```bash
# Verificar interfaz detectada
docker-compose exec ntopng cat /tmp/target_interface

# Ver logs detallados
docker-compose logs ntopng
```

### Extractor no conecta
```bash
# Verificar conectividad a ntopng
curl -u admin:password http://localhost:3000/lua/rest/get/host/active.json

# Verificar variables de entorno
docker-compose exec data_extractor env | grep NTOPNG
```

### ZeroTier no conecta
```bash
# Verificar estado ZeroTier
docker-compose exec zerotier zerotier-cli info
docker-compose exec zerotier zerotier-cli listnetworks
```

## 📝 Personalización

### Cambiar Frecuencia de Polling
```bash
# En .env
POLL_INTERVAL=5  # Cada 5 segundos para mayor frecuencia
```

### Agregar Más Métricas
Modifica `extractor/main.py` para incluir métricas adicionales de ntopng:

```python
# Ejemplo: agregar geolocalización
geo_data = await self.get_ntopng_data(f"get/host/geo.json?host={host_key}")
```

### Configurar Alertas
Implementa alertas basadas en umbrales:

```python
if total_bytes > ALERT_THRESHOLD:
    await self.send_alert(f"Alto consumo detectado: {total_bytes} bytes")
```

## 🤝 Contribuir

1. Fork del proyecto
2. Crear rama para feature (`git checkout -b feature/nueva-feature`)
3. Commit cambios (`git commit -am 'Agregar nueva feature'`)
4. Push a la rama (`git push origin feature/nueva-feature`)
5. Crear Pull Request

## 📄 Licencia

Este proyecto está bajo la Licencia MIT. Ver `LICENSE` para más detalles.

## 📧 Soporte

Para problemas o preguntas:
- Abrir issue en GitHub
- Revisar logs del sistema: `./deploy.sh logs`
- Consultar documentación de ntopng: https://www.ntop.org/guides/ntopng/