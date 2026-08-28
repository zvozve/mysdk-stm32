# ============================================================
# mystm32-sdk 组件构建脚本
# ------------------------------------------------------------
# 本文件属于 SDK 仓库，由 sync_lib.py 拉取到工程 <dest>/CMakeLists.txt，
# 随拉取自动更新；工程侧不要手工修改（会被下次拉取覆盖）。
#
# 工程侧接入（目录名与 sdk.toml 的 dest 保持一致）：
#     add_subdirectory(MySDK)          # 需放在 add_subdirectory(cmake/stm32cubemx) 之后
#     target_link_libraries(<主目标> PRIVATE mystm32)
#
# 依赖宿主工程提供：
#     - HAL/RTOS/LwIP 头文件路径与全局宏（STM32Fxxx / USE_HAL_DRIVER / DEBUG）
#     - CubeMX CMake 导出的工程统一由 INTERFACE 目标 stm32cubemx 提供，
#       本组件自动链接之；非 CubeMX 工程请自行注入（见下方 WARNING 分支）
# ============================================================

add_library(mystm32 STATIC)

# ---- 源文件（按 layer/module 组织：<layer>/<module>/Src/*.c）----
# CONFIGURE_DEPENDS：拉取增删模块后，下次构建自动重扫源文件，无需手动重跑 cmake
file(GLOB_RECURSE MYSTM32_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/chip/*/Src/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/devices/*/Src/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/middleware/*/Src/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/*/Src/*.c
)
target_sources(mystm32 PRIVATE ${MYSTM32_SOURCES})

# ---- 头文件路径（PUBLIC：SDK 头互相引用，工程侧也要包含 SDK 头）----
file(GLOB MYSTM32_INCS LIST_DIRECTORIES true
    ${CMAKE_CURRENT_SOURCE_DIR}/chip/*/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/devices/*/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/middleware/*/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/*/Inc
)
target_include_directories(mystm32 PUBLIC ${MYSTM32_INCS})

# ---- 宿主依赖注入 ----
if(TARGET stm32cubemx)
    target_link_libraries(mystm32 PUBLIC stm32cubemx)
else()
    message(WARNING
        "mystm32: 未找到宿主目标 stm32cubemx，"
        "请自行为 mystm32 注入 HAL 头文件路径与宏定义（STM32Fxxx/USE_HAL_DRIVER）")
endif()
