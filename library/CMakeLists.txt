# ============================================================
# mysdk-stm32 组件构建脚本
# ------------------------------------------------------------
# 位置：本文件是 SDK 源仓的 <SDK>/library/CMakeLists.txt —— 也就是各 layer 目录
#       (chip/ devices/ protocols/ services/ middleware/) 所在那一层的根。
#       下面所有 GLOB 都用 ${CMAKE_CURRENT_SOURCE_DIR}（= 本文件所在目录）取路径，
#       故它在源仓内与工程侧都成立（源仓可直接 cmake -S library -B build）。
#
# 本文件由 sdk-pull.py 拉取到工程 <dest>/CMakeLists.txt，
# 随拉取自动更新；工程侧不要手工修改（会被下次拉取覆盖）。
#
# 工程侧接入（目录名与 sdk.toml 的 dest 保持一致）：
#     add_subdirectory(MySDK)          # 需放在 add_subdirectory(cmake/stm32cubemx) 之后
#     target_link_libraries(<主目标> PRIVATE mysdk)
#
# 依赖宿主工程提供：
#     - HAL/RTOS/LwIP 头文件路径与全局宏（STM32Fxxx / USE_HAL_DRIVER / DEBUG）
#     - CubeMX CMake 导出的工程统一由 INTERFACE 目标 stm32cubemx 提供，
#       本组件自动链接之；非 CubeMX 工程请自行注入（见下方 WARNING 分支）
# ============================================================

add_library(mysdk STATIC)

# ---- 位置自检 ----
# 本脚本必须与各 layer 目录同级（源仓 <SDK>/library/，或工程侧 <dest>/）。
# 若一个 layer 目录都不存在，说明它被放错了位置 -> 立即报错退出，
# 否则下面的 GLOB 会静默匹配 0 个文件，产出一个空的 mysdk 静态库（极难发现）。
set(_mysdk_layer_found FALSE)
foreach(_mysdk_layer chip devices middleware protocols services)
    if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${_mysdk_layer}")
        set(_mysdk_layer_found TRUE)
    endif()
endforeach()
if(NOT _mysdk_layer_found)
    message(FATAL_ERROR
        "mysdk: no layer dir (chip/ devices/ protocols/ services/ middleware/) under "
        "${CMAKE_CURRENT_SOURCE_DIR}. "
        "本 CMakeLists.txt 必须与各 layer 目录同级：源码中位于 <SDK>/library/，"
        "工程侧由 sdk-pull.py 复制到 <dest>/（与 chip/ 同级）。")
endif()

# ---- 源文件（按 layer/module 组织：<layer>/<module>/Src/*.c）----
# CONFIGURE_DEPENDS：拉取增删模块后，下次构建自动重扫源文件，无需手动重跑 cmake
file(GLOB_RECURSE MYSDK_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/chip/*/Src/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/devices/*/Src/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/middleware/*/Src/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/*/Src/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/services/*/Src/*.c
    # 汇编源（如 SEGGER_RTT_ASM_ARMv7M.S 位于 middleware/SEGGER_RTT/Inc/）
    ${CMAKE_CURRENT_SOURCE_DIR}/chip/*/Src/*.S
    ${CMAKE_CURRENT_SOURCE_DIR}/chip/*/Inc/*.S
    ${CMAKE_CURRENT_SOURCE_DIR}/devices/*/Src/*.S
    ${CMAKE_CURRENT_SOURCE_DIR}/devices/*/Inc/*.S
    ${CMAKE_CURRENT_SOURCE_DIR}/middleware/*/Src/*.S
    ${CMAKE_CURRENT_SOURCE_DIR}/middleware/*/Inc/*.S
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/*/Src/*.S
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/*/Inc/*.S
    ${CMAKE_CURRENT_SOURCE_DIR}/services/*/Src/*.S
    ${CMAKE_CURRENT_SOURCE_DIR}/services/*/Inc/*.S
)
target_sources(mysdk PRIVATE ${MYSDK_SOURCES})

# ---- 头文件路径（PUBLIC：SDK 头互相引用，工程侧也要包含 SDK 头）----
file(GLOB MYSDK_INCS LIST_DIRECTORIES true
    ${CMAKE_CURRENT_SOURCE_DIR}/chip/*/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/devices/*/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/middleware/*/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/*/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/services/*/Inc
)
target_include_directories(mysdk PUBLIC ${MYSDK_INCS})

# ---- 宿主依赖注入 ----
if(TARGET stm32cubemx)
    target_link_libraries(mysdk PUBLIC stm32cubemx)
else()
    message(WARNING
        "mysdk: 未找到宿主目标 stm32cubemx，"
        "请自行为 mysdk 注入 HAL 头文件路径与宏定义（STM32Fxxx/USE_HAL_DRIVER）")
endif()

# ---- 多槽 OTA（services.ota_core 模块提供）----
# 三分片的构建逻辑（base/A/B + 三份链接脚本）在本模块的 cmake/ota_slots.cmake 里，
# 这里自动挂载 —— 于是**工程根 CMakeLists 不需要任何 OTA 槽相关内容**。
# 它内部用 cmake_language(DEFER DIRECTORY <工程根>) 把「建 A/B 目标」推迟到工程根
# 处理结束时执行（那时 base 目标的源/链接库才齐全，复制属性才不会漏源）。
# 关掉：工程侧 set(OTA_MULTISLOT OFF)；改槽几何 / 基版 .ld：见该文件头部说明。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/services/ota_core/cmake/ota_slots.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/services/ota_core/cmake/ota_slots.cmake")
endif()

# ---- 产物生成：给宿主主目标自动出 .hex / .bin ----
# 放在 SDK 侧，是为了省掉每个工程根 CMakeLists 都要抄一遍的 objcopy 样板
# （CubeMX 生成的 CMakeLists 里没有这段，历来靠手加）。
#   关掉：set(MYSDK_ARTIFACTS OFF)
#
# ⚠ 必须经 DEFER 到**工程根作用域**执行：add_custom_command(TARGET ...) 要求目标与调用方
#   在同一目录 —— 本文件跑在 <MySDK>/ 子目录，而宿主主目标建在工程根，直接调用会报
#   "TARGET ... was not created in this directory"（实测踩到）。
#   目标判定为保护：宿主若没用 ${CMAKE_PROJECT_NAME} 建可执行目标（只当静态库用时），
#   这里静默跳过，不报错。
# 注：多槽工程的 <name>_A / <name>_B 由 services/ota_core/cmake/ota_slots.cmake 各自生成
#     —— 它同样是在工程根的 DEFER 里做的（同一个道理）。
macro(mysdk_enable_artifacts)
    if((NOT DEFINED MYSDK_ARTIFACTS OR MYSDK_ARTIFACTS) AND TARGET ${CMAKE_PROJECT_NAME})
        add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} -O ihex
                    $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
                    $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/${CMAKE_PROJECT_NAME}.hex
            COMMAND ${CMAKE_OBJCOPY} -O binary
                    $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
                    $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/${CMAKE_PROJECT_NAME}.bin
            COMMENT "Generating HEX and BIN firmware (${CMAKE_PROJECT_NAME})")
    endif()
endmacro()

if(COMMAND cmake_language)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL mysdk_enable_artifacts)
else()
    message(FATAL_ERROR "mysdk: 需要 CMake >= 3.19（用到 cmake_language(DEFER)）")
endif()
