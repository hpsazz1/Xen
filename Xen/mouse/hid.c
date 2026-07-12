/*
 * hid.c —— Windows HID API 底层实现
 *
 * 本文件提供了 hidapi（HID 应用程序接口）在 Windows 平台上的完整实现，
 * 负责处理 HID（人机交互设备）的发现、打开、关闭、读写报告以及功能报告操作。
 * 它通过动态加载 hid.dll 并使用 SetupAPI 进行设备枚举来实现与 HID 设备的通信，
 * 无需依赖 Windows DDK（驱动程序开发套件）。
 *
 * 主要功能：
 *   - 动态加载 hid.dll 并解析所有 HID API 函数指针
 *   - 使用 SetupAPI（SetupDiGetClassDevsA 等）枚举 HID 类设备
 *   - 通过 CreateFile 打开设备并设置重叠 I/O（Overlapped I/O）进行异步读写
 *   - 同步写操作（WriteFile + GetOverlappedResult）
 *   - 支持超时的重叠读操作（ReadFile + WaitForSingleObject）
 *   - 发送/获取功能报告（Feature Report）
 *   - 查询制造商、产品、序列号字符串
 *   - 解析设备路径中的接口编号
 */

#include <windows.h>

#ifndef _NTDEF_
typedef LONG NTSTATUS;
#endif

#ifdef __MINGW32__
#include <ntdef.h>
#include <winbase.h>
#endif

#ifdef __CYGWIN__
#include <ntdef.h>
#define _wcsdup wcsdup
#endif

/* 传递给 HidD_Get*String 系列函数的最大字符数，超过此值将导致调用失败 */
#define MAX_STRING_WCHARS 0xFFF

/*#define HIDAPI_USE_DDK*/

#ifdef __cplusplus
extern "C" {
#endif
		#include <setupapi.h>
		#include <winioctl.h>
		#ifdef HIDAPI_USE_DDK
			#include <hidsdi.h>
		#endif

		/* 以下宏定义复制自 Windows DDK 中的 inc/ddk/hidclass.h */
		#define HID_OUT_CTL_CODE(id)  \
			CTL_CODE(FILE_DEVICE_KEYBOARD, (id), METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
		#define IOCTL_HID_GET_FEATURE                   HID_OUT_CTL_CODE(100)

#ifdef __cplusplus
} /* extern "C" */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>


#include "hidapi.h"

#undef MIN
#define MIN(x,y) ((x) < (y)? (x): (y))

/**
 * size_to_dword —— 将 size_t 值安全转换为 DWORD
 *
 * 检查传入的 size_t 值是否超出 DWORD 的最大范围（MAXDWORD），
 * 防止因类型转换导致的数据截断或溢出。如果值合法，则通过输出参数
 * 返回转换后的 DWORD 值。
 *
 * @param value  待转换的 size_t 值
 * @param out    输出参数，接收转换后的 DWORD 值
 * @return       转换成功返回 1，失败（指针为空或值溢出）返回 0
 */
static int size_to_dword(size_t value, DWORD *out)
{
	if (!out || value > (size_t)MAXDWORD)
		return 0;
	*out = (DWORD)value;
	return 1;
}

/**
 * size_to_ulong —— 将 size_t 值安全转换为 ULONG
 *
 * 检查传入的 size_t 值是否超出 ULONG 的最大范围（ULONG_MAX），
 * 确保在调用需要 ULONG 参数的系统 API 时不会发生数据溢出。
 *
 * @param value  待转换的 size_t 值
 * @param out    输出参数，接收转换后的 ULONG 值
 * @return       转换成功返回 1，失败返回 0
 */
static int size_to_ulong(size_t value, ULONG *out)
{
	if (!out || value > (size_t)ULONG_MAX)
		return 0;
	*out = (ULONG)value;
	return 1;
}

/**
 * size_to_int —— 将 size_t 值安全转换为 int
 *
 * 检查传入的 size_t 值是否超出 int 的最大范围（INT_MAX），
 * 用于需要以 int 类型返回数据长度的场景。
 *
 * @param value  待转换的 size_t 值
 * @param out    输出参数，接收转换后的 int 值
 * @return       转换成功返回 1，失败返回 0
 */
static int size_to_int(size_t value, int *out)
{
	if (!out || value > (size_t)INT_MAX)
		return 0;
	*out = (int)value;
	return 1;
}

#ifdef _MSC_VER
		/* 禁用 MSVC 关于 strncpy 的安全警告，因为本代码正确使用了 strncpy */
		#pragma warning(disable:4996)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef HIDAPI_USE_DDK
		/*
		 * 由于没有使用 DDK 构建，且 HID 头文件不属于 Windows SDK 标准组成部分，
		 * 因此需要在此处自行定义所有相关结构和类型。
		 * 在 lookup_functions() 函数中，下方定义的函数指针将被动态解析并赋值。
		 */

		/**
		 * HIDD_ATTRIBUTES —— HID 设备属性结构体
		 *
		 * 包含 HID 设备的基本标识信息：供应商 ID（VID）、产品 ID（PID）和版本号。
		 * 通过 HidD_GetAttributes 函数获取。
		 */
		typedef struct _HIDD_ATTRIBUTES{
			ULONG Size;           /* 结构体大小，调用前必须初始化 */
			USHORT VendorID;      /* 供应商 ID（VID） */
			USHORT ProductID;     /* 产品 ID（PID） */
			USHORT VersionNumber; /* 设备版本号 */
		} HIDD_ATTRIBUTES, *PHIDD_ATTRIBUTES;

		typedef USHORT USAGE;

		/**
		 * HIDP_CAPS —— HID 设备能力结构体
		 *
		 * 描述 HID 设备的能力信息，包括用途（Usage）、用途页（UsagePage）、
		 * 以及输入/输出/功能报告的长度。通过 HidP_GetCaps 函数获取。
		 */
		typedef struct _HIDP_CAPS {
			USAGE Usage;                     /* HID 用途 */
			USAGE UsagePage;                 /* HID 用途页 */
			USHORT InputReportByteLength;    /* 输入报告字节长度 */
			USHORT OutputReportByteLength;   /* 输出报告字节长度 */
			USHORT FeatureReportByteLength;  /* 功能报告字节长度 */
			USHORT Reserved[17];             /* 保留字段 */
			USHORT fields_not_used_by_hidapi[10]; /* hidapi 未使用的字段 */
		} HIDP_CAPS, *PHIDP_CAPS;
		typedef void* PHIDP_PREPARSED_DATA;
		#define HIDP_STATUS_SUCCESS 0x110000

		/* ---------- HID API 函数指针类型定义 ---------- */

		/** HidD_GetAttributes_ 类型：获取 HID 设备属性 */
		typedef BOOLEAN (__stdcall *HidD_GetAttributes_)(HANDLE device, PHIDD_ATTRIBUTES attrib);
		/** HidD_GetSerialNumberString_ 类型：获取设备序列号字符串 */
		typedef BOOLEAN (__stdcall *HidD_GetSerialNumberString_)(HANDLE device, PVOID buffer, ULONG buffer_len);
		/** HidD_GetManufacturerString_ 类型：获取制造商字符串 */
		typedef BOOLEAN (__stdcall *HidD_GetManufacturerString_)(HANDLE handle, PVOID buffer, ULONG buffer_len);
		/** HidD_GetProductString_ 类型：获取产品字符串 */
		typedef BOOLEAN (__stdcall *HidD_GetProductString_)(HANDLE handle, PVOID buffer, ULONG buffer_len);
		/** HidD_SetFeature_ 类型：向设备发送功能报告 */
		typedef BOOLEAN (__stdcall *HidD_SetFeature_)(HANDLE handle, PVOID data, ULONG length);
		/** HidD_GetFeature_ 类型：从设备获取功能报告 */
		typedef BOOLEAN (__stdcall *HidD_GetFeature_)(HANDLE handle, PVOID data, ULONG length);
		/** HidD_GetIndexedString_ 类型：通过索引获取设备字符串 */
		typedef BOOLEAN (__stdcall *HidD_GetIndexedString_)(HANDLE handle, ULONG string_index, PVOID buffer, ULONG buffer_len);
		/** HidD_GetPreparsedData_ 类型：获取设备的预解析数据 */
		typedef BOOLEAN (__stdcall *HidD_GetPreparsedData_)(HANDLE handle, PHIDP_PREPARSED_DATA *preparsed_data);
		/** HidD_FreePreparsedData_ 类型：释放预解析数据 */
		typedef BOOLEAN (__stdcall *HidD_FreePreparsedData_)(PHIDP_PREPARSED_DATA preparsed_data);
		/** HidP_GetCaps_ 类型：从预解析数据中获取设备能力信息 */
		typedef NTSTATUS (__stdcall *HidP_GetCaps_)(PHIDP_PREPARSED_DATA preparsed_data, HIDP_CAPS *caps);
		/** HidD_SetNumInputBuffers_ 类型：设置输入缓冲区数量 */
		typedef BOOLEAN (__stdcall *HidD_SetNumInputBuffers_)(HANDLE handle, ULONG number_buffers);

		/* ---------- HID API 静态函数指针 ---------- */

		/** HidD_GetAttributes —— 获取 HID 设备属性（通过 hid.dll 动态加载） */
		static HidD_GetAttributes_ HidD_GetAttributes;
		/** HidD_GetSerialNumberString —— 获取设备序列号字符串（通过 hid.dll 动态加载） */
		static HidD_GetSerialNumberString_ HidD_GetSerialNumberString;
		/** HidD_GetManufacturerString —— 获取制造商字符串（通过 hid.dll 动态加载） */
		static HidD_GetManufacturerString_ HidD_GetManufacturerString;
		/** HidD_GetProductString —— 获取产品字符串（通过 hid.dll 动态加载） */
		static HidD_GetProductString_ HidD_GetProductString;
		/** HidD_SetFeature —— 向设备发送功能报告（通过 hid.dll 动态加载） */
		static HidD_SetFeature_ HidD_SetFeature;
		/** HidD_GetFeature —— 从设备获取功能报告（通过 hid.dll 动态加载） */
		static HidD_GetFeature_ HidD_GetFeature;
		/** HidD_GetIndexedString —— 通过索引获取设备字符串（通过 hid.dll 动态加载） */
		static HidD_GetIndexedString_ HidD_GetIndexedString;
		/** HidD_GetPreparsedData —— 获取设备预解析数据（通过 hid.dll 动态加载） */
		static HidD_GetPreparsedData_ HidD_GetPreparsedData;
		/** HidD_FreePreparsedData —— 释放预解析数据（通过 hid.dll 动态加载） */
		static HidD_FreePreparsedData_ HidD_FreePreparsedData;
		/** HidP_GetCaps —— 从预解析数据中获取能力（通过 hid.dll 动态加载） */
		static HidP_GetCaps_ HidP_GetCaps;
		/** HidD_SetNumInputBuffers —— 设置输入缓冲区数量（通过 hid.dll 动态加载） */
		static HidD_SetNumInputBuffers_ HidD_SetNumInputBuffers;

		/** lib_handle —— 动态链接库 hid.dll 的模块句柄 */
		static HMODULE lib_handle = NULL;
		/** initialized —— 标识 hidapi 是否已完成初始化 */
		static BOOLEAN initialized = FALSE;
#endif /* HIDAPI_USE_DDK */

/**
 * hid_device_ —— HID 设备内部表示结构体
 *
 * 该结构体封装了 Windows 平台上与 HID 设备交互所需的所有状态信息，
 * 包括设备句柄、同步/异步 I/O 状态、报告长度以及错误信息。
 * 用户通过 hid_open / hid_open_path 获取指向此结构的 hid_device 指针，
 * 并对该指针进行后续的所有操作。
 */
struct hid_device_ {
		HANDLE device_handle;         /* Windows 设备句柄（CreateFile 返回） */
		BOOL blocking;                /* 是否阻塞模式读取（TRUE=阻塞，FALSE=非阻塞） */
		USHORT output_report_length;  /* 输出报告字节长度（来自 HIDP_CAPS） */
		size_t input_report_length;   /* 输入报告字节长度（来自 HIDP_CAPS） */
		void *last_error_str;         /* 最近一次错误的错误消息字符串 */
		DWORD last_error_num;         /* 最近一次错误的错误码 */
		BOOL read_pending;            /* 是否有未完成的异步读操作 */
		char *read_buf;               /* 读操作的缓冲区 */
		OVERLAPPED ol;                /* 重叠 I/O 结构体，用于异步操作 */
};

/**
 * new_hid_device —— 创建并初始化一个新的 hid_device 结构体
 *
 * 分配 hid_device 结构体内存并初始化为默认值。
 * 创建 I/O 完成事件对象（手动重置、初始无信号状态），
 * 供后续异步读写操作使用。
 *
 * @return  返回新创建的 hid_device 指针，失败返回 NULL
 */
static hid_device *new_hid_device()
{
	hid_device *dev = (hid_device*) calloc(1, sizeof(hid_device));
	dev->device_handle = INVALID_HANDLE_VALUE;
	dev->blocking = TRUE;
	dev->output_report_length = 0;
	dev->input_report_length = 0;
	dev->last_error_str = NULL;
	dev->last_error_num = 0;
	dev->read_pending = FALSE;
	dev->read_buf = NULL;
	memset(&dev->ol, 0, sizeof(dev->ol));
		dev->ol.hEvent = CreateEvent(NULL, TRUE, FALSE /*初始状态为无信号*/, NULL);  // TRUE=手动重置, 避免重叠I/O信号丢失

	return dev;
}

/**
 * free_hid_device —— 释放 hid_device 结构体及其关联资源
 *
 * 依次释放：I/O 完成事件、设备句柄、错误消息字符串、读缓冲区，
 * 最后释放结构体本身。
 *
 * @param dev  待释放的 hid_device 指针
 */
static void free_hid_device(hid_device *dev)
{
	CloseHandle(dev->ol.hEvent);
	CloseHandle(dev->device_handle);
	LocalFree(dev->last_error_str);
	free(dev->read_buf);
	free(dev);
}

/**
 * register_error —— 注册错误信息到 hid_device
 *
 * 使用 FormatMessageW 将当前线程的最近一次错误码（GetLastError）
 * 转换为可读的 Unicode 错误消息字符串，并保存到设备结构体中，
 * 供 hid_error() 函数后续获取。
 * 同时去除 FormatMessageW 在消息末尾添加的回车换行符。
 *
 * @param device  目标 hid_device 指针
 * @param op      发生错误的操作名称（当前未使用，保留供扩展）
 */
static void register_error(hid_device *device, const char *op)
{
	WCHAR *ptr, *msg;

	FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		GetLastError(),
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPVOID)&msg, 0/*sz*/,
		NULL);

	/* 去除 FormatMessageW 附加在消息末尾的回车和换行符 */
	ptr = msg;
	while (*ptr) {
		if (*ptr == '\r') {
			*ptr = 0x0000;
			break;
		}
		ptr++;
	}

	/* 将错误消息保存到设备条目中，以便 hid_error() 函数能够获取 */
	LocalFree(device->last_error_str);
	device->last_error_str = msg;
}

#ifndef HIDAPI_USE_DDK
/**
 * lookup_functions —— 动态加载 hid.dll 并解析所有 HID API 函数指针
 *
 * 使用 LoadLibraryA 加载系统 hid.dll，然后通过 GetProcAddress
 * 逐一解析所有所需的 HID API 函数。如果 hid.dll 加载失败或任一
 * 函数地址解析失败，则返回 -1 表示错误。
 *
 * 解析的函数包括：HidD_GetAttributes、HidD_GetSerialNumberString、
 * HidD_GetManufacturerString、HidD_GetProductString、HidD_SetFeature、
 * HidD_GetFeature、HidD_GetIndexedString、HidD_GetPreparsedData、
 * HidD_FreePreparsedData、HidP_GetCaps、HidD_SetNumInputBuffers。
 *
 * @return  成功返回 0，失败返回 -1
 */
static int lookup_functions()
{
	lib_handle = LoadLibraryA("hid.dll");
	if (lib_handle) {
#define RESOLVE(x) x = (x##_)GetProcAddress(lib_handle, #x); if (!x) return -1;
		RESOLVE(HidD_GetAttributes);
		RESOLVE(HidD_GetSerialNumberString);
		RESOLVE(HidD_GetManufacturerString);
		RESOLVE(HidD_GetProductString);
		RESOLVE(HidD_SetFeature);
		RESOLVE(HidD_GetFeature);
		RESOLVE(HidD_GetIndexedString);
		RESOLVE(HidD_GetPreparsedData);
		RESOLVE(HidD_FreePreparsedData);
		RESOLVE(HidP_GetCaps);
		RESOLVE(HidD_SetNumInputBuffers);
#undef RESOLVE
	}
	else
		return -1;

	return 0;
}
#endif

/**
 * open_device —— 通过设备路径打开 HID 设备
 *
 * 使用 CreateFileA 打开指定路径的 HID 设备。在枚举模式下（enumerate=TRUE）
 * 请求访问权限为 0（仅查询），在正常模式下请求读写访问权限。
 * 始终使用 FILE_FLAG_OVERLAPPED 标志以支持异步 I/O 操作。
 *
 * @param path      设备路径字符串
 * @param enumerate 是否为枚举模式（TRUE=仅枚举查询，FALSE=正常读写）
 * @return          设备句柄（INVALID_HANDLE_VALUE 表示失败）
 */
static HANDLE open_device(const char *path, BOOL enumerate)
{
	HANDLE handle;
	DWORD desired_access = (enumerate)? 0: (GENERIC_WRITE | GENERIC_READ);
	DWORD share_mode = FILE_SHARE_READ|FILE_SHARE_WRITE;

	handle = CreateFileA(path,
		desired_access,
		share_mode,
		NULL,
		OPEN_EXISTING,
		FILE_FLAG_OVERLAPPED,/*FILE_ATTRIBUTE_NORMAL,*/
		0);

	return handle;
}

/**
 * hid_init —— 初始化 hidapi 库
 *
 * 执行 hidapi 的一次性初始化操作：检查是否已初始化，如果未初始化则
 * 调用 lookup_functions() 动态加载 hid.dll 并解析所有函数指针。
 * 该函数可多次调用，只有第一次调用会执行实际的初始化工作。
 *
 * @return  成功返回 0，失败返回 -1
 */
int HID_API_EXPORT hid_init(void)
{
#ifndef HIDAPI_USE_DDK
	if (!initialized) {
		if (lookup_functions() < 0) {
			hid_exit();
			return -1;
		}
		initialized = TRUE;
	}
#endif
	return 0;
}

/**
 * hid_exit —— 清理 hidapi 库
 *
 * 释放动态加载的 hid.dll，并将所有全局状态重置为初始值。
 * 在此函数之后再次调用 hid_init() 可以重新初始化库。
 *
 * @return  始终返回 0
 */
int HID_API_EXPORT hid_exit(void)
{
#ifndef HIDAPI_USE_DDK
	if (lib_handle)
		FreeLibrary(lib_handle);
	lib_handle = NULL;
	initialized = FALSE;
#endif
	return 0;
}

/**
 * hid_enumerate —— 枚举系统中的 HID 设备
 *
 * 使用 Windows SetupAPI 枚举系统中的所有 HID 类设备。该函数：
 *   1. 通过 SetupDiGetClassDevsA 获取 HID 类设备信息集
 *   2. 遍历设备接口，对每个设备检查是否属于 "HIDClass" 并绑定了驱动程序
 *   3. 对匹配 VID/PID 过滤条件的设备，创建一个 hid_device_info 记录
 *   4. 获取设备的用途页（UsagePage）、用途（Usage）、序列号、制造商和产品字符串
 *   5. 尝试从设备路径中解析接口编号（interface_number）
 *
 * 如果 vendor_id 和 product_id 都为 0，则返回所有 HID 设备。
 *
 * @param vendor_id  供应商 ID（设为 0 则匹配所有供应商）
 * @param product_id 产品 ID（设为 0 则匹配所有产品）
 * @return            hid_device_info 链表头指针，未找到设备或失败时返回 NULL
 */
struct hid_device_info HID_API_EXPORT * HID_API_CALL hid_enumerate(unsigned short vendor_id, unsigned short product_id)
{
	BOOL res;
	struct hid_device_info *root = NULL; /* 返回的链表根节点 */
	struct hid_device_info *cur_dev = NULL;

	/* 用于与驱动程序交互的 Windows 对象 */
	GUID InterfaceClassGuid = {0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30} };
	SP_DEVINFO_DATA devinfo_data;
	SP_DEVICE_INTERFACE_DATA device_interface_data;
	SP_DEVICE_INTERFACE_DETAIL_DATA_A *device_interface_detail_data = NULL;
	HDEVINFO device_info_set = INVALID_HANDLE_VALUE;
	int device_index = 0;
	int i;

	if (hid_init() < 0)
		return NULL;

	/* 初始化 Windows 结构体 */
	memset(&devinfo_data, 0x0, sizeof(devinfo_data));
	devinfo_data.cbSize = sizeof(SP_DEVINFO_DATA);
	device_interface_data.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

	/* 获取属于 HID 类的所有设备信息 */
	device_info_set = SetupDiGetClassDevsA(&InterfaceClassGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

	/* 遍历 HID 类中的每个设备，查找匹配的设备 */

	for (;;) {
		HANDLE write_handle = INVALID_HANDLE_VALUE;
		DWORD required_size = 0;
		HIDD_ATTRIBUTES attrib;

		res = SetupDiEnumDeviceInterfaces(device_info_set,
			NULL,
			&InterfaceClassGuid,
			device_index,
			&device_interface_data);

		if (!res) {
			/* 此函数返回 FALSE 表示没有更多设备了 */
			break;
		}

		/* 使用大小为 0 的详细信息调用函数，让函数告诉我们
		   详细信息结构体需要多大。大小会写入 &required_size 中 */
		res = SetupDiGetDeviceInterfaceDetailA(device_info_set,
			&device_interface_data,
			NULL,
			0,
			&required_size,
			NULL);

		/* 为 device_interface_detail_data 分配足够长度的结构体内存 */
		device_interface_detail_data = (SP_DEVICE_INTERFACE_DETAIL_DATA_A*) malloc(required_size);
		device_interface_detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

		/* 获取该设备的详细信息。详细数据中包含设备路径，
		   该路径随后会传递给 CreateFile() 以获取设备句柄 */
		res = SetupDiGetDeviceInterfaceDetailA(device_info_set,
			&device_interface_data,
			device_interface_detail_data,
			required_size,
			NULL,
			NULL);

		if (!res) {
			/* register_error(dev, "Unable to call SetupDiGetDeviceInterfaceDetail");
			   继续处理下一个设备 */
			goto cont;
		}

		/* 确保该设备属于 "HIDClass" 安装类并且绑定了驱动程序 */
		for (i = 0; ; i++) {
			char driver_name[256];

			/* 填充 devinfo_data。当没有更多接口时此函数将返回失败 */
			res = SetupDiEnumDeviceInfo(device_info_set, i, &devinfo_data);
			if (!res)
				goto cont;

			res = SetupDiGetDeviceRegistryPropertyA(device_info_set, &devinfo_data,
			               SPDRP_CLASS, NULL, (PBYTE)driver_name, sizeof(driver_name), NULL);
			if (!res)
				goto cont;

			if (strcmp(driver_name, "HIDClass") == 0) {
				/* 检查是否有驱动程序绑定 */
				res = SetupDiGetDeviceRegistryPropertyA(device_info_set, &devinfo_data,
				           SPDRP_DRIVER, NULL, (PBYTE)driver_name, sizeof(driver_name), NULL);
				if (res)
					break;
			}
		}

		//wprintf(L"HandleName: %s\n", device_interface_detail_data->DevicePath);

		/* 打开设备句柄 */
		write_handle = open_device(device_interface_detail_data->DevicePath, TRUE);

		/* 检查 write_handle 的有效性 */
		if (write_handle == INVALID_HANDLE_VALUE) {
			/* 无法打开设备 */
			//register_error(dev, "CreateFile");
			goto cont_close;
		}


		/* 获取该设备的供应商 ID 和产品 ID */
		attrib.Size = sizeof(HIDD_ATTRIBUTES);
		HidD_GetAttributes(write_handle, &attrib);
		//wprintf(L"Product/Vendor: %x %x\n", attrib.ProductID, attrib.VendorID);

		/* 检查 VID/PID 是否匹配，以决定是否将该设备添加到枚举列表中 */
		if ((vendor_id == 0x0 || attrib.VendorID == vendor_id) &&
		    (product_id == 0x0 || attrib.ProductID == product_id)) {

			#define WSTR_LEN 512
			const char *str;
			struct hid_device_info *tmp;
			PHIDP_PREPARSED_DATA pp_data = NULL;
			HIDP_CAPS caps;
			BOOLEAN res;
			NTSTATUS nt_res;
			wchar_t wstr[WSTR_LEN]; /* TODO: 确定合适的大小 */
			size_t len;

			/* VID/PID 匹配，创建记录 */
			tmp = (struct hid_device_info*) calloc(1, sizeof(struct hid_device_info));
			if (cur_dev) {
				cur_dev->next = tmp;
			}
			else {
				root = tmp;
			}
			cur_dev = tmp;

			/* 获取该设备的用途页（Usage Page）和用途（Usage） */
			res = HidD_GetPreparsedData(write_handle, &pp_data);
			if (res) {
				nt_res = HidP_GetCaps(pp_data, &caps);
				if (nt_res == HIDP_STATUS_SUCCESS) {
					cur_dev->usage_page = caps.UsagePage;
					cur_dev->usage = caps.Usage;
				}

				HidD_FreePreparsedData(pp_data);
			}

			/* 填充记录信息 */
			cur_dev->next = NULL;
			str = device_interface_detail_data->DevicePath;
			if (str) {
				len = strlen(str);
				cur_dev->path = (char*) calloc(len+1, sizeof(char));
				strncpy(cur_dev->path, str, len+1);
				cur_dev->path[len] = '\0';
			}
			else
				cur_dev->path = NULL;

			/* 获取序列号 */
			res = HidD_GetSerialNumberString(write_handle, wstr, sizeof(wstr));
			wstr[WSTR_LEN-1] = 0x0000;
			if (res) {
				cur_dev->serial_number = _wcsdup(wstr);
			}

			/* 获取制造商字符串 */
			res = HidD_GetManufacturerString(write_handle, wstr, sizeof(wstr));
			wstr[WSTR_LEN-1] = 0x0000;
			if (res) {
				cur_dev->manufacturer_string = _wcsdup(wstr);
			}

			/* 获取产品字符串 */
			res = HidD_GetProductString(write_handle, wstr, sizeof(wstr));
			wstr[WSTR_LEN-1] = 0x0000;
			if (res) {
				cur_dev->product_string = _wcsdup(wstr);
			}

			/* 记录 VID/PID */
			cur_dev->vendor_id = attrib.VendorID;
			cur_dev->product_id = attrib.ProductID;

			/* 记录发布版本号 */
			cur_dev->release_number = attrib.VersionNumber;

			/*
			 * 接口编号。在 Windows 上，如果设备具有多个接口，
			 * 有时可以从设备路径中解析出接口编号。
			 * 参考：http://msdn.microsoft.com/en-us/windows/hardware/gg487473
			 * 或在 MSDN 上搜索 "Hardware IDs for HID Devices"。
			 * 如果路径中不包含接口编号，则设置为 -1。
			 */
			cur_dev->interface_number = -1;
			if (cur_dev->path) {
				char *interface_component = strstr(cur_dev->path, "&mi_");
				if (interface_component) {
					char *hex_str = interface_component + 4;
					char *endptr = NULL;
					cur_dev->interface_number = strtol(hex_str, &endptr, 16);
					if (endptr == hex_str) {
						/* 解析失败，将 interface_number 设为 -1 */
						cur_dev->interface_number = -1;
					}
				}
			}
		}

cont_close:
		CloseHandle(write_handle);
cont:
		/* 不再需要详细信息数据，可以释放 */
		free(device_interface_detail_data);

		device_index++;

	}

	/* 关闭设备信息集句柄 */
	SetupDiDestroyDeviceInfoList(device_info_set);

	return root;

}

/**
 * hid_free_enumeration —— 释放由 hid_enumerate 返回的设备枚举链表
 *
 * 遍历枚举链表，逐个释放每个节点的 path、serial_number、
 * manufacturer_string、product_string 以及节点本身。
 *
 * @param devs  待释放的 hid_device_info 链表头指针
 */
void  HID_API_EXPORT HID_API_CALL hid_free_enumeration(struct hid_device_info *devs)
{
	/* TODO: 与 Linux 版本合并。此函数是平台无关的 */
	struct hid_device_info *d = devs;
	while (d) {
		struct hid_device_info *next = d->next;
		free(d->path);
		free(d->serial_number);
		free(d->manufacturer_string);
		free(d->product_string);
		free(d);
		d = next;
	}
}


/**
 * hid_open —— 通过 VID、PID 和序列号打开 HID 设备
 *
 * 首先调用 hid_enumerate 获取所有匹配 VID/PID 的设备列表，
 * 然后遍历列表查找与指定序列号（如果提供）匹配的设备，
 * 最后调用 hid_open_path 打开该设备。
 *
 * @param vendor_id      供应商 ID
 * @param product_id     产品 ID
 * @param serial_number  序列号（可选，如果为 NULL 则匹配第一个设备）
 * @return               成功返回 hid_device 指针，失败返回 NULL
 */
HID_API_EXPORT hid_device * HID_API_CALL hid_open(unsigned short vendor_id, unsigned short product_id, const wchar_t *serial_number)
{
	/* TODO: 与此函数的 Linux 版本合并。此函数应该平台无关 */
	struct hid_device_info *devs, *cur_dev;
	const char *path_to_open = NULL;
	hid_device *handle = NULL;

	devs = hid_enumerate(vendor_id, product_id);
	cur_dev = devs;
	while (cur_dev) {
		if (cur_dev->vendor_id == vendor_id &&
		    cur_dev->product_id == product_id) {
			if (serial_number) {
				if (wcscmp(serial_number, cur_dev->serial_number) == 0) {
					path_to_open = cur_dev->path;
					break;
				}
			}
			else {
				path_to_open = cur_dev->path;
				break;
			}
		}
		cur_dev = cur_dev->next;
	}

	if (path_to_open) {
		/* 打开设备 */
		handle = hid_open_path(path_to_open);
	}

	hid_free_enumeration(devs);

	return handle;
}

/**
 * hid_open_path —— 通过设备路径打开 HID 设备
 *
 * 该函数执行打开 HID 设备的完整流程：
 *   1. 初始化 hidapi 库
 *   2. 创建新的 hid_device 结构体
 *   3. 使用 CreateFile 打开设备（非枚举模式，请求读写权限）
 *   4. 设置输入报告缓冲区数量为 64
 *   5. 获取预解析数据（Preparsed Data）并查询设备能力（Caps）
 *   6. 记录输入/输出报告长度
 *   7. 分配读缓冲区
 *
 * 如果任何步骤失败，则释放已分配的资源并返回 NULL。
 *
 * @param path  设备路径字符串
 * @return      成功返回 hid_device 指针，失败返回 NULL
 */
HID_API_EXPORT hid_device * HID_API_CALL hid_open_path(const char *path)
{
	hid_device *dev;
	HIDP_CAPS caps;
	PHIDP_PREPARSED_DATA pp_data = NULL;
	BOOLEAN res;
	NTSTATUS nt_res;

	if (hid_init() < 0) {
		return NULL;
	}

	dev = new_hid_device();

	/* 打开设备句柄 */
	dev->device_handle = open_device(path, FALSE);

	/* 检查 write_handle 的有效性 */
	if (dev->device_handle == INVALID_HANDLE_VALUE) {
		/* 无法打开设备 */
		register_error(dev, "CreateFile");
		goto err;
	}

	/* 将输入报告缓冲区大小设置为 64 个报告 */
	res = HidD_SetNumInputBuffers(dev->device_handle, 64);
	if (!res) {
		register_error(dev, "HidD_SetNumInputBuffers");
		goto err;
	}

	/* 获取设备的输入报告长度 */
	res = HidD_GetPreparsedData(dev->device_handle, &pp_data);
	if (!res) {
		register_error(dev, "HidD_GetPreparsedData");
		goto err;
	}
	nt_res = HidP_GetCaps(pp_data, &caps);
	if (nt_res != HIDP_STATUS_SUCCESS) {
		register_error(dev, "HidP_GetCaps");
		goto err_pp_data;
	}
	dev->output_report_length = caps.OutputReportByteLength;
	dev->input_report_length = caps.InputReportByteLength;
	HidD_FreePreparsedData(pp_data);

	dev->read_buf = (char*) malloc(dev->input_report_length);

	return dev;

err_pp_data:
		HidD_FreePreparsedData(pp_data);
err:
		free_hid_device(dev);
		return NULL;
}

/**
 * hid_write2 —— hid_write 的简化封装（用于与旧版本兼容）
 *
 * 直接调用 hid_write，忽略返回值。
 *
 * @param dev     HID 设备指针
 * @param data    待写入的数据缓冲区
 * @param length  数据长度
 */
void HID_API_EXPORT HID_API_CALL hid_write2(hid_device *dev, const unsigned char *data, size_t length)
{
	(void)hid_write(dev, data, length);
}

/**
 * hid_write —— 向 HID 设备写入输出报告
 *
 * 以同步方式向设备写入数据。Windows 期望传递给 WriteFile 的数据长度
 * 等于最长的输出报告字节数（OutputReportByteLength，来自设备能力信息），
 * 即使实际发送的报告较短也需要填充到该长度。如果用户传入的数据不足
 * 输出报告长度，函数会创建一个临时缓冲区，将数据拷贝进去并用零填充剩余部分。
 *
 * 使用重叠 I/O（Overlapped I/O）执行写入，然后等待操作完成以实现同步行为。
 *
 * @param dev     HID 设备指针
 * @param data    待写入的数据缓冲区（第一个字节通常为报告编号）
 * @param length  数据长度
 * @return        成功返回实际写入的字节数，失败返回 -1
 */
int HID_API_EXPORT HID_API_CALL hid_write(hid_device *dev, const unsigned char *data, size_t length)
{
	DWORD bytes_written;
	DWORD write_len;
	BOOL res;

	OVERLAPPED ol;
	unsigned char *buf;
	memset(&ol, 0, sizeof(ol));

	/*
	 * 确保传递给 WriteFile 的字节数正确。Windows 期望传入的字节数等于
	 * _最长_ 报告的长度（加上报告编号的一个字节），即使实际数据较短。
	 * Windows 通过 caps.OutputReportByteLength 提供此值。
	 * 如果用户传入的字节数不足，则创建合适大小的临时缓冲区。
	 */
	if (length >= dev->output_report_length) {
		/* 用户传入了正确的字节数，直接使用原缓冲区 */
		buf = (unsigned char *) data;
	} else {
		/* 创建临时缓冲区，将用户数据拷贝进去，剩余部分用零填充 */
		buf = (unsigned char *) malloc(dev->output_report_length);
		memcpy(buf, data, length);
		memset(buf + length, 0, dev->output_report_length - length);
		length = dev->output_report_length;
	}

	if (!size_to_dword(length, &write_len)) {
		register_error(dev, "WriteFile");
		bytes_written = (DWORD)-1;
		goto end_of_function;
	}

	res = WriteFile(dev->device_handle, buf, write_len, NULL, &ol);

	if (!res) {
		if (GetLastError() != ERROR_IO_PENDING) {
			/* WriteFile 失败，返回错误 */
			register_error(dev, "WriteFile");
			bytes_written = -1;
			goto end_of_function;
		}
	}

	/* 在此等待写入完成，使 hid_write() 表现为同步操作 */
	res = GetOverlappedResult(dev->device_handle, &ol, &bytes_written, TRUE/*wait*/);
	if (!res) {
		/* 写入操作失败 */
		register_error(dev, "WriteFile");
		bytes_written = -1;
		goto end_of_function;
	}

end_of_function:
	if (buf != data)
		free(buf);

	if (bytes_written == (DWORD)-1 || bytes_written > (DWORD)INT_MAX)
		return -1;
	return (int)bytes_written;
}


/**
 * hid_read_timeout —— 从 HID 设备读取输入报告（支持超时）
 *
 * 使用重叠 I/O（Overlapped I/O）从设备读取数据。支持两种模式：
 *   - 阻塞模式（milliseconds = -1）：无限等待直到数据到达
 *   - 超时模式（milliseconds >= 0）：等待指定的毫秒数，超时返回 0
 *
 * 读取的数据会去除 Windows 添加的前导 0x00 报告编号字节（如果
 * 设备未使用报告编号），以匹配其他平台的行为和 HID 规范。
 *
 * @param dev           HID 设备指针
 * @param data          接收数据的缓冲区
 * @param length        缓冲区长度
 * @param milliseconds  超时时间（毫秒），-1 表示无限等待，0 表示非阻塞
 * @return              成功返回读取的字节数，超时返回 0，失败返回 -1
 */
int HID_API_EXPORT HID_API_CALL hid_read_timeout(hid_device *dev, unsigned char *data, size_t length, int milliseconds)
{
	DWORD bytes_read = 0;
	DWORD read_len = 0;
	size_t copy_len = 0;
	int copy_len_i = 0;
	BOOL res;

	/* 为方便使用而拷贝事件句柄 */
	HANDLE ev = dev->ol.hEvent;

	if (!dev->read_pending) {
		/* 启动一个重叠 I/O 读操作 */
		dev->read_pending = TRUE;
		memset(dev->read_buf, 0, dev->input_report_length);
		ResetEvent(ev);
		if (!size_to_dword(dev->input_report_length, &read_len)) {
			register_error(dev, "ReadFile");
			dev->read_pending = FALSE;
			return -1;
		}
		res = ReadFile(dev->device_handle, dev->read_buf, read_len, &bytes_read, &dev->ol);

		if (!res) {
			if (GetLastError() != ERROR_IO_PENDING) {
				/* ReadFile 失败，清理并返回错误 */
				CancelIo(dev->device_handle);
				dev->read_pending = FALSE;
				goto end_of_function;
			}
		}
	}

	if (milliseconds >= 0) {
		/* 检查数据是否已到达 */
		res = WaitForSingleObject(ev, milliseconds);
		if (res != WAIT_OBJECT_0) {
			/* 本次没有数据到达，返回 0 字节，
			   但保持重叠 I/O 继续运行 */
			return 0;
		}
	}

	/*
	 * 当 WaitForSingleObject 告诉我们 ReadFile 已完成，或者
	 * 我们处于非阻塞模式时，获取实际读取的字节数。
	 * 实际数据已拷贝到传递给 ReadFile 的 data[] 数组。
	 */
	res = GetOverlappedResult(dev->device_handle, &dev->ol, &bytes_read, TRUE/*wait*/);

	/* 即使 GetOverlappedResult 返回错误，也将 pending 设回 FALSE */
	dev->read_pending = FALSE;

	if (res && bytes_read > 0) {
		if (dev->read_buf[0] == 0x0) {
			/*
			 * 如果设备未使用报告编号，Windows 仍然会在报告开头
			 * 添加一个报告编号字节（0x0）。为了让此行为与其他
			 * 平台一致，并更符合 HID 规范，我们跳过这个字节。
			 */
			bytes_read--;
			copy_len = length > bytes_read ? bytes_read : length;
			memcpy(data, dev->read_buf+1, copy_len);
		}
		else {
			/* 拷贝整个缓冲区，包括报告编号 */
			copy_len = length > bytes_read ? bytes_read : length;
			memcpy(data, dev->read_buf, copy_len);
		}
	}

end_of_function:
	if (!res) {
		register_error(dev, "GetOverlappedResult");
		return -1;
	}

	if (!size_to_int(copy_len, &copy_len_i))
		return -1;
	return copy_len_i;
}

/**
 * hid_read —— 从 HID 设备读取输入报告（阻塞/非阻塞取决于设备设置）
 *
 * 根据 hid_device 的 blocking 标志决定读取模式：
 *   - 阻塞模式（blocking = TRUE）：调用 hid_read_timeout 并设置超时为 -1（无限等待）
 *   - 非阻塞模式（blocking = FALSE）：调用 hid_read_timeout 并设置超时为 0（立即返回）
 *
 * @param dev     HID 设备指针
 * @param data    接收数据的缓冲区
 * @param length  缓冲区长度
 * @return        成功返回读取的字节数，失败返回 -1
 */
int HID_API_EXPORT HID_API_CALL hid_read(hid_device *dev, unsigned char *data, size_t length)
{
	return hid_read_timeout(dev, data, length, (dev->blocking)? -1: 0);
}

/**
 * hid_set_nonblocking —— 设置 HID 设备的非阻塞模式
 *
 * 切换设备的读取模式。非阻塞模式下，hid_read 调用会立即返回，
 * 如果没有可用数据则返回 0。
 *
 * @param dev      HID 设备指针
 * @param nonblock 非阻塞标志（非零值启用非阻塞模式，0 启用阻塞模式）
 * @return         始终返回 0（成功）
 */
int HID_API_EXPORT HID_API_CALL hid_set_nonblocking(hid_device *dev, int nonblock)
{
	dev->blocking = !nonblock;
	return 0; /* 成功 */
}

/**
 * hid_send_feature_report —— 向 HID 设备发送功能报告
 *
 * 通过 HidD_SetFeature 接口向设备发送功能报告（Feature Report）。
 * 功能报告是一种特殊类型的 HID 报告，可以双向传输数据，
 * 与输入/输出报告不同，它不通过常规的输入/输出管道传输。
 *
 * @param dev     HID 设备指针
 * @param data    功能报告数据缓冲区（第一个字节为报告编号）
 * @param length  数据长度
 * @return        成功返回数据长度，失败返回 -1
 */
int HID_API_EXPORT HID_API_CALL hid_send_feature_report(hid_device *dev, const unsigned char *data, size_t length)
{
	ULONG feature_len = 0;
	int length_i = 0;
	BOOL res;

	if (!size_to_ulong(length, &feature_len) || !size_to_int(length, &length_i)) {
		register_error(dev, "HidD_SetFeature");
		return -1;
	}

	res = HidD_SetFeature(dev->device_handle, (PVOID)data, feature_len);
	if (!res) {
		register_error(dev, "HidD_SetFeature");
		return -1;
	}

	return length_i;
}


/**
 * hid_get_feature_report —— 从 HID 设备获取功能报告
 *
 * 使用 DeviceIoControl 通过 IOCTL_HID_GET_FEATURE 控制码从设备
 * 获取功能报告（Feature Report）。采用重叠 I/O 并以同步方式等待结果。
 *
 * 注：原始实现中使用 HidD_GetFeature 的方式已被注释掉，因为该 API
 * 不返回实际读取的字节数。当前实现使用 DeviceIoControl，其返回的
 * bytes_returned 不包含报告编号字节，因此需要额外加 1。
 *
 * @param dev     HID 设备指针
 * @param data    缓冲区，调用时包含报告编号和输入缓冲区，返回时包含功能报告数据
 * @param length  缓冲区长度
 * @return        成功返回实际读取的字节数（包括报告编号），失败返回 -1
 */
int HID_API_EXPORT HID_API_CALL hid_get_feature_report(hid_device *dev, unsigned char *data, size_t length)
{
	BOOL res;
#if 0
	res = HidD_GetFeature(dev->device_handle, data, length);
	if (!res) {
		register_error(dev, "HidD_GetFeature");
		return -1;
	}
	return 0; /* HidD_GetFeature 不返回实际长度，很遗憾 */
#else
	DWORD bytes_returned;
	DWORD io_len = 0;

	OVERLAPPED ol;
	memset(&ol, 0, sizeof(ol));

	if (!size_to_dword(length, &io_len)) {
		register_error(dev, "Send Feature Report DeviceIoControl");
		return -1;
	}

	res = DeviceIoControl(dev->device_handle,
		IOCTL_HID_GET_FEATURE,
		data, io_len,
		data, io_len,
		&bytes_returned, &ol);

	if (!res) {
		if (GetLastError() != ERROR_IO_PENDING) {
			/* DeviceIoControl 失败，返回错误 */
			register_error(dev, "Send Feature Report DeviceIoControl");
			return -1;
		}
	}

	/* 在此等待操作完成，使 hid_get_feature_report() 表现为同步操作 */
	res = GetOverlappedResult(dev->device_handle, &ol, &bytes_returned, TRUE/*wait*/);
	if (!res) {
		/* 操作失败 */
		register_error(dev, "Send Feature Report GetOverLappedResult");
		return -1;
	}

	/*
	 * bytes_returned 不包含第一个字节（报告编号）。
	 * 实际数据缓冲区中比 bytes_returned 多一个字节。
	 */
	bytes_returned++;

	if (bytes_returned > (DWORD)INT_MAX)
		return -1;
	return (int)bytes_returned;
#endif
}

/**
 * hid_close —— 关闭 HID 设备并释放资源
 *
 * 取消所有挂起的 I/O 操作，然后释放 hid_device 结构体
 * 及其关联的所有资源。
 *
 * @param dev  待关闭的 HID 设备指针
 */
void HID_API_EXPORT HID_API_CALL hid_close(hid_device *dev)
{
	if (!dev)
		return;
	CancelIo(dev->device_handle);
	free_hid_device(dev);
}

/**
 * hid_get_manufacturer_string —— 获取 HID 设备的制造商字符串
 *
 * 通过 HidD_GetManufacturerString 读取设备的制造商名称。
 * 字符串长度会被限定在 MAX_STRING_WCHARS 以内，以防止
 * Windows API 调用失败。
 *
 * @param dev     HID 设备指针
 * @param string  接收制造商字符串的宽字符缓冲区
 * @param maxlen  缓冲区最大字符数
 * @return        成功返回 0，失败返回 -1
 */
int HID_API_EXPORT_CALL HID_API_CALL hid_get_manufacturer_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	size_t clamped_wchars;
	ULONG buffer_len;
	BOOL res;

	clamped_wchars = MIN(maxlen, (size_t)MAX_STRING_WCHARS);
	if (!size_to_ulong(sizeof(wchar_t) * clamped_wchars, &buffer_len)) {
		register_error(dev, "HidD_GetManufacturerString");
		return -1;
	}

	res = HidD_GetManufacturerString(dev->device_handle, string, buffer_len);
	if (!res) {
		register_error(dev, "HidD_GetManufacturerString");
		return -1;
	}

	return 0;
}

/**
 * hid_get_product_string —— 获取 HID 设备的产品字符串
 *
 * 通过 HidD_GetProductString 读取设备的产品名称。
 * 字符串长度会被限定在 MAX_STRING_WCHARS 以内。
 *
 * @param dev     HID 设备指针
 * @param string  接收产品字符串的宽字符缓冲区
 * @param maxlen  缓冲区最大字符数
 * @return        成功返回 0，失败返回 -1
 */
int HID_API_EXPORT_CALL HID_API_CALL hid_get_product_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	size_t clamped_wchars;
	ULONG buffer_len;
	BOOL res;

	clamped_wchars = MIN(maxlen, (size_t)MAX_STRING_WCHARS);
	if (!size_to_ulong(sizeof(wchar_t) * clamped_wchars, &buffer_len)) {
		register_error(dev, "HidD_GetProductString");
		return -1;
	}

	res = HidD_GetProductString(dev->device_handle, string, buffer_len);
	if (!res) {
		register_error(dev, "HidD_GetProductString");
		return -1;
	}

	return 0;
}

/**
 * hid_get_serial_number_string —— 获取 HID 设备的序列号字符串
 *
 * 通过 HidD_GetSerialNumberString 读取设备的序列号。
 * 字符串长度会被限定在 MAX_STRING_WCHARS 以内。
 *
 * @param dev     HID 设备指针
 * @param string  接收序列号字符串的宽字符缓冲区
 * @param maxlen  缓冲区最大字符数
 * @return        成功返回 0，失败返回 -1
 */
int HID_API_EXPORT_CALL HID_API_CALL hid_get_serial_number_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	size_t clamped_wchars;
	ULONG buffer_len;
	BOOL res;

	clamped_wchars = MIN(maxlen, (size_t)MAX_STRING_WCHARS);
	if (!size_to_ulong(sizeof(wchar_t) * clamped_wchars, &buffer_len)) {
		register_error(dev, "HidD_GetSerialNumberString");
		return -1;
	}

	res = HidD_GetSerialNumberString(dev->device_handle, string, buffer_len);
	if (!res) {
		register_error(dev, "HidD_GetSerialNumberString");
		return -1;
	}

	return 0;
}

/**
 * hid_get_indexed_string —— 通过索引获取 HID 设备的字符串
 *
 * 某些 HID 设备支持通过索引号来获取特定的字符串描述符。
 * 此函数通过 HidD_GetIndexedString 实现该功能。
 * 字符串长度会被限定在 MAX_STRING_WCHARS 以内。
 *
 * @param dev          HID 设备指针
 * @param string_index 字符串索引号
 * @param string       接收字符串的宽字符缓冲区
 * @param maxlen       缓冲区最大字符数
 * @return             成功返回 0，失败返回 -1
 */
int HID_API_EXPORT_CALL HID_API_CALL hid_get_indexed_string(hid_device *dev, int string_index, wchar_t *string, size_t maxlen)
{
	size_t clamped_wchars;
	ULONG buffer_len;
	BOOL res;

	clamped_wchars = MIN(maxlen, (size_t)MAX_STRING_WCHARS);
	if (!size_to_ulong(sizeof(wchar_t) * clamped_wchars, &buffer_len)) {
		register_error(dev, "HidD_GetIndexedString");
		return -1;
	}

	res = HidD_GetIndexedString(dev->device_handle, string_index, string, buffer_len);
	if (!res) {
		register_error(dev, "HidD_GetIndexedString");
		return -1;
	}

	return 0;
}


/**
 * hid_error —— 获取 HID 设备的最后一次错误消息
 *
 * 返回之前通过 register_error 注册的错误消息字符串。
 * 如果没有错误发生，则返回 NULL。
 *
 * @param dev  HID 设备指针
 * @return     错误消息的宽字符串指针，无错误时返回 NULL
 */
HID_API_EXPORT const wchar_t * HID_API_CALL  hid_error(hid_device *dev)
{
	return (wchar_t*)dev->last_error_str;
}


/*#define PICPGM*/
/*#define S11*/
#define P32
#ifdef S11
	unsigned short VendorID = 0xa0a0;
	unsigned short ProductID = 0x0001;
#endif

#ifdef P32
	unsigned short VendorID = 0x04d8;
	unsigned short ProductID = 0x3f;
#endif


#ifdef PICPGM
	unsigned short VendorID = 0x04d8;
	unsigned short ProductID = 0x0033;
#endif


#if 0
int __cdecl main(int argc, char* argv[])
{
	int res;
	unsigned char buf[65];

	UNREFERENCED_PARAMETER(argc);
	UNREFERENCED_PARAMETER(argv);

	/* 设置命令缓冲区 */
	memset(buf,0x00,sizeof(buf));
	buf[0] = 0;
	buf[1] = 0x81;


	/* 打开设备 */
	int handle = open(VendorID, ProductID, L"12345");
	if (handle < 0)
		printf("unable to open device\n");


	/* 切换 LED（命令 0x80） */
	buf[1] = 0x80;
	res = write(handle, buf, 65);
	if (res < 0)
		printf("Unable to write()\n");

	/* 请求状态（命令 0x81） */
	buf[1] = 0x81;
	write(handle, buf, 65);
	if (res < 0)
		printf("Unable to write() (2)\n");

	/* 读取请求的状态 */
	read(handle, buf, 65);
	if (res < 0)
		printf("Unable to read()\n");

	/* 打印返回的缓冲区内容 */
	for (int i = 0; i < 4; i++)
		printf("buf[%d]: %d\n", i, buf[i]);

	return 0;
}
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif
