/**
 ****************************************************************************************
 *
 * @file user_peripheral.c
 *
 * @brief 外设项目源代码文件，主要实现BLE外设功能、时钟管理、EPD屏幕显示及OTP数据读取等功能
 *
 * Copyright (C) 2015-2023 Renesas Electronics Corporation and/or its affiliates.
 * All rights reserved. Confidential Information.
 *
 *
 ****************************************************************************************
 */

/**
 ****************************************************************************************
 * @addtogroup APP
 * @{
 ****************************************************************************************
 */

/*
 * 包含头文件
 ****************************************************************************************
 */

#include "rwip_config.h"             // 软件配置
#include "gattc_task.h"              // GATT客户端任务相关定义
#include "gap.h"                     // GAP层相关定义
#include "app_easy_timer.h"          // 应用层定时器功能
#include "user_peripheral.h"         // 本文件接口声明
#include "user_custs1_impl.h"        // 自定义服务1实现
#include "user_custs1_def.h"         // 自定义服务1定义
#include "co_bt.h"                   // 蓝牙协议协议相关定义
#include "hw_otpc.h"                 // OTP控制器硬件接口

#include "epd.h"                     // EPD电子纸屏幕驱动
#include "st7789.h"                  // ★ ST7789 240x320 IPS 屏驱动
#include "monitor_ui.h"              // ★ PC 资源监控界面
#include "ui_cfg.h"                  // ★ 界面/LED 配置 (从 Flash 加载)

/*
 * 类型定义
 ****************************************************************************************
 */


/*
 * 全局变量定义
 ****************************************************************************************
 */

int app_connection_idx                          __SECTION_ZERO("retention_mem_area0"); // 连接索引，使用 retention 内存区域保存
timer_hnd app_clock_timer_used                  __SECTION_ZERO("retention_mem_area0"); // 时钟定时器句柄，retention内存保存
timer_hnd app_param_update_request_timer_used   __SECTION_ZERO("retention_mem_area0"); // 参数更新请求定时器句柄，retention内存保存

int adv_state = 0;                          // 广播状态：0-未广播，1-正在广播
static int otp_btaddr[2];                      // 从OTP读取的蓝牙地址
static int otp_boot;                           // 从OTP读取的启动相关数据
static char adv_name[20];                      // 广播名称缓冲区
char *bt_id = adv_name+12;                     // 蓝牙ID在广播名称中的起始位置
int clock_interval;                            // 时钟更新间隔（秒）
int clock_fixup_value;                         // 时钟修正值
int clock_fixup_count;                         // 时钟修正计数器
static int first_timer_trigger = 0;            // 标志：是否为第一次定时器触发（用于整分钟对齐）
static int first_update_seconds = 0;           // 第一次触发时需要更新的秒数

// EPD版本信息（volatile确保不被优化，用于版本检测）
const volatile u32 epd_version[3] = {0xF9A51379, ~0xF9A51379, EPD_VERSION};

extern int year,month,date,hour,minute; // 当前时间变量 (date 为 0-based)
extern int second; // 当前秒数，用于计算到整分钟的剩余时间
extern void cal_sync_date(void);   // 万年历: 按公历重算农历/星期/节气/节日

/*
 * 函数定义
 ****************************************************************************************
*/


/**
 ****************************************************************************************
 * @brief 在GAPM_START_ADVERTISE_CMD参数结构的广播或扫描响应数据中添加AD结构
 * @param[in] cmd               GAPM_START_ADVERTISE_CMD参数结构
 * @param[in] ad_struct_data    AD结构数据缓冲区
 * @param[in] ad_struct_len     AD结构长度
 * @param[in] adv_connectable   是否为可连接广播事件，控制广播数据最大长度（可连接时28字节，否则31字节）
 ****************************************************************************************
 */
static void app_add_ad_struct(struct gapm_start_advertise_cmd *cmd, void *ad_struct_data, uint8_t ad_struct_len, uint8_t adv_connectable)
{
    // 根据是否可连接确定广播数据最大长度
    uint8_t adv_data_max_size = (adv_connectable) ? (ADV_DATA_LEN - 3) : (ADV_DATA_LEN);

    // 优先添加到广播数据
    if ((adv_data_max_size - cmd->info.host.adv_data_len) >= ad_struct_len)
    {
        memcpy(&cmd->info.host.adv_data[cmd->info.host.adv_data_len], ad_struct_data, ad_struct_len);
        cmd->info.host.adv_data_len += ad_struct_len;
    }
    // 广播数据空间不足时添加到扫描响应数据
    else if ((SCAN_RSP_DATA_LEN - cmd->info.host.scan_rsp_data_len) >= ad_struct_len)
    {
        memcpy(&cmd->info.host.scan_rsp_data[cmd->info.host.scan_rsp_data_len], ad_struct_data, ad_struct_len);
        cmd->info.host.scan_rsp_data_len += ad_struct_len;
    }
    // 空间不足时触发断言警告
    else
    {
        ASSERT_WARNING(0);
    }
}


/**
 ****************************************************************************************
 * @brief 参数更新请求定时器回调函数
 *        当定时器超时，发起连接参数更新请求
 ****************************************************************************************
*/
static void param_update_request_timer_cb()
{
    app_easy_gap_param_update_start(app_connection_idx);  // 发起参数更新
    app_param_update_request_timer_used = EASY_TIMER_INVALID_TIMER;  // 重置定时器句柄
}


/**
 ****************************************************************************************
 * @brief 读取OTP（一次性可编程）存储器中的值
 *        主要读取蓝牙地址和启动信息，并生成广播名称
 ****************************************************************************************
 */
static void read_otp_value(void)
{
	hw_otpc_init();               // 初始化OTP控制器
	hw_otpc_manual_read_on(false); // 关闭手动读取模式
	
	// 从OTP特定地址读取数据
	otp_boot = *(u32*)(0x07f8fe00);    // 读取启动相关数据
	otp_btaddr[0] = *(u32*)(0x07f8ffa8); // 读取蓝牙地址低32位
	otp_btaddr[1] = *(u32*)(0x07f8ffac); // 读取蓝牙地址高32位
	
	hw_otpc_disable();            // 禁用OTP控制器

	// 处理蓝牙地址，生成设备唯一标识
	u32 ba0 = otp_btaddr[0];
	u32 ba1 = otp_btaddr[1];

	ba1 = (ba1<<8)|(ba0>>24);
	ba0 &= 0x00ffffff;
	ba0 ^= ba1;

	// 生成广播名称（格式：DLG-CLOCK-XXYYZZ，XXYYZZ为蓝牙地址后三段）
	u8 *ba = (u8*)&ba0;
	sprintf(adv_name+2, "DLG-CLOCK-%02x%02x%02x", ba[2], ba[1], ba[0]);
	int name_len = strlen(adv_name+2);
	
	// 如果设备名称未设置，则使用生成的名称
	if(device_info.dev_name.length==0){
		device_info.dev_name.length = name_len;
		memcpy(device_info.dev_name.name, adv_name+2, name_len);
	}

	// 构造AD结构：第一个字节为长度，第二个字节为AD类型（完整名称）
	adv_name[0] = name_len+1;
	adv_name[1] = GAP_AD_TYPE_COMPLETE_NAME;
}

// 外部声明的区域表基地址（用于内存相关操作）
extern int Region$$Table$$Base;

/**
 ****************************************************************************************
 * @brief 应用初始化函数
 *        初始化OTP数据、定时器、屏幕、蓝牙等模块
 ****************************************************************************************
 */
void user_app_init(void)
{
	read_otp_value();  // 读取OTP数据，初始化广播名称

	printk("\n\nuser_app_init! %s %08x\n", __TIME__, epd_version[2]);
    app_param_update_request_timer_used = EASY_TIMER_INVALID_TIMER;  // 初始化参数更新定时器
	app_clock_timer_used = EASY_TIMER_INVALID_TIMER;                 // 初始化时钟定时器

	clock_interval = 1; // ★ 时钟更新间隔：1秒（显示秒数，用户要求不考虑功耗）
	clock_fixup_value = 0; // 初始化时钟修正值
	clock_fixup_count = 0; // 初始化时钟修正计数器

	first_timer_trigger = 0; // 初始化第一次触发标志

	adv_state = 0; // 初始化为未广播状态
	fspi_config(0x00030605); // 配置FSPI接口

	selflash(otp_boot); // 根据OTP启动数据执行自闪存操作

	// ★ 屏幕已改为 ST7789 240x320 IPS：停用 EPD 硬件层（两者共用同一组引脚），
	//   只保留 epd_hw_init 以维持 scr_w/scr_h 等变量有效，避免上层绘图层除零。
	epd_hw_init(0x23200700, 0x05210006, detect_w, detect_h, detect_mode | ROTATE_3);
	epd_hw_disabled = 1;

	cfg_load();          // ★ 从 SPI Flash 读取界面/LED 配置 (无有效配置则用默认值)

	/* ★ 独立万年历: 断电后从 SPI Flash 恢复上次时间; 首次烧录则用编译时刻兜底。
	 * 之后完全靠本地 RTC 走时, 不需要电脑常开。
	 * 注意: Flash 快照里存的是 1-based 日, 而固件内部 date 是 0-based,
	 *       恢复后必须减 1, 否则日期会多一天。 */
	{
		int d1;
		if(rtc_restore(&year, &month, &d1, &hour, &minute, &second))
			date = d1 - 1;
		else
			rtc_seed_default();
	}

	st7789_init();
	ui_init();

	app_connection_idx = -1; // 初始化连接索引为无效值
    default_app_on_init();   // 执行默认应用初始化
}


/**
 ****************************************************************************************
 * @brief 时钟快慢修正函数
 * @param[in] diff_sec  自上次对时后的误差秒数（正数表示快了，负数表示慢了）
 * @param[in] minutes   自上次对时后经过的分钟数
 * @note 计算并累积时钟修正值，用于调整定时器间隔，补偿时钟误差
 ****************************************************************************************
 */
void clock_fixup_set(int diff_sec, int minutes)
{
	// 计算新的修正值（基于4096精度的分数计算）
	int new_fixup_value = diff_sec*100*4096/minutes;
	clock_fixup_value += new_fixup_value; // 累积修正值
}


/**
 ****************************************************************************************
 * @brief 应用时钟修正值
 * @return 本次需要调整的毫秒数
 * @note 从累积的修正计数器中提取整数部分作为本次调整值，保留余数
 ****************************************************************************************
 */
static int clock_fixup_div = 0;

static int clock_fixup(void)
{
	int value;

	// ★ 秒级 tick：clock_fixup_value 是"每分钟"的修正量（10ms×4096单位），
	//   每 60 次 tick（1分钟）才累加一次，保持与原分钟级校准语义一致
	if(++clock_fixup_div >= 60){
		clock_fixup_div = 0;
		clock_fixup_count += clock_fixup_value;
	}

	value = clock_fixup_count>>12; // 右移12位（除以4096）得到整数部分（10ms单位）
	clock_fixup_count &= 0xfff;    // 保留低12位作为余数

	return value;
}

extern int adcval;  // ADC电压值变量
/**
 ****************************************************************************************
 * @brief 应用时钟定时器回调函数
 *        定时更新时钟、推送时钟数据、处理屏幕显示，并根据需要重启广播
 ****************************************************************************************
 */
static void app_clock_timer_cb(void)
{
	int adj = clock_fixup(); // 获取时钟修正值
	int update_seconds;
	int retv;

	if(first_timer_trigger) {
		first_timer_trigger = 0;
		update_seconds = first_update_seconds;
		printk("First trigger: second=%d, update_seconds=%d\n", second, update_seconds);
	} else {
		update_seconds = clock_interval;
	}

	// 重启定时器，应用修正后的间隔（单位：10ms，故乘以100）
	app_clock_timer_used = app_easy_timer(clock_interval*100+adj, app_clock_timer_cb);

	// 维持原有时间变量更新
	retv = clock_update(update_seconds);

	/* ★ 独立万年历: 每分钟(以及跨小时/跨天)把时间快照写入 SPI Flash。
	 *   顺序追加到 0x3C000 扇区的 256 个 16B 槽, 写满才整片擦除。
	 *   为什么是每分钟: 本芯片没有掉电保持的 RTC, 重启只能恢复到"最后一次快照",
	 *   若每 10 分钟才存一次, 断电重启最多会慢 10 分钟(之前 08:43 快照重启后
	 *   在 08:52 上电仍显示 08:43 就是这个原因)。改成每分钟最多只慢 1 分钟。
	 *   磨损: 256 槽 / 1 分钟 ≈ 4.3 小时才擦一次, 按 10 万次擦写寿命约 49 年。 */
	if(retv >= 1)
		rtc_save_now(year, month, date + 1, hour, minute, second);

	// ★ ST7789 界面刷新：时间始终来自固件本地 RTC，与蓝牙/电脑是否连接无关
	g_mon.hour = (uint8_t)hour;
	g_mon.min  = (uint8_t)minute;
	g_mon.sec  = (uint8_t)second;
	ui_tick();

	/* ★ Web Bluetooth 对时页 (HMWEBCTRL) 通过 readValue(0xFF01) 读设备时间。
	 *   之前只在开机/连接时 clock_push() 一次, 值不会走, 网页的"时间校准"
	 *   轮询等待分钟跳变会永远等不到。连接期间每秒把当前时间写回长值特征,
	 *   断开后停推, 不占用内核消息队列。 */
	if(app_connection_idx != -1)
		clock_push();

	// 未连接时保持广播
	if(app_connection_idx == -1){
		user_app_adv_start();
	}
}


/**
 ****************************************************************************************
 * @brief 对时后重新对齐应用时钟定时器
 *        本固件是秒级刷新 (clock_interval=1)，不能沿用墨水屏时代"等到整分钟再跳"的
 *        逻辑，否则写完 0x91 后秒数会冻结最多 60 秒。这里直接重启 1 秒节拍。
 ****************************************************************************************
 */
void app_clock_timer_restart(void)
{
	app_easy_timer_cancel(app_clock_timer_used);
	first_timer_trigger = 0;
	first_update_seconds = 1;
	app_clock_timer_used = app_easy_timer(100, app_clock_timer_cb);
}


/**
 ****************************************************************************************
 * @brief 数据库初始化完成回调函数
 *        当GATT数据库初始化完成后调用，初始化ADC、显示时钟并启动广播
 ****************************************************************************************
 */
void user_app_on_db_init_complete( void )
{
	printk("\nuser_app_on_db_init_complete!\n");

	// 更新ADC值并打印电压
	int adcval = adc1_update();
	printk("Voltage: %d\n", adcval);

	// 万年历: 开机先按当前公历重算农历/星期/节气/节日 (之后逐日自动进位)
	cal_sync_date();

	// 打印并推送时钟数据
	clock_print();
	clock_push();

	// ★ 屏幕已由 ST7789 监控界面接管，不再绘制二维码
	user_app_adv_start();

	// 启动时钟定时器：直接按 1 秒节拍开始刷新（不再等待整分钟对齐）
	first_update_seconds = 1;
	first_timer_trigger = 1;
	app_clock_timer_used = app_easy_timer(100, app_clock_timer_cb);
}


/**
 ****************************************************************************************
 * @brief 启动应用广播
 *        构造广播数据（包含设备名称和EPD版本），并启动带超时的无向广播
 ****************************************************************************************
 */
void user_app_adv_start(void)
{
	u8 vbuf[4]; // 版本信息AD结构缓冲区

	// 如果已在广播状态，直接返回
	if(adv_state)
		return;
	adv_state = 1; // 标记为正在广播

    // 获取广播命令结构
	struct gapm_start_advertise_cmd* cmd = app_easy_gap_undirected_advertise_get_active();
	// 添加设备名称AD结构
	app_add_ad_struct(cmd, adv_name, adv_name[0]+1, 1);

	// 构造版本信息AD结构（长度+类型+版本号低两位）
	vbuf[0] = 0x03;
	vbuf[1] = GAP_AD_TYPE_MANU_SPECIFIC_DATA;
	vbuf[2] = EPD_VERSION&0xff;
	vbuf[3] = (EPD_VERSION>>8)&0xff;
	app_add_ad_struct(cmd, vbuf, vbuf[0]+1, 1);

	// 启动带超时的无向广播
	app_easy_gap_undirected_advertise_with_timeout_start(user_default_hnd_conf.advertise_period, NULL);
	printk("\nuser_app_adv_start! %s\n", adv_name+2);
}


/**
 ****************************************************************************************
 * @brief 连接事件回调函数
 *        当收到连接请求时调用，更新连接索引，检查连接参数并在需要时请求参数更新
 * @param[in] connection_idx 连接索引
 * @param[in] param          连接请求参数
 ****************************************************************************************
 */
void user_app_connection(uint8_t connection_idx, struct gapc_connection_req_ind const *param)
{
	printk("user_app_connection: %d\n", connection_idx);

    // 检查连接是否有效
    if (app_env[connection_idx].conidx != GAP_INVALID_CONIDX)
    {
        app_connection_idx = connection_idx; // 更新连接索引
		g_mon.connected = 1;   // ★ 任意 BLE 主机(手机/一键对时工具)连上即点亮蓝牙图标

		// 打印连接参数
		printk("  interval: %d\n", param->con_interval);
		printk("  latency : %d\n", param->con_latency);
		printk("  sup_to  : %d\n", param->sup_to);
        
        // 检查连接参数是否符合预期，不符合则调度参数更新请求
        if ((param->con_interval < user_connection_param_conf.intv_min) ||
            (param->con_interval > user_connection_param_conf.intv_max) ||
            (param->con_latency != user_connection_param_conf.latency) ||
            (param->sup_to != user_connection_param_conf.time_out))
        {
            app_param_update_request_timer_used = app_easy_timer(APP_PARAM_UPDATE_REQUEST_TO, param_update_request_timer_cb);
        }
		
		// 推送时钟数据到客户端
		clock_push();
    } else {
		adv_state = 0; // 连接无效时，标记为未广播
    }

    // 执行默认连接处理
    default_app_on_connection(connection_idx, param);
}

/**
 ****************************************************************************************
 * @brief 无向广播完成回调函数
 *        当广播超时或异常结束时调用，更新广播状态并刷新屏幕
 * @param[in] status 广播结束状态码
 ****************************************************************************************
 */
void user_app_adv_undirect_complete(uint8_t status)
{
	printk("user_app_adv_undirect_complete: %02x\n", status);
	/* 广播结束(含正常超时 status=0)都必须清标志: 否则 user_app_adv_start()
	 * 会一直直接 return, 设备超时/断连后再也不广播。
	 * 1 秒时钟回调在 app_connection_idx==-1 时会立刻重启广播, 保持随时可连接。 */
	adv_state = 0;
	(void)status;
}


/**
 ****************************************************************************************
 * @brief 断开连接回调函数
 *        当连接断开时调用，清理定时器，更新连接状态，并根据断开原因决定是否重启广播
 * @param[in] param 断开连接参数（包含断开原因）
 ****************************************************************************************
 */
void user_app_disconnect(struct gapc_disconnect_ind const *param)
{
	printk("user_app_disconnect! reason=%02x\n", param->reason);

    // 取消参数更新请求定时器
    if (app_param_update_request_timer_used != EASY_TIMER_INVALID_TIMER)
    {
        app_easy_timer_cancel(app_param_update_request_timer_used);
        app_param_update_request_timer_used = EASY_TIMER_INVALID_TIMER;
    }

	app_connection_idx = -1; // 重置连接索引为无效值
	adv_state = 0; // 标记为未广播
	g_mon.connected = 0;   // 蓝牙断开: 屏幕右上角图标转灰, 走时完全不受影响

	/* ★ 独立万年历: 不管断开原因, 都立即恢复可连接广播,
	 *   保证手机/对时工具随时能再连上写 0x91。 */
	user_app_adv_start();
}


/**
 ****************************************************************************************
 * @brief 未处理消息的捕获处理函数
 *        处理各类未被默认处理的消息，包括特征值读写、参数更新、MTU变更等事件
 * @param[in] msgid   消息ID
 * @param[in] param   消息参数
 * @param[in] dest_id 目标任务ID
 * @param[in] src_id  源任务ID
 ****************************************************************************************
 */
void user_catch_rest_hndl(ke_msg_id_t const msgid,
                          void const *param,
                          ke_task_id_t const dest_id,
                          ke_task_id_t const src_id)
{
    switch(msgid)
    {
        // 特征值写入通知（值已写入数据库）
        case CUSTS1_VAL_WRITE_IND:
        {
            struct custs1_val_write_ind const *msg_param = (struct custs1_val_write_ind const *)(param);

            // 根据句柄分发到对应的处理函数
            switch (msg_param->handle)
            {
                case SVC1_IDX_CONTROL_POINT_VAL:
                    user_svc1_ctrl_wr_ind_handler(msgid, msg_param, dest_id, src_id);
                    break;

                case SVC1_IDX_LONG_VALUE_VAL:
                    user_svc1_long_val_wr_ind_handler(msgid, msg_param, dest_id, src_id);
                    break;

                default:
                    break;
            }
        } break;

        // Notification确认（请求已发出）
        case CUSTS1_VAL_NTF_CFM:
        {
        } break;

        // Indication确认（请求已发出）
        case CUSTS1_VAL_IND_CFM:
        {
        } break;

        // 读ATT_INFO请求（需要返回数据）
        case CUSTS1_ATT_INFO_REQ:
        {
            struct custs1_att_info_req const *msg_param = (struct custs1_att_info_req const *)param;

            // 根据属性索引分发处理
            switch (msg_param->att_idx)
            {
                case SVC1_IDX_LONG_VALUE_VAL:
                    user_svc1_long_val_att_info_req_handler(msgid, msg_param, dest_id, src_id);
                    break;

                default:
                    user_svc1_rest_att_info_req_handler(msgid, msg_param, dest_id, src_id);
                    break;
             }
        } break;

        // 连接参数更新通知
        case GAPC_PARAM_UPDATED_IND:
        {
            struct gapc_param_updated_ind const *msg_param = (struct gapc_param_updated_ind const *)(param);
			printk("GAPC_PARAM_UPDATED_IND!\n");
			// 打印更新后的参数
			printk("  interval: %d\n", msg_param->con_interval);
			printk("  latency : %d\n", msg_param->con_latency);
			printk("  sup_to  : %d\n", msg_param->sup_to);

            // 检查更新后的参数是否符合预期
            if ((msg_param->con_interval >= user_connection_param_conf.intv_min) &&
                (msg_param->con_interval <= user_connection_param_conf.intv_max) &&
                (msg_param->con_latency == user_connection_param_conf.latency) &&
                (msg_param->sup_to == user_connection_param_conf.time_out))
            {
				printk("  match!\n");
            }
        } break;

        // 特征值读取请求
        case CUSTS1_VALUE_REQ_IND:
        {
			printk("CUSTS1_VALUE_REQ_IND!\n");
            struct custs1_value_req_ind const *msg_param = (struct custs1_value_req_ind const *) param;

            // 处理未定义的读取请求，返回错误
            switch (msg_param->att_idx)
            {
                default:
                {
                    struct custs1_value_req_rsp *rsp = KE_MSG_ALLOC(CUSTS1_VALUE_REQ_RSP,
                                                                    src_id,
                                                                    dest_id,
                                                                    custs1_value_req_rsp);

                    rsp->conidx  = app_env[msg_param->conidx].conidx;
                    rsp->att_idx = msg_param->att_idx;
                    rsp->length = 0;
                    rsp->status  = ATT_ERR_APP_ERROR;
                    KE_MSG_SEND(rsp);
                } break;
             }
        } break;

        // GATT事件请求指示（确认未处理的指示以避免超时）
        case GATTC_EVENT_REQ_IND:
        {
            struct gattc_event_ind const *ind = (struct gattc_event_ind const *) param;
            struct gattc_event_cfm *cfm = KE_MSG_ALLOC(GATTC_EVENT_CFM, src_id, dest_id, gattc_event_cfm);
            cfm->handle = ind->handle;
            KE_MSG_SEND(cfm);
        } break;
		
		// MTU（最大传输单元）变更指示
		case GATTC_MTU_CHANGED_IND:
		{
			struct gattc_mtu_changed_ind *ind = (struct gattc_mtu_changed_ind *) param;
			printk("GATTC_MTU_CHANGED_IND: %d\n", ind->mtu);
		} break;

        // 未处理的消息
        default:
		{
			printk("Unhandled msgid=%08x\n", msgid);
		} break;
    }
}

/// @} APP