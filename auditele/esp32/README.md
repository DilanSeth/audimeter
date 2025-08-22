# Sistema de Medición de Audiencia Televisiva - ESP32

## Descripción
Sistema de monitoreo de audiencia televisiva basado en ESP32 que utiliza audio fingerprinting para identificar contenido televisivo y enviar estadísticas a un servidor central.

## Características
-  Captura de audio continua con micrófono INMP441
-  Procesamiento DSP avanzado (FFT, MFCC, Pre-énfasis)
-  Generación de huellas digitales de audio (fingerprints)
-  Transmisión segura vía WiFi HTTP/HTTPS
-  Interfaz HMI con pantalla OLED y botones
-  Configuración flexible de parámetros de calidad
-  Sincronización de tiempo NTP
-  Monitoreo del sistema y estadísticas
-  Detección automática de ruido vs contenido

## Hardware Requerido

### Componentes principales:
- **ESP32-WROOM-32** (módulo dual core)
- **INMP441** - Micrófono I2S digital
- **SSD1306** - Display OLED 128x64 (I2C)
- **2 Botones** - Pulsadores para interfaz
- **Resistencias pull-up** 10kΩ para botones

### Conexiones:

#### Micrófono INMP441 (I2S):
```
INMP441    ESP32
VDD    →   3.3V
GND    →   GND
SCK    →   GPIO26 (I2S_SCK)
WS     →   GPIO25 (I2S_WS)
SD     →   GPIO27 (I2S_SD)
L/R    →   GND (canal izquierdo)
```

#### Display SSD1306 (I2C):
```
SSD1306    ESP32
VCC    →   3.3V
GND    →   GND
SDA    →   GPIO21
SCL    →   GPIO22
```

#### Botones:
```
Botón 1    ESP32
Pin 1  →   GPIO32
Pin 2  →   GND

Botón 2    ESP32
Pin 1  →   GPIO33
Pin 2  →   GND
```

## Instalación

### 1. Preparar entorno ESP-IDF

```bash
# Instalar ESP-IDF v4.4 o superior
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh
. ./export.sh
```

### 2. Clonar y configurar proyecto

```bash
# Crear directorio del proyecto
mkdir tv_audience_monitor
cd tv_audience_monitor

# Crear estructura de directorios
mkdir main
mkdir components
mkdir components/ssd1306

# Copiar archivos del código proporcionado
# - tv_audience_main.c → main/
# - CMakeLists.txt → raíz del proyecto
# - main/CMakeLists.txt → main/
# - sdkconfig.defaults → raíz del proyecto
# - components/ssd1306/ssd1306.h → components/ssd1306/
```

### 3. Configurar WiFi y servidor

Editar `main/tv_audience_main.c`:
```c
#define WIFI_SSID      "TU_RED_WIFI"
#define WIFI_PASS      "TU_PASSWORD_WIFI"  
#define SERVER_URL     "https://tu-servidor.com/api/fingerprint"
#define DEVICE_ID      "ESP32_AUDIO_001"
```

### 4. Compilar y flashear

```bash
# Configurar target ESP32
idf.py set-target esp32

# Compilar
idf.py build

# Flashear (conectar ESP32 vía USB)
idf.py -p /dev/ttyUSB0 flash

# Monitorear salida serie
idf.py -p /dev/ttyUSB0 monitor
```

## Uso del Sistema

### Interfaz de Usuario

**Estados del display:**
- **INIT**: Inicializando sistema
- **CONNECTING**: Conectando a WiFi  
- **SAMPLING**: Capturando audio del TV
- **PROCESSING**: Generando fingerprint
- **TRANSMITTING**: Enviando datos al servidor
- **CONFIG**: Menú de configuración
- **ERROR**: Estado de error

**Controles:**
- **Botón 1**: Navegar menú / Reintentar en error
- **Botón 2**: Editar parámetro / Cambiar modo

### Configuración de Parámetros

Presionar **Botón 1** para acceder al menú de configuración:

1. **Sample Rate**: 8kHz, 16kHz, 22kHz, 44kHz
2. **FFT Size**: 512, 1024, 2048 puntos
3. **MFCC Coeffs**: 10-20 coeficientes  
4. **Duración Captura**: 15-60 segundos
5. **Intervalo**: 30-300 segundos entre capturas
6. **Umbral Ruido**: 0.001-0.1 sensibilidad
7. **Calidad**: 1-5 (presets predefinidos)

**Presets de Calidad:**
- **Nivel 1**: Básico (8kHz, bajo consumo)
- **Nivel 2**: Baja (16kHz, consumo moderado)  
- **Nivel 3**: Media (16kHz, balanceado) ⭐ **Por defecto**
- **Nivel 4**: Alta (22kHz, mayor precisión)
- **Nivel 5**: Máxima (44kHz, máxima precisión)

## Protocolo de Datos

### Formato JSON enviado al servidor:

```json
{
  "device_id": "ESP32_AUDIO_001",
  "timestamp": 1640995200000000,
  "hash": "a1b2c3d4e5f67890abcdef1234567890",
  "confidence": 0.85,
  "duration": 30,
  "features": "base64_encoded_mfcc_data==",
  "sample_rate": 16000,
  "quality_level": 3
}
```

### Campos:
- **device_id**: Identificador único del dispositivo
- **timestamp**: Marca temporal en microsegundos
- **hash**: Hash MD5 de las características de audio
- **confidence**: Confianza de la muestra (0.0-1.0)
- **duration**: Duración de la captura en segundos
- **features**: Características MFCC codificadas en Base64
- **sample_rate**: Frecuencia de muestreo utilizada
- **quality_level**: Nivel de calidad configurado

## Servidor de Recepción

El servidor debe implementar un endpoint HTTP/HTTPS que:

1. **Reciba** los datos JSON via POST
2. **Almacene** fingerprints en base de datos
3. **Compare** con patrones de referencia de canales TV
4. **Genere** estadísticas de audiencia
5. **Responda** con status 200/201 para confirmar recepción

### Ejemplo de endpoint (Python/Flask):
```python
@app.route('/api/fingerprint', methods=['POST'])
def receive_finger