#include "usb_host_port.h"
#include "SEGGER_RTT_Log.h"
#include "oop_dwt.h"          /* oop_GetTickMS()：替代 HAL_GetTick() */

// ===========================
// 板级绑定（替代原 extern hUsbHostFS：SDK 不引用 CubeMX 全局句柄）
// ===========================
static USBH_HandleTypeDef *s_phost = NULL;

void usb_host_port_bind(USBH_HandleTypeDef *phost)
{
    s_phost = phost;
}

/* ===========================
 * 应用事件入口（替代原 extern ApplicationTypeDef Appli_state）
 *
 * 工程在 USBH_UserProcess() 的 USER CODE 区内把 CubeMX 的
 * APPLICATION_START / READY / DISCONNECT 翻译成本枚举上报。
 * 语义与原先直接读 Appli_state 一致：只保留最新值。
 * =========================== */
static volatile usb_host_event_t s_app_state = USB_HOST_EVT_START;

void usb_host_port_event(usb_host_event_t evt)
{
    s_app_state = evt;
}

// 鼠标原始报告数据（定义在 usbh_hid_mouse.c 中，用于读取滚轮字节）
extern uint8_t mouse_report_data[];

// ===========================
// 环形缓冲区（键盘/扫码枪字符）
// ===========================
static uint8_t s_rx_buf[USB_HOST_RX_BUF_SIZE];
static volatile uint16_t s_rx_head = 0;
static volatile uint16_t s_rx_tail = 0;

// ===========================
// 内部状态
// ===========================
static usb_host_dev_state_t s_dev_state = USB_HOST_DEV_IDLE;
static HID_TypeTypeDef s_dev_type = HID_UNKNOWN;
static uint8_t s_keybd_inited = 0;
static uint8_t s_mouse_inited = 0;

// 键盘去重（HID 按住不放会持续上报同一个 key）
static uint8_t s_last_key = 0;

// 鼠标状态跟踪
static uint8_t  s_last_buttons = 0;   // 上次按键状态 (bit0=左, bit1=右, bit2=中)
static int16_t  s_mouse_acc_x  = 0;   // 累计 X 位移
static int16_t  s_mouse_acc_y  = 0;   // 累计 Y 位移
static int16_t  s_mouse_acc_wheel = 0; // 累计滚轮

// 诊断: 跟踪 USB Host 状态机变化
static HOST_StateTypeDef s_last_gState = HOST_IDLE;
static ENUM_StateTypeDef s_last_enumState = ENUM_IDLE;
static uint8_t s_vid_logged = 0;

// 看门狗: 检测枚举卡死
static uint32_t s_state_change_tick = 0;      // 上次 gState 变化的时刻
static uint32_t s_reenum_cooldown_tick = 0;    // 上次触发 ReEnumerate 的时刻
#define USB_HOST_ENUM_TIMEOUT_MS       15000   // 过渡状态超时阈值
#define USB_HOST_IDLE_CONNECTED_TIMEOUT_MS 5000 // IDLE+已连接 超时阈值
#define USB_HOST_REENUM_COOLDOWN_MS    5000    // ReEnumerate 冷却时间

// ===========================
// 环形缓冲区操作
// ===========================

static void rx_buf_push(uint8_t ch)
{
    uint16_t next = (uint16_t)((s_rx_head + 1) % USB_HOST_RX_BUF_SIZE);
    if (next != s_rx_tail) {
        s_rx_buf[s_rx_head] = ch;
        s_rx_head = next;
    } else {
        // 缓冲区满，丢弃
        WARN_LOG("USB RX buf full!\n");
    }
}

uint8_t usb_host_port_getchar(void)
{
    if (s_rx_head == s_rx_tail) {
        return 0;   // 无数据
    }
    uint8_t ch = s_rx_buf[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1) % USB_HOST_RX_BUF_SIZE);
    return ch;
}

uint8_t usb_host_port_has_data(void)
{
    return (s_rx_head != s_rx_tail) ? 1 : 0;
}

// ===========================
// 状态接口
// ===========================

usb_host_dev_state_t usb_host_port_get_state(void)
{
    return s_dev_state;
}

HID_TypeTypeDef usb_host_port_get_dev_type(void)
{
    return s_dev_type;
}

// ===========================
// 诊断: USB Host 状态名查找
// ===========================

static const char *gState_name(HOST_StateTypeDef s)
{
    switch (s) {
    case HOST_IDLE:                 return "IDLE";
    case HOST_DEV_WAIT_FOR_ATTACHMENT: return "WAIT_ATTACH";
    case HOST_DEV_ATTACHED:         return "DEV_ATTACHED";
    case HOST_DEV_DISCONNECTED:     return "DEV_DISCONNECTED";
    case HOST_DETECT_DEVICE_SPEED:  return "DETECT_SPEED";
    case HOST_ENUMERATION:          return "ENUMERATION";
    case HOST_CLASS_REQUEST:        return "CLASS_REQUEST";
    case HOST_INPUT:                return "INPUT";
    case HOST_SET_CONFIGURATION:    return "SET_CONFIG";
    case HOST_SET_WAKEUP_FEATURE:   return "SET_WAKEUP";
    case HOST_CHECK_CLASS:          return "CHECK_CLASS";
    case HOST_CLASS:                return "CLASS";
    case HOST_SUSPENDED:            return "SUSPENDED";
    case HOST_ABORT_STATE:          return "ABORT";
    default:                        return "?";
    }
}

static const char *enumState_name(ENUM_StateTypeDef s)
{
    switch (s) {
    case ENUM_IDLE:                  return "IDLE";
    case ENUM_GET_FULL_DEV_DESC:     return "GET_DEV_DESC";
    case ENUM_SET_ADDR:              return "SET_ADDR";
    case ENUM_GET_CFG_DESC:          return "GET_CFG_DESC";
    case ENUM_GET_FULL_CFG_DESC:     return "GET_FULL_CFG_DESC";
    case ENUM_GET_MFC_STRING_DESC:   return "GET_MFC_STR";
    case ENUM_GET_PRODUCT_STRING_DESC: return "GET_PROD_STR";
    case ENUM_GET_SERIALNUM_STRING_DESC: return "GET_SERIAL_STR";
    default:                         return "?";
    }
}

static void log_usbh_state(void)
{
    HOST_StateTypeDef gs = s_phost->gState;

    if (gs != s_last_gState) {
        DBG_LOG("USBH gState: %s -> %s\n",
                gState_name(s_last_gState), gState_name(gs));
        s_last_gState = gs;
        s_state_change_tick = oop_GetTickMS();
    }

    // 枚举子状态
    if (gs == HOST_ENUMERATION) {
        ENUM_StateTypeDef es = s_phost->EnumState;
        if (es != s_last_enumState) {
            DBG_LOG("USBH Enum: %s -> %s\n",
                    enumState_name(s_last_enumState), enumState_name(es));
            s_last_enumState = es;
        }
    }

    // VID/PID 日志（设备描述符获取后）
    if (!s_vid_logged && s_phost->device.DevDesc.idVendor != 0) {
        DBG_LOG("USBH VID=%04X PID=%04X Class=%02X Sub=%02X Proto=%02X\n",
                s_phost->device.DevDesc.idVendor,
                s_phost->device.DevDesc.idProduct,
                s_phost->device.DevDesc.bDeviceClass,
                s_phost->device.DevDesc.bDeviceSubClass,
                s_phost->device.DevDesc.bDeviceProtocol);
        s_vid_logged = 1;
    }
}

// ===========================
// 看门狗: 检测 USB Host 状态机卡死并强制恢复
// ===========================

static uint8_t is_stable_state(HOST_StateTypeDef gs)
{
    // 这些状态是可长期停留的正常状态
    return (gs == HOST_IDLE ||
            gs == HOST_CLASS ||
            gs == HOST_DEV_DISCONNECTED ||
            gs == HOST_SUSPENDED ||
            gs == HOST_ABORT_STATE);
}

static void usb_host_watchdog(void)
{
    HOST_StateTypeDef gs = s_phost->gState;
    uint32_t now = oop_GetTickMS();

    // 冷却期内不重复触发
    if (s_reenum_cooldown_tick != 0 &&
        (now - s_reenum_cooldown_tick) < USB_HOST_REENUM_COOLDOWN_MS) {
        return;
    }

    // 1. 过渡状态卡死检测 (ENUMERATION / DEV_ATTACHED / CLASS_REQUEST 等)
    if (!is_stable_state(gs) && s_state_change_tick != 0) {
        uint32_t elapsed = now - s_state_change_tick;
        if (elapsed > USB_HOST_ENUM_TIMEOUT_MS) {
            WARN_LOG("USB Host stuck in [%s] for %ums, forcing re-enumeration\n",
                     gState_name(gs), elapsed);
            USBH_ReEnumerate(s_phost);
            s_reenum_cooldown_tick = now;
            return;
        }
    }

    // 2. IDLE + 设备已连接 但无进展 (CTRL_ERROR 恢复失败)
    if (gs == HOST_IDLE && s_phost->device.is_connected) {
        uint32_t elapsed = now - s_state_change_tick;
        if (elapsed > USB_HOST_IDLE_CONNECTED_TIMEOUT_MS) {
            WARN_LOG("USB Host stuck in IDLE with device connected, forcing re-enumeration\n");
            USBH_ReEnumerate(s_phost);
            s_reenum_cooldown_tick = now;
            return;
        }
    }
}

// ===========================
// 鼠标数据处理
// ===========================

static void process_mouse(void)
{
    HID_MOUSE_Info_TypeDef *mouse = USBH_HID_GetMouseInfo(s_phost);
    if (mouse == NULL) {
        return;  // 无新数据
    }

    int16_t dx = 0;
    int16_t dy = 0;
    int8_t wheel = 0;
    uint8_t cur_buttons = 0;

    // 报告长度：区分标准 Boot Mouse(4B) 与 12-bit 高精度鼠标(5B)
    // 12-bit 布局: [buttons+pad][X:12b LE][Y:12b][Wheel]，ST 库 8-bit 解析会错位
    HID_HandleTypeDef *HID_Handle = (HID_HandleTypeDef *)s_phost->pActiveClass->pData;
    uint16_t rpt_len = (HID_Handle != NULL) ? HID_Handle->length : 0U;

    if (rpt_len >= 5U) {
        uint16_t raw_x = (uint16_t)(mouse_report_data[1] | ((uint16_t)(mouse_report_data[2] & 0x0FU) << 8));
        uint16_t raw_y = (uint16_t)(((uint16_t)(mouse_report_data[2] >> 4)) | ((uint16_t)mouse_report_data[3] << 4));
        dx = (int16_t)((raw_x << 4) >> 4);   // 12bit 有符号 -> int16
        dy = (int16_t)((raw_y << 4) >> 4);
        wheel = (int8_t)mouse_report_data[4];
        cur_buttons = mouse_report_data[0] & 0x07U;
    } else {
        // 标准 Boot Mouse: [buttons, X, Y, Wheel]
        dx = (int8_t)mouse->x;
        dy = (int8_t)mouse->y;
        wheel = (int8_t)mouse_report_data[3];
        cur_buttons = 0;
        if (mouse->buttons[0]) cur_buttons |= 0x01;  // 左键
        if (mouse->buttons[1]) cur_buttons |= 0x02;  // 右键
        if (mouse->buttons[2]) cur_buttons |= 0x04;  // 中键
    }

    // --- 按键状态 (检测按下/释放边沿) ---
    uint8_t changed = cur_buttons ^ s_last_buttons;
    if (changed & 0x01) {
        USB_LOG("[MOUSE] Left  %s\n", (cur_buttons & 0x01) ? "DOWN" : "UP");
    }
    if (changed & 0x02) {
        USB_LOG("[MOUSE] Right %s\n", (cur_buttons & 0x02) ? "DOWN" : "UP");
    }
    if (changed & 0x04) {
        USB_LOG("[MOUSE] Mid   %s\n", (cur_buttons & 0x04) ? "DOWN" : "UP");
    }
    s_last_buttons = cur_buttons;

    // --- 坐标移动 ---
    if (dx != 0 || dy != 0) {
        s_mouse_acc_x += dx;
        s_mouse_acc_y += dy;
        DBG_LOG("[MOUSE] Move dx=%+d dy=%+d  (total x=%d y=%d)\n",
                dx, dy, s_mouse_acc_x, s_mouse_acc_y);
    }

    // --- 滚轮 ---
    if (wheel != 0) {
        s_mouse_acc_wheel += wheel;
        USB_LOG("[MOUSE] Wheel %+d  (total %d)\n", wheel, s_mouse_acc_wheel);
    }
}

// ===========================
// 初始化
// ===========================

void usb_host_port_init(void)
{
    if (s_phost == NULL) {
        WARN_LOG("USB Host port: not bound, call usb_host_port_bind() first\n");
        return;
    }
    s_rx_head = 0;
    s_rx_tail = 0;
    s_dev_state = USB_HOST_DEV_IDLE;
    s_dev_type = HID_UNKNOWN;
    s_keybd_inited = 0;
    s_mouse_inited = 0;
    s_last_key = 0;
    s_last_buttons = 0;
    s_mouse_acc_x = 0;
    s_mouse_acc_y = 0;
    s_mouse_acc_wheel = 0;
    s_last_gState = HOST_IDLE;
    s_last_enumState = ENUM_IDLE;
    s_vid_logged = 0;
    s_state_change_tick = oop_GetTickMS();
    s_reenum_cooldown_tick = 0;
    SYS_LOG("USB Host port init done\n");
}

// ===========================
// 轮询处理（由任务调用）
// ===========================

void usb_host_port_process(void)
{
    if (s_phost == NULL) return;
    // 诊断: 跟踪 USB Host 状态机变化
    log_usbh_state();

    // 看门狗: 检测卡死并强制恢复
    usb_host_watchdog();

    // 根据 CubeMX 回调设置的 Appli_state 更新内部状态
    switch (s_app_state) {
    case USB_HOST_EVT_START:
        if (s_dev_state != USB_HOST_DEV_CONNECTED) {
            s_dev_state = USB_HOST_DEV_CONNECTED;
            s_keybd_inited = 0;
            s_mouse_inited = 0;
            s_last_key = 0;
            s_last_buttons = 0;
            s_mouse_acc_x = 0;
            s_mouse_acc_y = 0;
            s_mouse_acc_wheel = 0;
            USB_LOG("USB Device Connected\n");
        }
        break;

    case USB_HOST_EVT_READY:
        if (s_dev_state != USB_HOST_DEV_READY) {
            s_dev_state = USB_HOST_DEV_READY;
            // 设备就绪，获取类型并初始化对应 HID 子类
            s_dev_type = USBH_HID_GetDeviceType(s_phost);
            USB_LOG("USB Device Ready, type: %s\n",
                    (s_dev_type == HID_KEYBOARD) ? "Keyboard" :
                    (s_dev_type == HID_MOUSE)    ? "Mouse" : "Unknown");
            if (s_dev_type == HID_KEYBOARD) {
                USBH_HID_KeybdInit(s_phost);
                s_keybd_inited = 1;
                USB_LOG("HID Keyboard initialized\n");
            } else if (s_dev_type == HID_MOUSE) {
                USBH_HID_MouseInit(s_phost);
                s_mouse_inited = 1;
                s_last_buttons = 0;
                s_mouse_acc_x = 0;
                s_mouse_acc_y = 0;
                s_mouse_acc_wheel = 0;
                USB_LOG("HID Mouse initialized\n");
            }
        }
        break;

    case USB_HOST_EVT_DISCONNECT:
        if (s_dev_state != USB_HOST_DEV_DISCONNECTED) {
            s_dev_state = USB_HOST_DEV_DISCONNECTED;
            s_keybd_inited = 0;
            s_mouse_inited = 0;
            s_dev_type = HID_UNKNOWN;
            s_last_key = 0;
            s_last_buttons = 0;
            s_vid_logged = 0;
            // 设备断开时清空环形缓冲区，防止残留字符混入下次扫码
            s_rx_head = 0;
            s_rx_tail = 0;
            WARN_LOG("USB Device Disconnected\n");
        }
        break;

    default:
        break;
    }

    // 键盘/扫码枪: 轮询按键
    if (s_dev_state == USB_HOST_DEV_READY && s_keybd_inited) {
        HID_KEYBD_Info_TypeDef *kbd_info = USBH_HID_GetKeybdInfo(s_phost);
        if (kbd_info != NULL) {
            uint8_t ascii = USBH_HID_GetASCIICode(kbd_info);
            if (ascii != 0 && ascii != s_last_key) {
                // 新按键，存入缓冲区
                rx_buf_push(ascii);
                DBG_LOG("USB Key: 0x%02X '%c'\n", ascii,
                        (ascii >= 0x20 && ascii < 0x7F) ? ascii : '.');
            }
            s_last_key = ascii;
        } else {
            // 无按键按下时清除上次按键记录
            s_last_key = 0;
        }
    }

    // 鼠标: 轮询按键/坐标/滚轮
    if (s_dev_state == USB_HOST_DEV_READY && s_mouse_inited) {
        process_mouse();
    }
}
