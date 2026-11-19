/**
 ****************************************************************************************
 *
 * @file user_custs1_impl.c
 *
 * @brief Peripheral project Custom1 Server implementation source code.
 *
 * Copyright (C) 2015-2023 Renesas Electronics Corporation and/or its affiliates.
 * All rights reserved. Confidential Information.
 *
 * This software ("Software") is supplied by Renesas Electronics Corporation and/or its
 * affiliates ("Renesas"). Renesas grants you a personal, non-exclusive, non-transferable,
 * revocable, non-sub-licensable right and license to use the Software, solely if used in
 * or together with Renesas products. You may make copies of this Software, provided this
 * copyright notice and disclaimer ("Notice") is included in all such copies. Renesas
 * reserves the right to change or discontinue the Software at any time without notice.
 *
 * THE SOFTWARE IS PROVIDED "AS IS". RENESAS DISCLAIMS ALL WARRANTIES OF ANY KIND,
 * WHETHER EXPRESS, IMPLIED, OR STATUTORY, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. TO THE
 * MAXIMUM EXTENT PERMITTED UNDER LAW, IN NO EVENT SHALL RENESAS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, SPECIAL, INCIDENTAL OR CONSEQUENTIAL DAMAGES ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE, EVEN IF RENESAS HAS BEEN ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGES. USE OF THIS SOFTWARE MAY BE SUBJECT TO TERMS AND CONDITIONS CONTAINED IN
 * AN ADDITIONAL AGREEMENT BETWEEN YOU AND RENESAS. IN CASE OF CONFLICT BETWEEN THE TERMS
 * OF THIS NOTICE AND ANY SUCH ADDITIONAL LICENSE AGREEMENT, THE TERMS OF THE AGREEMENT
 * SHALL TAKE PRECEDENCE. BY CONTINUING TO USE THIS SOFTWARE, YOU AGREE TO THE TERMS OF
 * THIS NOTICE.IF YOU DO NOT AGREE TO THESE TERMS, YOU ARE NOT PERMITTED TO USE THIS
 * SOFTWARE.
 *
 ****************************************************************************************
 */

/*
 * INCLUDE FILES
 ****************************************************************************************
 */

#include "gpio.h"               // GPIO控制相关头文件
#include "app_api.h"            // 应用程序API
#include "app.h"                // 应用程序核心功能
#include "prf_utils.h"          // BLE配置文件工具
#include "custs1.h"             // 自定义服务1
#include "custs1_task.h"        // 自定义服务1任务
#include "user_custs1_def.h"    // 自定义服务1定义
#include "user_custs1_impl.h"   // 自定义服务1实现
#include "user_peripheral.h"    // 用户外设相关
#include "user_periph_setup.h"  // 用户外设设置
#include "adc.h"                // ADC(模数转换)相关

#include "epd.h"                // 电子墨水屏驱动
#include "monitor_ui.h"         // ★ PC 资源监控界面 (g_mon / ui_update)
#include "ui_cfg.h"             // ★ 界面/LED 运行配置

/*
 * 全局变量定义
 * 这些变量使用__SECTION_ZERO("retention_mem_area0")属性放置在掉电保持内存区域
 ****************************************************************************************
 */

// 定时器ID，用于系统定时任务
ke_msg_id_t timer_used      __SECTION_ZERO("retention_mem_area0"); //@RETENTION MEMORY
// 指示计数器，用于BLE通知计数
uint16_t indication_counter __SECTION_ZERO("retention_mem_area0"); //@RETENTION MEMORY
// 非数据库值计数器
uint16_t non_db_val_counter __SECTION_ZERO("retention_mem_area0"); //@RETENTION MEMORY
// ADC采样值，用于电池电量检测
int adcval;

static uint8_t h24_format = 1; // 24小时制标志
static int tempreture = 25;

static void get_holiday(void);
void cal_sync_date(void);          /* 万年历: 由公历重算农历/星期/节气/节日 */

extern int adv_state;
/*
 * FUNCTION DEFINITIONS
 ****************************************************************************************
 */


/**
 * @brief 更新ADC采样值并通过BLE发送电池电压数据
 * 
 * 该函数执行以下操作：
 * 1. 校准ADC偏移量
 * 2. 获取电池电压采样值
 * 3. 将采样值转换为实际电压值
 * 4. 通过BLE发送电压值给连接的设备
 * 
 * @return 返回计算后的电压值
 */
int adc1_update(void)
{
    // 校准ADC偏移，使用单端输入模式
    adc_offset_calibrate(ADC_INPUT_MODE_SINGLE_ENDED);
    // 获取电池电压采样值
    adcval = adc_get_vbat_sample(false);
    // 将ADC值转换为实际电压值 (单位: mV)
    int volt = (adcval*225)>>7;

    // 分配内存并构造BLE消息
    struct custs1_val_set_req *req = KE_MSG_ALLOC_DYN(CUSTS1_VAL_SET_REQ, 
                                                      prf_get_task_from_id(TASK_ID_CUSTS1), 
                                                      TASK_APP, 
                                                      custs1_val_set_req, 
                                                      DEF_SVC1_ADC_VAL_1_CHAR_LEN);
    // 设置连接索引
    req->conidx = app_env->conidx;
    // 设置特征值句柄
    req->handle = SVC1_IDX_ADC_VAL_1_VAL;
    // 设置数据长度
    req->length = DEF_SVC1_ADC_VAL_1_CHAR_LEN;
    // 设置电压值（16位，低字节在前）
    req->value[0] = volt&0xff;
    req->value[1] = volt>>8;
    // 发送BLE消息
    KE_MSG_SEND(req);
    
    return volt;
}


/****************************************************************************************/

/**
 * 农历年份数据表（2020-2051年）
 * 每个16位数据包含以下信息：
 * - 高4位(bit 15-12): 闰月月份，0表示当年无闰月
 * - 低12位(bit 11-0): 每个月的大小月标记，1表示大月(30天)，0表示小月(29天)
 * 例如：0x07954
 * - 0: 无闰月
 * - 7954: 从正月到十二月的大小月情况
 */
static const uint16_t lunar_year_info[32] =
{
    0x07954, 0x06aa0, 0x0ad50, 0x05b52, 0x04b60, 0x0a6e6, 0x0a4e0, 0x0d260, 0x0ea65, 0x0d530, //2020-2029
    0x05aa0, 0x076a3, 0x096d0, 0x04afb, 0x04ad0, 0x0a4d0, 0x0d0b6, 0x0d250, 0x0d520, 0x0dd45, //2030-2039
    0x0b5a0, 0x056d0, 0x055b2, 0x049b0, 0x0a577, 0x0a4b0, 0x0aa50, 0x0b255, 0x06d20, 0x0ada0, //2040-2049
    0x04b63, 0x09370,                                                                         //2050-2051
};

// 额外的农历年份信息，用于标记特殊年份的闰月情况
static const uint32_t lunar_year_info2 = 0x48010000;


/**
 * 24节气时间数据表
 * 存储了一年中24个节气相对于"小寒"的时间间隔（以秒为单位）
 * 
 * 节气顺序：
 * 1-6月：小寒、大寒、立春、雨水、惊蛰、春分
 * 7-12月：清明、谷雨、立夏、小满、芒种、夏至
 * 13-18月：小暑、大暑、立秋、处暑、白露、秋分
 * 19-24月：寒露、霜降、立冬、小雪、大雪、冬至
 */
static int jieqi_info[] =
{
    0,        1272283,  2547462,  3828568,  5117483,  6416376,  // 小寒到春分
    7726093,  9047327,  10379235, 11721860, 13072410, 14428379, // 清明到夏至
    15787551, 17145937, 18501082, 19850188, 21190911, 22520708, // 小暑到秋分
    23839844, 25146961, 26443845, 27730671, 29010666, 30284551, // 寒露到冬至
};

/**
 * 2020年"小寒"节气的基准时间
 * 相对于2020年1月1日的秒数
 * 用于计算其他年份的节气时间
 */
#define xiaohan_2020  451804


/**
 * 全局时间变量定义
 * 公历日期相关变量：
 * year: 年份，如2025
 * month: 月份，0-11表示1-12月
 * date: 日期，0-30表示1-31日
 * wday: 星期，0-6表示星期日到星期六
 * 
 * 农历日期相关变量：
 * l_year: 农历年在lunar_year_info数组中的索引，如4表示2024年
 * l_month: 农历月份，0-11表示正月到腊月，最高位1表示闰月
 * l_date: 农历日期，0-29表示初一到三十
 * 
 * 时间相关变量：
 * hour: 小时，0-23
 * minute: 分钟，0-59
 * second: 秒，0-59
 */
int year=2025, month=0, date=0, wday=3;
int l_year=4, l_month=11, l_date=1;
int hour=0, minute=0, second=0;
// 上次对时后，经过的分钟数
int cal_minute=-1;

/* ---------------------------------------------------------------- 首次上电兜底
 * Flash 里还没有任何 RTC 快照时(例如刚烧完固件), 用固件编译时刻作为初始时间,
 * 至少不会显示 2025-01-01。之后由 0x91 蓝牙对时精确校准, 并写入 Flash 长期保存。
 * __DATE__ = "Nov 19 2026" (单数字日前面是空格), __TIME__ = "08:04:47"。
 */
void rtc_seed_default(void)
{
	static const char *mon[12] = {
		"Jan","Feb","Mar","Apr","May","Jun",
		"Jul","Aug","Sep","Oct","Nov","Dec"
	};
	static const char d[] = __DATE__;
	static const char t[] = __TIME__;
	int i, m = 0;

	for(i = 0; i < 12; i++){
		if(d[0]==mon[i][0] && d[1]==mon[i][1] && d[2]==mon[i][2]){
			m = i;
			break;
		}
	}

	year   = (d[7]-'0')*1000 + (d[8]-'0')*100 + (d[9]-'0')*10 + (d[10]-'0');
	month  = m;
	date   = ((d[4]==' ') ? 0 : (d[4]-'0')*10) + (d[5]-'0') - 1;
	hour   = (t[0]-'0')*10 + (t[1]-'0');
	minute = (t[3]-'0')*10 + (t[4]-'0');
	second = (t[6]-'0')*10 + (t[7]-'0');

	if(year < 2020 || year > 2099) year = 2026;
	if(date < 0)   date = 0;
	if(date > 30)  date = 30;
}


//GUIQRLB
// 来自 pic.py 自动生成的 C 数组
const unsigned char QR_31x31[31][4] = {
    {0x00, 0x00, 0x00, 0x00},
    {0x7F, 0x1E, 0x99, 0xFC},
    {0x41, 0x3B, 0xC5, 0x04},
    {0x5D, 0x5D, 0x6D, 0x74},
    {0x5D, 0x11, 0x9D, 0x74},
    {0x5D, 0x63, 0xD1, 0x74},
    {0x41, 0x43, 0x59, 0x04},
    {0x7F, 0x55, 0x55, 0xFC},
    {0x00, 0x06, 0xB0, 0x00},
    {0x25, 0x43, 0x96, 0xD0},
    {0x74, 0x55, 0x2C, 0x74},
    {0x7B, 0xB1, 0x22, 0xC4},
    {0x5E, 0xB4, 0xE2, 0xE4},
    {0x53, 0xBA, 0x6E, 0x98},
    {0x72, 0x54, 0xE1, 0xF4},
    {0x0B, 0xC8, 0xD5, 0x1C},
    {0x32, 0xEA, 0xCD, 0x20},
    {0x7B, 0x13, 0xCC, 0x4C},
    {0x4A, 0xC0, 0x1A, 0x9C},
    {0x1B, 0x55, 0xB5, 0x7C},
    {0x1A, 0x8E, 0xF5, 0x54},
    {0x77, 0xBD, 0x27, 0xE0},
    {0x00, 0x6B, 0xDC, 0x74},
    {0x7F, 0x0A, 0xED, 0x54},
    {0x41, 0x1B, 0x3C, 0x64},
    {0x5D, 0x55, 0x9F, 0xE4},
    {0x5D, 0x3E, 0x48, 0x38},
    {0x5D, 0x2B, 0x4E, 0x0C},
    {0x41, 0x60, 0x20, 0x2C},
    {0x7F, 0x0D, 0xB0, 0xC8},
    {0x00, 0x00, 0x00, 0x00},
};

const unsigned char LB_31x31[31][4] = {
    {0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00},
    {0x00, 0x07, 0xC0, 0x00},
    {0x00, 0x04, 0x40, 0x00},
    {0x00, 0xFF, 0xFE, 0x00},
    {0x00, 0x80, 0x02, 0x00},
    {0x01, 0x80, 0x03, 0x00},
    {0x01, 0x00, 0x01, 0x00},
    {0x01, 0x00, 0x01, 0x00},
    {0x01, 0x03, 0x81, 0x00},
    {0x01, 0x03, 0x81, 0x00},
    {0x01, 0x03, 0x81, 0x00},
    {0x01, 0x03, 0x81, 0x00},
    {0x01, 0x03, 0x81, 0x00},
    {0x01, 0x03, 0x81, 0x00},
    {0x01, 0x03, 0x81, 0x00},
    {0x01, 0x03, 0x81, 0x00},
    {0x01, 0x03, 0x81, 0x00},
    {0x01, 0x00, 0x01, 0x00},
    {0x01, 0x00, 0x01, 0x00},
    {0x01, 0x03, 0x81, 0x00},
    {0x01, 0x03, 0x81, 0x00},
    {0x01, 0x03, 0x81, 0x00},
    {0x01, 0x00, 0x01, 0x00},
    {0x01, 0x00, 0x01, 0x00},
    {0x01, 0xC0, 0x07, 0x00},
    {0x00, 0x40, 0x04, 0x00},
    {0x00, 0x7F, 0xFC, 0x00},
    {0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00},
};


/**
 * 获取农历月份的天数
 * 
 * @param mon 月份编号，最高位为1表示闰月
 * @param yinfo_out 输出参数，用于返回年份信息
 * @return 返回该月的天数（29或30）
 */
static int get_lunar_mdays(int mon, int *yinfo_out)
{
    // 获取闰月标志（最高位）
    int lflag = mon&0x80;
    // 获取实际月份（去除闰月标志）
    mon &= 0x7f;

	// 取得当年的信息
	int yinfo = lunar_year_info[l_year];
	if(lunar_year_info2&(1<<l_year))
		yinfo |= 0x10000;

	// 取得当月的天数
	int mdays = 29;
	if(lflag){
		if(yinfo&0x10000) mdays += 1;
	}else{
		if(yinfo&(0x8000>>mon))	mdays += 1;
	}

	if(yinfo_out)
		*yinfo_out = yinfo;
	return mdays;
}


// 农历增加一天
void ldate_inc(void)
{
	int lflag = l_month&0x80;
	int mon = l_month&0x7f;
	int yinfo;

	int mdays = get_lunar_mdays(l_month, &yinfo);

	l_date += 1;
	if(l_date==mdays){
		l_date = 0;
		mon += 1;
		if(lflag==0 && mon==(yinfo&0x0f)){
			lflag = 0x80;
			mon -= 1;
		}else{
			lflag = 0;
		}
		if(mon==12){
			mon = 0;
			l_year += 1;
		}
		l_month = lflag|mon;
	}
}


// 给出年月日，返回是否是节气日
int jieqi(int year, int month, int date)
{
	uint8_t d2m[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
	int is_leap = (year%4)? 0 : (year%100)? 1: (year%400)? 0: 1;
	d2m[1] += is_leap;

	// 计算当前日期是本年第几天
	int i, d=0;
	for(i=0; i<month; i++){
		d += d2m[i];
	}
	d += date;

	// 计算闰年的天数。因为只考虑2020-2052年，这里做了简化。
	int Y = year-2020;
	int L = (Y)? (Y-1)/4+1 : 0;
	// 计算当年小寒的秒数
	int xiaohan_sec = xiaohan_2020 + 20950*Y - L*86400;
	//    20926是一个回归年(365.2422)不足一天的秒数(.2422*86400).
	//    直接用有明显的误差。这里稍微增大了一点(20926+24)。

	for(i=0; i<24; i++){
		int sec = xiaohan_sec + jieqi_info[i];
		int day = sec/86400;
		if(day==d)
			return i;
	}

	return -1;
}


/****************************************************************************************/


static int get_month_day(int mon)
{
	uint8_t d2m[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
	int is_leap = (year%4)? 0 : (year%100)? 1: (year%400)? 0: 1;
	d2m[1] += is_leap;

	return d2m[month];
}


// 增加1天
void date_inc(void)
{
	wday += 1;
	if(wday>=7)
		wday = 0;
	
	date += 1;
	if(date==get_month_day(month)){
		date = 0;
		month += 1;
		if(month>=12){
			month = 0;
			year += 1;
		}
	}
}

// 0: 状态不变
// 1: 分钟改变
// 2: 分钟改变10分钟
// 3: 小时改变
// 4: 天数改变

int clock_update(int inc)
{
	int retv = 0;

	second += inc;
	if(second<60)
		return retv;
	second -= 60;

	minute += 1;
	retv = 1;
	if((minute%10)==0)
		retv = 2;

	if(cal_minute>=0)
		cal_minute += 1;

	if(minute>=60){
		minute = 0;
		hour += 1;
		retv = 3;
		if(hour>=24){
			hour = 0;
			date_inc();
			ldate_inc();
			get_holiday();
			retv = 4;
		}
	}

	return retv;
}

void clock_set(uint8_t *buf)
{
	/* 0x91 对时包: [1..2]=年 [3]=月(0-based) [4]=日(1-based)
	 * [5]=时 [6]=分 [7]=秒, 其余字节忽略。
	 * 农历/星期/节气/节日一律由固件按公历重算, 蓝牙断开也能独立走时。 */
	year   = buf[1] + buf[2]*256;
	month  = buf[3];
	date   = buf[4]-1;
	hour   = buf[5];
	minute = buf[6];
	second = buf[7];

	if(month<0 || month>11) month = 0;
	if(date<0)  date = 0;
	if(date>30) date = 30;
	if(hour>23) hour = 23;
	if(minute>59) minute = 59;
	if(second>59) second = 59;

	cal_sync_date();

	cal_minute = 0;

	app_clock_timer_restart();   /* 重新对齐 1 秒节拍, 避免对时后秒数停住 */

	/* 独立万年历: 对时结果立刻写入 Flash, 断电重启后仍能恢复。
	 * 不在这里整屏重绘(软件 SPI 很慢, 会挤占 BLE 主循环);
	 * 由下一个 1 秒 tick 的 card_calendar() 按差异缓存自动刷新。 */
	rtc_save_now(year, month, date + 1, hour, minute, second);
}


void clock_push(void)
{
	/* value[] 是 __ARRAY_EMPTY, KE_MSG_ALLOC_DYN 最后一个参数就是可用字节数。
	 * 下面写 13 字节 (value[0..12]), 原版只申请 11 会越界 2 字节; 修正为 13。 */
	struct custs1_val_set_req *req = KE_MSG_ALLOC_DYN(CUSTS1_VAL_SET_REQ, prf_get_task_from_id(TASK_ID_CUSTS1), TASK_APP, custs1_val_set_req, 13);

	req->conidx = app_env->conidx;
	req->handle = SVC1_IDX_LONG_VALUE_VAL;
	req->length = 13;
	req->value[0] = year&0xff;
	req->value[1] = year>>8;
	req->value[2] = month;
	req->value[3] = date+1;
	req->value[4] = hour;
	req->value[5] = minute;
	req->value[6] = second;
	req->value[7] = (cal_minute&0xff);
	req->value[8] = (cal_minute>>8 )&0xff;
	req->value[9] = (cal_minute>>16)&0xff;
	req->value[10]= (cal_minute>>24)&0xff;
	req->value[11] = (clock_fixup_value&0xff);
	req->value[12] = (clock_fixup_value>>8 )&0xff;
	KE_MSG_SEND(req);
}


void clock_print(void)
{
	printk("\n%04d-%02d-%02d %02d:%02d:%02d  L: %d-%d\n", year, month+1, date+1, hour, minute, second, l_month+1, l_date+1);
}


/****************************************************************************************/

static char *jieqi_name[] = {
	"小寒", "大寒", "立春", "雨水", "惊蛰", "春分",
	"清明", "谷雨", "立夏", "小满", "芒种", "夏至",
	"小暑", "大暑", "立秋", "处暑", "白露", "秋分",
	"寒露", "霜降", "立冬", "小雪", "大雪", "冬至",
};
static char *wday_str[] = {"日", "一", "二", "三", "四", "五", "六"};
static char *lday_str_lo[] = {"一", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "腊", "正"};
static char *lday_str_hi[] = {"初", "十", "廿", "二", "三"};

static int epd_wait_state;
static timer_hnd epd_wait_hnd;

typedef struct {
    char *name;
    uint8_t mon;
    uint8_t day;
}HOLIDAY_INFO;

HOLIDAY_INFO hday_info[] = {
    // 农历传统节日（0x80 表示农历日期，0xc0 表示月末）
    {"腊八",     0x80|12,  8},   // 农历腊月初八
    {"小年",     0x80|12, 23},   // 农历腊月廿三
    {"除夕",     0xc0|12, 30},   // 农历腊月最后一天
    {"春节",     0x80| 1,  1},   // 农历正月初一
    {"元宵节",   0x80| 1, 15},   // 农历正月十五
    {"龙抬头",   0x80| 2,  2},   // 农历二月初二
    {"寒食节",   0x80| 3,  3},   // 农历三月初三
    {"端午节",   0x80| 5,  5},   // 农历五月初五
    {"七夕节",   0x80| 7,  7},   // 农历七月初七
    {"中元节",   0x80| 7, 15},   // 农历七月十五
    {"中秋节",   0x80| 8, 15},   // 农历八月十五
    {"重阳节",   0x80| 9,  9},   // 农历九月初九
    {"下元节",   0x80|10, 15},   // 农历十月十五

    // 阳历法定节假日和纪念日
    {"元旦",         1,  1},
    {"湿地日",       2,  2},
    {"情人节",       2, 14},
    {"妇女节",       3,  8},
    {"植树节",       3, 12},
    {"权益日",       3, 15},
    {"愚人节",       4,  1},
    {"读书日",       4, 23},
    {"航天日",       4, 24},
    {"劳动节",       5,  1},
    {"青年节",       5,  4},
    {"护士节",       5, 12},
    {"儿童节",       6,  1},
    {"环境日",       6,  5},
    {"遗产日",       6,  8},
    {"建党节",       7,  1},
    {"建军节",       8,  1},
    {"抗战日",       9,  3},
    {"教师节",       9, 10},
    {"安全日",       9, 15},
    {"烈士日",       9, 30},
    {"国庆节",      10,  1},
	{"程序员节",    10, 24},
	{"万圣节",      10, 31},
    {"消防日",      11,  9},
    {"记者节",      11,  8},
	{"光棍节",      11, 11},
    {"宪法日",      12,  4},
    {"志愿日",      12,  5},
    {"公祭日",      12, 13},
	{"圣诞节",      12, 25},

    // 特殊周期性节日
    {"母亲节",       5, 0x97},  // 5月第二个周日
    {"父亲节",       6, 0xa7},  // 6月第三个周日
    {"感恩节",      11, 0xa4},  // 11月第四个周四

    {"",             0,  0}     // 结束标记
};


static char *jieqi_str = "小寒";
static char *holiday_str = "元旦";

static void ldate_str(char *buf)
{
	char *lflag = (l_month&0x80)? "闰" : "";
	int lm = l_month&0x7f;
	if(lm==0){
		lm = 12;
	}

	int hi = l_date/10;
	int lo = l_date%10;
	
	if(lo==9){
		if(hi==1)
			hi = 3;
		else if(hi==2)
			hi = 4;
	}
	
	sprintf(buf, "%s%s月%s%s", lflag, lday_str_lo[lm], lday_str_hi[hi], lday_str_lo[lo]);
}


static void set_holiday(int index)
{
	if(holiday_str==NULL){
		holiday_str = hday_info[index].name;
	}else if(jieqi_str==NULL){
		// 已经有一个农历节日了，将其转移到节气位置。
		jieqi_str = holiday_str;
		holiday_str = hday_info[index].name;
	}else{
		// printf("OOPS! 节日溢出!\n");
	}
}

void get_holiday(void)
{
	int i;

	jieqi_str = NULL;
	holiday_str = NULL;

	i = jieqi(year, month, date);
	if(i>=0){
		jieqi_str = jieqi_name[i];
	}

	i = 0;
	while(hday_info[i].mon){
		int mon = hday_info[i].mon;
		int day = hday_info[i].day;
		int mflag = mon&0xc0;
		int dflag = day;
		mon = (mon&0x0f)-1;
		day = (day&0x1f)-1;
		if(mflag&0x80){
			// 农历节日
			if(mflag&0x40){
				// 当月最后一天
				int mdays = get_lunar_mdays(l_month, NULL);
				day = mdays-1;
			}
			if(l_month==mon && l_date==day){
				set_holiday(i);
			}
		}else{
			// 公历节日
			if(dflag&0x80){
				// 第几个周天
				int wc = date/7;
				int hwc = (dflag>>4)&0x03;
				day &= 0x07;
				if(month==mon && wc==hwc && wday==day){
					set_holiday(i);
				}
			}else if(month==mon && date==day){
				set_holiday(i);
			}
		}
		i += 1;
	}

	return;
}


/* ============================================================================
 * 万年历: 公历 -> 农历 / 星期 (完全离线, 不依赖上位机)
 * 基准: 公历 2020-01-25 = 农历 2020 年正月初一 (l_year=0,l_month=0,l_date=0)
 * ============================================================================ */

static int cal_is_leap(int y)
{
	return ((y%4)==0 && (y%100)!=0) || ((y%400)==0);
}

/* 自 2020-01-01 起的天数: m 为 0-based 月份, d 为 1-based 日期 */
static int cal_day_no(int y, int m, int d)
{
	static const uint8_t md[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
	int days = 0;
	int i;

	if(y<2020) y = 2020;
	for(i=2020; i<y; i++)
		days += cal_is_leap(i) ? 366 : 365;
	for(i=0; i<m; i++){
		days += md[i];
		if(i==1 && cal_is_leap(y)) days += 1;
	}
	return days + d - 1;
}

/* 农历推进一天: 完全复刻 ldate_inc(), 但只操作局部变量 */
static void cal_lunar_next(int *ply, int *plm, int *pld)
{
	int ly = *ply, lm = *plm, ld = *pld;
	int lflag = lm & 0x80;
	int mon   = lm & 0x7f;
	int yinfo = lunar_year_info[ly];
	int mdays;

	if(lunar_year_info2 & (1u<<ly)) yinfo |= 0x10000;

	mdays = 29;
	if(lflag){
		if(yinfo & 0x10000) mdays += 1;
	}else{
		if(yinfo & (0x8000>>mon)) mdays += 1;
	}

	ld += 1;
	if(ld==mdays){
		ld = 0;
		mon += 1;
		if(lflag==0 && mon==(yinfo&0x0f)){
			lflag = 0x80;
			mon -= 1;
		}else{
			lflag = 0;
		}
		if(mon==12){
			mon = 0;
			ly += 1;
		}
		lm = lflag | mon;
	}
	*ply = ly; *plm = lm; *pld = ld;
}

void cal_solar_to_lunar(int y, int m, int d, int *ply, int *plm, int *pld)
{
	int n = cal_day_no(y,m,d) - cal_day_no(2020,0,25);
	int ly = 0, lm = 0, ld = 0;

	while(n-- > 0){
		cal_lunar_next(&ly,&lm,&ld);
		if(ly >= 32){ ly = 31; break; }   /* 农历表覆盖 2020..2051 */
	}
	*ply = ly; *plm = lm; *pld = ld;
}

/* 0=星期日 .. 6=星期六; 2020-01-25 为星期六 */
int cal_wday(int y, int m, int d)
{
	int n = cal_day_no(y,m,d) - cal_day_no(2020,0,25);
	int w = (6 + (n%7)) % 7;
	return (w<0) ? w+7 : w;
}

/* 依据当前公历重算农历/星期/节气/节日。开机与对时调用, 之后由走时逐日进位 */
void cal_sync_date(void)
{
	/* 注意: 固件内部 date/month 都是 0-based, 而 cal_day_no() 的日参数是
	 * 1-based, 必须 +1, 否则农历与星期都会少一天。 */
	cal_solar_to_lunar(year, month, date + 1, &l_year, &l_month, &l_date);
	wday = cal_wday(year, month, date + 1);
	get_holiday();
}

const char *cal_jieqi_str(void)   { return jieqi_str; }
const char *cal_holiday_str(void) { return holiday_str; }


/****************************************************************************************/


static uint8_t batt_cal(uint16_t adc_sample)
{
    uint8_t batt_lvl;

    if (adc_sample > 1705)
        batt_lvl = 100;
    else if (adc_sample <= 1705 && adc_sample > 1584)
        batt_lvl = 28 + (uint8_t)(( ( ((adc_sample - 1584) << 16) / (1705 - 1584) ) * 72 ) >> 16) ;
    else if (adc_sample <= 1584 && adc_sample > 1360)
        batt_lvl = 4 + (uint8_t)(( ( ((adc_sample - 1360) << 16) / (1584 - 1360) ) * 24 ) >> 16) ;
    else if (adc_sample <= 1360 && adc_sample > 1136)
        batt_lvl = (uint8_t)(( ( ((adc_sample - 1136) << 16) / (1360 - 1136) ) * 4 ) >> 16) ;
    else
        batt_lvl = 0;

    return batt_lvl;
}


/**
 * 绘制电池电量图标
 * 
 * @param x 图标左上角的x坐标
 * @param y 图标中心的y坐标
 * 
 * 图标说明：
 * - 外框大小：16x8像素
 * - 电量显示：根据实际电量百分比填充内部
 * - 电池正极：2x2像素
 */
static void draw_batt(int x, int y)
{
    // 获取电池电量百分比并转换为显示段数（0-10）
    int p = batt_cal(adcval);
    p /= 10;

    // 绘制电池外框
    draw_rect(x, y-4, x+14, y+4, BLACK);
    // 绘制电池正极
    draw_box(x-2, y-1, x-1, y+1, BLACK);

    // 绘制电量填充部分
    draw_box(x+12-p, y-2, x+12, y+2, BLACK);
}


const u8 font_bt[] = {
	0x08, 0x08, 0x0f, 0x00, 0x00,
	0x10, 0x18, 0x14, 0x92, 0x51, 0x32, 0x14, 0x18,
	0x14, 0x32, 0x51, 0x92, 0x14, 0x18, 0x10,
};

/**
 * 绘制蓝牙图标
 * 
 * @param x 图标中心的x坐标
 * @param y 图标中心的y坐标
 * 
 * 图标说明: 一个8x15的字符
 */
static void draw_bt(int x, int y)
{
	fb_draw_font_info(x, y, font_bt, BLACK);
}


/****************************************************************************************/

typedef struct {
	int xres, yres;
	int font_char;
	int font_dseg;
	u16 x[8];
	u16 y[8];
}LAYOUT;

// ★ 新版两栏布局（2026-10）：不再使用坐标表，全部按屏幕尺寸动态计算
//   顶部：大字时间（含秒）+ 电池/蓝牙图标 + 节气节日/上下午
//   底栏：2026-10-22 星期四 九月十三 26°C

int epd_update_count = 0;   // 已完成的刷屏次数（调试观测用）


/**
 * 电子墨水屏更新等待定时器
 * 
 * 功能说明：
 * - 检查电子墨水屏是否处于忙状态
 * - 如果忙，则40ms后再次检查
 * - 如果空闲，则完成更新流程并进入省电模式
 * 
 * 电子墨水屏更新完成后的处理：
 * 1. 读取温度
 * 2. ★ 秒级刷新模式：保持屏幕上电与 RAM 内容，不再深睡/断电
 *    （用户要求：不考虑功耗，显示秒数 + 局部刷新 + 不休眠）
 */
static void epd_wait_timer(void)
{
    if(epd_busy() && ++epd_wait_polls < 12){
        // 屏幕仍在忙，400ms 后再次检查（最多约 4.8 秒，超时后强制视为完成）
        epd_wait_hnd = app_easy_timer(40, epd_wait_timer);
    }else{
        // 屏幕更新完成
        epd_wait_hnd = EASY_TIMER_INVALID_TIMER;
        epd_update_pending = 0;   // 清除软件忙标志
        epd_update_count += 1;
		// 读取温度值
		tempreture = epd_temp();
        // 发送深度睡眠命令
        epd_cmd1(0x10, 0x01);
        // 关闭电源（★ 恢复作者原流程：控制器每次冷启动，百发百中）
        epd_power(0);
        // 关闭硬件接口
        epd_hw_close();
        // ★ 蓝牙休眠保持禁用（用户要求不考虑功耗、秒级刷新）：
        // arch_set_sleep_mode(ARCH_EXT_SLEEP_ON);
    }
}



void QR_draw()
{
	/* 墨水屏已停用(改为 ST7789 IPS), 原 EPD 绘图层整体失效。
	 * 保留空实现: 让 epd_gui 的双帧缓冲(fb_bw/fb_rr 共 9.5KB)和字库
	 * 不再被引用, 由 --gc-sections 回收, 腾出 RAM 给 TFT 字库。 */
}

void LB_draw()
{
	/* 同上: 墨水屏低电压码页面已停用 */
}

/**
 * 绘制时钟界面（★ 2026-10 v10 两栏布局）
 *
 * 顶栏（占满除底栏外全部高度）：
 *   大字 HH:MM（40px，垂直居中）+ 右下角小字秒数（20px，与大字底对齐）
 * 底栏（高度=日期文字高度 12px）：
 *   2026-10-22 星期四 九月十三 26°C
 *
 * 刷新：恢复作者验证过的完整流程（上电→初始化→全帧写入→lut_fly 触发→断电），
 * 视觉上的"局部刷新"由 lut_fly 波形实现（未变化的像素不被驱动，无闪刷新）。
 * 每秒一次；软件忙标志防止刷新重叠。
 */
void clock_draw(int flags)
{
	/* 墨水屏时钟页已停用(改为 ST7789 监控界面), 保留空实现以回收 EPD 绘图层 RAM */
	(void)flags;
}



/****************************************************************************************/


/**
 * 控制点写入指示处理函数
 * 
 * @param msgid 消息ID
 * @param param 写入参数
 * @param dest_id 目标任务ID
 * @param src_id 源任务ID
 * 
 * 处理通过BLE接收到的控制命令
 */
void user_svc1_ctrl_wr_ind_handler(ke_msg_id_t const msgid, 
                                  struct custs1_val_write_ind const *param, 
                                  ke_task_id_t const dest_id, 
                                  ke_task_id_t const src_id)
{
    // 打印接收到的控制命令
    printk("Control Point: %02x\n", param->value[0]);
}

/**
 * 长值特征写入指示处理函数
 * 
 * @param msgid 消息ID
 * @param param 写入参数
 * @param dest_id 目标任务ID
 * @param src_id 源任务ID
 * 
 * 处理命令：
 * - 0x91: 时钟设置命令
 * - 0xA0及以上: OTA升级相关命令
 */
void user_svc1_long_val_wr_ind_handler(ke_msg_id_t const msgid, 
                                      struct custs1_val_write_ind const *param, 
                                      ke_task_id_t const dest_id, 
                                      ke_task_id_t const src_id)
{
	if(param->value[0]==0x91){
		/* 0x91 对时包至少 8 字节; 手机 nRF Connect / 一键对时脚本都走这里。
		 * 不依赖任何 PC 常驻程序, 断开蓝牙后设备凭本地 RTC 独立走时。 */
		if(param->length < 8) return;
		clock_set((uint8_t*)param->value);
		clock_print();
	}else if(param->value[0]==0x90){
		//修改24-12小时制
		h24_format = !h24_format;
		clock_draw(DRAW_BT|UPDATE_FAST);
	}else if(param->value[0]==0x92){
		int diff_sec;
		/* HMWEBCTRL 网页校准: 必须先对时且间隔 >=2 天。这里再挡一次除零,
		 * 避免 cal_minute<=0 时 clock_fixup_set() 内 /cal_minute 触发 HardFault。 */
		if(param->length < 4 || cal_minute <= 0){
			printk("Calibration: skip (cal_minute=%d)\n", cal_minute);
			return;
		}
		diff_sec  = param->value[1];
		diff_sec |= param->value[2]<<8;
		diff_sec  = (diff_sec<<16)>>16;
		printk("Calibration: %02x\n", diff_sec);
		clock_fixup_set(diff_sec, cal_minute);
		cal_minute = 0;
	}else if(param->value[0]==0xb0){
		// ★ PC 资源监控数据包 (20 字节):
		//   [1]=CPU% [2]=RAM% [3]=flags
		//   [4..5]=RAM已用MB(LE) [6..7]=RAM总MB(LE)
		//   [8..11]=下载字节/秒(LE) [12..15]=上传字节/秒(LE)
		//   [16]=时 [17]=分 [18]=秒 [19]=C盘占用%
		if(param->length >= 20){
			const uint8_t *v = (const uint8_t*)param->value;
			g_mon.cpu_pct = (v[1] > 100) ? 100 : v[1];
			g_mon.ram_pct = (v[2] > 100) ? 100 : v[2];
			g_mon.ram_used_mb  = (uint16_t)(v[4] | (v[5] << 8));
			g_mon.ram_total_mb = (uint16_t)(v[6] | (v[7] << 8));
			g_mon.net_dl_bps = (uint32_t)v[8]  | ((uint32_t)v[9]  << 8) |
			                   ((uint32_t)v[10] << 16) | ((uint32_t)v[11] << 24);
			g_mon.net_ul_bps = (uint32_t)v[12] | ((uint32_t)v[13] << 8) |
			                   ((uint32_t)v[14] << 16) | ((uint32_t)v[15] << 24);
			g_mon.hour = v[16];
			g_mon.min  = v[17];
			g_mon.sec  = v[18];
			g_mon.disk_pct = (v[19] > 100) ? 100 : v[19];
			g_mon.connected = 1;      // 收到 PC 数据即视为在线
		}
	}else if(param->value[0]==0xb1){
		// ★ 电脑名称包 (可变长): [1]=字符数 [2..]=ASCII 主机名
		if(param->length >= 2){
			char name[20];
			uint8_t len = param->value[1];
			uint8_t i;
			if(len > 15) len = 15;
			if(len > (uint8_t)(param->length - 2)) len = (uint8_t)(param->length - 2);
			for(i = 0; i < len; i++) name[i] = (char)param->value[2 + i];
			name[len] = 0;
			ui_set_host_name(name);
		}
	}else if(param->value[0]==0xb2){
		// ★ 扩展数据包:
		//   [1]=GPU% (0xFF=未知) [2]=网络类型 0未知/1有线/2WiFi
		//   [3]=保留 [4]=CPU温度℃ [5]=GPU温度℃ (0xFF=未知)
		if(param->length >= 4){
			const uint8_t *v = (const uint8_t*)param->value;
			g_mon.gpu_pct  = (v[1] > 100) ? 0xFF : v[1];
			g_mon.net_type = (v[2] > 2) ? 0 : v[2];
			if(param->length >= 6){
				g_mon.cpu_temp = v[4];
				g_mon.gpu_temp = v[5];
			}
		}
	}else if(param->value[0]==0xb3){
		// ★ 网络名称位图分块: [1]=宽(px) [2]=总字节 [3]=偏移 [4..]=数据
		if(param->length >= 5){
			const uint8_t *v = (const uint8_t*)param->value;
			uint8_t n = (uint8_t)(param->length - 4);
			ui_net_bmp_chunk(v[1], v[2], v[3], &v[4], n);
		}
	}else if(param->value[0]==0xc0){
		// ★ 显示字段掩码: [1..2] LE
		if(param->length >= 3){
			g_cfg.field_mask = (uint16_t)(param->value[1] | (param->value[2] << 8));
			ui_repaint_all();
		}
	}else if(param->value[0]==0xc1){
		// ★ 调色: [1]=索引 [2..3]=RGB565 LE
		if(param->length >= 4 && param->value[1] < PAL_N){
			g_cfg.pal[param->value[1]] = (uint16_t)(param->value[2] | (param->value[3] << 8));
			ui_repaint_all();
		}
	}else if(param->value[0]==0xc2){
		// ★ LED 控制: [1]=总开关 [2]=颜色位 [3]=次数 [4]=周期(100ms)
		if(param->length >= 5){
			g_cfg.led_en = param->value[1] ? 1 : 0;
			ui_led_play(param->value[2] & 7, param->value[3], param->value[4] ? param->value[4] : 10);
		}
	}else if(param->value[0]==0xc3){
		// ★ 过热规则: [1]=阈值℃ [2]=颜色 [3]=次数 [4..5]=冷却秒
		if(param->length >= 6){
			g_cfg.overheat_temp  = param->value[1];
			g_cfg.overheat_color = param->value[2] & 7;
			g_cfg.overheat_times = param->value[3];
			g_cfg.overheat_cool  = (uint16_t)(param->value[4] | (param->value[5] << 8));
		}
	}else if(param->value[0]==0xc4){
		// ★ 整点规则: [1]=开关 [2]=颜色 [3]=次数
		if(param->length >= 4){
			g_cfg.hourly_en    = param->value[1] ? 1 : 0;
			g_cfg.hourly_color = param->value[2] & 7;
			g_cfg.hourly_times = param->value[3];
		}
	}else if(param->value[0]==0xc5){
		// ★ 自定义灯效: [1]=槽 [2]=颜色 [3]=次数 [4]=周期 [5]=动作(0存/1播放/2清除)
		if(param->length >= 6 && param->value[1] < CFG_SLOTS){
			uint8_t slot = param->value[1];
			if(param->value[5] == 1){
				ui_led_play(param->value[2] & 7, param->value[3], param->value[4] ? param->value[4] : 10);
			}else if(param->value[5] == 2){
				g_cfg.preset[slot][0] = 0;
				g_cfg.preset[slot][1] = 0;
				g_cfg.preset[slot][2] = 0;
			}else{
				g_cfg.preset[slot][0] = param->value[2] & 7;
				g_cfg.preset[slot][1] = param->value[3];
				g_cfg.preset[slot][2] = param->value[4];
			}
		}
	}else if(param->value[0]==0xc6){
		// ★ 配置操作: [1]=0 保存到 Flash / 1 恢复默认 / 2 立即重绘
		if(param->length >= 2){
			if(param->value[1] == 0){
				cfg_save();
			}else if(param->value[1] == 1){
				cfg_defaults();
				cfg_apply();
			}else{
				ui_repaint_all();
			}
		}
	}else if(param->value[0]==0xc7){
		// ★ 回读: 把当前配置写回长值特征, 上位机立即读取即可
		struct custs1_val_set_req *req = KE_MSG_ALLOC_DYN(CUSTS1_VAL_SET_REQ,
		                                                  prf_get_task_from_id(TASK_ID_CUSTS1),
		                                                  TASK_APP,
		                                                  custs1_val_set_req,
		                                                  sizeof(g_cfg));
		req->conidx = app_env->conidx;
		req->handle = SVC1_IDX_LONG_VALUE_VAL;
		req->length = sizeof(g_cfg);
		memcpy(req->value, &g_cfg, sizeof(g_cfg));
		KE_MSG_SEND(req);
	}else if(param->value[0]==0xc8){
		// ★ 切换卡片: [1] = 0 监控 / 1 纯时钟 / 2 天气 / 3 TODO / 4 画册
		if(param->length >= 2)
			ui_set_card(param->value[1]);
	}else if(param->value[0]==0xcb){
		// ★ 图像卡开始: [1..2]=宽 [3..4]=高 (小端)
		ui_image_start(param->value[1] | (param->value[2]<<8),
		               param->value[3] | (param->value[4]<<8));
	}else if(param->value[0]==0xcc){
		// ★ 图像数据: [8..] 顺序追加 1bpp 字节
		if(param->length > 8)
			ui_image_data((const u8*)param->value + 8, param->length - 8);
	}else if(param->value[0]==0xcd){
		// ★ 图像结束: 补最后一条带
		ui_image_end();
	}else if(param->value[0]>=0xa0){
		// 处理OTA升级命令
		ota_handle((u8*)param->value, param->length);
    }
}

/**
 * 长值特征属性信息请求处理函数
 * 
 * @param msgid 消息ID
 * @param param 请求参数
 * @param dest_id 目标任务ID
 * @param src_id 源任务ID
 * 
 * 响应BLE客户端的属性信息请求
 */
void user_svc1_long_val_att_info_req_handler(ke_msg_id_t const msgid, 
                                            struct custs1_att_info_req const *param, 
                                            ke_task_id_t const dest_id, 
                                            ke_task_id_t const src_id)
{
    // 分配响应消息内存
    struct custs1_att_info_rsp *rsp = KE_MSG_ALLOC(CUSTS1_ATT_INFO_RSP, 
                                                   src_id, 
                                                   dest_id, 
                                                   custs1_att_info_rsp);
    // 设置连接索引
    rsp->conidx  = app_env[param->conidx].conidx;
    // 设置属性索引
    rsp->att_idx = param->att_idx;
    // 设置长度为0
    rsp->length  = 0;
    // 设置状态为无错误
    rsp->status  = ATT_ERR_NO_ERROR;

    // 发送响应消息
    KE_MSG_SEND(rsp);
}

void user_svc1_rest_att_info_req_handler(ke_msg_id_t const msgid,
                                            struct custs1_att_info_req const *param,
                                            ke_task_id_t const dest_id,
                                            ke_task_id_t const src_id)
{
    struct custs1_att_info_rsp *rsp = KE_MSG_ALLOC(CUSTS1_ATT_INFO_RSP,
                                                   src_id,
                                                   dest_id,
                                                   custs1_att_info_rsp);
    // Provide the connection index.
    rsp->conidx  = app_env[param->conidx].conidx;
    // Provide the attribute index.
    rsp->att_idx = param->att_idx;
    // Force current length to zero.
    rsp->length  = 0;
    // Provide the ATT error code.
    rsp->status  = ATT_ERR_WRITE_NOT_PERMITTED;

    KE_MSG_SEND(rsp);
}

