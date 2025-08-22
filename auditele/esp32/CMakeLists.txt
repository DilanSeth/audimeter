# CMakeLists.txt principal del proyecto
cmake_minimum_required(VERSION 3.5)

# Configuración del proyecto
set(PROJECT_NAME "tv_audience_monitor")

# Incluir ESP-IDF
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Definir el proyecto
project(${PROJECT_NAME})

# Configuraciones específicas del proyecto
target_compile_definitions(${PROJECT_NAME}.elf PRIVATE
    CONFIG_DSP_MAX_FFT_SIZE=2048
    CONFIG_FREERTOS_HZ=1000
)