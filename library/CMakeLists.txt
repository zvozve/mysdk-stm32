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
# ---- HAL 弱符号强覆盖（*_hal.c）：抽成 OBJECT 库，强制进链接 ----
# ★★★ 2026-09-22 STM32F407/TP_MDC 实测踩坑，勿删、勿改回「与其余源同放静态库」 ★★★
#   病因：mysdk 是 STATIC 库，链接器只把「尚存在未定义符号引用」的成员从库里拉出来。
#        而 *_hal.c 提供的 HAL_UART_TxCpltCallback / HAL_UARTEx_RxEventCallback /
#        HAL_UART_ErrorCallback / HAL_GPIO_EXTI_Callback / HAL_I2S_TxCpltCallback
#        全是「对 HAL 弱符号的强覆盖」——HAL 驱动自带 __weak 空实现，
#        链接器认为符号已定义 → 这些 .o 永远不进镜像。全程零报错，只在运行时发作。
#   症状（chip.oop_uart）：
#        · uart_drv_send() 只有第一次「看起来成功」——DMA 是硬件自主搬运，首帧照样
#          上线（所以串口能看到第一行日志），但 TxCplt 回调走空实现 →
#          drv->state 永久停在 UART_DRV_TX_BUSY → 之后每次发送返回 -2 被丢弃；
#        · uart_drv_on_idle()（空闲中断收包）整体失效 → 收不到任何数据。
#   为什么不用 -Wl,-u：实测无效。命令行里的普通 .o（HAL）在库之前就被载入，
#        弱定义已把该符号「解析」掉，扫描到静态库时已无未定义引用 → 仍不拉取成员。
#   为什么不用 --whole-archive：把整个 SDK 无条件链进去，体积失控。
#   对策：把 *_hal.c 全部摘出来单独成 OBJECT 库并 PUBLIC 链接 —— OBJECT 库的对象
#        文件总是进入最终可执行文件，与「是否被引用」无关（CMake >= 3.12 支持
#        经静态库接口传递对象文件）。
file(GLOB_RECURSE MYSDK_HAL_GLUE CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*/*/Src/*_hal.c
)
if(MYSDK_HAL_GLUE)
    list(REMOVE_ITEM MYSDK_SOURCES ${MYSDK_HAL_GLUE})   # 避免同一份源被编译两次
endif()

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

# ---- HAL 胶水目标（承接上面摘出的 *_hal.c）----
if(MYSDK_HAL_GLUE)
    add_library(mysdk_hal_glue OBJECT ${MYSDK_HAL_GLUE})
    target_include_directories(mysdk_hal_glue PUBLIC ${MYSDK_INCS})
    if(TARGET stm32cubemx)
        target_link_libraries(mysdk_hal_glue PUBLIC stm32cubemx)
    endif()

    # ★ 关键：必须由**宿主可执行目标直接链接**该 OBJECT 库，对象文件才会出现在最终
    #   链接行上。以下三条路都实测过，勿回退：
    #     · 与其余源一起放静态库          → 库成员按需拉取，弱符号覆盖永远拉不出来（本 bug）；
    #     · target_link_libraries(mysdk PUBLIC ...)   → CMake 把 .obj 塞回 libmysdk.a，同样失效；
    #     · target_link_libraries(mysdk INTERFACE ...) → 对象既不进 .a 也不进可执行文件，失效。
    #   此处 add_executable(<CMAKE_PROJECT_NAME>) 在本文件之前就已执行（工程根 CMakeLists），
    #   可直接挂；cmake_minimum_required >= 3.13 时 CMP0079 为 NEW，跨目录链接合法。
    #   ⚠ 这里必须用 **plain 形式**（不带 PRIVATE/PUBLIC/INTERFACE）：宿主主目标的
    #     target_link_libraries 用法由 cmake/stm32cubemx/CMakeLists.txt 定为 plain，
    #     CMake 要求同一目标全程不得混用两种写法（实测报 "All uses ... must be
    #     either all-keyword or all-plain"），带关键字会直接配置失败。
    #   ⚠ 多槽 OTA（services/ota_core）：A/B 目标由基目标克隆属性得到，克隆会带上本项；
    #     若将来改成各自独立建目标，需在那里同样挂一次，否则功能在 A/B 槽里静默失效。
    if(TARGET ${CMAKE_PROJECT_NAME})
        target_link_libraries(${CMAKE_PROJECT_NAME} mysdk_hal_glue)
        message(STATUS "mysdk: HAL glue objects -> linked into ${CMAKE_PROJECT_NAME}")
    else()
        message(WARNING
            "mysdk: 宿主没有可执行目标 ${CMAKE_PROJECT_NAME} —— HAL 胶水 (*_hal.c) 不会被链接，"
            "HAL 弱符号回调（UART TxCplt/RxEvent/Error、GPIO EXTI、I2S TxCplt）将静默走 HAL 空实现。")
    endif()
endif()

# 工程侧通常在 add_subdirectory(MySDK) 之后才给 mysdk 注入 include 路径
# （User/board_cfg.h、User/Inc）；胶水是独立 target，不会自动继承那些路径，
# 故在工程根作用域收尾时把 mysdk 的接口 include 路径再同步给它一次。
macro(mysdk_sync_glue_includes)
    if(TARGET mysdk_hal_glue)
        set_property(TARGET mysdk_hal_glue APPEND PROPERTY INCLUDE_DIRECTORIES
            "$<TARGET_PROPERTY:mysdk,INTERFACE_INCLUDE_DIRECTORIES>")
    endif()
endmacro()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL mysdk_sync_glue_includes)

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
