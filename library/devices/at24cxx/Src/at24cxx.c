#include "at24cxx.h"
#include "oop_i2c_drv.h"
#include "oop_dwt.h"
#include <string.h>
#include <stdio.h>          /* snprintf：落盘监控 hex */
#include "SEGGER_RTT_Log.h"
#include "SEGGER_RTT.h"


/**
 * @brief       缓存初始化
 * @param       无
 * @retval      无
 */

uint8_t EEPROM_ReadBuffer[EE_TYPE];
uint8_t EEPROM_WriteBuffer[EE_TYPE];
bool EEPROM_isFirstUse = true;
bool EEPROM_StatusOK   = false;

static soft_i2c_t g_at24cxx_i2c;

static void eep_DelayMS(uint32_t ms) 
{
    oop_DelayMS(ms);
}

/**
 * @brief       初始化IIC接口
 * @param       无
 * @retval      检测结果 0: 检测失败 1: 检测成功 
 */
bool at24cxx_init(GPIO_TypeDef* scl_port, uint16_t scl_pin,
                  GPIO_TypeDef* sda_port, uint16_t sda_pin,
                  uint32_t delay_us, bool erase_data) {
    oop_i2c_init(&g_at24cxx_i2c, scl_port, scl_pin, sda_port, sda_pin, delay_us);
    // at24cxx检查
    EEPROM_StatusOK = at24cxx_check();

    if (!EEPROM_StatusOK) {
        return false;
    }

    // 擦除EEPROM
    if (EEPROM_StatusOK && erase_data) {
        at24cxx_Erase();
    }

    return true;
}

/**
 * @brief       检查AT24CXX是否正常
 * @note        检测原理: 在器件的末地址写如0X55, 然后再读取, 如果读取值为0X55
 *              则表示检测正常. 否则,则表示检测失败.
 *
 * @param       无
 * @retval      检测结果 0: 检测失败 1: 检测成功
 */
bool at24cxx_check(void) {
    uint16_t addr = EE_TYPE;    
    uint8_t  crc  = EE_CRC;
    uint8_t  data = 0;

    data = at24cxx_read_one_byte(addr);     /* 避免每次开机都写AT24CXX */
    if (data == crc) {   /* 读取数据正常 */
        EEPROM_isFirstUse = false;
        return 1;
    } else {    /* 排除第一次初始化的情况 */
        at24cxx_write_one_byte(addr, crc); /* 先写入数据 */
        eep_DelayMS(10);
        data = at24cxx_read_one_byte(addr);  /* 再读取数据 */
        if (data == crc) {
            EEPROM_isFirstUse = true;
            return 1;
        } else {
            return 0;
        }
    }
}

/**
 * @brief       擦除 EEPROM
 */ 
void at24cxx_Erase(void){
    uint16_t addr  = 0;
    uint8_t  value = EE_INIT;

    // 一次写一个字节
    for (addr = 0; addr < EE_TYPE; addr ++)
    {
        at24cxx_write_one_byte(addr, value);
        eep_DelayMS(10);
        uint8_t dat = at24cxx_read_one_byte(addr);
        DBG_LOG("ERASE OK!  dat %d : %02X\n", addr, dat);        
        if (value != dat) {
            DBG_LOG("ERASE ERR! dat %d : %02X\n", addr, dat);    
            return;
        }
    }

    EEPROM_isFirstUse = true;
}

/**
 * @brief       在AT24CXX指定地址读出一个数据
 * @param       readaddr: 开始读数的地址
 * @retval      读到的数据
 */
uint8_t  at24cxx_read_one_byte(uint16_t addr) {
    uint8_t temp = 0;
    oop_i2c_start(&g_at24cxx_i2c);    /* 发送起始信号 */

    /* 根据不同的24CXX型号, 发送高位地址
     * 1, 24C16以上的型号, 分2个字节发送地址
     * 2, 24C16及以下的型号, 分1个低字节地址 + 占用器件地址的bit1~bit3位 用于表示高位地址, 最多11位地址
     *    对于24C01/02, 其器件地址格式(8bit)为: 1  0  1  0  A2  A1  A0  R/W
     *    对于24C04,    其器件地址格式(8bit)为: 1  0  1  0  A2  A1  a8  R/W
     *    对于24C08,    其器件地址格式(8bit)为: 1  0  1  0  A2  a9  a8  R/W
     *    对于24C16,    其器件地址格式(8bit)为: 1  0  1  0  a10 a9  a8  R/W
     *    R/W      : 读/写控制位 0,表示写; 1,表示读;
     *    A0/A1/A2 : 对应器件的1,2,3引脚(只有24C01/02/04/8有这些脚)
     *    a8/a9/a10: 对应存储整列的高位地址, 11bit地址最多可以表示2048个位置, 可以寻址24C16及以内的型号
     */    
    if (EE_TYPE > AT24C16)      /* 24C16以上的型号, 分2个字节发送地址 */
    {
        oop_i2c_send_byte(&g_at24cxx_i2c, 0xA0);    /* 发送写命令, IIC规定最低位是0, 表示写入 */
        oop_i2c_wait_ack(&g_at24cxx_i2c);         /* 每次发送完一个字节,都要等待ACK */
        oop_i2c_send_byte(&g_at24cxx_i2c, addr >> 8);   /* 发送高字节地址 */
    }
    else 
    {
        oop_i2c_send_byte(&g_at24cxx_i2c, 0xA0 + ((addr >> 8) << 1));   /* 发送器件 0xA0 + 高位a8/a9/a10地址,写数据 */
    }
    
    oop_i2c_wait_ack(&g_at24cxx_i2c);             /* 每次发送完一个字节,都要等待ACK */
    oop_i2c_send_byte(&g_at24cxx_i2c, addr % 256);  /* 发送低位地址 */
    oop_i2c_wait_ack(&g_at24cxx_i2c);             /* 等待ACK, 此时地址发送完成了 */
    
    oop_i2c_start(&g_at24cxx_i2c);                /* 重新发送起始信号 */ 
    oop_i2c_send_byte(&g_at24cxx_i2c, 0xA1);        /* 进入接收模式, IIC规定最低位是1, 表示读取 */
    oop_i2c_wait_ack(&g_at24cxx_i2c);             /* 每次发送完一个字节,都要等待ACK */
    temp = oop_i2c_read_byte(&g_at24cxx_i2c, 0);    /* 接收一个字节数据 */
    oop_i2c_stop(&g_at24cxx_i2c);                 /* 产生一个停止条件 */
//    DBG_LOG("addr %d : %02X\n", addr, temp);
    return temp;
}
/**
 * @brief       在AT24CXX里面的指定地址开始读出指定个数的数据
 * @param       addr    : 开始读出的地址 对24c02为0~255
 * @param       pbuf    : 数据数组首地址
 * @param       datalen : 要读出数据的个数
 * @retval      无
 */
void at24cxx_read_u8(uint16_t addr, uint8_t *pbuf, uint16_t datalen) {
    while (datalen--)
    {
        *pbuf++ = at24cxx_read_one_byte(addr++);
    }
}

// 读取u16（逻辑地址addr对应物理地址addr×2）
void at24cxx_read_u16(uint16_t addr, uint16_t *pbuf, uint16_t datalen) {
    uint8_t buf[datalen * 2];
    at24cxx_read_u8(addr * 2, buf, datalen * 2);
    
    for (uint16_t i = 0; i < datalen; i++) {
        // 小端重组（第一个字节是LSB）
        pbuf[i] = buf[i*2] | (buf[i*2+1] << 8);
    }
}

// 读取u32（逻辑地址addr对应物理地址addr×4）
void at24cxx_read_u32(uint16_t addr, uint32_t *pbuf, uint16_t datalen) {
    uint8_t buf[datalen * 4];
    at24cxx_read_u8(addr * 4, buf, datalen * 4);
    
    for (uint16_t i = 0; i < datalen; i++) {
        // 小端模式重组
        pbuf[i] = (uint32_t)buf[i*4]        | 
                 ((uint32_t)buf[i*4+1] << 8) |
                 ((uint32_t)buf[i*4+2] << 16)|
                 ((uint32_t)buf[i*4+3] << 24);
    }
}

/**
 * @brief       在AT24CXX指定地址写入一个数据
 * @param       addr: 写入数据的目的地址
 * @param       data: 要写入的数据
 * @retval      无
 */
void at24cxx_write_one_byte(uint16_t addr, uint8_t data) {
    /* 原理说明见:at24cxx_read_one_byte函数, 本函数完全类似 */
    oop_i2c_start(&g_at24cxx_i2c);                /* 发送起始信号 */

    if (EE_TYPE > AT24C16)      /* 24C16以上的型号, 分2个字节发送地址 */
    {
        oop_i2c_send_byte(&g_at24cxx_i2c, 0xA0);    /* 发送写命令, IIC规定最低位是0, 表示写入 */
        oop_i2c_wait_ack(&g_at24cxx_i2c);         /* 每次发送完一个字节,都要等待ACK */
        oop_i2c_send_byte(&g_at24cxx_i2c, addr >> 8);   /* 发送高字节地址 */
    }
    else
    {
        oop_i2c_send_byte(&g_at24cxx_i2c, 0xA0 + ((addr >> 8) << 1));   /* 发送器件 0xA0 + 高位a8/a9/a10地址,写数据 */
    }
    
    oop_i2c_wait_ack(&g_at24cxx_i2c);             /* 每次发送完一个字节,都要等待ACK */
    oop_i2c_send_byte(&g_at24cxx_i2c, addr % 256);  /* 发送低位地址 */
    oop_i2c_wait_ack(&g_at24cxx_i2c);             /* 等待ACK, 此时地址发送完成了 */
    
    /* 因为写数据的时候,不需要进入接收模式了,所以这里不用重新发送起始信号了 */
    oop_i2c_send_byte(&g_at24cxx_i2c, data);        /* 发送1字节 */
    oop_i2c_wait_ack(&g_at24cxx_i2c);             /* 等待ACK */
    oop_i2c_stop(&g_at24cxx_i2c);                 /* 产生一个停止条件 */
//    eep_DelayMS(10);              /* 注意: EEPROM 写入比较慢,必须等到10ms后再写下一个字节 */
}

/**
 * @brief 安全的页写入函数（自动处理跨页和长度限制）
 * @param addr 起始地址
 * @param data 数据指针
 * @param len 数据长度（函数内会自动分页）
 */
/**
 * @brief 优化的页写入函数（短数据无延时，自动处理跨页）
 * @param addr 起始地址
 * @param data 数据指针
 * @param len 数据长度
 */
void at24cxx_write_page(uint16_t addr, uint8_t *data, uint8_t len) {
    uint8_t page_size = (EE_TYPE <= AT24C16) ? 16 : 32;
    uint8_t is_first_page = 1;

    while (len > 0)
    {
        // 计算当前页可写入量
        uint8_t chunk_size = page_size - (addr % page_size);
        if (chunk_size > len) chunk_size = len;
        
        // I2C通信流程
        oop_i2c_start(&g_at24cxx_i2c);
        
        if (EE_TYPE > AT24C16) {
            oop_i2c_send_byte(&g_at24cxx_i2c, 0xA0);
            oop_i2c_wait_ack(&g_at24cxx_i2c);
            oop_i2c_send_byte(&g_at24cxx_i2c, addr >> 8);
        } else {
            oop_i2c_send_byte(&g_at24cxx_i2c, 0xA0 + ((addr >> 8) << 1));
        }
        oop_i2c_wait_ack(&g_at24cxx_i2c);
        
        oop_i2c_send_byte(&g_at24cxx_i2c, addr & 0xFF);
        oop_i2c_wait_ack(&g_at24cxx_i2c);
        
        for (uint8_t i = 0; i < chunk_size; i++) {
            oop_i2c_send_byte(&g_at24cxx_i2c, data[i]);
            oop_i2c_wait_ack(&g_at24cxx_i2c);
        }
        oop_i2c_stop(&g_at24cxx_i2c);
        
        // 仅多页写入或跨页时延时（关键优化）
        if (!is_first_page || (chunk_size < len)) {
            eep_DelayMS(10);
        }
        
        // 更新指针
        addr += chunk_size;
        data += chunk_size;
        len -= chunk_size;
        is_first_page = 0;
    }
}

/**
 * @brief       在AT24CXX里面的指定地址开始写入指定个数的数据
 * @param       addr    : 开始写入的地址 对24c02为0~255
 * @param       pbuf    : 数据数组首地址
 * @param       datalen : 要写入数据的个数
 * @retval      无
 */
void at24cxx_write_u8(uint16_t addr, uint8_t *pbuf, uint16_t datalen) {
    uint8_t cnt = datalen / 16 + 1;
    
    for(uint8_t i = 0; i < cnt; i ++){
        at24cxx_write_page(addr, pbuf, datalen);
    }
}

// 写入u16（逻辑地址addr对应物理地址addr×2）
void at24cxx_write_u16(uint16_t addr, uint16_t *pbuf, uint16_t datalen) {
    uint8_t buf[datalen * 2]; // 独立临时缓冲区
    
    for (uint16_t i = 0; i < datalen; i++) {
        // 明确小端存储（低字节在前）
        buf[i*2]   = pbuf[i] & 0xFF;       // 低字节
        buf[i*2+1] = (pbuf[i] >> 8) & 0xFF; // 高字节
    }
    at24cxx_write_page(addr * 2, buf, datalen * 2);
}

// 写入u32（逻辑地址addr对应物理地址addr×4）
void at24cxx_write_u32(uint16_t addr, uint32_t *pbuf, uint16_t datalen) {
    uint8_t buf[datalen * 4]; // 独立临时缓冲区
    
    for (uint16_t i = 0; i < datalen; i++) {
        // 默认小端模式（STM32）
        buf[i*4]   = pbuf[i] & 0xFF;         // 字节0 (LSB)
        buf[i*4+1] = (pbuf[i] >> 8) & 0xFF;   // 字节1
        buf[i*4+2] = (pbuf[i] >> 16) & 0xFF;  // 字节2
        buf[i*4+3] = (pbuf[i] >> 24) & 0xFF;  // 字节3 (MSB)
    }
    at24cxx_write_page(addr * 4, buf, datalen * 4);
}

/* ============================================================
 * 注册管理实现（原工程依赖，SDK 迁入时被裁。此处按需补齐：
 * 只保留工程实际使用的 register / read_by_ptr / write_by_ptr。）
 * ============================================================ */

typedef struct {
    void    *data_ptr;
    uint16_t eeprom_addr;
    uint16_t size;
} eeprom_reg_item_t;

static eeprom_reg_item_t g_reg_table[EEPROM_REG_MAX_ITEMS];
static uint16_t          g_reg_count     = 0;
static uint16_t          g_reg_next_addr = 0;   /* 下一可用地址（从 0 开始分配） */

bool eeprom_reg_register(void *ptr, uint16_t size) {
    if (ptr == NULL || size == 0) return false;
    if (g_reg_count >= EEPROM_REG_MAX_ITEMS) return false;
    if ((uint32_t)g_reg_next_addr + size > (uint32_t)EE_TYPE) return false;

    g_reg_table[g_reg_count].data_ptr    = ptr;
    g_reg_table[g_reg_count].eeprom_addr = g_reg_next_addr;
    g_reg_table[g_reg_count].size        = size;

    g_reg_next_addr += size;
    g_reg_count++;
    return true;
}

static eeprom_reg_item_t *eeprom_reg_find(void *ptr) {
    for (uint16_t i = 0; i < g_reg_count; i++) {
        if (g_reg_table[i].data_ptr == ptr) return &g_reg_table[i];
    }
    return NULL;
}

void eeprom_reg_read_by_ptr(void *ptr) {
    eeprom_reg_item_t *item = eeprom_reg_find(ptr);
    if (item == NULL) {
        SYS_LOG("[EEP] RD skip: ptr %p not registered", ptr);
        return;
    }
    uint8_t *dst = (uint8_t *)item->data_ptr;
    for (uint16_t i = 0; i < item->size; i++) {
        dst[i] = at24cxx_read_one_byte((uint16_t)(item->eeprom_addr + i));
    }
}

void eeprom_reg_write_by_ptr(void *ptr) {
    eeprom_reg_item_t *item = eeprom_reg_find(ptr);
    if (item == NULL) {
        SYS_LOG("[EEP] WR skip: ptr %p not registered", ptr);
        return;
    }
    uint8_t  *src    = (uint8_t *)item->data_ptr;
    uint16_t remain  = item->size;
    uint16_t off     = 0;

    /* write_page 的 len 形参是 uint8_t 且内部已处理跨页，这里按块切分即可 */
    while (remain > 0) {
        uint8_t chunk = (remain > 128u) ? 128u : (uint8_t)remain;
        at24cxx_write_page((uint16_t)(item->eeprom_addr + off), src + off, chunk);
        off    += chunk;
        remain -= chunk;
    }

    /* 落盘监控：确认真的写进 eeprom（排查"读完仍是默认值"） */
    char hex[128];
    int  hp = 0;
    for (uint16_t i = 0; i < item->size && hp < (int)sizeof(hex) - 4; i++) {
        hp += snprintf(hex + hp, (size_t)(sizeof(hex) - hp), " %02X", src[i]);
    }
    SYS_LOG("[EEP] WR @0x%04X len=%u :%s", item->eeprom_addr, item->size, hex);
}

/**
 * @brief   EEPROM 读写测试函数
 * @param   start_addr 起始地址
 * @param   len 测试数据长度（1-256）
 * @retval  0: 通过, -1: 失败
 */
int at24cxx_test(uint16_t start_addr, uint16_t len) {
    if (len == 0 || len > 256) return -1;
    
    uint8_t write_buf[256];
    uint8_t read_buf[256];
    uint16_t i;
    
    SYS_LOG("========== EEPROM Test (len=%d) ==========", len);
    
    // 1. 准备测试数据（0x00, 0x01, 0x02, ...）
    for (i = 0; i < len; i++) {
        write_buf[i] = (uint8_t)(i & 0xFF);
    }
    
    // 2. 写入
    SYS_LOG("Writing %d bytes to addr 0x%04X...", len, start_addr);
    at24cxx_write_u8(start_addr, write_buf, len);
    eep_DelayMS(100);
    
    // 3. 读取
    SYS_LOG("Reading %d bytes from addr 0x%04X...", len, start_addr);
    at24cxx_read_u8(start_addr, read_buf, len);
    
    // 4. 比较
    for (i = 0; i < len; i++) {
        if (write_buf[i] != read_buf[i]) {
            ERR_LOG("Mismatch at [%d]: expected 0x%02X, got 0x%02X", 
                    i, write_buf[i], read_buf[i]);
            return -1;
        }
    }
    
    // 5. 打印前 16 个字节
    SYS_LOG("First 16 bytes:");
    for (i = 0; i < (len > 16 ? 16 : len); i++) {
        SEGGER_RTT_printf(0, "%02X ", read_buf[i]);
    }
    SEGGER_RTT_WriteString(0, "\n");
    
    SYS_LOG("[OK] EEPROM test passed! %d bytes verified.", len);
    return 0;
}

/**
 * @brief   快速测试：写入和读取一个字符串
 */
void at24cxx_test_string(uint16_t addr) {
    char write_str[] = "Hello EEPROM! 1234567890";
    char read_str[64] = {0};
    uint16_t len = strlen(write_str);
    
    SYS_LOG("Writing string: \"%s\" (len=%d)", write_str, len);
    at24cxx_write_u8(addr, (uint8_t*)write_str, len);
    eep_DelayMS(100);
    
    at24cxx_read_u8(addr, (uint8_t*)read_str, len);
    read_str[len] = '\0';
    
    SYS_LOG("Read string: \"%s\"", read_str);
    
    if (strcmp(write_str, read_str) == 0) {
        SYS_LOG("[OK] String test passed");
    } else {
        ERR_LOG("[FAIL] String test failed");
    }
}

/**
 * @brief   运行所有测试
 */
void at24cxx_run_all_tests(void) {
    // 测试不同长度
    at24cxx_test(0, 1);      // 1 字节
    at24cxx_test(10, 8);     // 8 字节
    at24cxx_test(20, 16);    // 16 字节（跨页）
    at24cxx_test(40, 32);    // 32 字节
    at24cxx_test(80, 64);    // 64 字节
    
    // 字符串测试
    at24cxx_test_string(200);
    
    // 边界测试：从页边界开始写
    at24cxx_test(256 - 8, 16);  // 跨页边界
    
    SYS_LOG("========== All tests complete ==========");
}