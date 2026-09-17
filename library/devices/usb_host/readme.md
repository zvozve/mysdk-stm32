# 功能说明

此驱动基于 CubeMX 初始化 USB-HOST，可以支持键盘、鼠标、扫码枪。

分层说明：

| 文件 | 层 | 职责 |
| :--- | :--- | :--- |
| `library/middleware/usbh/` | middleware | ST USB Host Library（厂商库，随 SDK 提供，不归本项目维护） |
| `library/devices/usb_host/` | devices | 应用适配层：HID 设备识别、按键环形缓冲、鼠标原始报文解析、枚举卡死看门狗 |

板级绑定与事件上报（工程侧，驱动不再直接引用 CubeMX 全局）：

```c
usb_host_port_bind(&hUsbHostFS);          /* 注入 USBH 句柄，须在 usb_host_port_init() 之前 */
usb_host_port_event(USB_HOST_EVT_READY);  /* 在 USBH_UserProcess 内翻译 CubeMX 应用状态上报 */
```

> **为什么不再直接读 `Appli_state`：** SDK devices 层禁止 include CubeMX 生成的工程头
> （`main.h` / `usb_host.h`）。因此把「CubeMX 应用状态 → SDK 事件」的翻译放回工程侧
> 的 `USBH_UserProcess()` USER CODE 区，SDK 只依赖 middleware/usbh 的公开 API。

# CubeMX修改内容

|                             |                                  |
| --------------------------- | -------------------------------- |
| Class for FS IP             | Human Interface Host Class (HID) |
| Drive_ VBUS FS              | GPIO:Output PA9[USB_PWR]         |
| USBH_PROCESS_STACK_SIZE     | 4096                             |
| USBH_MAX_NUM_ENDPOINTS      | 4                                |
| USBH_MAX_NUM_INTERFACES     | 4                                |
| USBH_MAX_SIZE_CONFIGURATION | 512                              |
| USBH_MAX_DATA_BUFFER        | 1024                             |

# 生成后需手动修改内容

| 文件 | 修改内容 | CubeMX 重新生成后 |
| :--- | :--- | :--- |
| `usbh_conf.c` | `HAL_Delay(200)`→`osDelay(200)` | **需手动恢复**，在 `USBH_LL_DriverVBUS` 函数中 |
| `usbh_conf.c` | `USBH_LL_ClosePipe` 内改为调用 `HAL_HCD_HC_Halt` | **需手动恢复**，解决鼠标键盘拔掉后再次插入卡住 |
| `usbh_core.c` | 跳过 `SET_WAKEUP_FEATURE` | **不受 CubeMX 影响**（Middlewares 文件不重新生成）；但更新固件包会覆盖 → 见下方说明 |
| `USB_HOST/App/usb_host.c` | `USBH_UserProcess()` 的 `USER CODE BEGIN CALL_BACK_1` 内加入 `usb_host_port_event()` 上报 | **需手动恢复**（本案新增，替代原先直接读 `Appli_state`） |
| `USB_HOST/Target/usbh_platform.c` | `PREPARE_GPIO_DATA_VBUS_FS`（VBUS 使能钩子） | **需手动恢复** |

## 1. usbh_core.c —— 跳过 SET_WAKEUP_FEATURE

> 本修复随 `library/middleware/usbh/Src/usbh_core.c` 一起入库（ST 厂商库**不重新生成**）。

```c
case  HOST_SET_WAKEUP_FEATURE:

      /* ===================================================================
       * MODIFIED: Skip SET_FEATURE(REMOTE_WAKEUP).
       *
       * Root cause: Some USB devices (e.g. barcode scanners, VID=060E)
       * advertise Remote Wakeup (CfgDesc.bmAttributes bit5 = 1) but do NOT
       * respond to the SET_FEATURE control transfer. The control state
       * machine retries until errorcount exceeds USBH_MAX_ERROR_COUNT, then
       * the CTRL_ERROR handler (usbh_ctlreq.c) forces gState = HOST_IDLE
       * AND frees the control pipes -- effectively aborting the entire
       * enumeration. The device never reaches HOST_CHECK_CLASS / HOST_CLASS,
       * so the HID keyboard interface is never initialized.
       *
       * The keyboard works because its bmAttributes bit5 = 0 (no remote
       * wakeup), so it skips this state entirely.
       *
       * Remote Wakeup is an OPTIONAL USB feature, not required for
       * keyboard/scanner input. Skip it and proceed directly to class check.
       * =================================================================== */
      phost->gState = HOST_CHECK_CLASS;

#if (USBH_USE_OS == 1U)
      USBH_OS_PutMessage(phost, USBH_PORT_EVENT, 0U, 0U);
#endif /* (USBH_USE_OS == 1U) */
      break;
```

## 2. usbh_conf.c —— USBH_LL_ClosePipe

> 属 **CubeMX 生成文件**，不进 SDK，需在工程侧维护。

```c
USBH_StatusTypeDef USBH_LL_ClosePipe(USBH_HandleTypeDef *phost, uint8_t pipe)
{
  /* 关闭 HCD 物理通道，避免设备拔出后通道残留导致重插枚举卡死 */
  HAL_StatusTypeDef hal_status = HAL_OK;

  if (phost->pData != NULL)
  {
    hal_status = HAL_HCD_HC_Halt(phost->pData, pipe);
  }

  if (hal_status == HAL_OK)
  {
    return USBH_OK;
  }
  else
  {
    return USBH_FAIL;
  }
}
```

## 3. usbh_conf.c —— USBH_LL_DriverVBUS 时基

> 属 **CubeMX 生成文件**，不进 SDK，需在工程侧维护。

```c
USBH_StatusTypeDef USBH_LL_DriverVBUS(USBH_HandleTypeDef *phost, uint8_t state)
{
  if (phost->id == HOST_FS) {
    MX_DriverVbusFS(state);
  }

  /* USER CODE BEGIN 0 */

  /* USER CODE END 0*/

  osDelay(200);
  return USBH_OK;
}
```

# 驱动版本

---
## V2.0
	2026-09-17
	按 oop/SDK 分层迁移：应用适配层入 library/devices/usb_host，ST USB Host Library
	入 library/middleware/usbh；摘除对 CubeMX 全局（hUsbHostFS / Appli_state）与
	工程头（main.h / usb_host.h）的依赖，改为 usb_host_port_bind() 注入句柄 +
	usb_host_port_event() 上报应用事件；HAL_GetTick() 改走 oop_GetTickMS()；
	日志标签由工程 APP_LOG 改为模块自身 USB_LOG。

---
## V1.1
	2026年7月31日
	解决鼠标键盘拔掉后再次插入卡住

---
## V1.0
	2026年7月31日
