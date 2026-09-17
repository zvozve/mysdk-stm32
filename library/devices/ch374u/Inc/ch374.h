#ifndef __CH374_H
#define __CH374_H

/* Includes ------------------------------------------------------------------*/
#include "hal_platform.h"   /* 系列无关 HAL 入口（本文件仅用基础类型） */
#include "ch374_sys.h"
#include "ch374inc.h"
#include "string.h"
#include "stdbool.h"
/* Exported types ------------------------------------------------------------*/
/* Exported constants --------------------------------------------------------*/
/* Exported macro ------------------------------------------------------------*/
/* Exported functions ------------------------------------------------------- */
//#define BIG_ENDIAN
//#define NUM_ITF   2                //兼容两个接口
//#define NUM_EDP   3                //每个接口下兼容3个端点

//#define    DefEp0MaxPacket  8       //所有HUB初始默认端点0包长为8

//#define    Success          0x14
//#define    Failure          0xff
//#define    Disable          0
//#define    Enable           1
////DeviceStatus
//#define    UnConnect        0
//#define    UnInit           1
//#define    InitComplete     2
////DeviceSpeed
//#define    FullSpeed        0
//#define    LowSpeed         1


//#ifdef BIG_ENDIAN
//#define USB_BO_CBW_SIG		0x55534243	/* 命令块CBW识别标志'USBC' */
//#define USB_BO_CSW_SIG		0x55534253	/* 命令状态块CSW识别标志'USBS' */
//#else
//#define USB_BO_CBW_SIG		0x43425355	/* 命令块CBW识别标志'USBC' */
//#define USB_BO_CSW_SIG		0x53425355	/* 命令状态块CSW识别标志'USBS' */
//#endif

//#define USB_BO_CBW_SIZE			0x1F	/* 命令块CBW的总长度 */
//#define USB_BO_CSW_SIZE			0x0D	/* 命令状态块CSW的总长度 */

//typedef enum _UDISK_State
//{
//	DiskInfo,DiskCapacity,Write_Sec,ReadSec,Undef,
//} UDISK_State;


//typedef struct EDP_INFO_{
//	u8 EDPNum;                  //端点号(包含输入输出信息)
//	u8 Attributes;              //端点类型
//	u8 Tog;                     //端点翻转状态
//	u8 MaxPacket;               //最大包大小
//} EDP_INFO,*PEDP_INFO;
//typedef struct INTER_INFO_{
//	u8 ITFNum;                  //接口号
//	u8 Class;                   //类
//	u8 SubClass;                //子类
//	u8 Protocol;                //协议码
//	u16 HID_Desc_Len;           //HID描述符长度，如果为0，表示不存在
//	u8 NumEndpoints;            //接口端点数	
//	EDP_INFO Endp[NUM_EDP];        //接口下定义4个端点集合
//} INTER_INFO;

//typedef struct DEV_INFO_{
//	u8 DeviceStatus;             //0:未连接 ； 1：刚连接未使能初始化 ； 2：初始化完成（已配置）
//	u8 InitFailTimes;            //初始化失败次数计数，失败三次后禁止端口
//	u8 DeviceSpeed;              //0:12Mbps ； 1:1.5Mbps
//	u8 DeviceAddress;            //设备地址。hub0：1  hub1: 2  hub2: 3
//	u8 Ep0MaxPacket;             //端点0最大包大小
//	u8 ConfigurationValue;       //配置值
//	u8 NumInterfaces;            //接口数
//	INTER_INFO ITF[NUM_ITF];        //定义两个接口
//} DEV_INFO;

//typedef union _BULK_ONLY_CMD {
//	struct {
//		u32	mCBW_Sig;
//		u32	mCBW_Tag;
//		u32	mCBW_DataLen;			/* 输入: 数据传输长度 */
//		u8	mCBW_Flag;				/* 输入: 传输方向等标志 */
//		u8	mCBW_LUN;
//		u8	mCBW_CB_Len;			/* 输入: 命令块的长度,有效值是1到16 */
//		u8	mCBW_CB_Buf[16];		/* 输入: 命令块,该缓冲区最多为16个字节 */
//	} mCBW;								/* BulkOnly协议的命令块, 输入CBW结构 */
//	struct {
//		u32	mCSW_Sig;
//		u32	mCSW_Tag;
//		u32	mCSW_Residue;			/* 返回: 剩余数据长度 */
//		u8	mCSW_Status;			/* 返回: 命令执行结果状态 */
//	} mCSW;								/* BulkOnly协议的命令状态块, 输出CSW结构 */
//} BULK_ONLY_CMD,*PBULK_ONLY_CMD;

///* INQUIRY命令的返回数据 */
//typedef struct _INQUIRY_DATA {
//	u8	DeviceType;					/* 00H, 设备类型 */
//	u8	RemovableMedia;				/* 01H, 位7为1说明是移动存储 */
//	u8	Versions;					/* 02H, 协议版本 */
//	u8	DataFormatAndEtc;			/* 03H, 指定返回数据格式 */
//	u8	AdditionalLength;			/* 04H, 后续数据的长度 */
//	u8	Reserved1;
//	u8	Reserved2;
//	u8	MiscFlag;					/* 07H, 一些控制标志 */
//	u8	VendorIdStr[8];				/* 08H, 厂商信息 */
//	u8	ProductIdStr[16];			/* 10H, 产品信息 */
//	u8	ProductRevStr[4];			/* 20H, 产品版本 */
//} INQUIRY_DATA, *P_INQUIRY_DATA;		/* 24H */

//void ClearHub( u8 HubIndex );
//void HostMode_Init(void);
//void Spi374Stop( void );  /* SPI结束 */
//void Spi374Start( u8 addr, u8 cmd );  /* SPI开始 */
//void Write374Byte( u8 mAddr, u8 mData )  ;
//u8 Read374Byte( u8 mAddr );
//void Write374Block( u8 mAddr, u8 mLen, u8 *mBuf ) ;/* 外部定义的被CH374程序库调用的子程序,向指定起始地址写入数据块 */
//void Read374Block( u8 mAddr, u8 mLen, u8 *mBuf ) ;  /* 外部定义的被CH374程序库调用的子程序,从指定起始地址读出数据块 */
//void	Modify374Byte( u8 mAddr, u8 mAndData, u8 mOrData );
//void ScanConnect(void) ;
//void CheckHubConnect( u8 HubIndex );
//void SelectHub( u8 HubIndex );
//u8	Wait374Interrupt( void );
//u8	Query374Interrupt( void );
///* HID类命令 Set_Report */
//u8 Set_Report( u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 *DataBuffer, u16 *len );
///* HID类命令 Get_Report */
//u8 Get_Report( u8 HubIndex, PUSB_SETUP_REQ Setup_Req ,u8 Itf_Num, u8 *DataBuffer, u16 *len );
///* HID类命令 Set_Idle */
//u8 Set_Idle( u8 HubIndex, PUSB_SETUP_REQ Setup_Req ,u8 Itf_Num);
///* 设置配置 */
//u8 Set_Config( u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 Cfg_Value );
///* 分析配置描述符 */
//u8 Analy_CfgDesc( u8 HubIndex, u8 *DataBuffer );
///* 获取配置描述符 */
///* len表示输入输出参数长度 ,返回执行状态 */
//u8 Get_CfgDesc( u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 *DataBuffer, u16 *len );
///* 设置设备地址 */  /* 返回操作状态 */
//u8 Set_DevAddr( u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 addr );
///* 获取设备描述符 */
///* len表示输入输出参数长度 ,返回执行状态 */
//u8 Get_DevDesc( u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 *DataBuffer, u16 *len );
///* 清除端点特性 */ 
//u8 Clear_Feature( u8 HubIndex, u8 Edp, PUSB_SETUP_REQ Setup_Req );

///* 执行控制传输 */ 
//u8 USBHOST_Issue_Control( u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 *DataBuffer, u16 *len );
///* 事物传输 */ /* SET IN OUT */ /* Timeout=0不重试 Timeout=0xffff无限重试 ,返回PID*/ 
//u8 Issue_Token( u8 PID, u8 Endp, u8 Tog, u16 Timeout );

///* 设备拔插检测 */ 
//void Dev_Detect(void);
///* 根据拔插事件搜索需要初始化的Hub端口设备 */
//void Init_USB_Device(void);
///* 初始化该Hub端口设备 */
//void Init_Hub( u8 HubIndex );
///* 分析Hub下设备信息，并操作 */
//void Operate_Hub_Device(void);
///* 操作HUB设备 */
//void Operate_Hub( u8 HubIndex );
///* 分析接口信息，获取中断端点 *//* Edp[0]、Edp[1]保存上传结构体地址 返回中断端点个数 */
//u8 Get_Interrupt_Edp( u8 HubIndex, u8 ITFNum, u8 **Edp );
//extern u8 Get_HidData( u8 HubIndex, u8 **Edp, u8 *DataBuffer, u32 *len );
///* 分析接口信息，获取批量端点 *//* Edp[0]保存下传、Edp[1]保存上传 返回端点个数 */
//extern u8 Get_Bulk_Edp( u8 HubIndex, u8 ITFNum, u8 **Edp );
///* 获取磁盘信息 */
//extern u8 Get_DiskInfo( u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd, u8 *DataBuffer, u32 *len );
///* 获取磁盘容量 */
//extern u8 Get_DiskCapacity( u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd, u8 *DataBuffer, u32 *len );
///* 检查磁盘错误 */
//extern u8 Check_Erro( u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd, u8 *DataBuffer, u32 *len );
///* 大小端数据转换 */
//u32	mSwapEndian( u32 dat );
///* Bulk传输 */
//u8 USBHOST_BulkOnly( u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd, PUSB_SETUP_REQ Setup_Req, u8 *DataBuffer, u32 *len );

/* ============================================================
 * 驱动内部函数声明区
 *
 * 说明：原 ch374_sys.h 承载了这些 extern 声明；本 SDK 版把 ch374_sys.h 精简为
 * 纯数据类型定义后，函数声明归位到本模块公共头 —— ch374.c 内部存在
 * 「先使用后定义」的调用顺序（如 Query374Interrupt 调用 Read374Byte），
 * 必须保留声明，否则 C11 下会触发隐式函数声明。
 * ============================================================ */
extern void CH374_PORT_INIT(void);
extern u8 Read374Byte( u8 mAddr );
extern void Write374Byte( u8 mAddr, u8 mData );
extern void Modify374Byte( u8 mAddr, u8 mAndData, u8 mOrData );
extern void Read374Block( u8 mAddr, u8 mLen, PUINT8 mBuf );
extern void Write374Block( u8 mAddr, u8 mLen, PUINT8 mBuf );
extern u8 Query374Interrupt( void );
extern u8 Wait374Interrupt( void ) ;
extern void Spi374Start( u8 addr, u8 cmd );
extern void Spi374Stop( void );
extern void HostMode_Init(void);
extern void ScanConnect(void);
extern void Dev_Detect(void);
extern void Init_USB_Device(void);
extern void Operate_Hub_Device(void);
/* 注：RecvData_t 的 extern 声明在本文件末尾（RECV_USB_DATA 类型定义之后） */

/* 变量定义区 */
#define BIG_ENDIAN

#define NUM_ITF   2                //兼容两个接口
#define NUM_EDP   3                //每个接口下兼容3个端点

#ifdef BIG_ENDIAN
#define USB_BO_CBW_SIG		0x55534243	/* 命令块CBW识别标志'USBC' */
#define USB_BO_CSW_SIG		0x55534253	/* 命令状态块CSW识别标志'USBS' */
#else
#define USB_BO_CBW_SIG		0x43425355	/* 命令块CBW识别标志'USBC' */
#define USB_BO_CSW_SIG		0x53425355	/* 命令状态块CSW识别标志'USBS' */
#endif

#define USB_BO_CBW_SIZE			0x1F	/* 命令块CBW的总长度 */
#define USB_BO_CSW_SIZE			0x0D	/* 命令状态块CSW的总长度 */


typedef struct EDP_INFO_{
	u8 EDPNum;                  //端点号(包含输入输出信息)
	u8 Attributes;              //端点类型
	u8 Tog;                     //端点翻转状态
	u8 MaxPacket;               //最大包大小
} EDP_INFO,*PEDP_INFO;
typedef struct INTER_INFO_{
	u8 ITFNum;                  //接口号
	u8 Class;                   //类
	u8 SubClass;                //子类
	u8 Protocol;                //协议码
	u16 HID_Desc_Len;           //HID描述符长度，如果为0，表示不存在
	u8 NumEndpoints;            //接口端点数	
	EDP_INFO Endp[NUM_EDP];        //接口下定义4个端点集合
} INTER_INFO;

typedef struct DEV_INFO_{
	u8 DeviceStatus;             //0:未连接 ； 1：刚连接未使能初始化 ； 2：初始化完成（已配置）
	u8 InitFailTimes;            //初始化失败次数计数，失败三次后禁止端口
	u8 DeviceSpeed;              //0:12Mbps ； 1:1.5Mbps
	u8 DeviceAddress;            //设备地址。hub0：1  hub1: 2  hub2: 3
	u8 Ep0MaxPacket;             //端点0最大包大小
	u8 ConfigurationValue;       //配置值
	u8 NumInterfaces;            //接口数
	INTER_INFO ITF[NUM_ITF];        //定义两个接口
} DEV_INFO;

typedef union _BULK_ONLY_CMD {
	struct {
		u32	mCBW_Sig;
		u32	mCBW_Tag;
		u32	mCBW_DataLen;			/* 输入: 数据传输长度 */
		u8	mCBW_Flag;				/* 输入: 传输方向等标志 */
		u8	mCBW_LUN;
		u8	mCBW_CB_Len;			/* 输入: 命令块的长度,有效值是1到16 */
		u8	mCBW_CB_Buf[16];		/* 输入: 命令块,该缓冲区最多为16个字节 */
	} mCBW;								/* BulkOnly协议的命令块, 输入CBW结构 */
	struct {
		u32	mCSW_Sig;
		u32	mCSW_Tag;
		u32	mCSW_Residue;			/* 返回: 剩余数据长度 */
		u8	mCSW_Status;			/* 返回: 命令执行结果状态 */
	} mCSW;								/* BulkOnly协议的命令状态块, 输出CSW结构 */
} BULK_ONLY_CMD,*PBULK_ONLY_CMD;

/* INQUIRY命令的返回数据 */
typedef struct _INQUIRY_DATA {
	u8	DeviceType;					/* 00H, 设备类型 */
	u8	RemovableMedia;				/* 01H, 位7为1说明是移动存储 */
	u8	Versions;					/* 02H, 协议版本 */
	u8	DataFormatAndEtc;			/* 03H, 指定返回数据格式 */
	u8	AdditionalLength;			/* 04H, 后续数据的长度 */
	u8	Reserved1;
	u8	Reserved2;
	u8	MiscFlag;					/* 07H, 一些控制标志 */
	u8	VendorIdStr[8];				/* 08H, 厂商信息 */
	u8	ProductIdStr[16];			/* 10H, 产品信息 */
	u8	ProductRevStr[4];			/* 20H, 产品版本 */
} INQUIRY_DATA, *P_INQUIRY_DATA;		/* 24H */

typedef enum _UDISK_State
{
	DiskInfo,DiskCapacity,Write_Sec,ReadSec,Undef,
} UDISK_State;

typedef struct RECV_USB_INFO {
	u8 receiveStr[256];				/* 数据接收 */
	u8 receiveLen;					/* 接收长度 */
	uint16_t validcount;		    /* 保存有效设备个数 */
	bool isRecvEnd;					/* 是否接收结束 */
	u8 changeCount;						/* 是否接收开始 */
	u8 lastNum;					/* 保存上一个设备的序号 */
} RECV_USB_DATA;

#define    DefEp0MaxPacket  8       //所有HUB初始默认端点0包长为8

#define    Success          0x14
#define    Failure          0xff
#define    Disable          0
#define    Enable           1

//DeviceStatus
#define    UnConnect        0
#define    UnInit           1
#define    InitComplete     2

//DeviceSpeed
#define    FullSpeed        0
#define    LowSpeed         1

/* 函数声明区 */
extern void CH374_PORT_INIT(void); 
extern u8 Read374Byte( u8 mAddr );
extern void Write374Byte( u8 mAddr, u8 mData );
extern void Modify374Byte( u8 mAddr, u8 mAndData, u8 mOrData );
extern void Read374Block( u8 mAddr, u8 mLen, PUINT8 mBuf );
extern void Write374Block( u8 mAddr, u8 mLen, PUINT8 mBuf );
extern u8 Query374Interrupt( void );
extern u8 Wait374Interrupt( void ) ;

extern void ClearHub( u8 HubIndex );         /* 复位HUB结构体 */
extern void CheckHubConnect( u8 HubIndex );  /* 检查HUB连接状态 */
extern void ResetHub( u8 HubIndex );         /* 复位HUB口 */
extern void HubPort_Cmd( u8 HubIndex, u8 Status );                                             /* HUB端口使能或禁止使能 */ 
extern void SelectHub( u8 HubIndex );        /* 设置待操作Hub口 */
extern u8 Issue_Token( u8 PID, u8 Endp, u8 Tog, u16 Timeout );                        /* 事物传输 */
extern u8 USBHOST_Issue_Control( u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 *DataBuffer, u16 *len );   /* 执行控制传输 */ /* 被调用，需选端口和参数 */
extern u8 Clear_Feature( u8 HubIndex, u8 Edp, PUSB_SETUP_REQ Setup_Req );                   /* 清除端点特性 */ 
extern u8 Get_DevDesc( u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 *DataBuffer, u16 *len); /* 获取设备描述符 */
extern u8 Set_DevAddr( u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 addr );                    /* 设置设备地址 */
extern u8 Get_CfgDesc( u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 *DataBuffer, u16 *len); /* 获取配置描述符 */
extern u8 Analy_CfgDesc( u8 HubIndex, u8 *DataBuffer );                                     /* 分析配置描述符 */
extern u8 Set_Config( u8 HubIndex,  PUSB_SETUP_REQ Setup_Req, u8 Cfg_Value );               /* 设置配置 */
extern u8 Set_Idle( u8 HubIndex, PUSB_SETUP_REQ Setup_Req ,u8 Itf_Num);                     /* Set_Idle */
extern u8 Set_Protocol( u8 HubIndex, PUSB_SETUP_REQ Setup_Req ,u8 Itf_Num, u8 protocol );   /* Set_Protocol */
extern u8 Get_Report( u8 HubIndex, PUSB_SETUP_REQ Setup_Req ,u8 Itf_Num, u8 *DataBuffer, u16 *len );  /* Get_Report */
extern u8 Set_Report( u8 HubIndex, PUSB_SETUP_REQ Setup_Req ,u8 *DataBuffer, u16 *len );                 /* Set_Report */
extern u8 Get_MaxLun( u8 HubIndex, PUSB_SETUP_REQ Setup_Req, u8 *DataBuffer, u16 *len );                 /* Get_MaxLun */
extern u8 Class_Issue( u8 HubIndex );     /* 类命令处理 */
extern u8 EnumHub( u8 HubIndex );         /* 枚举设备 */
extern void Init_Hub( u8 HubIndex );         /* 初始化该Hub端口设备 */

extern u32 mSwapEndian( u32 dat );        /* 大小端数据转换 */
extern u8 USBHOST_BulkOnly( u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd, PUSB_SETUP_REQ Setup_Req, u8 *DataBuffer, u32 *len );/* Bulk传输 */
extern u8 Get_Bulk_Edp( u8 HubIndex, u8 ITFNum, u8 **Edp );     /* 分析接口信息，获取批量端点 *//* Edp[0]保存下传、Edp[1]保存上传 返回中断端点个数*/
extern u8 Get_Interrupt_Edp( u8 HubIndex, u8 ITFNum, u8 **Edp );/* 分析接口信息，获取中断端点 *//* Edp[0]、Edp[1]保存上传结构体地址 返回中断端点个数 */
extern u8 Get_DiskInfo( u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd, u8 *DataBuffer, u32 *len ); /* 获取磁盘信息 */
extern u8 Get_DiskCapacity( u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd, u8 *DataBuffer, u32 *len ); /* 获取磁盘容量 */
extern u8 Check_Erro( u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd, u8 *DataBuffer, u32 *len );   /* 检查磁盘错误 */
extern u8 TestUnit( u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd );                                     /* 测试准备好与否 */
extern u8 Write_DiskSec( u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd, u8 *DataBuffer, u32 Sector, u16 Count );/* 写磁盘扇区 */
extern u8 Read_DiskSec( u8 HubIndex, u8 **Edp, PBULK_ONLY_CMD Bulk_Only_Cmd, u8 *DataBuffer, u32 Sector, u16 Count ); /* 读磁盘扇区 */
extern void Operate_Hub( u8 HubIndex );      /* 操作HUB设备 */


/* 外部调用 */
extern void HostMode_Init(void);                    /* 初始化主机HUB模式 */
extern void ScanConnect(void);                      /* 查询拔插事件 */ /* 主函数内只能执行一次，防止上电之前已有设备，以后只能由Dev_Detect调用 */
extern void Dev_Detect(void);                       /* 设备拔插检测 */ 
extern void Init_USB_Device(void);                  /* 根据拔插事件搜索需要初始化的Hub端口设备 */
extern void Operate_Hub_Device(void);               /* 分析Hub下设备信息，并操作 */
extern RECV_USB_DATA RecvData_t;
#endif /* __CH374_H */
