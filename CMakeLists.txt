cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME LUNAR)

# Include toolchain file
include("cmake/gcc-arm-none-eabi.cmake")

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# ============================================================================
# 调试日志开关
#   -DENABLE_LOG=ON   编译 app/core/log.c，启用 LOG_PRINTF
#   -DENABLE_LOG=OFF  不编译 app/core/log.c，LOG_PRINTF 展开为空语句（默认）
#   两种模式都不改源码；关闭时 log.c 不产生任何目标文件。
# ============================================================================
option(ENABLE_LOG "Compile the debug log module (app/core/log.c)" OFF)
if(ENABLE_LOG)
    message("Debug log: ENABLED")
else()
    message("Debug log: DISABLED (app/core/log.c will not be compiled)")
endif()

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
set(PROJECT_SOURCES
    # BSP sources (v2.0 — unified interface, no OS dependency, handle pattern)
    bsp/bsp_bt401.c
    bsp/bsp_buzzer.c
    bsp/bsp_heat.c
    bsp/bsp_key.c
    bsp/bsp_led.c
    bsp/bsp_ntc.c
    bsp/bsp_pwm.c
    bsp/bsp_rtc.c
    
    # Components - PID
    components/pid/pid.c
    
    # Components - CRC16
    components/crc16/crc16.c
    
    # 事件总线已移除：唯一订阅者（app_main）改用直接回调，
    # 详见 key_service.h / heat_service.h 的 register_callback 接口。
    
    # Components - Protocol
    components/protocol/modbus_slave.c
    components/protocol/at_parser.c
    # at_command_manager.c 未加入编译：全工程零引用（机制未接入 ble_service），
    # 保留源文件待未来需要「带响应的 AT 命令队列」时再启用。
    
    # Core utilities 见下方 LOG_SOURCES（受 ENABLE_LOG 控制）

    # Service sources
    app/service/key_service.c
    app/service/heat_control.c
    app/service/heat_indicator.c
    app/service/heat_service.c
    app/service/ble_service.c
)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${PROJECT_SOURCES})

# 日志模块：仅在 ENABLE_LOG=ON 时参与编译
if(ENABLE_LOG)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE app/core/log.c)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE LOG_ENABLE=1)
else()
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE LOG_ENABLE=0)
endif()

# Add app_main separately
set(APP_MAIN app/app_main.c)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${APP_MAIN})


# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    bsp
    app
    app/core
    app/service
    components/pid
    components/crc16
    # components/event_bus  (已移除)
    components/protocol
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# 生成 HEX 和 BIN 文件
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
    BYPRODUCTS ${CMAKE_PROJECT_NAME}.hex ${CMAKE_PROJECT_NAME}.bin
    COMMENT "Generating HEX and binary files"
)

# target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -u _printf_float)
