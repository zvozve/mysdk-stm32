#include "ch374.h"
#include "ch374_port.h"      /* 板级适配：CS/INT/SPI/延时（EUSB_LOG 标签也在此定义） */
#include "SEGGER_RTT_Log.h"
#include "oop_dwt.h"         /* oop_DelayUS / oop_DelayMS / oop_InitDWT */

/* 原工程把 printf 重定向到调试串口；SDK 不依赖 libc printf 重定向，
   这里统一改投 SEGGER_RTT，等价输出、不引入 libc semihosting。 */
#define printf(...)   SEGGER_RTT_printf(0, __VA_ARGS__)

DEV_INFO HUB[3];				 /* Hub设备结构体数组 */
UDISK_State Udisk_Oper_State[3]; /* U盘操作状态机 */
USB_SETUP_REQ CtlTrans;			 /* 控制传输的8位请求码 */
BULK_ONLY_CMD mBOC;				 /* BulkOnly传输结构 */
UDISK_State Udisk_Oper_State[3]; /* U盘操作状态机 */
u16 Sec_Len[3];					 /* 保存扇区大小 */
u8 Buffer[512];					 /* 数据缓冲区 */
USB_SETUP_REQ CtlTrans;			 /* 控制传输的8位请求码 */
RECV_USB_DATA RecvData_t;		 /* 接收数据的结构体 */

/* 每端口键盘去重与就绪上报状态 */
static uint8_t s_last_key[3] = {0, 0, 0};
static uint8_t s_last_buttons[3] = {0, 0, 0};   /* 鼠标按键：bit0=左 bit1=右 bit2=中 */
static uint8_t s_dev_ready_reported[3] = {0, 0, 0};

/* 标准 Boot Keyboard 用法表（index = HID keyCode 0x04~0x38，越界返回 0） */
static const char key_map_normal[] = {
	/* 0x04-0x1D: a-z */
	'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
	'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
	/* 0x1E-0x27: 数字 */
	'1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
	/* 0x28-0x2C: Enter/Esc/Backspace/Tab/Space */
	'\n', 0x1B, '\b', '\t', ' ',
	/* 0x2D-0x38: 符号 */
	'-', '=', '[', ']', '\\', 0, ';', '\'', '`', ',', '.', '/',
};

static const char key_map_shift[] = {
	/* 0x04-0x1D: A-Z */
	'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
	'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
	/* 0x1E-0x27: 符号 */
	'!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
	/* 0x28-0x2C */
	'\n', 0x1B, '\b', '\t', ' ',
	/* 0x2D-0x38 */
	'_', '+', '{', '}', '|', 0, ':', '"', '~', '<', '>', '?',
};

/* 板级 GPIO 初始化（原 GPIO_Toggle_INIT：CS / INT / USB_PWR 引脚）
 * 已移入 ch374_port.c 的 CH374_Bind()，引脚由工程 board_cfg 注入。 */
u8 Query374Interrupt(void)
{
#ifdef CH374_INT_WIRE
	return (CH374_INT_WIRE ? 0 : 1); /* 如果连接了CH374的中断引脚则直接查询中断引脚 */
#else
	return (Read374Byte(REG_INTER_FLAG) & BIT_IF_INTER_FLAG ? TRUE : FALSE); /* 如果未连接CH374的中断引脚则查询中断标志寄存器 */
#endif
}

// 等待CH374中断(INT#低电平)，超时则返回ERR_USB_UNKNOWN
u8 Wait374Interrupt(void)
{
	u16 i;
	for (i = 0; i < 10000; i++)
	{ // 计数防止超时
		if (Query374Interrupt())
			return (0);
	}
	return (0xFA); // 不应该发生的情况
}

void Spi374Stop(void) /* SPI结束 */
{
	CH374T_CS_HIGH; // GPIOA_SetBits(CH374_SPI_SCS);/* SPI片选无效 */
}

void Spi374Start(u8 addr, u8 cmd) /* SPI开始 */
{
	CH374T_CS_LOW; // GPIOA_ResetBits(CH374_SPI_SCS);/* SPI片选有效 */
	CH374_SPI_ReadWriteByte(addr);
	oop_DelayUS(3);
	CH374_SPI_ReadWriteByte(cmd);
}

void Write374Byte(u8 mAddr, u8 mData) /* 外部定义的被CH374程序库调用的子程序,向指定寄存器写入数据 */
{
	Spi374Start(mAddr, CMD_SPI_374WRITE);
	CH374_SPI_ReadWriteByte(mData);
	Spi374Stop();
}

// 提供给库调用的函数
u8 Read374Byte(u8 mAddr) /* 外部定义的被CH374程序库调用的子程序,从指定寄存器读取数据 */
{
	u8 d;
	Spi374Start(mAddr, CMD_SPI_374READ);
	d = CH374_SPI_ReadWriteByte(0xff);
	Spi374Stop();
	return (d);
}

void Read374Block(u8 mAddr, u8 mLen, u8 *mBuf) /* 外部定义的被CH374程序库调用的子程序,从指定起始地址读出数据块 */
{
	Spi374Start(mAddr, CMD_SPI_374READ);
	while (mLen--)
		*mBuf++ = CH374_SPI_ReadWriteByte(0xff);
	Spi374Stop();
}

void Write374Block(u8 mAddr, u8 mLen, u8 *mBuf) /* 外部定义的被CH374程序库调用的子程序,向指定起始地址写入数据块 */
{
	Spi374Start(mAddr, CMD_SPI_374WRITE);
	while (mLen--)
		CH374_SPI_ReadWriteByte(*mBuf++);
	Spi374Stop();
}

void Modify374Byte(u8 mAddr, u8 mAndData, u8 mOrData) /* 修改指定寄存器的数据,先与再或 */
{
	u8 d;
	d = Read374Byte(mAddr);
	Write374Byte(mAddr, (d & mAndData) | mOrData);
}
#if 0
/* 主机模式初始化 */
void HostMode_Init(void)
{
	u8 j;
//	CH374_PORT_INIT( );
	Write374Byte( REG_USB_ADDR,  0xaa ) ;
	j=Read374Byte( REG_USB_ADDR );
	printf("addrrev:%02x\n",(u16)j);
	Write374Byte( REG_USB_ADDR,  0x55 ) ;
	j=Read374Byte( REG_USB_ADDR );
	printf("addrrev:%02x\n",(u16)j);
	Write374Byte( REG_SYS_CTRL, BIT_CTRL_OE_POLAR );                           //UEN低电平使能USB输出
	Write374Byte( REG_USB_SETUP, BIT_SETP_HOST_MODE | BIT_SETP_AUTO_SOF );     //主机模式，自动发SOF包
	Modify374Byte( REG_INTER_EN, 0xff , BIT_IE_TRANSFER );                     //传输中断(至于检测连接，用中断标志查询)
	Write374Byte( REG_INTER_FLAG, 0x1f );                                      //清所有中断标志
	Write374Byte( REG_USB_H_CTRL, 0 );                                         //USB主机控制寄存器初始化
	Write374Byte( REG_HUB_SETUP, 0 );                                          //开启HUB
	Write374Byte( REG_HUB_CTRL, 0 );                                           //清HUB状态
	ClearHub( 0 );                                                             //初始化HUB0结构体
	ClearHub( 1 );                                                             //初始化HUB1结构体
	ClearHub( 2 );                                                             //初始化HUB2结构体
}
/* 任何数据传输前都要选择Hub口 */
void SelectHub( u8 HubIndex )
{
	/* 设置总线速度 */
	if( HUB[HubIndex].DeviceSpeed == FullSpeed )
	{
		Modify374Byte( REG_USB_SETUP, ~BIT_SETP_LOW_SPEED, 0 );
	}
	else
	{
		Modify374Byte( REG_USB_SETUP, 0xff, BIT_SETP_LOW_SPEED );
	}
	/* 设置设备地址 */
	Write374Byte( REG_USB_ADDR, HUB[HubIndex].DeviceAddress );
}
/* 清Hub端口信息 */
void ClearHub( u8 HubIndex )
{
	switch( HubIndex )
	{
		case 0:
			Modify374Byte( REG_HUB_SETUP, ~(BIT_HUB0_EN | BIT_HUB0_POLAR), 0 );  //关闭HUB端口（拔插自动关闭），复位端口极性
			memset( &HUB[HubIndex], 0, sizeof(DEV_INFO) );	                     //清除Hub口设备信息
			HUB[HubIndex].Ep0MaxPacket = DefEp0MaxPacket;
			Udisk_Oper_State[HubIndex] = DiskInfo;
			Sec_Len[HubIndex] = 0x200;
		break;
		case 1:
			Modify374Byte( REG_HUB_CTRL, ~(BIT_HUB1_EN | BIT_HUB1_POLAR), 0 );  //关闭HUB端口（拔插自动关闭），复位端口极性
			memset( &HUB[HubIndex], 0, sizeof(DEV_INFO) );	                     //清除Hub口设备信息
			HUB[HubIndex].Ep0MaxPacket = DefEp0MaxPacket;
			Udisk_Oper_State[HubIndex] = DiskInfo;
			Sec_Len[HubIndex] = 0x200;
		break;
		case 2:
			Modify374Byte( REG_HUB_CTRL, ~(BIT_HUB2_EN | BIT_HUB2_POLAR), 0 );  //关闭HUB端口（拔插自动关闭），复位端口极性
			memset( &HUB[HubIndex], 0, sizeof(DEV_INFO) );	                     //清除Hub口设备信息
			HUB[HubIndex].Ep0MaxPacket = DefEp0MaxPacket;
			Udisk_Oper_State[HubIndex] = DiskInfo;
			Sec_Len[HubIndex] = 0x200;
		break;
		default:
		break;
	}
}
/* 检查相关Hub口是否连接,根据HUB状态进行相关配置 */
void CheckHubConnect( u8 HubIndex )
{
	u8 s;
	switch( HubIndex )
	{
		case 0:
			s = Read374Byte( REG_HUB_SETUP );
			if( s & BIT_HUB0_ATTACH )
			{
				if(HUB[HubIndex].DeviceStatus <= UnInit)
				{
					HUB[HubIndex].DeviceStatus = UnInit;
					s = Read374Byte( REG_INTER_FLAG );                          //获取设备极性
					if( s & BIT_HUB0_DX_IN)                                     //极性匹配
					{
						s = Read374Byte( REG_HUB_SETUP );                       //获取当前极性
						if( s & BIT_HUB0_POLAR )
							HUB[HubIndex].DeviceSpeed = LowSpeed;
						else
							HUB[HubIndex].DeviceSpeed = FullSpeed;
					}
					else                                                        //极性不匹配(需要切换极性)
					{
						s = Read374Byte( REG_HUB_SETUP );                       //获取当前极性
						if( s & BIT_HUB0_POLAR )
						{
							HUB[HubIndex].DeviceSpeed = FullSpeed;
							Modify374Byte( REG_HUB_SETUP, ~BIT_HUB0_POLAR , 0 );
						}
						else
						{
							HUB[HubIndex].DeviceSpeed = LowSpeed;
							Modify374Byte( REG_HUB_SETUP, 0xff , BIT_HUB0_POLAR );
						}
					}
				}
			}
			else
			{
				ClearHub( HubIndex );                                            //清HUB口状态信息
			}
		break;
		case 1:
			s = Read374Byte( REG_HUB_CTRL );
			if( s & BIT_HUB1_ATTACH )
			{
				if( HUB[HubIndex].DeviceStatus <= UnInit )
				{
					HUB[HubIndex].DeviceStatus = UnInit;
					s = Read374Byte( REG_HUB_SETUP );                          //获取设备极性
					if( s & BIT_HUB1_DX_IN)                                    //极性匹配
					{
						s = Read374Byte( REG_HUB_CTRL );                       //获取当前极性
						if( s & BIT_HUB1_POLAR )
							HUB[HubIndex].DeviceSpeed = LowSpeed;
						else
							HUB[HubIndex].DeviceSpeed = FullSpeed;
					}
					else                                                        //极性不匹配
					{
						s = Read374Byte( REG_HUB_CTRL );                        //获取当前极性
						if( s & BIT_HUB1_POLAR )
						{
							HUB[HubIndex].DeviceSpeed = FullSpeed;
							Modify374Byte( REG_HUB_CTRL, ~BIT_HUB1_POLAR , 0 );
						}
						else
						{
							HUB[HubIndex].DeviceSpeed = LowSpeed;
							Modify374Byte( REG_HUB_CTRL, 0xff , BIT_HUB1_POLAR );
						}
					}
				}
			}
			else
			{
				ClearHub( HubIndex );                                            //清HUB口状态信息
			}
		break;
		case 2:
			s = Read374Byte( REG_HUB_CTRL );
			if( s & BIT_HUB2_ATTACH )
			{
				if( HUB[HubIndex].DeviceStatus <= UnInit )
				{
					HUB[HubIndex].DeviceStatus = UnInit;
					s = Read374Byte( REG_HUB_SETUP );                          //获取设备极性
					if( s & BIT_HUB2_DX_IN)                                    //极性匹配
					{
						s = Read374Byte( REG_HUB_CTRL );                       //获取当前极性
						if( s & BIT_HUB2_POLAR )
							HUB[HubIndex].DeviceSpeed = LowSpeed;
						else
							HUB[HubIndex].DeviceSpeed = FullSpeed;
					}
					else                                                        //极性不匹配
					{
						s = Read374Byte( REG_HUB_CTRL );                        //获取当前极性
						if( s & BIT_HUB2_POLAR )
						{
							HUB[HubIndex].DeviceSpeed = FullSpeed;
							Modify374Byte( REG_HUB_CTRL, ~BIT_HUB2_POLAR , 0 );
						}
						else
						{
							HUB[HubIndex].DeviceSpeed = LowSpeed;
							Modify374Byte( REG_HUB_CTRL, 0xff , BIT_HUB2_POLAR );
						}
					}
				}
			}
			else
			{
				ClearHub( HubIndex );                                            //清HUB口状态信息
			}
		break;
		default:
		break;
	}
}
/* 查询连接状态 */                                                              //不用BIT_IF_DEV_DETECT，因为如果是插上上电则检测不到
void ScanConnect(void)
{
	u8 HubIndex;
    for( HubIndex = 0; HubIndex < 3; HubIndex++ )
	{
		CheckHubConnect( HubIndex );                                            //检查HUB连接情况
	}
}

/* 设备拔插检测 */
void Dev_Detect(void)
{
	u8 s;
	s = Read374Byte( REG_INTER_FLAG );
	if( s & BIT_IF_DEV_DETECT )
	{
		ScanConnect();
		Write374Byte(REG_INTER_FLAG, BIT_IF_DEV_DETECT );                      //清除标志位
	}
}

/* 复位HUB口 */
void ResetHub( u8 HubIndex )
{
	u8 i,s;
	switch( HubIndex )
	{
		case 0:
			Modify374Byte( REG_HUB_SETUP, 0xff, BIT_HUB0_RESET);
			delay_ms(15);
			Modify374Byte( REG_HUB_SETUP, ~BIT_HUB0_RESET, 0 );

			for( i = 0; i < 100; i++ )                                    /* 等待重连 */
			{
				s = Read374Byte( REG_HUB_SETUP );
				if( s & BIT_HUB0_ATTACH )
					break;
			}
		break;
		case 1:
			Modify374Byte( REG_HUB_CTRL, 0xff, BIT_HUB1_RESET);
			delay_ms(15);
			Modify374Byte( REG_HUB_CTRL, ~BIT_HUB1_RESET, 0 );

			for( i = 0; i < 100; i++ )                                    /* 等待重连 */
			{
				s = Read374Byte( REG_HUB_CTRL );
				if( s & BIT_HUB1_ATTACH )
					break;
			}
		break;
		case 2:
			Modify374Byte( REG_HUB_CTRL, 0xff, BIT_HUB2_RESET);
			delay_ms(15);
			Modify374Byte( REG_HUB_CTRL, ~BIT_HUB2_RESET, 0 );

			for( i = 0; i < 100; i++ )                                    /* 等待重连 */
			{
				s = Read374Byte( REG_HUB_CTRL );
				if( s & BIT_HUB2_ATTACH )
					break;
			}
		break;
		default:
		break;
	}
	Write374Byte(REG_INTER_FLAG, BIT_IF_DEV_DETECT );                    //清除重连标志位
}

/* HUB端口使能或禁止使能 */
void HubPort_Cmd( u8 HubIndex, u8 Status )                //Status 0：禁止  Status 1:使能
{
	switch( HubIndex )
	{
		case 0:
			if( Status )
				Modify374Byte( REG_HUB_SETUP, 0xff, BIT_HUB0_EN );
			else
				Modify374Byte( REG_HUB_SETUP, ~BIT_HUB0_EN, 0 );
		break;
		case 1:
			if( Status )
				Modify374Byte( REG_HUB_CTRL, 0xff, BIT_HUB1_EN );
			else
				Modify374Byte( REG_HUB_CTRL, ~BIT_HUB1_EN, 0 );
		break;
		case 2:
			if( Status )
				Modify374Byte( REG_HUB_CTRL, 0xff, BIT_HUB2_EN );
			else
				Modify374Byte( REG_HUB_CTRL, ~BIT_HUB2_EN, 0 );
		break;
		default:
		break;
	}
}
/* 枚举设备 */
u8 EnumHub( u8 HubIndex )
{
	u16 len;
	u8 s;
	u8 i;
/* 获取设备描述符,得到最大包长 */
	len = HUB[HubIndex].Ep0MaxPacket;                               //请求长度
	s = Get_DevDesc( HubIndex, &CtlTrans, Buffer, &len);
	if( s != Success ) return  s;
	len = Buffer[0];
	HUB[HubIndex].Ep0MaxPacket = Buffer[7];                         //EP0最大包长
/* HUB端口总线复位，并重新使能端口 */
	ResetHub( HubIndex );
	HubPort_Cmd( HubIndex, Enable );                                //开启端口使能(总线复位后HUB端口自动禁止，需重新使能)
/* 设置设备地址 */
	s = Set_DevAddr( HubIndex, &CtlTrans, HubIndex + 1 );           //设置地址，地址为端口号+1
	if( s != Success ) return  s;
/* 获取设备描述符 */
	s = Get_DevDesc( HubIndex, &CtlTrans, Buffer, &len);
	if( s != Success ) return  s;
	printf("HUB #%2x Device:\n",(u16)HubIndex );
	for(i=0;i<len;i++)
		printf("0x%02x ",(u16)Buffer[i]);
	printf("\n");
/* 获取配置描述符 */
	len = 4;
	s = Get_CfgDesc( HubIndex, &CtlTrans, Buffer, &len);
	if( s != Success ) return  s;
	len = (((u16)Buffer[3])<<8 ) + Buffer[2];
	s = Get_CfgDesc( HubIndex, &CtlTrans, Buffer, &len);
	if( s != Success ) return  s;
	printf("HUB #%2x Config:\n",(u16)HubIndex );
	for(i=0;i<len;i++)
		printf("0x%02x ",(u16)Buffer[i]);
	printf("\n");
/* 分析配置描述符 */
   s = Analy_CfgDesc( HubIndex, Buffer );
	if( s != Success )
	{
		printf("Invalid Configuration Descriptor\n");
		return  s;
	}
/* 设置配置 */
	s = Set_Config( HubIndex, &CtlTrans, HUB[HubIndex].ConfigurationValue );
	if( s != Success ) return  s;
/* 类命令处理 */
	s = Class_Issue( HubIndex );
	if( s != Success ) return  s;


	return Success;
}
/* 初始化该Hub端口设备 */
void Init_Hub( u8 HubIndex )
{
	ResetHub( HubIndex );                                           //总线复位，复位并不能改变端口极性位
	HubPort_Cmd( HubIndex, Enable );                                //开启端口使能
	delay_ms( 30 );                                                 //等待设备稳定后发包
	if( EnumHub( HubIndex ) == Success )                            //枚举
		HUB[HubIndex].DeviceStatus = InitComplete;                  //枚举成功后刷新状态
	else
		HUB[HubIndex].InitFailTimes++;                              //初始化失败累加
	if( HUB[HubIndex].InitFailTimes == 3 )                          //失败三次后
		ClearHub( HubIndex );
}

/* 根据拔插事件搜索需要初始化的Hub端口设备 */
void Init_USB_Device(void)
{
	u8 HubIndex;
	for( HubIndex = 0; HubIndex < 3; HubIndex++ )
	{
		if( HUB[HubIndex].DeviceStatus == UnInit )
		{
			printf("HUB %d Speed %d \n",(u16)HubIndex,(u16)HUB[HubIndex].DeviceSpeed );
			Init_Hub( HubIndex );
		}
	}
}

/* 分析Hub下设备信息，并操作 */
void Operate_Hub_Device(void)
{
	u8 HubIndex;
	for( HubIndex = 0; HubIndex < 3; HubIndex++ )
	{
		if( HUB[HubIndex].DeviceStatus == InitComplete )
		{
			Operate_Hub( HubIndex );
		}
	}

}

/* 清除端点特性 */
u8 Clear_Feature( u8 HubIndex, u8 Edp, PUSB_SETUP_REQ Setup_Req )
{
	u8 s;
	/* 设定控制请求 */
	Setup_Req->bType = 0x02;
	Setup_Req->bReq = DEF_USB_CLR_FEATURE;
	Setup_Req->wValueL = 0x00;
	Setup_Req->wValueH = 0x00;
	Setup_Req->wIndexL = Edp;
	Setup_Req->wIndexH = 0x00;
	Setup_Req->wLengthL = 0x00;
	Setup_Req->wLengthH = 0x00;
	s = USBHOST_Issue_Control( HubIndex, Setup_Req, NULL, NULL );        /* 执行控制传输 */
	return s;
}

/* 获取设备描述符 */
/* len表示输入输出参数长度 ,返回执行状态 */
u8 Get_DevDesc( u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 *DataBuffer, u16 *len )
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub( HubIndex );
	/* 设定控制请求 */
	Setup_Req->bType = 0x80;
	Setup_Req->bReq = DEF_USB_GET_DESCR;
	Setup_Req->wValueL = 0x00;
	Setup_Req->wValueH = USB_DEVICE_DESCR_TYPE;
	Setup_Req->wIndexL = 0x00;
	Setup_Req->wIndexH = 0x00;
	Setup_Req->wLengthL = (u8)(*len);
	Setup_Req->wLengthH = (u8)((*len)>>8);
	s = USBHOST_Issue_Control( HubIndex, Setup_Req, DataBuffer, len );        /* 执行控制传输 */
	return s;

}
/* 设置设备地址 */  /* 返回操作状态 */
u8 Set_DevAddr( u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 addr )
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub( HubIndex );
	/* 设定控制请求 */
	Setup_Req->bType = 0x00;
	Setup_Req->bReq = DEF_USB_SET_ADDRESS;
	Setup_Req->wValueL = addr;
	Setup_Req->wValueH = 0x00;
	Setup_Req->wIndexL = 0x00;
	Setup_Req->wIndexH = 0x00;
	Setup_Req->wLengthL = 0x00;
	Setup_Req->wLengthH = 0x00;
	s = USBHOST_Issue_Control( HubIndex, Setup_Req, NULL, NULL );        /* 执行控制传输 */
	if( s != Success ) return  s;
	HUB[HubIndex].DeviceAddress = addr;                                  /* 更新HUB口地址 */
	return  s;
}
/* 获取配置描述符 */
/* len表示输入输出参数长度 ,返回执行状态 */
u8 Get_CfgDesc( u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 *DataBuffer, u16 *len )
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub( HubIndex );
	/* 设定控制请求 */
	Setup_Req->bType = 0x80;
	Setup_Req->bReq = DEF_USB_GET_DESCR;
	Setup_Req->wValueL = 0x00;
	Setup_Req->wValueH = USB_CONFIG_DESCR_TYPE;
	Setup_Req->wIndexL = 0x00;
	Setup_Req->wIndexH = 0x00;
	Setup_Req->wLengthL = (u8)(*len);
	Setup_Req->wLengthH = (u8)((*len)>>8);
	s = USBHOST_Issue_Control( HubIndex, Setup_Req, DataBuffer, len );        /* 执行控制传输 */
	return s;

}
/* 分析配置描述符 */
u8 Analy_CfgDesc( u8 HubIndex, u8 *DataBuffer )
{
	u16 i;
	u8 itf_num = 0;                                                                  //表示当前分析的的接口
	u8 edp_num = 0;                                                                  //表示当前分析的的端点
	u16 length;
	u8 *pData;
	pData = DataBuffer;
	if( pData[1] == USB_CONFIG_DESCR_TYPE )                                              //先判断输入DataBuffer是否有效，并获取长度
	{
		length = pData[2] + ((u16)( pData[3] )<<8);
		for(i=0;i<length;i++)
		{
			if(pData[i+1]==USB_CONFIG_DESCR_TYPE)
			{
				HUB[HubIndex].ConfigurationValue = ((PUSB_CFG_DESCR)&pData[i])->bConfigurationValue;     //保存配置值
				HUB[HubIndex].NumInterfaces = ((PUSB_CFG_DESCR)&pData[i])->bNumInterfaces;	             //接口数目
				if((HUB[HubIndex].NumInterfaces==0) || (HUB[HubIndex].NumInterfaces>NUM_ITF))            //不支持的接口数目
					return Failure;
			}
			else if(pData[i+1]==USB_INTERF_DESCR_TYPE)
			{
				HUB[HubIndex].ITF[itf_num].ITFNum = ((PUSB_ITF_DESCR)&pData[i])->bInterfaceNumber;        //接口号
				HUB[HubIndex].ITF[itf_num].Class = ((PUSB_ITF_DESCR)&pData[i])->bInterfaceClass;          //设备类
				HUB[HubIndex].ITF[itf_num].SubClass = ((PUSB_ITF_DESCR)&pData[i])->bInterfaceSubClass;
				HUB[HubIndex].ITF[itf_num].Protocol = ((PUSB_ITF_DESCR)&pData[i])->bInterfaceProtocol;
				HUB[HubIndex].ITF[itf_num].NumEndpoints = ((PUSB_ITF_DESCR)&pData[i])->bNumEndpoints;     //该接口下的端点数
				if(HUB[HubIndex].ITF[itf_num].NumEndpoints>NUM_EDP)
					return Failure;
				edp_num = 0;                                                                              //当前接口分析端点清零
				itf_num++;                                                                                //接口号加一
			}
			else if(pData[i+1]==USB_HID_DESCR_TYPE)
			{
				HUB[HubIndex].ITF[itf_num-1].HID_Desc_Len = pData[i+7] + (((u16)pData[i+8])<<8);          //HID描述符长度
			}
			else if(pData[i+1]==USB_ENDP_DESCR_TYPE)
			{
				HUB[HubIndex].ITF[itf_num-1].Endp[edp_num].EDPNum = ((PUSB_ENDP_DESCR)&pData[i])->bEndpointAddress;  //端点地址
				HUB[HubIndex].ITF[itf_num-1].Endp[edp_num].Attributes = ((PUSB_ENDP_DESCR)&pData[i])->bmAttributes;  //端点类型
				HUB[HubIndex].ITF[itf_num-1].Endp[edp_num].MaxPacket = ((PUSB_ENDP_DESCR)&pData[i])->wMaxPacketSize; //端点大小
				edp_num++;
			}
			i += ( pData[i]-1);                                                                          //跳转到下一个长度
		}
		return Success;
	}
	else
		return Failure;
}
/* 设置配置 */
u8 Set_Config( u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 Cfg_Value )
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub( HubIndex );
	/* 设定控制请求 */
	Setup_Req->bType = 0x00;
	Setup_Req->bReq = DEF_USB_SET_CONFIG;
	Setup_Req->wValueL = Cfg_Value;
	Setup_Req->wValueH = 0x00;
	Setup_Req->wIndexL = 0x00;
	Setup_Req->wIndexH = 0x00;
	Setup_Req->wLengthL = 0x00;
	Setup_Req->wLengthH = 0x00;
	s = USBHOST_Issue_Control( HubIndex, Setup_Req, NULL, NULL );        /* 执行控制传输 */
	return  s;

}
/* HID类命令 Set_Idle */
u8 Set_Idle( u8 HubIndex, PUSB_SETUP_REQ Setup_Req ,u8 Itf_Num)
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub( HubIndex );
	/* 设定控制请求 */
	Setup_Req->bType = 0x21;
	Setup_Req->bReq = 0x0a;
	Setup_Req->wValueL = 0x00;
	Setup_Req->wValueH = 0x00;
	Setup_Req->wIndexL = Itf_Num;
	Setup_Req->wIndexH = 0x00;
	Setup_Req->wLengthL = 0x00;
	Setup_Req->wLengthH = 0x00;
	s = USBHOST_Issue_Control( HubIndex, Setup_Req, NULL, NULL );        /* 执行控制传输 */
	return  s;
}
/* HID类命令 Get_Report */
u8 Get_Report( u8 HubIndex, PUSB_SETUP_REQ Setup_Req ,u8 Itf_Num, u8 *DataBuffer, u16 *len )
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub( HubIndex );
	/* 设定控制请求 */
	Setup_Req->bType = 0x81;
	Setup_Req->bReq = DEF_USB_GET_DESCR;
	Setup_Req->wValueL = 0x00;
	Setup_Req->wValueH = 0x22;
	Setup_Req->wIndexL = Itf_Num;
	Setup_Req->wIndexH = 0x00;
	Setup_Req->wLengthL = (u8)(*len);
	Setup_Req->wLengthH = (u8)((*len)>>8);
	s = USBHOST_Issue_Control( HubIndex, Setup_Req, DataBuffer, len );        /* 执行控制传输 */
	return s;
}

/* HID类命令 Set_Report */
u8 Set_Report( u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 *DataBuffer, u16 *len )
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub( HubIndex );
	/* 设定控制请求 */
	Setup_Req->bType = 0x21;
	Setup_Req->bReq = 0x09;
	Setup_Req->wValueL = 0x00;
	Setup_Req->wValueH = 0x02;
	Setup_Req->wIndexL = 0x00;
	Setup_Req->wIndexH = 0x00;
	Setup_Req->wLengthL = 0x01;
	Setup_Req->wLengthH = 0x00;
	s = USBHOST_Issue_Control( HubIndex, Setup_Req, DataBuffer, len );        /* 执行控制传输 */
	return  s;

}
/* 事物传输 */ /* SET IN OUT */ /* Timeout=0不重试 Timeout=0xffff无限重试 ,返回PID*/
u8 Issue_Token( u8 PID, u8 Endp, u8 Tog, u16 Timeout )
{
	u8 s, resp, err_resp, err_num = 0;
	while(1)                                                        /* 虽是死循环，但终究有返回的 */
	{
		Write374Byte( REG_USB_H_PID, ( PID<<4 ) | Endp );
		Write374Byte( REG_USB_H_CTRL, Tog? BIT_HOST_TRAN_TOG|BIT_HOST_RECV_TOG|BIT_HOST_START:BIT_HOST_START ); /* 启动传输 */
		s = Wait374Interrupt( );
		if( s == Failure )                                           /* 设备超时无响应,最多重试三次*/
			err_num++;
		else err_num = 0;
		if( err_num == 3 )
			return s;

		Write374Byte( REG_INTER_FLAG, BIT_IF_TRANSFER | BIT_IF_USB_PAUSE ); /* 清中断 ,必须清除传输暂停位*/
		if( err_num == 0 )
		{
			s = Read374Byte( REG_USB_STATUS );
			resp = s & BIT_STAT_DEV_RESP;
			switch( PID )                                            /* 分析令牌PID */
			{
				case DEF_USB_PID_SETUP:
				case DEF_USB_PID_OUT:
					if( resp == DEF_USB_PID_ACK )                    /* 返回ACK */
						return resp;
					else if( resp == DEF_USB_PID_NAK )               /* 根据是否超时决定返回NAK */
					{
						if( Timeout == 0 )
							return resp;
						else
						{
							if( Timeout < 0xffff) Timeout--;
						}
					}
					else if( resp == DEF_USB_PID_STALL )             /* 返回STALL */
						return resp;
					else                                             /* 出错或超时等 */
					{
						err_resp++;
						if( err_resp == 3 )
							return resp;
					}
				break;

				case DEF_USB_PID_IN:
					if( resp == DEF_USB_PID_DATA0 || resp == DEF_USB_PID_DATA1 ) /* 说明收到了数据包，但不一定同步 */
					{
						if( s & BIT_STAT_TOG_MATCH )                 /* 翻转同步返回ACK */
							return DEF_USB_PID_ACK;
						else                                         /* 翻转不同步返回当前DATA值 */
							return resp;
					}
					else if( resp == DEF_USB_PID_NAK )               /* 根据是否超时决定返回NAK */
					{
						if( Timeout == 0 )
							return resp;
						else
						{
							if( Timeout < 0xffff) Timeout--;
						}
					}
					else if( resp == DEF_USB_PID_STALL )             /* 返回STALL */
						return resp;
					else                                             /* 出错或超时等 */
					{
						err_resp++;
						if( err_resp == 3 )
							return resp;
					}
				break;
				default:
					return Failure;                                  /* 非法PID */
				break;
			}
		}
	}
}
/* 执行控制传输 */
u8 USBHOST_Issue_Control( u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 *DataBuffer, u16 *len )
{
	u8 s;
	u8 tog = 0;
	u16 Req_len, length = 0;                                   /* Req_len表示请求长度，length为当前收到的数据包长度 */
	*len = 0;                                                     /* 代表收到的数据总长度 */
	Write374Block( RAM_HOST_TRAN, 8, (u8 *)Setup_Req );        /* 请求数据写入缓冲区 */
	Write374Byte( REG_USB_LENGTH, 8 );
	s = Issue_Token( DEF_USB_PID_SETUP, 0, tog, 0xffff);
	if( s == DEF_USB_PID_STALL )
		return Success;
	else if( s == DEF_USB_PID_ACK )
	{
		tog ^= 1;
		Req_len = (((u16)( Setup_Req->wLengthH ))<<8) + Setup_Req->wLengthL;
		if(( Setup_Req->bType ) & 0x80 )                                     /* 收 */
		{
			while( Req_len )
			{
				s = Issue_Token( DEF_USB_PID_IN, 0, tog, 0xffff );
				if( s == DEF_USB_PID_STALL )
					return Success;
				else if( s == DEF_USB_PID_ACK )                                  //同步
				{
					tog ^= 1;
					length = Read374Byte( REG_USB_LENGTH );
					Read374Block( RAM_HOST_RECV, length, DataBuffer );
					Req_len -= length;
					*len += length;
					DataBuffer += length;
					if( length < HUB[HubIndex].Ep0MaxPacket )                //收到一个短包，说明数据结束了
						break;
				}
				else if( s == DEF_USB_PID_DATA0 || s == DEF_USB_PID_DATA1 )  //不同步，丢包，不翻转
				{}
				else                                                         //出错
					return s;
			}
			tog = 1;
		}
		else                                                                 /* 发 */
		{
			while( Req_len )
			{
				if( Req_len > HUB[HubIndex].Ep0MaxPacket )
					length = HUB[HubIndex].Ep0MaxPacket;
				else length = Req_len;
				Write374Block( RAM_HOST_TRAN, length, DataBuffer );
				Write374Byte( REG_USB_LENGTH, length );
				s = Issue_Token( DEF_USB_PID_OUT, 0, tog, 0xffff );
				if( s == DEF_USB_PID_STALL )
					return Success;
				else if(s == DEF_USB_PID_ACK )
				{
					tog ^= 1;
					Req_len -= length;
					*len += length;
					DataBuffer += length;
				}
				else return s;

			}
			tog = 0;
		}

		Write374Byte( REG_USB_LENGTH, 0 );                      /* 状态阶段 */
		s = Issue_Token( tog? DEF_USB_PID_OUT : DEF_USB_PID_IN, 0, 1, 0xffff );
		if( s == DEF_USB_PID_STALL )
			return Success;
		else if( s == DEF_USB_PID_ACK )
		{
			if( tog == 0 )
			{
				if( Read374Byte( REG_USB_LENGTH ))              //接收出错，不是零包
					return Failure;
			}
		}
		else
			return s;
	}
	else
		return s;

	return Success;
}

/* 操作HUB设备 */
void Operate_Hub( u8 HubIndex )
{
	u8 i;
	u16 j;
	u8 num;
	u8 s;
	static u8 try_times = 0;                                             /* 失败重试次数 */
	u32 len;
	u8 *Edp[2];                                                          /* 保存批量端点信息首地址 */ /* Edp[0]保存下传、Edp[1]保存上传 */
	for(i=0;i<NUM_ITF;i++)                                                  //对同一设备的不同接口分别分析、操作
	{
		switch( HUB[HubIndex].ITF[i].Class )
		{
			case USB_DEV_CLASS_HUMAN_IF:                                    //HID接口
				num = Get_Interrupt_Edp( HubIndex, i, &Edp[0] );
				for(j=0;j<num;j++)
				{
					s = Get_HidData( HubIndex, &Edp[j], Buffer, &len );	   //获取HID数据
					if( s == Success )
					{
						for(j=0;j<len;j++)
							printf("0x%02x ", (u16)Buffer[j] );
						printf("\n");
					}
				}

			break;
			case USB_DEV_CLASS_STORAGE:                                     //大容量接口(分状态机操作，防止占时，保证HID正常传输)
				if((HUB[HubIndex].ITF[i].SubClass == 0x06) && (HUB[HubIndex].ITF[i].Protocol == 0x50))  //SCSI命令集、批量传输
				{
					s = Get_Bulk_Edp( HubIndex, i, &Edp[0] );
					if( s != 2 )  return;                                   //保证必须有批量上传和下传短点
					switch( Udisk_Oper_State[HubIndex] )                    //此时只区分HUB口，不区分接口了
					{
						case DiskInfo:
							len = 0x24;
							s = Get_DiskInfo( HubIndex, &Edp[0], &mBOC, Buffer, &len ); //获取磁盘信息
							if( s == Success )
							{
								if( mBOC.mCSW.mCSW_Status )
								{
									len = 0x12;
									Check_Erro( HubIndex, &Edp[0], &mBOC, Buffer, &len );
									try_times++;
									if( try_times > 4 )
									{
										try_times = 0;
										Udisk_Oper_State[HubIndex] = DiskCapacity;
									}
								}
								else
								{
									for(j=0;j<28;j++)
										printf("%c",(&(((P_INQUIRY_DATA)Buffer)->VendorIdStr))[j]);
									printf("\n");

									try_times = 0;
									Udisk_Oper_State[HubIndex] = DiskCapacity;
								}
							}
							Udisk_Oper_State[HubIndex] = DiskCapacity;
						break;
						case DiskCapacity:
							len = 0x08;
							s = Get_DiskCapacity( HubIndex, &Edp[0], &mBOC, Buffer, &len );
							if( s == Success )
							{
								if( mBOC.mCSW.mCSW_Status )
								{
									len = 0x12;
									Check_Erro( HubIndex, &Edp[0], &mBOC, Buffer, &len );
									try_times++;
									if( try_times > 4 )
									{
										try_times = 0;
										Udisk_Oper_State[HubIndex] = Undef;
									}
								}
								else
								{
#ifdef BIG_ENDIAN
									Sec_Len[HubIndex] = *(u32 *)(Buffer+4);
									printf("Capacity Num:0x%lx\n",*(u32 *)Buffer+1);
									printf("Per:0x%02x\n",Sec_Len[HubIndex] );
#else
									Sec_Len[HubIndex] = mSwapEndian(*(u32 *)(Buffer+4));
									printf("Capacity Num:0x%lx\n",mSwapEndian(*(u32 *)Buffer)+1);
									printf("Per:0x%02x\n", Sec_Len[HubIndex] );
#endif

									try_times = 0;
									Udisk_Oper_State[HubIndex] = ReadSec;
								}
							}
						break;
						case Write_Sec:
							s = Write_DiskSec( HubIndex, &Edp[0], &mBOC, Buffer, 1, 1 );
							if( s == Success )
							{
								Udisk_Oper_State[HubIndex] = ReadSec;
							}
							else
							{
								Udisk_Oper_State[HubIndex] = Undef;
							}
						break;
						case ReadSec:
							memset(Buffer,0,sizeof(Buffer));
							s = Read_DiskSec( HubIndex, &Edp[0], &mBOC, Buffer, 1, 1 );
							if( s == Success )
							{
								Udisk_Oper_State[HubIndex] = Undef;
							}
							else
							{
								Udisk_Oper_State[HubIndex] = Undef;
							}

						break;
						default:
							TestUnit( HubIndex, &Edp[0], &mBOC );
							Udisk_Oper_State[HubIndex] = Undef;
						break;

					}

				}
			break;
			case USB_DEV_CLASS_PRINTER:                                     //打印机接口

			break;
			default:
			break;
		}

	}
}

/* 分析接口信息，获取中断端点 *//* Edp[0]、Edp[1]保存上传结构体地址 返回中断端点个数 */
u8 Get_Interrupt_Edp( u8 HubIndex, u8 ITFNum, u8 **Edp )
{
	u8 i,s = 0;
	for( i = 0; i < HUB[HubIndex].ITF[ITFNum].NumEndpoints; i++ )
	{
		if( HUB[HubIndex].ITF[ITFNum].Endp[i].Attributes == USB_ENDP_TYPE_INTER )
		{
			if( HUB[HubIndex].ITF[ITFNum].Endp[i].EDPNum & 0x80 )              //上传端点
			{
				Edp[s] = (u8 *)&HUB[HubIndex].ITF[ITFNum].Endp[i].EDPNum;
				s++;
			}
		}
	}
	return s;
}
u8 Get_HidData( u8 HubIndex, u8 **Edp, u8 *DataBuffer, u32 *len )
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub( HubIndex );
	/* 发送IN包 */
	*len = 0;
	s = Issue_Token( DEF_USB_PID_IN, ((PEDP_INFO)*Edp)->EDPNum, ((PEDP_INFO)*Edp)->Tog, 0 );      //不重试
	if( s == DEF_USB_PID_ACK )
	{
		((PEDP_INFO)*Edp)->Tog ^= 1;
		*len = Read374Byte( REG_USB_LENGTH );
		Read374Block( RAM_HOST_RECV, *len, DataBuffer );
	}
	else
		return s;
	return Success;
}

/* 分析接口信息，获取批量端点 *//* Edp[0]保存下传、Edp[1]保存上传 返回端点个数 */
u8 Get_Bulk_Edp( u8 HubIndex, u8 ITFNum, u8 **Edp )
{
	u8 i,s = 0;
	for( i = 0; i < HUB[HubIndex].ITF[ITFNum].NumEndpoints; i++ )
	{
		if( HUB[HubIndex].ITF[ITFNum].Endp[i].Attributes == USB_ENDP_TYPE_BULK )
		{
			if( HUB[HubIndex].ITF[ITFNum].Endp[i].EDPNum & 0x80 )              //上传端点
			{
				s |= 0x01;
				Edp[1] = (u8 *)&HUB[HubIndex].ITF[ITFNum].Endp[i].EDPNum;
			}
			else                                                               //下传端点
			{
				s |= 0x02;
				Edp[0] = (u8 *)&HUB[HubIndex].ITF[ITFNum].Endp[i].EDPNum;
			}
		}
	}
	if( s == 3 )                                                               //必须有一个上传、一个下传
		return 2;
	else
		return 1;

}

/* 获取磁盘信息 */
u8 Get_DiskInfo( u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd, u8 *DataBuffer, u32 *len )
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub( HubIndex );
	/* 设定SCSI/UFI请求 */
	memset( Bulk_Only_Cmd, 0, sizeof(BULK_ONLY_CMD));                /* 清空结构体 */
	Bulk_Only_Cmd->mCBW.mCBW_DataLen = mSwapEndian( *len );
	Bulk_Only_Cmd->mCBW.mCBW_Flag = 0x80;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Len = 6;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[0] = 0x12;                        /* 命令码 */
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[1] = 0x00;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[2] = 0x00;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[3] = 0x00;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[4] = 0x24;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[5] = 0x00;
	s = USBHOST_BulkOnly( HubIndex, Edp, Bulk_Only_Cmd, &CtlTrans, DataBuffer, len );     /* 执行基于BulkOnly协议的命令 */
	return s;
}
/* 获取磁盘容量 */
u8 Get_DiskCapacity( u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd, u8 *DataBuffer, u32 *len )
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub( HubIndex );
	/* 设定SCSI/UFI请求 */
	memset( Bulk_Only_Cmd, 0, sizeof(BULK_ONLY_CMD));                /* 清空结构体 */
	Bulk_Only_Cmd->mCBW.mCBW_DataLen = mSwapEndian( *len );
	Bulk_Only_Cmd->mCBW.mCBW_Flag = 0x80;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Len = 0x0a;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[0] = 0x25;                        /* 命令码 */
	s = USBHOST_BulkOnly( HubIndex, Edp, Bulk_Only_Cmd, &CtlTrans, DataBuffer, len );     /* 执行基于BulkOnly协议的命令 */
	return s;
}
/* 检查磁盘错误 */
u8 Check_Erro( u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd, u8 *DataBuffer, u32 *len )
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub( HubIndex );
	/* 设定SCSI/UFI请求 */
	memset( Bulk_Only_Cmd, 0, sizeof(BULK_ONLY_CMD));                /* 清空结构体 */
	Bulk_Only_Cmd->mCBW.mCBW_DataLen = mSwapEndian( *len );
	Bulk_Only_Cmd->mCBW.mCBW_Flag = 0x80;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Len = 0x0c;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[0] = 0x03;                        /* 命令码 */
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[1] = 0x00;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[2] = 0x00;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[3] = 0x00;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[4] = 0x12;
	s = USBHOST_BulkOnly( HubIndex, Edp, Bulk_Only_Cmd, &CtlTrans, DataBuffer, len );     /* 执行基于BulkOnly协议的命令 */
	return s;
}

/* 大小端数据转换 */
u32	mSwapEndian( u32 dat )
{
	return( ( dat << 24 ) & 0xFF000000 | ( dat << 8 ) & 0x00FF0000 | ( dat >> 8 ) & 0x0000FF00 | ( dat >> 24 ) & 0x000000FF );
}
/* Bulk传输 */
u8 USBHOST_BulkOnly( u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd, PUSB_SETUP_REQ Setup_Req, u8 *DataBuffer, u32 *len )
{
	u8 s;
	u32 Req_len;                                                                 /* 请求的长度 */
	u8 length = 0;                                                               /* 单次传输的长度 */
	Bulk_Only_Cmd->mCBW.mCBW_Sig = USB_BO_CBW_SIG;
	Bulk_Only_Cmd->mCBW.mCBW_Tag = 0x00000100;
	Bulk_Only_Cmd->mCBW.mCBW_LUN = 0;
	*len = 0;                                                                       /* 代表实际收发数据总长度 */
	Write374Block( RAM_HOST_TRAN, USB_BO_CBW_SIZE, (u8 *)&(Bulk_Only_Cmd->mCBW) );/* 请求数据写入缓冲区 */
	Write374Byte( REG_USB_LENGTH, USB_BO_CBW_SIZE );
	s = Issue_Token( DEF_USB_PID_OUT, ((PEDP_INFO)Edp[0])->EDPNum, ((PEDP_INFO)Edp[0])->Tog, 1000);
	if( s == DEF_USB_PID_ACK )
	{
		((PEDP_INFO)Edp[0])->Tog ^= 1;
		Req_len = mSwapEndian( Bulk_Only_Cmd->mCBW.mCBW_DataLen );
		if(( Bulk_Only_Cmd->mCBW.mCBW_Flag ) & 0x80 )                                /* 收 */
		{
			while( Req_len )
			{
				s = Issue_Token( DEF_USB_PID_IN, ((PEDP_INFO)Edp[1])->EDPNum, ((PEDP_INFO)Edp[1])->Tog, 1000 );
				if( s == DEF_USB_PID_ACK )                                          //同步
				{
					((PEDP_INFO)Edp[1])->Tog ^= 1;
					length = Read374Byte( REG_USB_LENGTH );
					Read374Block( RAM_HOST_RECV, length, DataBuffer );
					Req_len -= length;
					*len += length;
					DataBuffer += length;
					if( length < ((PEDP_INFO)Edp[1])->MaxPacket )                   //收到一个短包，说明数据结束了
						break;
				}
				else if( s == DEF_USB_PID_DATA0 || s == DEF_USB_PID_DATA1 )         //不同步，丢包，不翻转
				{}
				else if( s == DEF_USB_PID_STALL )
				{
					s = Clear_Feature( HubIndex, ((PEDP_INFO)Edp[1])->EDPNum, Setup_Req );//清除端点特性
					if( s == Success )
					{
						((PEDP_INFO)Edp[1])->Tog = 0;
						break;
					}
				}
				else                                                                //出错
					return s;
			}
		}
		else                                                                         /* 发 */
		{
			while( Req_len )
			{
				if( Req_len > ((PEDP_INFO)Edp[0])->MaxPacket )
					length = ((PEDP_INFO)Edp[0])->MaxPacket;
				else length = Req_len;
				Write374Block( RAM_HOST_TRAN, length, DataBuffer );
				Write374Byte( REG_USB_LENGTH, length );
				s = Issue_Token( DEF_USB_PID_OUT, ((PEDP_INFO)Edp[0])->EDPNum, ((PEDP_INFO)Edp[0])->Tog, 1000);
				if(s == DEF_USB_PID_ACK )
				{
					((PEDP_INFO)Edp[0])->Tog ^= 1;
					Req_len -= length;
					*len += length;
					DataBuffer += length;
				}
				else if( s == DEF_USB_PID_STALL )
				{
					s = Clear_Feature( HubIndex, ((PEDP_INFO)Edp[0])->EDPNum, Setup_Req );//清除端点特性
					if( s == Success )
					{
						((PEDP_INFO)Edp[0])->Tog = 0;
						break;
					}
				}
				else return s;
			}
		}

		s = Issue_Token( DEF_USB_PID_IN, ((PEDP_INFO)Edp[1])->EDPNum, ((PEDP_INFO)Edp[1])->Tog, 1000 );  /* 状态阶段 */
		if( s == DEF_USB_PID_ACK )
		{
			((PEDP_INFO)Edp[1])->Tog ^= 1;
			length = Read374Byte( REG_USB_LENGTH );
			Read374Block( RAM_HOST_RECV, length, (u8 *)&(Bulk_Only_Cmd->mCSW) );
			if ( length != USB_BO_CSW_SIZE || Bulk_Only_Cmd->mCSW.mCSW_Sig != USB_BO_CSW_SIG )
				return Failure;
		}
		else
			return s;
	}
	else
		return s;
	return Success;
}
#endif
void CH374_PORT_INIT() /* 由于使用通用I/O模拟并口读写时序,所以进行初始化 */
{
	/* 如果是硬件SPI接口,那么可使用mode3(CPOL=1&CPHA=1)或mode0(CPOL=0&CPHA=0),CH374在时钟上升沿采样输入,下降沿输出,数据位是高位在前 */
	//	CH374T_SCK_HIGH;  //CH374_SPI_SCS = 1;  /* 禁止SPI片选 */
	CH374T_CS_HIGH; // CH374_SPI_SCK = 1;  /* 默认为高电平,SPI模式3,也可以用SPI模式0,但模拟程序可能需稍做修改 */
					/* 对于双向I/O引脚模拟SPI接口,那么必须在此设置SPI_SCS,SPI_SCK,SPI_SDI为输出方向,SPI_SDO为输入方向 */
}
/* 主机模式初始化 */
void HostMode_Init(void)
{
	u8 j;
	CH374_PORT_INIT();
	oop_InitDWT();                                          // 初始化DWT，oop_DelayUS 依赖其周期计数器
	Write374Byte(REG_USB_ADDR, 0xaa);
	j = Read374Byte(REG_USB_ADDR);
	EUSB_LOG("addrrev:%02x\n", (u16)j);
	Write374Byte(REG_USB_ADDR, 0x55);
	j = Read374Byte(REG_USB_ADDR);
	EUSB_LOG("addrrev:%02x\n", (u16)j);
	Write374Byte(REG_SYS_CTRL, BIT_CTRL_OE_POLAR);						 // UEN低电平使能USB输出
	Write374Byte(REG_USB_SETUP, BIT_SETP_HOST_MODE | BIT_SETP_AUTO_SOF); // 主机模式，自动发SOF包
	Modify374Byte(REG_INTER_EN, 0xff, BIT_IE_TRANSFER);					 // 传输中断(至于检测连接，用中断标志查询)
	Write374Byte(REG_INTER_FLAG, 0x1f);									 // 清所有中断标志
	Write374Byte(REG_USB_H_CTRL, 0);									 // USB主机控制寄存器初始化
	Write374Byte(REG_HUB_SETUP, 0);										 // 开启HUB
	Write374Byte(REG_HUB_CTRL, 0);										 // 清HUB状态
	ClearHub(0);														 // 初始化HUB0结构体
	ClearHub(1);														 // 初始化HUB1结构体
	ClearHub(2);														 // 初始化HUB2结构体
}
/* 清Hub端口信息 */
void ClearHub(u8 HubIndex)
{
	switch (HubIndex)
	{
	case 0:
		Modify374Byte(REG_HUB_SETUP, ~(BIT_HUB0_EN | BIT_HUB0_POLAR), 0); // 关闭HUB端口（拔插自动关闭），复位端口极性
		memset(&HUB[HubIndex], 0, sizeof(DEV_INFO));					  // 清除Hub口设备信息
		HUB[HubIndex].Ep0MaxPacket = DefEp0MaxPacket;
		Udisk_Oper_State[HubIndex] = DiskInfo;
		Sec_Len[HubIndex] = 0x200;
		break;
	case 1:
		Modify374Byte(REG_HUB_CTRL, ~(BIT_HUB1_EN | BIT_HUB1_POLAR), 0); // 关闭HUB端口（拔插自动关闭），复位端口极性
		memset(&HUB[HubIndex], 0, sizeof(DEV_INFO));					 // 清除Hub口设备信息
		HUB[HubIndex].Ep0MaxPacket = DefEp0MaxPacket;
		Udisk_Oper_State[HubIndex] = DiskInfo;
		Sec_Len[HubIndex] = 0x200;
		break;
	case 2:
		Modify374Byte(REG_HUB_CTRL, ~(BIT_HUB2_EN | BIT_HUB2_POLAR), 0); // 关闭HUB端口（拔插自动关闭），复位端口极性
		memset(&HUB[HubIndex], 0, sizeof(DEV_INFO));					 // 清除Hub口设备信息
		HUB[HubIndex].Ep0MaxPacket = DefEp0MaxPacket;
		Udisk_Oper_State[HubIndex] = DiskInfo;
		Sec_Len[HubIndex] = 0x200;
		break;
	default:
		break;
	}
	// 复位该端口的去重与就绪上报状态
	s_last_key[HubIndex] = 0;
	s_last_buttons[HubIndex] = 0;
	// 仅当该端口之前上报过"就绪"才上报断开，避免枚举失败重试/初始化时刷屏
	if (s_dev_ready_reported[HubIndex])
	{
		s_dev_ready_reported[HubIndex] = 0;
		const ch374_event_cb_t *cb = ch374_get_event_cb();
		if (cb != NULL && cb->on_dev_remove)
			cb->on_dev_remove(HubIndex);
	}
}
/* 检查相关Hub口是否连接,根据HUB状态进行相关配置 */
void CheckHubConnect(u8 HubIndex)
{
	u8 s;
	switch (HubIndex)
	{
	case 0:
		s = Read374Byte(REG_HUB_SETUP);
		if (s & BIT_HUB0_ATTACH)
		{
			if (HUB[HubIndex].DeviceStatus <= UnInit)
			{
				HUB[HubIndex].DeviceStatus = UnInit;
				s = Read374Byte(REG_INTER_FLAG); // 获取设备极性
				if (s & BIT_HUB0_DX_IN)			 // 极性匹配
				{
					s = Read374Byte(REG_HUB_SETUP); // 获取当前极性
					if (s & BIT_HUB0_POLAR)
						HUB[HubIndex].DeviceSpeed = LowSpeed;
					else
						HUB[HubIndex].DeviceSpeed = FullSpeed;
				}
				else // 极性不匹配(需要切换极性)
				{
					s = Read374Byte(REG_HUB_SETUP); // 获取当前极性
					if (s & BIT_HUB0_POLAR)
					{
						HUB[HubIndex].DeviceSpeed = FullSpeed;
						Modify374Byte(REG_HUB_SETUP, ~BIT_HUB0_POLAR, 0);
					}
					else
					{
						HUB[HubIndex].DeviceSpeed = LowSpeed;
						Modify374Byte(REG_HUB_SETUP, 0xff, BIT_HUB0_POLAR);
					}
				}
			}
		}
		else
		{
			ClearHub(HubIndex); // 清HUB口状态信息
		}
		break;
	case 1:
		s = Read374Byte(REG_HUB_CTRL);
		if (s & BIT_HUB1_ATTACH)
		{
			if (HUB[HubIndex].DeviceStatus <= UnInit)
			{
				HUB[HubIndex].DeviceStatus = UnInit;
				s = Read374Byte(REG_HUB_SETUP); // 获取设备极性
				if (s & BIT_HUB1_DX_IN)			// 极性匹配
				{
					s = Read374Byte(REG_HUB_CTRL); // 获取当前极性
					if (s & BIT_HUB1_POLAR)
						HUB[HubIndex].DeviceSpeed = LowSpeed;
					else
						HUB[HubIndex].DeviceSpeed = FullSpeed;
				}
				else // 极性不匹配
				{
					s = Read374Byte(REG_HUB_CTRL); // 获取当前极性
					if (s & BIT_HUB1_POLAR)
					{
						HUB[HubIndex].DeviceSpeed = FullSpeed;
						Modify374Byte(REG_HUB_CTRL, ~BIT_HUB1_POLAR, 0);
					}
					else
					{
						HUB[HubIndex].DeviceSpeed = LowSpeed;
						Modify374Byte(REG_HUB_CTRL, 0xff, BIT_HUB1_POLAR);
					}
				}
			}
		}
		else
		{
			ClearHub(HubIndex); // 清HUB口状态信息
		}
		break;
	case 2:
		s = Read374Byte(REG_HUB_CTRL);
		if (s & BIT_HUB2_ATTACH)
		{
			if (HUB[HubIndex].DeviceStatus <= UnInit)
			{
				HUB[HubIndex].DeviceStatus = UnInit;
				s = Read374Byte(REG_HUB_SETUP); // 获取设备极性
				if (s & BIT_HUB2_DX_IN)			// 极性匹配
				{
					s = Read374Byte(REG_HUB_CTRL); // 获取当前极性
					if (s & BIT_HUB2_POLAR)
						HUB[HubIndex].DeviceSpeed = LowSpeed;
					else
						HUB[HubIndex].DeviceSpeed = FullSpeed;
				}
				else // 极性不匹配
				{
					s = Read374Byte(REG_HUB_CTRL); // 获取当前极性
					if (s & BIT_HUB2_POLAR)
					{
						HUB[HubIndex].DeviceSpeed = FullSpeed;
						Modify374Byte(REG_HUB_CTRL, ~BIT_HUB2_POLAR, 0);
					}
					else
					{
						HUB[HubIndex].DeviceSpeed = LowSpeed;
						Modify374Byte(REG_HUB_CTRL, 0xff, BIT_HUB2_POLAR);
					}
				}
			}
		}
		else
		{
			ClearHub(HubIndex); // 清HUB口状态信息
		}
		break;
	default:
		break;
	}
}
/* 查询连接状态 */ // 不用BIT_IF_DEV_DETECT，因为如果是插上上电则检测不到
void ScanConnect()
{
	u8 HubIndex;
	for (HubIndex = 0; HubIndex < 3; HubIndex++)
	{
		CheckHubConnect(HubIndex); // 检查HUB连接情况
	}
}
/* 设备拔插检测 */
void Dev_Detect()
{
	u8 s;
	s = Read374Byte(REG_INTER_FLAG);
	if (s & BIT_IF_DEV_DETECT)
	{
		ScanConnect();
		Write374Byte(REG_INTER_FLAG, BIT_IF_DEV_DETECT); // 清除标志位
	}
}
/* 复位HUB口 */
void ResetHub(u8 HubIndex)
{
	u8 i, s;
	switch (HubIndex)
	{
	case 0:
		Modify374Byte(REG_HUB_SETUP, 0xff, BIT_HUB0_RESET);
		oop_DelayMS(15);					// 总线复位需≥10ms，此处原为 DelayUS(15) 导致HUB0枚举失败
		Modify374Byte(REG_HUB_SETUP, ~BIT_HUB0_RESET, 0);

		for (i = 0; i < 100; i++) /* 等待重连 */
		{
			s = Read374Byte(REG_HUB_SETUP);
			if (s & BIT_HUB0_ATTACH)
				break;
		}
		break;
	case 1:
		Modify374Byte(REG_HUB_CTRL, 0xff, BIT_HUB1_RESET);
		oop_DelayMS(15);
		Modify374Byte(REG_HUB_CTRL, ~BIT_HUB1_RESET, 0);

		for (i = 0; i < 100; i++) /* 等待重连 */
		{
			s = Read374Byte(REG_HUB_CTRL);
			if (s & BIT_HUB1_ATTACH)
				break;
		}
		break;
	case 2:
		Modify374Byte(REG_HUB_CTRL, 0xff, BIT_HUB2_RESET);
		oop_DelayMS(15);
		Modify374Byte(REG_HUB_CTRL, ~BIT_HUB2_RESET, 0);

		for (i = 0; i < 100; i++) /* 等待重连 */
		{
			s = Read374Byte(REG_HUB_CTRL);
			if (s & BIT_HUB2_ATTACH)
				break;
		}
		break;
	default:
		break;
	}
	Write374Byte(REG_INTER_FLAG, BIT_IF_DEV_DETECT); // 清除重连标志位
}

/* HUB端口使能或禁止使能 */
void HubPort_Cmd(u8 HubIndex, u8 Status) // Status 0：禁止  Status 1:使能
{
	switch (HubIndex)
	{
	case 0:
		if (Status)
			Modify374Byte(REG_HUB_SETUP, 0xff, BIT_HUB0_EN);
		else
			Modify374Byte(REG_HUB_SETUP, ~BIT_HUB0_EN, 0);
		break;
	case 1:
		if (Status)
			Modify374Byte(REG_HUB_CTRL, 0xff, BIT_HUB1_EN);
		else
			Modify374Byte(REG_HUB_CTRL, ~BIT_HUB1_EN, 0);
		break;
	case 2:
		if (Status)
			Modify374Byte(REG_HUB_CTRL, 0xff, BIT_HUB2_EN);
		else
			Modify374Byte(REG_HUB_CTRL, ~BIT_HUB2_EN, 0);
		break;
	default:
		break;
	}
}
/* 任何数据传输前都要选择Hub口 */
void SelectHub(u8 HubIndex)
{
	/* 设置总线速度 */
	if (HUB[HubIndex].DeviceSpeed == FullSpeed)
	{
		Modify374Byte(REG_USB_SETUP, ~BIT_SETP_LOW_SPEED, 0);
	}
	else
	{
		Modify374Byte(REG_USB_SETUP, 0xff, BIT_SETP_LOW_SPEED);
	}
	/* 设置设备地址 */
	Write374Byte(REG_USB_ADDR, HUB[HubIndex].DeviceAddress);
}
/* 事物传输 */ /* SET IN OUT */ /* Timeout=0不重试 Timeout=0xffff无限重试 ,返回PID*/
u8 Issue_Token(u8 PID, u8 Endp, u8 Tog, u16 Timeout)
{
	u8 s, resp, err_resp = 0, err_num = 0;
	while (1) /* 虽是死循环，但终究有返回的 */
	{
		Write374Byte(REG_USB_H_PID, (PID << 4) | Endp);
		Write374Byte(REG_USB_H_CTRL, Tog ? BIT_HOST_TRAN_TOG | BIT_HOST_RECV_TOG | BIT_HOST_START : BIT_HOST_START); /* 启动传输 */
		s = Wait374Interrupt();
		if (s == Failure) /* 设备超时无响应,最多重试三次*/
			err_num++;
		else
			err_num = 0;
		if (err_num == 3)
			return s;

		Write374Byte(REG_INTER_FLAG, BIT_IF_TRANSFER | BIT_IF_USB_PAUSE); /* 清中断 ,必须清除传输暂停位*/
		if (err_num == 0)
		{
			s = Read374Byte(REG_USB_STATUS);
			resp = s & BIT_STAT_DEV_RESP;
			switch (PID) /* 分析令牌PID */
			{
			case DEF_USB_PID_SETUP:
			case DEF_USB_PID_OUT:
				if (resp == DEF_USB_PID_ACK) /* 返回ACK */
					return resp;
				else if (resp == DEF_USB_PID_NAK) /* 根据是否超时决定返回NAK */
				{
					if (Timeout == 0)
						return resp;
					else
					{
						if (Timeout < 0xffff)
							Timeout--;
					}
				}
				else if (resp == DEF_USB_PID_STALL) /* 返回STALL */
					return resp;
				else /* 出错或超时等 */
				{
					err_resp++;
					if (err_resp == 3)
						return resp;
				}
				break;

			case DEF_USB_PID_IN:
				if (resp == DEF_USB_PID_DATA0 || resp == DEF_USB_PID_DATA1) /* 说明收到了数据包，但不一定同步 */
				{
					if (s & BIT_STAT_TOG_MATCH) /* 翻转同步返回ACK */
						return DEF_USB_PID_ACK;
					else /* 翻转不同步返回当前DATA值 */
						return resp;
				}
				else if (resp == DEF_USB_PID_NAK) /* 根据是否超时决定返回NAK */
				{
					if (Timeout == 0)
						return resp;
					else
					{
						if (Timeout < 0xffff)
							Timeout--;
					}
				}
				else if (resp == DEF_USB_PID_STALL) /* 返回STALL */
					return resp;
				else /* 出错或超时等 */
				{
					err_resp++;
					if (err_resp == 3)
						return resp;
				}
				break;
			default:
				return Failure; /* 非法PID */
			}
		}
	}
}
/* 执行控制传输 */
u8 USBHOST_Issue_Control(u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 *DataBuffer, u16 *len)
{
	u8 s;
	u8 tog = 0;
	u16 Req_len, length = 0;						  /* Req_len表示请求长度，length为当前收到的数据包长度 */
	*len = 0;										  /* 代表收到的数据总长度 */
	Write374Block(RAM_HOST_TRAN, 8, (u8 *)Setup_Req); /* 请求数据写入缓冲区 */
	Write374Byte(REG_USB_LENGTH, 8);
	s = Issue_Token(DEF_USB_PID_SETUP, 0, tog, 0xffff);
	if (s == DEF_USB_PID_STALL)
		return Success;
	else if (s == DEF_USB_PID_ACK)
	{
		tog ^= 1;
		Req_len = (((u16)(Setup_Req->wLengthH)) << 8) + Setup_Req->wLengthL;
		if ((Setup_Req->bType) & 0x80) /* 收 */
		{
			while (Req_len)
			{
				s = Issue_Token(DEF_USB_PID_IN, 0, tog, 0xffff);
				if (s == DEF_USB_PID_STALL)
					return Success;
				else if (s == DEF_USB_PID_ACK) // 同步
				{
					tog ^= 1;
					length = Read374Byte(REG_USB_LENGTH);
					Read374Block(RAM_HOST_RECV, length, DataBuffer);
					Req_len -= length;
					*len += length;
					DataBuffer += length;
					if (length < HUB[HubIndex].Ep0MaxPacket) // 收到一个短包，说明数据结束了
						break;
				}
				else if (s == DEF_USB_PID_DATA0 || s == DEF_USB_PID_DATA1) // 不同步，丢包，不翻转
				{
				}
				else // 出错
					return s;
			}
			tog = 1;
		}
		else /* 发 */
		{
			while (Req_len)
			{
				if (Req_len > HUB[HubIndex].Ep0MaxPacket)
					length = HUB[HubIndex].Ep0MaxPacket;
				else
					length = Req_len;
				Write374Block(RAM_HOST_TRAN, length, DataBuffer);
				Write374Byte(REG_USB_LENGTH, length);
				s = Issue_Token(DEF_USB_PID_OUT, 0, tog, 0xffff);
				if (s == DEF_USB_PID_STALL)
					return Success;
				else if (s == DEF_USB_PID_ACK)
				{
					tog ^= 1;
					Req_len -= length;
					*len += length;
					DataBuffer += length;
				}
				else
					return s;
			}
			tog = 0;
		}

		Write374Byte(REG_USB_LENGTH, 0); /* 状态阶段 */
		s = Issue_Token(tog ? DEF_USB_PID_OUT : DEF_USB_PID_IN, 0, 1, 0xffff);
		if (s == DEF_USB_PID_STALL)
			return Success;
		else if (s == DEF_USB_PID_ACK)
		{
			if (tog == 0)
			{
				if (Read374Byte(REG_USB_LENGTH)) // 接收出错，不是零包
					return Failure;
			}
		}
		else
			return s;
	}
	else
		return s;

	return Success;
}
/* 清除端点特性 */
u8 Clear_Feature(u8 HubIndex, u8 Edp, PUSB_SETUP_REQ Setup_Req)
{
	u8 s;
	/* 设定控制请求 */
	Setup_Req->bType = 0x02;
	Setup_Req->bReq = DEF_USB_CLR_FEATURE;
	Setup_Req->wValueL = 0x00;
	Setup_Req->wValueH = 0x00;
	Setup_Req->wIndexL = Edp;
	Setup_Req->wIndexH = 0x00;
	Setup_Req->wLengthL = 0x00;
	Setup_Req->wLengthH = 0x00;
	s = USBHOST_Issue_Control(HubIndex, Setup_Req, NULL, NULL); /* 执行控制传输 */
	return s;
}

/* 获取设备描述符 */
/* len表示输入输出参数长度 ,返回执行状态 */
u8 Get_DevDesc(u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 *DataBuffer, u16 *len)
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub(HubIndex);
	/* 设定控制请求 */
	Setup_Req->bType = 0x80;
	Setup_Req->bReq = DEF_USB_GET_DESCR;
	Setup_Req->wValueL = 0x00;
	Setup_Req->wValueH = USB_DEVICE_DESCR_TYPE;
	Setup_Req->wIndexL = 0x00;
	Setup_Req->wIndexH = 0x00;
	Setup_Req->wLengthL = (u8)(*len);
	Setup_Req->wLengthH = (u8)((*len) >> 8);
	s = USBHOST_Issue_Control(HubIndex, Setup_Req, DataBuffer, len); /* 执行控制传输 */
	return s;
}
/* 设置设备地址 */ /* 返回操作状态 */
u8 Set_DevAddr(u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 addr)
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub(HubIndex);
	/* 设定控制请求 */
	Setup_Req->bType = 0x00;
	Setup_Req->bReq = DEF_USB_SET_ADDRESS;
	Setup_Req->wValueL = addr;
	Setup_Req->wValueH = 0x00;
	Setup_Req->wIndexL = 0x00;
	Setup_Req->wIndexH = 0x00;
	Setup_Req->wLengthL = 0x00;
	Setup_Req->wLengthH = 0x00;
	s = USBHOST_Issue_Control(HubIndex, Setup_Req, NULL, NULL); /* 执行控制传输 */
	if (s != Success)
		return s;
	HUB[HubIndex].DeviceAddress = addr; /* 更新HUB口地址 */
	return s;
}
/* 获取配置描述符 */
/* len表示输入输出参数长度 ,返回执行状态 */
u8 Get_CfgDesc(u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 *DataBuffer, u16 *len)
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub(HubIndex);
	/* 设定控制请求 */
	Setup_Req->bType = 0x80;
	Setup_Req->bReq = DEF_USB_GET_DESCR;
	Setup_Req->wValueL = 0x00;
	Setup_Req->wValueH = USB_CONFIG_DESCR_TYPE;
	Setup_Req->wIndexL = 0x00;
	Setup_Req->wIndexH = 0x00;
	Setup_Req->wLengthL = (u8)(*len);
	Setup_Req->wLengthH = (u8)((*len) >> 8);
	s = USBHOST_Issue_Control(HubIndex, Setup_Req, DataBuffer, len); /* 执行控制传输 */
	return s;
}
/* 分析配置描述符 */
u8 Analy_CfgDesc(u8 HubIndex, u8 *DataBuffer)
{
	u16 i;
	u8 itf_num = 0; // 表示当前分析的的接口
	u8 edp_num = 0; // 表示当前分析的的端点
	u16 length;
	u8 *pData;
	pData = DataBuffer;
	if (pData[1] == USB_CONFIG_DESCR_TYPE) // 先判断输入DataBuffer是否有效，并获取长度
	{
		length = pData[2] + ((u16)(pData[3]) << 8);
		for (i = 0; i < length; i++)
		{
			if (pData[i + 1] == USB_CONFIG_DESCR_TYPE)
			{
				HUB[HubIndex].ConfigurationValue = ((PUSB_CFG_DESCR)&pData[i])->bConfigurationValue; // 保存配置值
				HUB[HubIndex].NumInterfaces = ((PUSB_CFG_DESCR)&pData[i])->bNumInterfaces;			 // 接口数目
				if ((HUB[HubIndex].NumInterfaces == 0) || (HUB[HubIndex].NumInterfaces > NUM_ITF))	 // 不支持的接口数目
					return Failure;
			}
			else if (pData[i + 1] == USB_INTERF_DESCR_TYPE)
			{
				HUB[HubIndex].ITF[itf_num].ITFNum = ((PUSB_ITF_DESCR)&pData[i])->bInterfaceNumber; // 接口号
				HUB[HubIndex].ITF[itf_num].Class = ((PUSB_ITF_DESCR)&pData[i])->bInterfaceClass;   // 设备类
				HUB[HubIndex].ITF[itf_num].SubClass = ((PUSB_ITF_DESCR)&pData[i])->bInterfaceSubClass;
				HUB[HubIndex].ITF[itf_num].Protocol = ((PUSB_ITF_DESCR)&pData[i])->bInterfaceProtocol;
				HUB[HubIndex].ITF[itf_num].NumEndpoints = ((PUSB_ITF_DESCR)&pData[i])->bNumEndpoints; // 该接口下的端点数
				if (HUB[HubIndex].ITF[itf_num].NumEndpoints > NUM_EDP)
					return Failure;
				edp_num = 0; // 当前接口分析端点清零
				itf_num++;	 // 接口号加一
			}
			else if (pData[i + 1] == USB_HID_DESCR_TYPE)
			{
				HUB[HubIndex].ITF[itf_num - 1].HID_Desc_Len = pData[i + 7] + (((u16)pData[i + 8]) << 8); // HID描述符长度
			}
			else if (pData[i + 1] == USB_ENDP_DESCR_TYPE)
			{
				HUB[HubIndex].ITF[itf_num - 1].Endp[edp_num].EDPNum = ((PUSB_ENDP_DESCR)&pData[i])->bEndpointAddress;  // 端点地址
				HUB[HubIndex].ITF[itf_num - 1].Endp[edp_num].Attributes = ((PUSB_ENDP_DESCR)&pData[i])->bmAttributes;  // 端点类型
				HUB[HubIndex].ITF[itf_num - 1].Endp[edp_num].MaxPacket = ((PUSB_ENDP_DESCR)&pData[i])->wMaxPacketSize; // 端点大小
				edp_num++;
			}
			i += (pData[i] - 1); // 跳转到下一个长度
		}
		return Success;
	}
	else
		return Failure;
}
/* 设置配置 */
u8 Set_Config(u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 Cfg_Value)
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub(HubIndex);
	/* 设定控制请求 */
	Setup_Req->bType = 0x00;
	Setup_Req->bReq = DEF_USB_SET_CONFIG;
	Setup_Req->wValueL = Cfg_Value;
	Setup_Req->wValueH = 0x00;
	Setup_Req->wIndexL = 0x00;
	Setup_Req->wIndexH = 0x00;
	Setup_Req->wLengthL = 0x00;
	Setup_Req->wLengthH = 0x00;
	s = USBHOST_Issue_Control(HubIndex, Setup_Req, NULL, NULL); /* 执行控制传输 */
	return s;
}
/* HID类命令 Set_Idle */
u8 Set_Idle(u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 Itf_Num)
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub(HubIndex);
	/* 设定控制请求 */
	Setup_Req->bType = 0x21;
	Setup_Req->bReq = 0x0a;
	Setup_Req->wValueL = 0x00;
	Setup_Req->wValueH = 0x00;
	Setup_Req->wIndexL = Itf_Num;
	Setup_Req->wIndexH = 0x00;
	Setup_Req->wLengthL = 0x00;
	Setup_Req->wLengthH = 0x00;
	s = USBHOST_Issue_Control(HubIndex, Setup_Req, NULL, NULL); /* 执行控制传输 */
	return s;
}

/* HID类命令 Set_Protocol：切换 Boot Protocol / Report Protocol */
/* protocol: 0=Boot, 1=Report。Boot 下键盘固定8字节、鼠标固定4字节报告 */
u8 Set_Protocol(u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 Itf_Num, u8 protocol)
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub(HubIndex);
	/* 设定控制请求 */
	Setup_Req->bType = 0x21;                       /* 类请求，写 */
	Setup_Req->bReq = 0x0b;                        /* SET_PROTOCOL */
	Setup_Req->wValueL = protocol;
	Setup_Req->wValueH = 0x00;
	Setup_Req->wIndexL = Itf_Num;
	Setup_Req->wIndexH = 0x00;
	Setup_Req->wLengthL = 0x00;
	Setup_Req->wLengthH = 0x00;
	s = USBHOST_Issue_Control(HubIndex, Setup_Req, NULL, NULL); /* 执行控制传输 */
	return s;
}
/* HID类命令 Get_Report */
u8 Get_Report(u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 Itf_Num, u8 *DataBuffer, u16 *len)
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub(HubIndex);
	/* 设定控制请求 */
	Setup_Req->bType = 0x81;
	Setup_Req->bReq = DEF_USB_GET_DESCR;
	Setup_Req->wValueL = 0x00;
	Setup_Req->wValueH = 0x22;
	Setup_Req->wIndexL = Itf_Num;
	Setup_Req->wIndexH = 0x00;
	Setup_Req->wLengthL = (u8)(*len);
	Setup_Req->wLengthH = (u8)((*len) >> 8);
	s = USBHOST_Issue_Control(HubIndex, Setup_Req, DataBuffer, len); /* 执行控制传输 */
	return s;
}

/* HID类命令 Set_Report */
u8 Set_Report(u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 *DataBuffer, u16 *len)
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub(HubIndex);
	/* 设定控制请求 */
	Setup_Req->bType = 0x21;
	Setup_Req->bReq = 0x09;
	Setup_Req->wValueL = 0x00;
	Setup_Req->wValueH = 0x02;
	Setup_Req->wIndexL = 0x00;
	Setup_Req->wIndexH = 0x00;
	Setup_Req->wLengthL = 0x01;
	Setup_Req->wLengthH = 0x00;
	s = USBHOST_Issue_Control(HubIndex, Setup_Req, DataBuffer, len); /* 执行控制传输 */
	return s;
}
/* Mass Storage类命令 Get_MaxLun */
u8 Get_MaxLun(u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 *DataBuffer, u16 *len)
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub(HubIndex);
	/* 设定控制请求 */
	Setup_Req->bType = 0xa1;
	Setup_Req->bReq = 0xfe;
	Setup_Req->wValueL = 0x00;
	Setup_Req->wValueH = 0x00;
	Setup_Req->wIndexL = 0x00;
	Setup_Req->wIndexH = 0x00;
	Setup_Req->wLengthL = (u8)(*len);
	Setup_Req->wLengthH = (u8)((*len) >> 8);
	s = USBHOST_Issue_Control(HubIndex, Setup_Req, DataBuffer, len); /* 执行控制传输 */
	return s;
}
/* 类命令处理 */
u8 Class_Issue(u8 HubIndex)
{
	u8 i, j;
	u8 s;
	u16 len;
	u8 hid_key_led = 0;
	for (i = 0; i < HUB[HubIndex].NumInterfaces; i++) // 先判断类，再对该接口处理，是否需要发类命令等
	{
		len = 0;
		if (HUB[HubIndex].ITF[i].Class == USB_DEV_CLASS_HUMAN_IF) // HID类
		{
			if (HUB[HubIndex].ITF[i].Protocol == 0x01) // 键盘
			{
				EUSB_LOG("Keyboard Device\n");
				hid_key_led = 1;
			}
			else if (HUB[HubIndex].ITF[i].Protocol == 0x02) // 鼠标
			{
				EUSB_LOG("Mouse Device\n");
			}
			else // 其他HID协议(如消费类键盘附加接口 Protocol=0x00)，跳过类命令
			{
				EUSB_LOG("HID Device (protocol %d)\n", (u16)HUB[HubIndex].ITF[i].Protocol);
				continue;
			}
			s = Set_Idle(HubIndex, &CtlTrans, HUB[HubIndex].ITF[i].ITFNum); // Set_Idle
			if (s != Success)
				return s;
			len = HUB[HubIndex].ITF[i].HID_Desc_Len;
			if (len)
			{
				len += 0x40;
				s = Get_Report(HubIndex, &CtlTrans, HUB[HubIndex].ITF[i].ITFNum, Buffer, &len); // Get_Report
				if (s != Success)
					return s;

				EUSB_LOG("HUB #%2x Report #%2x:\n", (u16)HubIndex, (u16)HUB[HubIndex].ITF[i].ITFNum);
				for (j = 0; j < len; j++)
					EUSB_LOG("0x%02x ", (u16)Buffer[j]);
				EUSB_LOG("\n");
				// 此处插入分析报表函数
			}
		}
		else if (HUB[HubIndex].ITF[i].Class == USB_DEV_CLASS_STORAGE) // 大容量存储类
		{
			EUSB_LOG("Mass Storage Device\n");
			len = 1;
			s = Get_MaxLun(HubIndex, &CtlTrans, Buffer, &len);
			if (s != Success)
				return s;
			if ((len != 1) || (Buffer[0] != 0)) // 不支持的逻辑单元数
				return Failure;
		}
		else if (HUB[HubIndex].ITF[i].Class == USB_DEV_CLASS_PRINTER) // 打印机类
		{
			EUSB_LOG("Printer Device\n");
		}
	}
	if (hid_key_led) // 键盘点灯
	{
		Buffer[0] = 1;
		s = Set_Report(HubIndex, &CtlTrans, Buffer, NULL); // Set_Report
		if (s != Success)
			return s;
	}
	return Success;
}
/* 枚举设备 */
u8 EnumHub(u8 HubIndex)
{
	u16 len;
	u8 s;
	u8 i;
	/* 获取设备描述符,得到最大包长 */
	len = HUB[HubIndex].Ep0MaxPacket; // 请求长度
	s = Get_DevDesc(HubIndex, &CtlTrans, Buffer, &len);
	if (s != Success)
		return s;
	len = Buffer[0];
	HUB[HubIndex].Ep0MaxPacket = Buffer[7]; // EP0最大包长
	/* HUB端口总线复位，并重新使能端口 */
	ResetHub(HubIndex);
	HubPort_Cmd(HubIndex, Enable); // 开启端口使能(总线复位后HUB端口自动禁止，需重新使能)
	/* 设置设备地址 */
	s = Set_DevAddr(HubIndex, &CtlTrans, HubIndex + 1); // 设置地址，地址为端口号+1
	if (s != Success)
		return s;
	/* 获取设备描述符 */
	s = Get_DevDesc(HubIndex, &CtlTrans, Buffer, &len);
	if (s != Success)
		return s;
	EUSB_LOG("HUB #%2x Device:\n", (u16)HubIndex);
	for (i = 0; i < len; i++)
		EUSB_LOG("0x%02x ", (u16)Buffer[i]);
	EUSB_LOG("\n");
	/* 获取配置描述符 */
	len = 4;
	s = Get_CfgDesc(HubIndex, &CtlTrans, Buffer, &len);
	if (s != Success)
		return s;
	len = (((u16)Buffer[3]) << 8) + Buffer[2];
	s = Get_CfgDesc(HubIndex, &CtlTrans, Buffer, &len);
	if (s != Success)
		return s;
	EUSB_LOG("HUB #%2x Config:\n", (u16)HubIndex);
	for (i = 0; i < len; i++)
		EUSB_LOG("0x%02x ", (u16)Buffer[i]);
	EUSB_LOG("\n");
	/* 分析配置描述符 */
	s = Analy_CfgDesc(HubIndex, Buffer);
	if (s != Success)
	{
		EUSB_LOG("Invalid Configuration Descriptor\n");
		return s;
	}
	/* 设置配置 */
	s = Set_Config(HubIndex, &CtlTrans, HUB[HubIndex].ConfigurationValue);
	if (s != Success)
		return s;
	/* 类命令处理 */
	s = Class_Issue(HubIndex);
	if (s != Success)
		return s;

	return Success;
}
/* 根据枚举结果判断 HID 设备类型（鼠标优先，其次键盘） */
static ch374_hid_type_t hub_dev_type(u8 HubIndex)
{
	u8 i;
	for (i = 0; i < HUB[HubIndex].NumInterfaces; i++)
	{
		if (HUB[HubIndex].ITF[i].Class == USB_DEV_CLASS_HUMAN_IF)
		{
			if (HUB[HubIndex].ITF[i].Protocol == 0x02)
				return CH374_HID_MOUSE;
			if (HUB[HubIndex].ITF[i].Protocol == 0x01)
				return CH374_HID_KEYBOARD;
		}
	}
	return CH374_HID_NONE;
}

/* 初始化该Hub端口设备 */
void Init_Hub(u8 HubIndex)
{
	ResetHub(HubIndex);							   // 总线复位，复位并不能改变端口极性位
	HubPort_Cmd(HubIndex, Enable);				   // 开启端口使能
	oop_DelayMS(30);							   // 等待设备稳定后发包
	if (EnumHub(HubIndex) == Success)			   // 枚举
	{
		HUB[HubIndex].DeviceStatus = InitComplete; // 枚举成功后刷新状态
		// 上报就绪事件（键盘/鼠标）
		const ch374_event_cb_t *cb = ch374_get_event_cb();
		if (cb != NULL && cb->on_dev_ready && !s_dev_ready_reported[HubIndex])
		{
			cb->on_dev_ready(HubIndex, hub_dev_type(HubIndex));
			s_dev_ready_reported[HubIndex] = 1;
		}
	}
	else
		HUB[HubIndex].InitFailTimes++;	  // 初始化失败累加
	if (HUB[HubIndex].InitFailTimes == 3) // 失败三次后
		ClearHub(HubIndex);
}
/* 根据拔插事件搜索需要初始化的Hub端口设备 */
void Init_USB_Device()
{
	u8 HubIndex;
	for (HubIndex = 0; HubIndex < 3; HubIndex++)
	{
		if (HUB[HubIndex].DeviceStatus == UnInit)
		{
			EUSB_LOG("HUB %d Speed %d \n", (u16)HubIndex, (u16)HUB[HubIndex].DeviceSpeed);
			Init_Hub(HubIndex);
		}
	}
}

/* 大小端数据转换 */
u32 mSwapEndian(u32 dat)
{
	return (((dat << 24) & 0xFF000000) |
			((dat << 8) & 0x00FF0000) |
			((dat >> 8) & 0x0000FF00) |
			((dat >> 24) & 0x000000FF));
}
/* Bulk传输 */
u8 USBHOST_BulkOnly(u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd, PUSB_SETUP_REQ Setup_Req, u8 *DataBuffer, u32 *len)
{
	u8 s;
	u32 Req_len;   /* 请求的长度 */
	u8 length = 0; /* 单次传输的长度 */
	Bulk_Only_Cmd->mCBW.mCBW_Sig = USB_BO_CBW_SIG;
	Bulk_Only_Cmd->mCBW.mCBW_Tag = 0x00000100;
	Bulk_Only_Cmd->mCBW.mCBW_LUN = 0;
	*len = 0;																	 /* 代表实际收发数据总长度 */
	Write374Block(RAM_HOST_TRAN, USB_BO_CBW_SIZE, (u8 *)&(Bulk_Only_Cmd->mCBW)); /* 请求数据写入缓冲区 */
	Write374Byte(REG_USB_LENGTH, USB_BO_CBW_SIZE);
	s = Issue_Token(DEF_USB_PID_OUT, ((PEDP_INFO)Edp[0])->EDPNum, ((PEDP_INFO)Edp[0])->Tog, 1000);
	if (s == DEF_USB_PID_ACK)
	{
		((PEDP_INFO)Edp[0])->Tog ^= 1;
		Req_len = mSwapEndian(Bulk_Only_Cmd->mCBW.mCBW_DataLen);
		if ((Bulk_Only_Cmd->mCBW.mCBW_Flag) & 0x80) /* 收 */
		{
			while (Req_len)
			{
				s = Issue_Token(DEF_USB_PID_IN, ((PEDP_INFO)Edp[1])->EDPNum, ((PEDP_INFO)Edp[1])->Tog, 1000);
				if (s == DEF_USB_PID_ACK) // 同步
				{
					((PEDP_INFO)Edp[1])->Tog ^= 1;
					length = Read374Byte(REG_USB_LENGTH);
					Read374Block(RAM_HOST_RECV, length, DataBuffer);
					Req_len -= length;
					*len += length;
					DataBuffer += length;
					if (length < ((PEDP_INFO)Edp[1])->MaxPacket) // 收到一个短包，说明数据结束了
						break;
				}
				else if (s == DEF_USB_PID_DATA0 || s == DEF_USB_PID_DATA1) // 不同步，丢包，不翻转
				{
				}
				else if (s == DEF_USB_PID_STALL)
				{
					s = Clear_Feature(HubIndex, ((PEDP_INFO)Edp[1])->EDPNum, Setup_Req); // 清除端点特性
					if (s == Success)
					{
						((PEDP_INFO)Edp[1])->Tog = 0;
						break;
					}
				}
				else // 出错
					return s;
			}
		}
		else /* 发 */
		{
			while (Req_len)
			{
				if (Req_len > ((PEDP_INFO)Edp[0])->MaxPacket)
					length = ((PEDP_INFO)Edp[0])->MaxPacket;
				else
					length = Req_len;
				Write374Block(RAM_HOST_TRAN, length, DataBuffer);
				Write374Byte(REG_USB_LENGTH, length);
				s = Issue_Token(DEF_USB_PID_OUT, ((PEDP_INFO)Edp[0])->EDPNum, ((PEDP_INFO)Edp[0])->Tog, 1000);
				if (s == DEF_USB_PID_ACK)
				{
					((PEDP_INFO)Edp[0])->Tog ^= 1;
					Req_len -= length;
					*len += length;
					DataBuffer += length;
				}
				else if (s == DEF_USB_PID_STALL)
				{
					s = Clear_Feature(HubIndex, ((PEDP_INFO)Edp[0])->EDPNum, Setup_Req); // 清除端点特性
					if (s == Success)
					{
						((PEDP_INFO)Edp[0])->Tog = 0;
						break;
					}
				}
				else
					return s;
			}
		}

		s = Issue_Token(DEF_USB_PID_IN, ((PEDP_INFO)Edp[1])->EDPNum, ((PEDP_INFO)Edp[1])->Tog, 1000); /* 状态阶段 */
		if (s == DEF_USB_PID_ACK)
		{
			((PEDP_INFO)Edp[1])->Tog ^= 1;
			length = Read374Byte(REG_USB_LENGTH);
			Read374Block(RAM_HOST_RECV, length, (u8 *)&(Bulk_Only_Cmd->mCSW));
			if (length != USB_BO_CSW_SIZE || Bulk_Only_Cmd->mCSW.mCSW_Sig != USB_BO_CSW_SIG)
				return Failure;
		}
		else
			return s;
	}
	else
		return s;
	return Success;
}
/* 分析接口信息，获取批量端点 */ /* Edp[0]保存下传、Edp[1]保存上传 返回端点个数 */
u8 Get_Bulk_Edp(u8 HubIndex, u8 ITFNum, u8 **Edp)
{
	u8 i, s = 0;
	for (i = 0; i < HUB[HubIndex].ITF[ITFNum].NumEndpoints; i++)
	{
		if (HUB[HubIndex].ITF[ITFNum].Endp[i].Attributes == USB_ENDP_TYPE_BULK)
		{
			if (HUB[HubIndex].ITF[ITFNum].Endp[i].EDPNum & 0x80) // 上传端点
			{
				s |= 0x01;
				Edp[1] = (u8 *)&HUB[HubIndex].ITF[ITFNum].Endp[i].EDPNum;
			}
			else // 下传端点
			{
				s |= 0x02;
				Edp[0] = (u8 *)&HUB[HubIndex].ITF[ITFNum].Endp[i].EDPNum;
			}
		}
	}
	if (s == 3) // 必须有一个上传、一个下传
		return 2;
	else
		return 1;
}
/* 分析接口信息，获取中断端点 */ /* Edp[0]、Edp[1]保存上传结构体地址 返回中断端点个数 */
u8 Get_Interrupt_Edp(u8 HubIndex, u8 ITFNum, u8 **Edp)
{
	u8 i, s = 0;
	for (i = 0; i < HUB[HubIndex].ITF[ITFNum].NumEndpoints; i++)
	{
		if (HUB[HubIndex].ITF[ITFNum].Endp[i].Attributes == USB_ENDP_TYPE_INTER)
		{
			if (HUB[HubIndex].ITF[ITFNum].Endp[i].EDPNum & 0x80) // 上传端点
			{
				Edp[s] = (u8 *)&HUB[HubIndex].ITF[ITFNum].Endp[i].EDPNum;
				s++;
			}
		}
	}
	return s;
}
u8 Get_HidData(u8 HubIndex, u8 **Edp, u8 *DataBuffer, u32 *len)
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub(HubIndex);
	/* 发送IN包 */
	*len = 0;
	s = Issue_Token(DEF_USB_PID_IN, ((PEDP_INFO)*Edp)->EDPNum, ((PEDP_INFO)*Edp)->Tog, 0); // 不重试
	if (s == DEF_USB_PID_ACK)
	{
		((PEDP_INFO)*Edp)->Tog ^= 1;
		*len = Read374Byte(REG_USB_LENGTH);
		Read374Block(RAM_HOST_RECV, *len, DataBuffer);
	}
	else
		return s;
	return Success;
}
/* 获取磁盘信息 */
u8 Get_DiskInfo(u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd, u8 *DataBuffer, u32 *len)
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub(HubIndex);
	/* 设定SCSI/UFI请求 */
	memset(Bulk_Only_Cmd, 0, sizeof(BULK_ONLY_CMD)); /* 清空结构体 */
	Bulk_Only_Cmd->mCBW.mCBW_DataLen = mSwapEndian(*len);
	Bulk_Only_Cmd->mCBW.mCBW_Flag = 0x80;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Len = 6;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[0] = 0x12; /* 命令码 */
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[1] = 0x00;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[2] = 0x00;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[3] = 0x00;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[4] = 0x24;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[5] = 0x00;
	s = USBHOST_BulkOnly(HubIndex, Edp, Bulk_Only_Cmd, &CtlTrans, DataBuffer, len); /* 执行基于BulkOnly协议的命令 */
	return s;
}
/* 获取磁盘容量 */
u8 Get_DiskCapacity(u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd, u8 *DataBuffer, u32 *len)
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub(HubIndex);
	/* 设定SCSI/UFI请求 */
	memset(Bulk_Only_Cmd, 0, sizeof(BULK_ONLY_CMD)); /* 清空结构体 */
	Bulk_Only_Cmd->mCBW.mCBW_DataLen = mSwapEndian(*len);
	Bulk_Only_Cmd->mCBW.mCBW_Flag = 0x80;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Len = 0x0a;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[0] = 0x25;										/* 命令码 */
	s = USBHOST_BulkOnly(HubIndex, Edp, Bulk_Only_Cmd, &CtlTrans, DataBuffer, len); /* 执行基于BulkOnly协议的命令 */
	return s;
}
/* 检查磁盘错误 */
u8 Check_Erro(u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd, u8 *DataBuffer, u32 *len)
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub(HubIndex);
	/* 设定SCSI/UFI请求 */
	memset(Bulk_Only_Cmd, 0, sizeof(BULK_ONLY_CMD)); /* 清空结构体 */
	Bulk_Only_Cmd->mCBW.mCBW_DataLen = mSwapEndian(*len);
	Bulk_Only_Cmd->mCBW.mCBW_Flag = 0x80;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Len = 0x0c;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[0] = 0x03; /* 命令码 */
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[1] = 0x00;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[2] = 0x00;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[3] = 0x00;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[4] = 0x12;
	s = USBHOST_BulkOnly(HubIndex, Edp, Bulk_Only_Cmd, &CtlTrans, DataBuffer, len); /* 执行基于BulkOnly协议的命令 */
	return s;
}
/* 测试准备好与否 */
u8 TestUnit(u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd)
{
	u8 s;
	/* 设置待操作Hub口 */
	SelectHub(HubIndex);
	/* 设定SCSI/UFI请求 */
	memset(Bulk_Only_Cmd, 0, sizeof(BULK_ONLY_CMD)); /* 清空结构体 */
	Bulk_Only_Cmd->mCBW.mCBW_DataLen = 0;
	Bulk_Only_Cmd->mCBW.mCBW_Flag = 0x00;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Len = 0x06;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[0] = 0x00;								   /* 命令码 */
	s = USBHOST_BulkOnly(HubIndex, Edp, Bulk_Only_Cmd, &CtlTrans, NULL, NULL); /* 执行基于BulkOnly协议的命令 */
	return s;
}
/* 写磁盘扇区 */
u8 Write_DiskSec(u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd, u8 *DataBuffer, u32 Sector, u16 Count)
{
	u8 s;
	u32 len;
	/* 设置待操作Hub口 */
	SelectHub(HubIndex);
	/* 设定SCSI/UFI请求 */
	memset(Bulk_Only_Cmd, 0, sizeof(BULK_ONLY_CMD)); /* 清空结构体 */
	len = Count * Sec_Len[HubIndex];
	Bulk_Only_Cmd->mCBW.mCBW_DataLen = mSwapEndian(len);
	Bulk_Only_Cmd->mCBW.mCBW_Flag = 0x00;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Len = 0x0a;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[0] = 0x2a; /* 命令码 */
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[1] = 0x00;
#ifdef BIG_ENDIAN
	*(u32 *)&Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[2] = Sector;
	*(u16 *)&Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[7] = Count;
#else
	*(u32 *)&Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[2] = mSwapEndian(Sector);
	*(u16 *)&Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[7] = mSwapEndian(Count);
#endif
	s = USBHOST_BulkOnly(HubIndex, Edp, Bulk_Only_Cmd, &CtlTrans, DataBuffer, &len); /* 执行基于BulkOnly协议的命令 */
	return s;
}
/* 读磁盘扇区 */
u8 Read_DiskSec(u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd, u8 *DataBuffer, u32 Sector, u16 Count)
{
	u8 s;
	u32 len;
	/* 设置待操作Hub口 */
	SelectHub(HubIndex);
	/* 设定SCSI/UFI请求 */
	memset(Bulk_Only_Cmd, 0, sizeof(BULK_ONLY_CMD)); /* 清空结构体 */
	len = Count * Sec_Len[HubIndex];
	Bulk_Only_Cmd->mCBW.mCBW_DataLen = mSwapEndian(len);
	Bulk_Only_Cmd->mCBW.mCBW_Flag = 0x80;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Len = 0x0a;
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[0] = 0x28; /* 命令码 */
	Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[1] = 0x00;
#ifdef BIG_ENDIAN
	*(u32 *)&Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[2] = Sector;
	*(u16 *)&Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[7] = Count;
#else
	*(u32 *)&Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[2] = mSwapEndian(Sector);
	*(u16 *)&Bulk_Only_Cmd->mCBW.mCBW_CB_Buf[7] = mSwapEndian(Count);
#endif
	s = USBHOST_BulkOnly(HubIndex, Edp, Bulk_Only_Cmd, &CtlTrans, DataBuffer, &len); /* 执行基于BulkOnly协议的命令 */
	return s;
}

/* // 标准 USB 键盘报告格式
uint8_t KeyboardReport[8] = {
	ModifierKeys,  // [0] 修饰键: Ctrl, Shift, Alt, GUI
	Reserved,      // [1] 保留，必须为0
	KeyCode1,      // [2] 第一个按下的键
	KeyCode3,      // [3] 第二个按下的键
	KeyCode4,      // [4] 第三个按下的键
	KeyCode5,      // [5] 第四个按下的键
	KeyCode6,      // [6] 第五个按下的键
	KeyCode7       // [7] 第六个按下的键
}; */
/* 操作HUB设备 */
void Operate_Hub(u8 HubIndex)
{
    u8 i;
    u16 j;
    u8 num;
    u8 s;
    static u8 try_times = 0; /* 失败重试次数 */
    u32 len;
    u8 *Edp[2]; /* 保存端点信息首地址 */
    const ch374_event_cb_t *cb = ch374_get_event_cb();

    for (i = 0; i < NUM_ITF; i++) // 对同一设备的不同接口分别分析、操作
    {
        switch (HUB[HubIndex].ITF[i].Class)
        {
            case USB_DEV_CLASS_HUMAN_IF:
                if (HUB[HubIndex].ITF[i].Protocol == 0x02)   // 鼠标
                {
                    num = Get_Interrupt_Edp(HubIndex, i, &Edp[0]);
                    for (j = 0; j < num; j++)
                    {
                        s = Get_HidData(HubIndex, &Edp[j], Buffer, &len);
                        if (s == Success && len >= 4)
                        {
                            int8_t mx, my, wheel;
                            uint8_t buttons = Buffer[0] & 0x07;      // bit0=左 bit1=右 bit2=中
                            uint8_t changed = buttons ^ s_last_buttons[HubIndex];

                            if (len >= 5) {
                                /* 12-bit 高精度鼠标 (Report Protocol 常见):
                                 * [buttons+填充 1B][X:12bit LE][Y:12bit LE][Wheel 1B] */
                                uint16_t raw_x = (uint16_t)(Buffer[1] | ((uint16_t)(Buffer[2] & 0x0F) << 8));
                                uint16_t raw_y = (uint16_t)((Buffer[2] >> 4) | ((uint16_t)Buffer[3] << 4));
                                mx  = (int8_t)((int16_t)(raw_x << 4) >> 4);  // 12bit -> 8bit
                                my  = (int8_t)((int16_t)(raw_y << 4) >> 4);
                                wheel = (int8_t)Buffer[4];
                            } else {
                                /* 标准 Boot Mouse: [buttons, X, Y, Wheel] */
                                mx = (int8_t)Buffer[1];
                                my = (int8_t)Buffer[2];
                                wheel = (int8_t)Buffer[3];
                            }

                            if (cb != NULL) {
                                if (mx != 0 || my != 0)
                                    cb->on_mouse_move(HubIndex, mx, my);
                                if (changed)
                                    cb->on_mouse_button(HubIndex, buttons, changed);
                                if (wheel != 0)
                                    cb->on_mouse_wheel(HubIndex, wheel);
                            }
                            s_last_buttons[HubIndex] = buttons;
                        }
                    }
                }
                else if (HUB[HubIndex].ITF[i].Protocol == 0x01)  // 键盘 (Boot Keyboard)
                {
                    num = Get_Interrupt_Edp(HubIndex, i, &Edp[0]);
                    for (j = 0; j < num; j++)
                    {
                        s = Get_HidData(HubIndex, &Edp[j], Buffer, &len);
                        if (s == Success && len >= 8)
                        {
                            uint8_t current_key = Buffer[2];
                            int is_shift = (Buffer[0] & 0x22);

                            // 按键去重逻辑：只有当按键状态改变时才处理
                            if (current_key != s_last_key[HubIndex])
                            {
                                if (current_key != 0) // 只处理按下事件（跳过弹起）
                                {
                                    uint8_t ascii = 0;
                                    if (current_key >= 0x04 && current_key <= 0x38) // 字母/数字/符号/Enter/Backspace/Space
                                        ascii = is_shift ? key_map_shift[current_key - 0x04] : key_map_normal[current_key - 0x04];

                                    if (cb != NULL && ascii != 0)
                                        cb->on_key(HubIndex, ascii);
                                }
                                // 更新旧按键状态
                                s_last_key[HubIndex] = current_key;
                            }
                        }
                    }
                }
                // Protocol==0x00 的 HID 接口（如多媒体/消费类键盘）不解析，跳过
                break;

            case USB_DEV_CLASS_STORAGE:
                if ((HUB[HubIndex].ITF[i].SubClass == 0x06) && (HUB[HubIndex].ITF[i].Protocol == 0x50))
                {
                    s = Get_Bulk_Edp(HubIndex, i, &Edp[0]);
                    if (s != 2) return;
                    
                    switch (Udisk_Oper_State[HubIndex])
                    {
                        case DiskInfo:
                            len = 0x24;
                            s = Get_DiskInfo(HubIndex, &Edp[0], &mBOC, Buffer, &len);
                            if (s == Success)
                            {
                                if (mBOC.mCSW.mCSW_Status)
                                {
                                    len = 0x12;
                                    Check_Erro(HubIndex, &Edp[0], &mBOC, Buffer, &len);
                                    if (++try_times > 4) { try_times = 0; Udisk_Oper_State[HubIndex] = DiskCapacity; }
                                }
                                else
                                {
                                    for (j = 0; j < 28; j++) EUSB_LOG("%c", (&(((P_INQUIRY_DATA)Buffer)->VendorIdStr))[j]);
                                    EUSB_LOG("\n");
                                    try_times = 0;
                                    Udisk_Oper_State[HubIndex] = DiskCapacity;
                                }
                            }
                            Udisk_Oper_State[HubIndex] = DiskCapacity;
                            break;
                            
                        case DiskCapacity:
                            len = 0x08;
                            s = Get_DiskCapacity(HubIndex, &Edp[0], &mBOC, Buffer, &len);
                            if (s == Success)
                            {
                                if (mBOC.mCSW.mCSW_Status)
                                {
                                    len = 0x12;
                                    Check_Erro(HubIndex, &Edp[0], &mBOC, Buffer, &len);
                                    if (++try_times > 4) { try_times = 0; Udisk_Oper_State[HubIndex] = Undef; }
                                }
                                else
                                {
#ifdef BIG_ENDIAN
                                    Sec_Len[HubIndex] = *(u32 *)(Buffer + 4);
#else
                                    Sec_Len[HubIndex] = mSwapEndian(*(u32 *)(Buffer + 4));
#endif
                                    try_times = 0;
                                    Udisk_Oper_State[HubIndex] = ReadSec;
                                }
                            }
                            break;

                        case Write_Sec:
                            if (Write_DiskSec(HubIndex, &Edp[0], &mBOC, Buffer, 1, 1) == Success)
                                Udisk_Oper_State[HubIndex] = ReadSec;
                            else
                                Udisk_Oper_State[HubIndex] = Undef;
                            break;

                        case ReadSec:
                            memset(Buffer, 0, sizeof(Buffer));
                            s = Read_DiskSec(HubIndex, &Edp[0], &mBOC, Buffer, 1, 1);
                            Udisk_Oper_State[HubIndex] = Undef;
                            break;

                        default:
                            TestUnit(HubIndex, &Edp[0], &mBOC);
                            Udisk_Oper_State[HubIndex] = Undef;
                            break;
                    }
                }
                break;

            case USB_DEV_CLASS_PRINTER:  // 打印机接口
                break;

            default:
                break;
        } // 结束 switch
    } // 结束 for
}

/* 分析Hub下设备信息，并操作 */
void Operate_Hub_Device()
{
	u8 HubIndex;
	for (HubIndex = 0; HubIndex < 3; HubIndex++)
	{
		if (HUB[HubIndex].DeviceStatus == InitComplete)
		{
			Operate_Hub(HubIndex);
		}
	}
}
