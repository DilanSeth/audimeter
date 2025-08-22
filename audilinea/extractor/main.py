#!/usr/bin/env python3
# extractor/main.py
import os
import time
import json
import requests
import logging
import psycopg2
from datetime import datetime, timedelta
from typing import Dict, List, Any, Optional
import asyncio
import aiohttp
import signal
import sys

class AudienceExtractor:
    def __init__(self):
        self.setup_logging()
        self.load_config()
        self.setup_database()
        self.running = True
        
        # Configurar manejador de señales para parada limpia
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)
    
    def setup_logging(self):
        """Configurar logging del sistema"""
        logging.basicConfig(
            level=logging.INFO,
            format='%(asctime)s - %(levelname)s - %(message)s',
            handlers=[
                logging.FileHandler('/app/logs/extractor.log'),
                logging.StreamHandler()
            ]
        )
        self.logger = logging.getLogger(__name__)
    
    def load_config(self):
        """Cargar configuración desde variables de entorno"""
        self.config = {
            'ntopng_host': os.getenv('NTOPNG_HOST', 'localhost'),
            'ntopng_port': int(os.getenv('NTOPNG_PORT', 3000)),
            'ntopng_user': os.getenv('NTOPNG_USER', 'admin'),
            'ntopng_password': os.getenv('NTOPNG_PASSWORD', 'admin123'),
            'remote_endpoint': os.getenv('REMOTE_ENDPOINT'),
            'remote_api_key': os.getenv('REMOTE_API_KEY'),
            'poll_interval': int(os.getenv('POLL_INTERVAL', 10)),
            'db_config': {
                'host': os.getenv('DB_HOST', 'localhost'),
                'port': int(os.getenv('DB_PORT', 5432)),
                'database': os.getenv('DB_NAME', 'audience_metrics'),
                'user': os.getenv('DB_USER', 'postgres'),
                'password': os.getenv('DB_PASSWORD', 'postgres123')
            }
        }
        
        self.ntopng_base_url = f"http://{self.config['ntopng_host']}:{self.config['ntopng_port']}"
        self.logger.info("Configuración cargada correctamente")
    
    def setup_database(self):
        """Configurar conexión a base de datos local"""
        try:
            self.db_conn = psycopg2.connect(**self.config['db_config'])
            self.db_conn.autocommit = True
            self.create_tables()
            self.logger.info("Conexión a base de datos establecida")
        except Exception as e:
            self.logger.error(f"Error conectando a la base de datos: {e}")
            raise
    
    def create_tables(self):
        """Crear tablas necesarias en la base de datos"""
        with self.db_conn.cursor() as cursor:
            cursor.execute("""
                CREATE TABLE IF NOT EXISTS traffic_metrics (
                    id SERIAL PRIMARY KEY,
                    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                    host_ip VARCHAR(45),
                    application VARCHAR(100),
                    category VARCHAR(100),
                    bytes_sent BIGINT,
                    bytes_received BIGINT,
                    packets_sent BIGINT,
                    packets_received BIGINT,
                    duration INTEGER,
                    flow_info JSONB,
                    sent_to_remote BOOLEAN DEFAULT FALSE,
                    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
                );
                
                CREATE INDEX IF NOT EXISTS idx_timestamp ON traffic_metrics(timestamp);
                CREATE INDEX IF NOT EXISTS idx_application ON traffic_metrics(application);
                CREATE INDEX IF NOT EXISTS idx_sent_to_remote ON traffic_metrics(sent_to_remote);
            """)
            
            cursor.execute("""
                CREATE TABLE IF NOT EXISTS application_summary (
                    id SERIAL PRIMARY KEY,
                    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                    application VARCHAR(100),
                    category VARCHAR(100),
                    total_bytes BIGINT,
                    total_flows INTEGER,
                    unique_hosts INTEGER,
                    avg_duration FLOAT,
                    summary_period VARCHAR(20),
                    sent_to_remote BOOLEAN DEFAULT FALSE
                );
                
                CREATE INDEX IF NOT EXISTS idx_app_timestamp ON application_summary(timestamp);
                CREATE INDEX IF NOT EXISTS idx_app_name ON application_summary(application);
            """)
    
    async def get_ntopng_data(self, endpoint: str) -> Optional[Dict]:
        """Obtener datos de la API de ntopng"""
        url = f"{self.ntopng_base_url}/lua/rest/{endpoint}"
        
        try:
            auth = aiohttp.BasicAuth(
                self.config['ntopng_user'], 
                self.config['ntopng_password']
            )
            
            async with aiohttp.ClientSession(auth=auth) as session:
                async with session.get(url, timeout=30) as response:
                    if response.status == 200:
                        return await response.json()
                    else:
                        self.logger.warning(f"Error HTTP {response.status} en {endpoint}")
                        return None
        except Exception as e:
            self.logger.error(f"Error obteniendo datos de {endpoint}: {e}")
            return None
    
    async def extract_traffic_data(self) -> List[Dict]:
        """Extraer datos de tráfico de ntopng"""
        traffic_data = []
        
        # Obtener hosts activos
        hosts_data = await self.get_ntopng_data("get/host/active.json")
        if not hosts_data:
            return traffic_data
        
        # Obtener flows activos
        flows_data = await self.get_ntopng_data("get/flow/active.json")
        
        # Obtener aplicaciones
        apps_data = await self.get_ntopng_data("get/application/data.json")
        
        # Procesar datos de hosts
        for host_key, host_info in hosts_data.get('rsp', {}).items():
            try:
                # Obtener detalles específicos del host
                host_details = await self.get_ntopng_data(f"get/host/data.json?host={host_key}")
                
                if host_details and 'rsp' in host_details:
                    host_data = host_details['rsp']
                    
                    # Extraer aplicaciones del host
                    for app_name, app_data in host_data.get('applications', {}).items():
                        traffic_entry = {
                            'host_ip': host_key,
                            'application': app_name,
                            'category': app_data.get('category', 'Unknown'),
                            'bytes_sent': app_data.get('bytes.sent', 0),
                            'bytes_received': app_data.get('bytes.rcvd', 0),
                            'packets_sent': app_data.get('packets.sent', 0),
                            'packets_received': app_data.get('packets.rcvd', 0),
                            'duration': app_data.get('duration', 0),
                            'flow_info': app_data
                        }
                        traffic_data.append(traffic_entry)
                        
            except Exception as e:
                self.logger.error(f"Error procesando host {host_key}: {e}")
                continue
        
        self.logger.info(f"Extraídos {len(traffic_data)} registros de tráfico")
        return traffic_data
    
    def store_local_data(self, traffic_data: List[Dict]):
        """Almacenar datos en la base de datos local"""
        if not traffic_data:
            return
        
        try:
            with self.db_conn.cursor() as cursor:
                for entry in traffic_data:
                    cursor.execute("""
                        INSERT INTO traffic_metrics 
                        (host_ip, application, category, bytes_sent, bytes_received, 
                         packets_sent, packets_received, duration, flow_info)
                        VALUES (%(host_ip)s, %(application)s, %(category)s, 
                               %(bytes_sent)s, %(bytes_received)s, %(packets_sent)s, 
                               %(packets_received)s, %(duration)s, %(flow_info)s)
                    """, {
                        'host_ip': entry['host_ip'],
                        'application': entry['application'],
                        'category': entry['category'],
                        'bytes_sent': entry['bytes_sent'],
                        'bytes_received': entry['bytes_received'],
                        'packets_sent': entry['packets_sent'],
                        'packets_received': entry['packets_received'],
                        'duration': entry['duration'],
                        'flow_info': json.dumps(entry['flow_info'])
                    })
            
            self.logger.info(f"Almacenados {len(traffic_data)} registros localmente")
            
        except Exception as e:
            self.logger.error(f"Error almacenando datos localmente: {e}")
    
    async def send_to_remote(self, data: List[Dict]) -> bool:
        """Enviar datos al servidor remoto en la red ZeroTier"""
        if not self.config['remote_endpoint']:
            self.logger.warning("No se ha configurado endpoint remoto")
            return False
        
        try:
            headers = {
                'Content-Type': 'application/json',
                'Authorization': f"Bearer {self.config['remote_api_key']}"
            }
            
            payload = {
                'timestamp': datetime.utcnow().isoformat(),
                'device_id': os.getenv('HOSTNAME', 'unknown'),
                'metrics': data
            }
            
            async with aiohttp.ClientSession() as session:
                async with session.post(
                    self.config['remote_endpoint'],
                    json=payload,
                    headers=headers,
                    timeout=60
                ) as response:
                    
                    if response.status == 200:
                        self.logger.info(f"Datos enviados exitosamente al servidor remoto")
                        return True
                    else:
                        self.logger.error(f"Error enviando datos: HTTP {response.status}")
                        return False
                        
        except Exception as e:
            self.logger.error(f"Error enviando datos al servidor remoto: {e}")
            return False
    
    def mark_as_sent(self, timestamp_from: datetime):
        """Marcar registros como enviados al servidor remoto"""
        try:
            with self.db_conn.cursor() as cursor:
                cursor.execute("""
                    UPDATE traffic_metrics 
                    SET sent_to_remote = TRUE 
                    WHERE timestamp >= %s AND sent_to_remote = FALSE
                """, (timestamp_from,))
                
                self.logger.info(f"Marcados {cursor.rowcount} registros como enviados")
                
        except Exception as e:
            self.logger.error(f"Error marcando registros como enviados: {e}")
    
    def generate_summary(self, traffic_data: List[Dict]):
        """Generar resumen de aplicaciones para el período"""
        if not traffic_data:
            return
        
        app_summary = {}
        
        for entry in traffic_data:
            app = entry['application']
            if app not in app_summary:
                app_summary[app] = {
                    'category': entry['category'],
                    'total_bytes': 0,
                    'total_flows': 0,
                    'hosts': set(),
                    'durations': []
                }
            
            app_summary[app]['total_bytes'] += entry['bytes_sent'] + entry['bytes_received']
            app_summary[app]['total_flows'] += 1
            app_summary[app]['hosts'].add(entry['host_ip'])
            app_summary[app]['durations'].append(entry['duration'])
        
        # Almacenar resumen
        try:
            with self.db_conn.cursor() as cursor:
                for app, summary in app_summary.items():
                    avg_duration = sum(summary['durations']) / len(summary['durations']) if summary['durations'] else 0
                    
                    cursor.execute("""
                        INSERT INTO application_summary 
                        (application, category, total_bytes, total_flows, 
                         unique_hosts, avg_duration, summary_period)
                        VALUES (%s, %s, %s, %s, %s, %s, %s)
                    """, (
                        app,
                        summary['category'],
                        summary['total_bytes'],
                        summary['total_flows'],
                        len(summary['hosts']),
                        avg_duration,
                        f"{self.config['poll_interval']}s"
                    ))
                    
        except Exception as e:
            self.logger.error(f"Error almacenando resumen: {e}")
    
    def signal_handler(self, signum, frame):
        """Manejar señales de terminación"""
        self.logger.info(f"Recibida señal {signum}, deteniendo extractor...")
        self.running = False
    
    async def run(self):
        """Ejecutar el bucle principal del extractor"""
        self.logger.info("Iniciando extractor de audiencias...")
        
        while self.running:
            try:
                start_time = datetime.utcnow()
                
                # Extraer datos de tráfico
                traffic_data = await self.extract_traffic_data()
                
                if traffic_data:
                    # Almacenar localmente
                    self.store_local_data(traffic_data)
                    
                    # Generar resumen
                    self.generate_summary(traffic_data)
                    
                    # Enviar al servidor remoto
                    if await self.send_to_remote(traffic_data):
                        self.mark_as_sent(start_time)
                
                # Esperar hasta el próximo ciclo
                await asyncio.sleep(self.config['poll_interval'])
                
            except Exception as e:
                self.logger.error(f"Error en el bucle principal: {e}")
                await asyncio.sleep(5)  # Esperar antes de reintentar
        
        self.logger.info("Extractor detenido")
        self.db_conn.close()

if __name__ == "__main__":
    extractor = AudienceExtractor()
    asyncio.run(extractor.run())