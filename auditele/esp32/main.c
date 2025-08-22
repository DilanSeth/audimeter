/*
 * Sistema de Medición de Audiencia Televisiva
 * ESP32 con Audio Fingerprinting y Transmisión HTTP
 * 
 * Componentes:
 * - INMP441 I2S Microphone
 * - SSD1306 OLED Display
 * - 2 Botones para HMI
 * - WiFi para transmisión de datos
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "driver/i2s.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "cJSON.h"
#include "esp_dsp.h"
#include "ssd1306.h"
#include "md5.h"

static const char *TAG = "TV_AUDIENCE";

// ================================
// CONFIGURACIONES DEL SISTEMA
// ================================

#define WIFI_SSID      "YOUR_WIFI_SSID"
#define WIFI_PASS      "YOUR_WIFI_PASSWORD"
#define SERVER_URL     "https://your-server.com/api/fingerprint"
#define DEVICE_ID      "ESP32_AUDIO_001"

// Pines GPIO
#define I2S_WS_PIN     25
#define I2S_SCK_PIN    26
#define I2S_SD_PIN     27
#define OLED_SDA_PIN   21
#define OLED_SCL_PIN   22
#define BUTTON_1_PIN   32  // Configuración
#define BUTTON_2_PIN   33  // Info/Modo

// Configuraciones de audio por defecto
typedef struct {
    uint32_t sample_rate;       // Hz
    uint16_t fft_size;         // Puntos FFT
    uint16_t hop_length;       // Salto entre ventanas
    uint16_t n_mels;           // Número de filtros mel
    float min_freq;            // Frecuencia mínima
    float max_freq;            // Frecuencia máxima
    uint16_t capture_duration; // Segundos de captura
    uint16_t capture_interval; // Segundos entre capturas
    float noise_threshold;     // Umbral de ruido
    uint8_t quality_level;     // 1-5 (1=básica, 5=alta)
} audio_config_t;

// Configuración por defecto
audio_config_t audio_config = {
    .sample_rate = 16000,
    .fft_size = 1024,
    .hop_length = 512,
    .n_mels = 13,
    .min_freq = 300.0,
    .max_freq = 8000.0,
    .capture_duration = 30,
    .capture_interval = 60,
    .noise_threshold = 0.01,
    .quality_level = 3
};

// Estados del sistema
typedef enum {
    STATE_INIT,
    STATE_CONNECTING,
    STATE_SAMPLING,
    STATE_PROCESSING,
    STATE_TRANSMITTING,
    STATE_CONFIG,
    STATE_ERROR
} system_state_t;

// Variables globales
static system_state_t current_state = STATE_INIT;
static SSD1306_t display;
static QueueHandle_t audio_queue;
static EventGroupHandle_t wifi_event_group;
static bool wifi_connected = false;
static uint32_t samples_processed = 0;
static uint32_t transmissions_sent = 0;
static uint32_t config_menu_index = 0;

const int WIFI_CONNECTED_BIT = BIT0;

// ================================
// ESTRUCTURAS DE DATOS
// ================================

typedef struct {
    float *data;
    size_t length;
    uint64_t timestamp;
} audio_sample_t;

typedef struct {
    char hash[33];          // MD5 hash como string
    uint64_t timestamp;
    float confidence;
    uint16_t duration;
    char features[256];     // Features codificados en base64
} fingerprint_t;

// ================================
// FUNCIONES DE UTILIDAD
// ================================

// Función para obtener timestamp actual
uint64_t get_timestamp() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

// Función para calcular MD5
void calculate_md5(const char* data, size_t len, char* output) {
    unsigned char digest[16];
    MD5_CTX ctx;
    
    MD5_Init(&ctx);
    MD5_Update(&ctx, (unsigned char*)data, len);
    MD5_Final(digest, &ctx);
    
    for(int i = 0; i < 16; i++) {
        sprintf(output + i*2, "%02x", digest[i]);
    }
    output[32] = '\0';
}

// Codificación Base64 simple
static const char base64_chars[] = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void base64_encode(const unsigned char* data, size_t len, char* output) {
    size_t i, j;
    for (i = 0, j = 0; i < len; ) {
        uint32_t octet_a = i < len ? data[i++] : 0;
        uint32_t octet_b = i < len ? data[i++] : 0;
        uint32_t octet_c = i < len ? data[i++] : 0;
        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;
        
        output[j++] = base64_chars[(triple >> 3 * 6) & 0x3F];
        output[j++] = base64_chars[(triple >> 2 * 6) & 0x3F];
        output[j++] = base64_chars[(triple >> 1 * 6) & 0x3F];
        output[j++] = base64_chars[(triple >> 0 * 6) & 0x3F];
    }
    
    // Padding
    int padding = 3 - ((len - 1) % 3);
    for (int k = 0; k < padding; k++) {
        output[j - 1 - k] = '=';
    }
    output[j] = '\0';
}

// ================================
// FUNCIONES DE PROCESAMIENTO DE AUDIO
// ================================

// Aplicar ventana de Hamming
void apply_hamming_window(float* data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        float w = 0.54 - 0.46 * cosf(2.0 * M_PI * i / (length - 1));
        data[i] *= w;
    }
}

// Pre-énfasis para mejorar altas frecuencias
void pre_emphasis(float* data, size_t length, float alpha) {
    for (int i = length - 1; i > 0; i--) {
        data[i] = data[i] - alpha * data[i-1];
    }
}

// Detectar si la muestra contiene principalmente ruido
bool is_noise(float* data, size_t length) {
    float energy = 0.0;
    for (size_t i = 0; i < length; i++) {
        energy += data[i] * data[i];
    }
    energy /= length;
    return (energy < audio_config.noise_threshold);
}

// Extraer características MFCC simplificadas
void extract_mfcc_features(float* audio_data, size_t length, float* features) {
    // Implementación simplificada de MFCC
    // En producción, usar librerías especializadas como librosa-equivalent
    
    size_t n_frames = (length - audio_config.fft_size) / audio_config.hop_length + 1;
    float* fft_buffer = malloc(audio_config.fft_size * sizeof(float));
    float* power_spectrum = malloc(audio_config.fft_size/2 * sizeof(float));
    
    // Inicializar DSP
    dsps_fft2r_init_fc32(NULL, audio_config.fft_size);
    
    for (size_t frame = 0; frame < n_frames && frame < audio_config.n_mels; frame++) {
        size_t start = frame * audio_config.hop_length;
        
        // Copiar ventana de audio
        memcpy(fft_buffer, &audio_data[start], audio_config.fft_size * sizeof(float));
        
        // Aplicar ventana
        apply_hamming_window(fft_buffer, audio_config.fft_size);
        
        // FFT
        dsps_fft2r_fc32(fft_buffer, audio_config.fft_size);
        dsps_bit_rev_fc32(fft_buffer, audio_config.fft_size);
        
        // Calcular espectro de potencia
        for (int i = 0; i < audio_config.fft_size/2; i++) {
            float real = fft_buffer[i*2];
            float imag = fft_buffer[i*2 + 1];
            power_spectrum[i] = real*real + imag*imag;
        }
        
        // Filtros Mel simplificados - promedio de bandas
        float mel_energy = 0.0;
        int start_bin = (audio_config.min_freq * audio_config.fft_size) / audio_config.sample_rate;
        int end_bin = (audio_config.max_freq * audio_config.fft_size) / audio_config.sample_rate;
        
        for (int i = start_bin; i < end_bin; i++) {
            mel_energy += power_spectrum[i];
        }
        
        // Log y DCT simplificado
        features[frame] = logf(mel_energy + 1e-10);
    }
    
    free(fft_buffer);
    free(power_spectrum);
}

// Generar fingerprint de la muestra de audio
void generate_fingerprint(audio_sample_t* sample, fingerprint_t* fingerprint) {
    if (is_noise(sample->data, sample->length)) {
        ESP_LOGW(TAG, "Muestra descartada: ruido detectado");
        fingerprint->confidence = 0.0;
        return;
    }
    
    // Pre-procesamiento
    pre_emphasis(sample->data, sample->length, 0.97);
    
    // Extraer características
    float* mfcc_features = malloc(audio_config.n_mels * sizeof(float));
    extract_mfcc_features(sample->data, sample->length, mfcc_features);
    
    // Codificar características en Base64
    base64_encode((unsigned char*)mfcc_features, 
                  audio_config.n_mels * sizeof(float), 
                  fingerprint->features);
    
    // Generar hash único de las características
    calculate_md5(fingerprint->features, strlen(fingerprint->features), fingerprint->hash);
    
    // Calcular confianza basada en energía y varianza
    float energy = 0.0, variance = 0.0, mean = 0.0;
    for (int i = 0; i < audio_config.n_mels; i++) {
        energy += mfcc_features[i] * mfcc_features[i];
        mean += mfcc_features[i];
    }
    mean /= audio_config.n_mels;
    
    for (int i = 0; i < audio_config.n_mels; i++) {
        float diff = mfcc_features[i] - mean;
        variance += diff * diff;
    }
    variance /= audio_config.n_mels;
    
    fingerprint->confidence = fminf(1.0, sqrtf(energy) * sqrtf(variance) * 10.0);
    fingerprint->timestamp = sample->timestamp;
    fingerprint->duration = audio_config.capture_duration;
    
    free(mfcc_features);
    
    ESP_LOGI(TAG, "Fingerprint generado - Hash: %.8s..., Confianza: %.2f", 
             fingerprint->hash, fingerprint->confidence);
}

// ================================
// FUNCIONES DE HARDWARE
// ================================

// Configurar I2S para micrófono
void init_i2s() {
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = audio_config.sample_rate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 1024,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD_PIN
    };

    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pin_config));
}

// Configurar display OLED
void init_display() {
    i2c_master_init(&display, OLED_SDA_PIN, OLED_SCL_PIN, -1);
    ssd1306_init(&display, 128, 64);
    ssd1306_clear_screen(&display, false);
    ssd1306_contrast(&display, 0xFF);
}

// Configurar botones
void init_buttons() {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_POSEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BUTTON_1_PIN) | (1ULL << BUTTON_2_PIN),
        .pull_down_en = 0,
        .pull_up_en = 1,
    };
    gpio_config(&io_conf);
}

// ================================
// INTERFAZ HMI
// ================================

void update_display() {
    ssd1306_clear_screen(&display, false);
    
    char line1[32], line2[32], line3[32], line4[32];
    
    switch(current_state) {
        case STATE_INIT:
            strcpy(line1, "TV Audience Monitor");
            strcpy(line2, "Inicializando...");
            sprintf(line3, "Calidad: %d/5", audio_config.quality_level);
            strcpy(line4, "");
            break;
            
        case STATE_CONNECTING:
            strcpy(line1, "Conectando WiFi");
            strcpy(line2, "Espere...");
            strcpy(line3, "");
            strcpy(line4, "");
            break;
            
        case STATE_SAMPLING:
            strcpy(line1, "Capturando Audio");
            sprintf(line2, "SR: %dkHz", audio_config.sample_rate/1000);
            sprintf(line3, "Muestras: %lu", samples_processed);
            sprintf(line4, "Enviadas: %lu", transmissions_sent);
            break;
            
        case STATE_PROCESSING:
            strcpy(line1, "Procesando...");
            sprintf(line2, "FFT: %d pts", audio_config.fft_size);
            sprintf(line3, "MFCC: %d coef", audio_config.n_mels);
            strcpy(line4, "Generando hash");
            break;
            
        case STATE_TRANSMITTING:
            strcpy(line1, "Transmitiendo");
            strcpy(line2, "Enviando datos");
            strcpy(line3, "al servidor");
            strcpy(line4, "");
            break;
            
        case STATE_CONFIG:
            strcpy(line1, "CONFIGURACION");
            switch(config_menu_index % 8) {
                case 0:
                    sprintf(line2, ">Sample Rate");
                    sprintf(line3, " %d Hz", audio_config.sample_rate);
                    break;
                case 1:
                    sprintf(line2, ">FFT Size");
                    sprintf(line3, " %d puntos", audio_config.fft_size);
                    break;
                case 2:
                    sprintf(line2, ">MFCC Coeffs");
                    sprintf(line3, " %d coef", audio_config.n_mels);
                    break;
                case 3:
                    sprintf(line2, ">Duracion Cap");
                    sprintf(line3, " %d seg", audio_config.capture_duration);
                    break;
                case 4:
                    sprintf(line2, ">Intervalo");
                    sprintf(line3, " %d seg", audio_config.capture_interval);
                    break;
                case 5:
                    sprintf(line2, ">Umbral Ruido");
                    sprintf(line3, " %.3f", audio_config.noise_threshold);
                    break;
                case 6:
                    sprintf(line2, ">Calidad");
                    sprintf(line3, " %d/5", audio_config.quality_level);
                    break;
                case 7:
                    strcpy(line2, ">Salir Config");
                    strcpy(line3, " Presionar B2");
                    break;
            }
            strcpy(line4, "B1:Nav B2:Edit/Exit");
            break;
            
        case STATE_ERROR:
            strcpy(line1, "ERROR");
            strcpy(line2, "Revisar conexion");
            strcpy(line3, "o configuracion");
            strcpy(line4, "B1: Reintentar");
            break;
    }
    
    ssd1306_display_text(&display, 0, line1, strlen(line1), false);
    ssd1306_display_text(&display, 1, line2, strlen(line2), false);
    ssd1306_display_text(&display, 2, line3, strlen(line3), false);
    ssd1306_display_text(&display, 3, line4, strlen(line4), false);
}

void handle_button_press(int button) {
    if (button == BUTTON_1_PIN) {
        if (current_state == STATE_CONFIG) {
            config_menu_index++;
        } else if (current_state == STATE_ERROR) {
            current_state = STATE_INIT;
        } else {
            current_state = STATE_CONFIG;
            config_menu_index = 0;
        }
    } else if (button == BUTTON_2_PIN) {
        if (current_state == STATE_CONFIG) {
            // Editar parámetro actual o salir
            switch(config_menu_index % 8) {
                case 0: // Sample Rate
                    audio_config.sample_rate = (audio_config.sample_rate == 16000) ? 22050 : 
                                               (audio_config.sample_rate == 22050) ? 44100 : 16000;
                    break;
                case 1: // FFT Size
                    audio_config.fft_size = (audio_config.fft_size == 512) ? 1024 : 
                                            (audio_config.fft_size == 1024) ? 2048 : 512;
                    break;
                case 2: // MFCC
                    audio_config.n_mels = (audio_config.n_mels + 2) % 20 + 10;
                    break;
                case 3: // Duración
                    audio_config.capture_duration = (audio_config.capture_duration % 60) + 15;
                    break;
                case 4: // Intervalo
                    audio_config.capture_interval = (audio_config.capture_interval % 300) + 30;
                    break;
                case 5: // Umbral
                    audio_config.noise_threshold += 0.01;
                    if (audio_config.noise_threshold > 0.1) audio_config.noise_threshold = 0.001;
                    break;
                case 6: // Calidad
                    audio_config.quality_level = (audio_config.quality_level % 5) + 1;
                    break;
                case 7: // Salir
                    current_state = STATE_SAMPLING;
                    break;
            }
        }
    }
    update_display();
}

// ================================
// FUNCIONES DE RED
// ================================

static void event_handler(void* arg, esp_event_base_t event_base,
                         int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        wifi_connected = false;
        ESP_LOGI(TAG, "Reintentando conexión WiFi");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_connected = true;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "WiFi conectado exitosamente");
    }
}

void init_wifi() {
    wifi_event_group = xEventGroupCreate();
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// Callback para manejo de respuesta HTTP
esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
    }
    return ESP_OK;
}

// Enviar fingerprint al servidor
bool send_fingerprint(fingerprint_t* fingerprint) {
    if (!wifi_connected) {
        ESP_LOGW(TAG, "WiFi no conectado, fingerprint no enviado");
        return false;
    }
    
    // Crear JSON payload
    cJSON *json = cJSON_CreateObject();
    cJSON *device_id = cJSON_CreateString(DEVICE_ID);
    cJSON *timestamp = cJSON_CreateNumber(fingerprint->timestamp);
    cJSON *hash = cJSON_CreateString(fingerprint->hash);
    cJSON *confidence = cJSON_CreateNumber(fingerprint->confidence);
    cJSON *duration = cJSON_CreateNumber(fingerprint->duration);
    cJSON *features = cJSON_CreateString(fingerprint->features);
    cJSON *sample_rate = cJSON_CreateNumber(audio_config.sample_rate);
    cJSON *quality = cJSON_CreateNumber(audio_config.quality_level);
    
    cJSON_AddItemToObject(json, "device_id", device_id);
    cJSON_AddItemToObject(json, "timestamp", timestamp);
    cJSON_AddItemToObject(json, "hash", hash);
    cJSON_AddItemToObject(json, "confidence", confidence);
    cJSON_AddItemToObject(json, "duration", duration);
    cJSON_AddItemToObject(json, "features", features);
    cJSON_AddItemToObject(json, "sample_rate", sample_rate);
    cJSON_AddItemToObject(json, "quality_level", quality);
    
    char *json_string = cJSON_Print(json);
    
    // Configurar cliente HTTP
    esp_http_client_config_t config = {
        .url = SERVER_URL,
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_data(client, json_string, strlen(json_string));
    
    esp_err_t err = esp_http_client_perform(client);
    bool success = false;
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200 || status_code == 201) {
            ESP_LOGI(TAG, "Fingerprint enviado exitosamente. Status: %d", status_code);
            transmissions_sent++;
            success = true;
        } else {
            ESP_LOGW(TAG, "Error en servidor. Status: %d", status_code);
        }
    } else {
        ESP_LOGE(TAG, "Error HTTP: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    cJSON_Delete(json);
    free(json_string);
    
    return success;
}

// ================================
// TAREAS PRINCIPALES
// ================================

// Tarea de captura de audio
void audio_capture_task(void *pvParameters) {
    size_t buffer_size = audio_config.sample_rate * audio_config.capture_duration;
    int32_t* raw_buffer = malloc(buffer_size * sizeof(int32_t));
    float* audio_buffer = malloc(buffer_size * sizeof(float));
    size_t bytes_read;
    
    while (1) {
        if (current_state == STATE_SAMPLING || current_state == STATE_PROCESSING) {
            current_state = STATE_SAMPLING;
            update_display();
            
            ESP_LOGI(TAG, "Iniciando captura de %d segundos", audio_config.capture_duration);
            
            // Capturar audio
            for (int i = 0; i < buffer_size; i++) {
                i2s_read(I2S_NUM_0, &raw_buffer[i], sizeof(int32_t), &bytes_read, portMAX_DELAY);
                // Convertir de int32 a float y normalizar
                audio_buffer[i] = (float)raw_buffer[i] / (float)INT32_MAX;
            }
            
            // Crear muestra de audio
            audio_sample_t sample = {
                .data = audio_buffer,
                .length = buffer_size,
                .timestamp = get_timestamp()
            };
            
            // Enviar a cola de procesamiento
            if (xQueueSend(audio_queue, &sample, 100) != pdTRUE) {
                ESP_LOGW(TAG, "Cola de audio llena, muestra descartada");
            } else {
                samples_processed++;
                ESP_LOGI(TAG, "Muestra capturada y enviada a procesamiento");
            }
        }
        
        // Esperar intervalo entre capturas
        vTaskDelay(pdMS_TO_TICKS(audio_config.capture_interval * 1000));
    }
    
    free(raw_buffer);
    free(audio_buffer);
    vTaskDelete(NULL);
}

// Tarea de procesamiento de audio y generación de fingerprints
void audio_processing_task(void *pvParameters) {
    audio_sample_t sample;
    fingerprint_t fingerprint;
    
    while (1) {
        // Esperar muestra de audio de la cola
        if (xQueueReceive(audio_queue, &sample, portMAX_DELAY) == pdTRUE) {
            current_state = STATE_PROCESSING;
            update_display();
            
            ESP_LOGI(TAG, "Procesando muestra de audio...");
            
            // Generar fingerprint
            generate_fingerprint(&sample, &fingerprint);
            
            // Solo enviar si tiene confianza suficiente
            if (fingerprint.confidence > 0.1) {
                current_state = STATE_TRANSMITTING;
                update_display();
                
                if (send_fingerprint(&fingerprint)) {
                    ESP_LOGI(TAG, "Fingerprint enviado exitosamente");
                } else {
                    ESP_LOGE(TAG, "Error al enviar fingerprint");
                    current_state = STATE_ERROR;
                    update_display();
                    vTaskDelay(pdMS_TO_TICKS(5000)); // Esperar antes de reintentar
                }
            } else {
                ESP_LOGW(TAG, "Fingerprint descartado por baja confianza: %.2f", 
                         fingerprint.confidence);
            }
            
            // Volver a estado de muestreo
            current_state = STATE_SAMPLING;
            update_display();
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    vTaskDelete(NULL);
}

// Tarea de manejo de botones
void button_task(void *pvParameters) {
    static uint32_t last_button1_time = 0;
    static uint32_t last_button2_time = 0;
    const uint32_t debounce_time = 200; // ms
    
    while (1) {
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        // Leer botón 1
        if (gpio_get_level(BUTTON_1_PIN) == 0) { // Activo bajo
            if (current_time - last_button1_time > debounce_time) {
                handle_button_press(BUTTON_1_PIN);
                last_button1_time = current_time;
                ESP_LOGI(TAG, "Botón 1 presionado");
            }
        }
        
        // Leer botón 2
        if (gpio_get_level(BUTTON_2_PIN) == 0) { // Activo bajo
            if (current_time - last_button2_time > debounce_time) {
                handle_button_press(BUTTON_2_PIN);
                last_button2_time = current_time;
                ESP_LOGI(TAG, "Botón 2 presionado");
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    vTaskDelete(NULL);
}

// Tarea de actualización de display
void display_task(void *pvParameters) {
    static system_state_t last_state = STATE_INIT;
    static uint32_t last_samples = 0;
    static uint32_t last_transmissions = 0;
    
    while (1) {
        // Actualizar display solo si hay cambios
        if (current_state != last_state || 
            samples_processed != last_samples ||
            transmissions_sent != last_transmissions) {
            
            update_display();
            last_state = current_state;
            last_samples = samples_processed;
            last_transmissions = transmissions_sent;
        }
        
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    vTaskDelete(NULL);
}

// Tarea de sincronización de tiempo
void time_sync_task(void *pvParameters) {
    // Configurar SNTP
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
    
    while (1) {
        if (wifi_connected) {
            time_t now;
            struct tm timeinfo;
            time(&now);
            localtime_r(&now, &timeinfo);
            
            if (timeinfo.tm_year > (2020 - 1900)) {
                ESP_LOGI(TAG, "Tiempo sincronizado: %04d-%02d-%02d %02d:%02d:%02d",
                         timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                         timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            } else {
                ESP_LOGW(TAG, "Tiempo no sincronizado, reintentando...");
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(3600000)); // Sincronizar cada hora
    }
    
    vTaskDelete(NULL);
}

// Tarea de monitoreo del sistema
void system_monitor_task(void *pvParameters) {
    while (1) {
        // Monitorear memoria libre
        size_t free_heap = esp_get_free_heap_size();
        if (free_heap < 50000) { // Menos de 50KB disponibles
            ESP_LOGW(TAG, "Memoria baja: %zu bytes libres", free_heap);
        }
        
        // Monitorear estado de WiFi
        if (!wifi_connected && current_state != STATE_CONNECTING) {
            ESP_LOGW(TAG, "WiFi desconectado, reintentando...");
            current_state = STATE_CONNECTING;
        }
        
        // Estadísticas del sistema
        ESP_LOGI(TAG, "Stats - Muestras: %lu, Enviadas: %lu, Memoria libre: %zu, Estado: %d",
                 samples_processed, transmissions_sent, free_heap, current_state);
        
        vTaskDelay(pdMS_TO_TICKS(30000)); // Monitoreo cada 30 segundos
    }
    
    vTaskDelete(NULL);
}

// ================================
// FUNCIONES DE CONFIGURACIÓN
// ================================

// Guardar configuración en NVS
void save_config() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("audio_config", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_set_blob(nvs_handle, "config", &audio_config, sizeof(audio_config));
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Configuración guardada");
    } else {
        ESP_LOGE(TAG, "Error guardando configuración: %s", esp_err_to_name(err));
    }
}

// Cargar configuración desde NVS
void load_config() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("audio_config", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        size_t required_size = sizeof(audio_config);
        err = nvs_get_blob(nvs_handle, "config", &audio_config, &required_size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Configuración cargada desde NVS");
        } else {
            ESP_LOGI(TAG, "Usando configuración por defecto");
        }
        nvs_close(nvs_handle);
    }
}

// Aplicar configuración de calidad predefinida
void apply_quality_preset() {
    switch(audio_config.quality_level) {
        case 1: // Básica - bajo consumo
            audio_config.sample_rate = 8000;
            audio_config.fft_size = 512;
            audio_config.n_mels = 10;
            audio_config.capture_duration = 15;
            audio_config.capture_interval = 120;
            break;
            
        case 2: // Baja
            audio_config.sample_rate = 16000;
            audio_config.fft_size = 512;
            audio_config.n_mels = 12;
            audio_config.capture_duration = 20;
            audio_config.capture_interval = 90;
            break;
            
        case 3: // Media (por defecto)
            audio_config.sample_rate = 16000;
            audio_config.fft_size = 1024;
            audio_config.n_mels = 13;
            audio_config.capture_duration = 30;
            audio_config.capture_interval = 60;
            break;
            
        case 4: // Alta
            audio_config.sample_rate = 22050;
            audio_config.fft_size = 1024;
            audio_config.n_mels = 15;
            audio_config.capture_duration = 45;
            audio_config.capture_interval = 45;
            break;
            
        case 5: // Máxima - mayor precisión
            audio_config.sample_rate = 44100;
            audio_config.fft_size = 2048;
            audio_config.n_mels = 20;
            audio_config.capture_duration = 60;
            audio_config.capture_interval = 30;
            break;
    }
    
    ESP_LOGI(TAG, "Configuración de calidad %d aplicada", audio_config.quality_level);
}

// ================================
// FUNCIÓN PRINCIPAL
// ================================

void app_main() {
    ESP_LOGI(TAG, "=== Sistema de Medición de Audiencia TV ===");
    ESP_LOGI(TAG, "Iniciando sistema...");
    
    current_state = STATE_INIT;
    
    // Inicializar NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Cargar configuración
    load_config();
    apply_quality_preset();
    
    // Inicializar hardware
    ESP_LOGI(TAG, "Inicializando hardware...");
    init_i2s();
    init_display();
    init_buttons();
    
    // Mostrar pantalla inicial
    update_display();
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Inicializar WiFi
    ESP_LOGI(TAG, "Configurando WiFi...");
    current_state = STATE_CONNECTING;
    update_display();
    init_wifi();
    
    // Esperar conexión WiFi
    xEventGroupWaitBits(wifi_event_group,
                        WIFI_CONNECTED_BIT,
                        false,
                        true,
                        portMAX_DELAY);
    
    ESP_LOGI(TAG, "WiFi conectado exitosamente");
    
    // Inicializar DSP
    dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE);
    
    // Crear cola para muestras de audio
    audio_queue = xQueueCreate(3, sizeof(audio_sample_t));
    if (audio_queue == NULL) {
        ESP_LOGE(TAG, "Error creando cola de audio");
        return;
    }
    
    // Cambiar a estado de muestreo
    current_state = STATE_SAMPLING;
    update_display();
    
    // Crear tareas
    ESP_LOGI(TAG, "Creando tareas del sistema...");
    
    xTaskCreatePinnedToCore(audio_capture_task, 
                           "audio_capture", 
                           8192, 
                           NULL, 
                           5,  // Alta prioridad para captura
                           NULL,
                           1); // Core 1
    
    xTaskCreatePinnedToCore(audio_processing_task, 
                           "audio_processing", 
                           16384, 
                           NULL, 
                           4, 
                           NULL,
                           0); // Core 0
    
    xTaskCreatePinnedToCore(button_task, 
                           "button_handler", 
                           2048, 
                           NULL, 
                           3, 
                           NULL,
                           0);
    
    xTaskCreatePinnedToCore(display_task, 
                           "display_update", 
                           4096, 
                           NULL, 
                           2, 
                           NULL,
                           0);
    
    xTaskCreatePinnedToCore(time_sync_task, 
                           "time_sync", 
                           4096, 
                           NULL, 
                           1, 
                           NULL,
                           0);
    
    xTaskCreatePinnedToCore(system_monitor_task, 
                           "system_monitor", 
                           4096, 
                           NULL, 
                           1, 
                           NULL,
                           0);
    
    ESP_LOGI(TAG, "Sistema iniciado exitosamente");
    ESP_LOGI(TAG, "Configuración actual:");
    ESP_LOGI(TAG, "- Sample Rate: %d Hz", audio_config.sample_rate);
    ESP_LOGI(TAG, "- FFT Size: %d puntos", audio_config.fft_size);
    ESP_LOGI(TAG, "- MFCC Coefficients: %d", audio_config.n_mels);
    ESP_LOGI(TAG, "- Duración captura: %d seg", audio_config.capture_duration);
    ESP_LOGI(TAG, "- Intervalo: %d seg", audio_config.capture_interval);
    ESP_LOGI(TAG, "- Calidad: %d/5", audio_config.quality_level);
    
    // Guardar configuración actual
    save_config();
    
    // La tarea principal termina aquí, las otras tareas continúan ejecutándose
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

//