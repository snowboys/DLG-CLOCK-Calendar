
#include "epd.h"




static int spio_clk;
static int spio_cs;
static int spio_di;
static int spio_do;

/******************************************************************************/


#define FSPI_CS(n)  gpio_set(spio_cs,  (n));
#define FSPI_CK(n)  gpio_set(spio_clk, (n));
#define FSPI_SI(n)  gpio_set(spio_di,  (n));
#define FSPI_SO()   gpio_get(spio_do);

#define FSPI_DELAY     10

void fspi_delay(void)
{
	int i;

	for(i=0; i<FSPI_DELAY; i++){
		__asm("nop");
	}
}


int fspi_trans(int byte)
{
	int i, data;

	data = 0;
	for(i=0; i<8; i++){
		FSPI_SI(byte&0x80);
		FSPI_CK(0);
		fspi_delay();

		data <<= 1;
		data |= FSPI_SO();

		FSPI_CK(1);
		fspi_delay();
		byte <<= 1;
	}

	return data;
}


int fspi_config(u32 gpio_word)
{
	spio_clk = (gpio_word>>24)&0xff;
	spio_cs  = (gpio_word>>16)&0xff;
	spio_di  = (gpio_word>> 8)&0xff;
	spio_do  = (gpio_word>> 0)&0xff;

	return 0;
}


/* 屏幕改为 ST7789 后 epd_hw_disabled=1, 会让 gpio_config/gpio_set/gpio_get
 * 全部直接 return, 运行期 SPI Flash 读写会静默失效。
 * Flash 用的是另一组引脚 (P0_0/P0_3/P0_5/P0_6), 与 TFT 引脚不冲突,
 * 因此做 Flash 读写前临时放开该标志, 完成后恢复。 */
static int fspi_hw_enter(void)
{
	int old = epd_hw_disabled;
	epd_hw_disabled = 0;
	return old;
}

static void fspi_hw_leave(int old)
{
	epd_hw_disabled = old;
}

int fspi_init(void)
{
	gpio_config(spio_clk, 0x0300, 0);
	gpio_config(spio_cs,  0x0300, 1);
	gpio_config(spio_di,  0x0300, 1);
	gpio_config(spio_do,  0x0100, 1);

	FSPI_CS(0);
	fspi_delay();
	fspi_trans(0xab);
	FSPI_CS(1);

	return 0;
}


int fspi_exit(void)
{
	gpio_config(spio_cs,  0x0300, 1);
	gpio_config(spio_clk, 0x0300, 0);
	gpio_config(spio_do,  0x0300, 0);
	gpio_config(spio_di,  0x0100, 0);
	return 0;
}


/******************************************************************************/
/* SPI flash                                                                  */
/******************************************************************************/ 

int sf_readid(void)
{
	FSPI_CS(0);
	fspi_delay();

	fspi_trans(0x90);
	fspi_trans(0x00);
	fspi_trans(0x00);
	fspi_trans(0x00);

	int mid = fspi_trans(0);
	int pid = fspi_trans(0);
	FSPI_CS(1);
	fspi_delay();

	return (mid<<8)|pid;
}

int sf_status(int id)
{
	int status;
	int cmd = (id)? 0x35 : 0x05;

	FSPI_CS(0);
	fspi_delay();
	fspi_trans(cmd);
	status = fspi_trans(0);
	FSPI_CS(1);
	fspi_delay();

	return status&0xff;
}

int sf_wstat(int id, int stat)
{
	int cmd = (id)? 0x31 : 0x01;

	// status write enable
	FSPI_CS(0);
	fspi_delay();
	fspi_trans(0x50);
	FSPI_CS(1);
	fspi_delay();

	FSPI_CS(0);
	fspi_delay();
	fspi_trans(cmd);
	fspi_trans(stat);
	FSPI_CS(1);
	fspi_delay();

	return 0;
}

int sf_wen(int en)
{
	FSPI_CS(0);
	fspi_delay();
	if(en)
		fspi_trans(0x06);
	else
		fspi_trans(0x04);
	FSPI_CS(1);
	fspi_delay();

	return 0;
}

int sf_wait(void)
{
	int status;

	while(1){
		status = sf_status(0);
		if((status&1)==0)
			break;
		fspi_delay();
	}

	status |= sf_status(1)<<8;
	return status;
}


int sf_sector_erase(int cmd, int addr, int wait)
{
	sf_wen(1);

	FSPI_CS(0);
	fspi_delay();
	fspi_trans(cmd);
	fspi_trans((addr>>16)&0xff);
	fspi_trans((addr>> 8)&0xff);
	fspi_trans((addr>> 0)&0xff);
	FSPI_CS(1);
	fspi_delay();

	if(wait){
		int status = sf_wait();
		return status;
	}else{
		return 0;
	}
}

int sf_erase(int addr, int size, int wait)
{
	int status;

	while(size>0){
		if((addr%0x8000)==0 && size>=0x8000){
			status = sf_sector_erase(ERASE_32K, addr, wait);
			printk("sf_erase: %08x 32K  stat=%04x\n", addr, status);
			addr += 0x8000;
			size -= 0x8000;
		}else
		{
			status = sf_sector_erase(ERASE_4K,  addr, wait);
			printk("sf_erase: %08x  4K  stat=%04x\n", addr, status);
			addr += 0x1000;
			size -= 0x1000;
		}
	}

	return 0;
}


int sf_page_write(int addr, u8 *buf, int size)
{
	int i;

	sf_wen(1);

	FSPI_CS(0);
	fspi_delay();

	fspi_trans(0x02);
	fspi_trans((addr>>16)&0xff);
	fspi_trans((addr>> 8)&0xff);
	fspi_trans((addr>> 0)&0xff);

	for(i=0; i<size; i++){
		fspi_trans(buf[i]);
	}

	FSPI_CS(1);
	fspi_delay();
	return 0;
}


int sf_read(int addr, int len, u8 *buf)
{
	int i;

	FSPI_CS(0);
	fspi_delay();

	fspi_trans(0x0b);
	fspi_trans((addr>>16)&0xff);
	fspi_trans((addr>> 8)&0xff);
	fspi_trans((addr>> 0)&0xff);

	fspi_trans(0);
	for(i=0; i<len; i++){
		buf[i] = fspi_trans(0);
	}

	FSPI_CS(1);
	fspi_delay();

	return len;
}

/******************************************************************************/


static const uint32_t crc32_tab[] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
    0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
    0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
    0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
    0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
    0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
    0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
    0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
    0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
    0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
    0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
    0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
    0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
    0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
    0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
    0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
    0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
    0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
    0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
    0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
    0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
    0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
    0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
    0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
    0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

uint32_t crc32(uint32_t crc, const void *buf, size_t size)
{
    const uint8_t *p;

    p = buf;
    crc = crc ^ ~0U;

    while (size--)
        crc = crc32_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);

    return crc ^ ~0U;
}


/******************************************************************************/

extern unsigned char __code_area_end__[];   /* GNU ld 脚本提供: .data 结束处 */
void sf_dumpp(int addr, int size);

int selflash(int otp_boot)
{
	u8 pbuf[256];
	u32 *p32 = (u32*)pbuf;
	int image_addr[2];
	int image_flag[2];

	/* J-Link 烧录时用的"写入 Flash"触发标记。
	 * 为什么不用 otp_boot: read_otp_value() 里的 hw_otpc_init() 会把 OTP 影子内存
	 * 重新搬到 0x07f8fe00, 外部写入的值会被覆盖。
	 * 这里选堆尾与栈之间的空闲 RAM (见 map: heap 结束 0x07fcec10, 栈底 0x07fd1bbc),
	 * 启动代码不会清它, 由 tools/flash.py 在跳转前写入。 */
	volatile u32 *trigger = (volatile u32 *)0x07FD1000u;
	#define FLASH_TRIGGER_MAGIC 0x464C5348u      /* "FLSH" */

	fspi_init();
	int id = sf_readid();
	printk("Flash  ID: %08x\n", id);

	sf_read(0x39000, 16, pbuf);
	printk("39000: ");
	for(int i=0; i<16; i++){
		printk(" %02x", pbuf[i]);
	}
	printk("\n");
	printk("EPD Type: %02x\n", pbuf[0]);
	if(pbuf[1]==0x01){
		printk("EPD Gpio: %02x %02x %02x %02x %02x %02x %02x %02x\n",
			pbuf[8], pbuf[9], pbuf[10], pbuf[11], pbuf[12], pbuf[13], pbuf[14], pbuf[15]);
	}

	sf_read(0x3a000, 16, pbuf);
	printk("3a000: ");
	for(int i=0; i<16; i++){
		printk(" %02x", pbuf[i]);
	}
	printk("\n");

	int xres = *(u16*)(pbuf+10);
	int yres = *(u16*)(pbuf+12);
	if(xres<512 && yres<512){
		detect_w = xres;
		detect_h = yres;
		detect_mode = pbuf[9]? EPD_BWR : EPD_BW;
		printk("EPD  Res: %dx%d  %d\n", xres, yres, pbuf[9]);
	}

	int firm_size = (int)__code_area_end__ - 0x07fc0000;
	printk("Firm size: %08x\n", firm_size);
	u32 firm_crc = crc32(0, (u8*)0x07fc0000, firm_size);
	printk("Firm  crc: %08x\n", firm_crc);
	printk("Firm  ver: %08x\n", EPD_VERSION);

	memset(pbuf, 0, 256);
	if(otp_boot==0x1234a5a5 || *trigger==FLASH_TRIGGER_MAGIC){
		// 从OTP启动。读product header。
		sf_read(0x38000, 16, pbuf);
		if(pbuf[0]!=0x70 || pbuf[1]!=0x52){
			printk("Build Product header ...\n");
			p32[0] = 0x00005270;
			p32[1] = 0x00004000;
			p32[2] = 0x0001f000;
			sf_sector_erase(ERASE_4K, 0x38000, 1);
			sf_page_write(0x38000, pbuf, 12);
			sf_wait();
		}
		image_addr[0] = p32[1];
		image_addr[1] = p32[2];

		// 读image header
		sf_read(image_addr[0], 32, pbuf+0 );
		sf_read(image_addr[1], 32, pbuf+32);
		printk("Product iamge0: %08x:  %08x %08x %08x %08x\n", image_addr[0], __REV(p32[0]), p32[1], p32[2], p32[7]);
		printk("        iamge1: %08x:  %08x %08x %08x %08x\n", image_addr[1], __REV(p32[8]), p32[9], p32[10],p32[15]);

		// 获取当前使用的image的id
		image_flag[0] = -1;
		image_flag[1] = -1;
		if(pbuf[ 0]==0x70 && pbuf[ 1]==0x51 && pbuf[ 2]==0xaa){
			image_flag[0] = (signed char)pbuf[ 3];
		}
		if(pbuf[32]==0x70 && pbuf[33]==0x51 && pbuf[34]==0xaa){
			image_flag[1] = (signed char)pbuf[35];
		}
		int active = (image_flag[0]>=image_flag[1]) ? 0 : 1;
		printk("Active image: %d  flag: %02x\n", active, image_flag[active]);
		
		/* 只比较版本号。
		 * 注意: 不能用 CRC 比较 —— firm_size 范围内包含 ROM patch table(0x07FC00C0)
		 * 和 TRNG 随机缓冲(0x07FC0110), 这两块每次启动都会被改写, 运行时算出的 CRC
		 * 永远不等于 Flash 里写入时的 CRC, 会导致每次上电都整片重写 Flash,
		 * 重写途中断电就会损坏镜像 -> 引导器回退旧分区 -> 黑屏。
		 * 版本号由构建脚本按源码内容自动生成(见 tools/build_pcmonitor.py), 不会漏更新。 */
		if(*trigger==FLASH_TRIGGER_MAGIC || EPD_VERSION != p32[active*8+7]){
			printk("Flash img ver %08x crc %08x != run ver %08x crc %08x\n",
			       p32[active*8+7], p32[active*8+2], EPD_VERSION, firm_crc);
			int new_flag = image_flag[active]+1;
			int new_id = active^1;
			
			// 擦除flash
			printk("Erase %08x ...\n", image_addr[new_id]);
			sf_erase(image_addr[new_id], firm_size+64, 1);
			// 初始化image header
			memset(pbuf, 0xff, 64);
			p32[0] = (new_flag<<24)|0x00aa5170;
			p32[1] = firm_size;
			p32[2] = firm_crc;
			p32[7] = EPD_VERSION;
			pbuf[0x20] = 0;

			// 写入flash
			u8 *firm_data = (u8*)0x07fc0000;
			int addr = image_addr[new_id];
			for(int i=0; i<firm_size+64; i+=256){
				if(i){
					memcpy(pbuf, firm_data, 256);
					firm_data += 256;
				}else{
					memcpy(pbuf+64, firm_data, 192);
					firm_data += 192;
				}
				sf_page_write(addr, pbuf, 256);
				sf_wait();
				addr += 256;
			}
			printk("Firm update done.\n\n");
			if(*trigger==FLASH_TRIGGER_MAGIC)
				*trigger = 0;      /* 一次性: 写完清标记, 避免每次上电都重写 */
		}

	}else{
		// 从Flash启动。读Boot header。
		sf_read(0, 16, pbuf);
		printk("Boot Header: %08x %08x\n", *(u32*)(pbuf+0), *(u32*)(pbuf+4));
	}

	fspi_exit();
	return 0;
}

/* ---------------------------------------------------------------- 配置存储
 * 0x3B000 扇区: 与镜像(0x4000/0x1f000)、product header(0x38000)、
 * EPD 参数(0x39000/0x3a000)都不冲突, 固定 256 字节。
 */
#define CFG_FLASH_ADDR  0x3B000u

int flash_cfg_read(void *buf, int len)
{
	int old;

	if(len <= 0 || len > 256)
		return 0;

	old = fspi_hw_enter();
	fspi_init();
	sf_read(CFG_FLASH_ADDR, len, (u8 *)buf);
	fspi_exit();
	fspi_hw_leave(old);
	return 1;
}

int flash_cfg_write(const void *buf, int len)
{
	u8 page[256];
	int old;

	if(len <= 0 || len > 256)
		return 0;

	memset(page, 0xFF, sizeof(page));
	memcpy(page, buf, len);

	old = fspi_hw_enter();
	fspi_init();
	sf_sector_erase(ERASE_4K, CFG_FLASH_ADDR, 1);
	sf_page_write(CFG_FLASH_ADDR, page, 256);
	sf_wait();
	fspi_exit();
	fspi_hw_leave(old);
	return 1;
}

/* ---------------------------------------------------------------- 离线 RTC 快照
 * 独立万年历断电后仍要记住时间: 0x3C000 单独 4K 扇区, 256 个 16 字节槽,
 * 顺序追加写满才擦除 (每 10 分钟一条 -> 约 1.8 天擦一次, 极大地降低磨损)。
 * 与镜像(0x4000/0x1f000)、product header(0x38000)、EPD 参数(0x39000/0x3a000)、
 * 配置(0x3b000) 都不冲突。
 *
 * 记录: magic(4) + seq(4) + year(2) + month/day/hour/minute/second(5) + sum(1) = 16
 *   month 为 0-based (0=1月), day 为 1-based。
 */
#define RTC_FLASH_ADDR  0x3C000u
#define RTC_SLOT_SIZE   16
#define RTC_SLOT_N      256
#define RTC_MAGIC       0x52544331u      /* "RTC1" */

static u32      rtc_last_seq = 0;
static int      rtc_next_slot = -1;

static u32 rtc_rd32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void rtc_wr32(u8 *p, u32 v)
{
	p[0] = (u8)(v & 0xff);
	p[1] = (u8)((v >> 8) & 0xff);
	p[2] = (u8)((v >> 16) & 0xff);
	p[3] = (u8)((v >> 24) & 0xff);
}

static u8 rtc_sum(const u8 *p)
{
	u8 s = 0;
	int i;
	for(i = 0; i < RTC_SLOT_SIZE - 1; i++)
		s = (u8)(s + p[i]);
	return s;
}

/* 调用前必须 fspi_init(); 返回第一个全 0xFF 的空槽, -1 表示写满 */
static int rtc_find_free(void)
{
	u8 b[RTC_SLOT_SIZE];
	int i, j;

	for(i = 0; i < RTC_SLOT_N; i++){
		sf_read(RTC_FLASH_ADDR + i * RTC_SLOT_SIZE, RTC_SLOT_SIZE, b);
		for(j = 0; j < RTC_SLOT_SIZE; j++)
			if(b[j] != 0xFF) break;
		if(j == RTC_SLOT_SIZE) return i;
	}
	return -1;
}

int rtc_restore(int *y, int *mo, int *d, int *h, int *mi, int *s)
{
	u8 b[RTC_SLOT_SIZE], best[RTC_SLOT_SIZE];
	u32 best_seq = 0;
	int have = 0, i, old;

	old = fspi_hw_enter();
	fspi_init();
	for(i = 0; i < RTC_SLOT_N; i++){
		sf_read(RTC_FLASH_ADDR + i * RTC_SLOT_SIZE, RTC_SLOT_SIZE, b);
		if(rtc_rd32(b) != RTC_MAGIC) continue;
		if(b[RTC_SLOT_SIZE - 1] != rtc_sum(b)) continue;
		{
			u32 seq = rtc_rd32(b + 4);
			if(!have || (int)(seq - best_seq) > 0){
				memcpy(best, b, RTC_SLOT_SIZE);
				best_seq = seq;
				have = 1;
			}
		}
	}
	rtc_next_slot = rtc_find_free();
	fspi_exit();
	fspi_hw_leave(old);

	rtc_last_seq = best_seq;
	if(!have) return 0;

	*y  = (int)(best[8] | (best[9] << 8));
	*mo = best[10];
	*d  = best[11];
	*h  = best[12];
	*mi = best[13];
	*s  = best[14];

	if(*y < 2020 || *y > 2099 || *mo > 11 || *d < 1 || *d > 31 ||
	   *h > 23 || *mi > 59 || *s > 59)
		return 0;
	return 1;
}

void rtc_save_now(int y, int mo, int d, int h, int mi, int s)
{
	u8 b[RTC_SLOT_SIZE];
	int slot, old;

	if(y < 2020 || y > 2099 || mo < 0 || mo > 11 || d < 1 || d > 31 ||
	   h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59)
		return;

	old = fspi_hw_enter();
	fspi_init();
	if(rtc_next_slot < 0 || rtc_next_slot >= RTC_SLOT_N)
		rtc_next_slot = rtc_find_free();

	slot = rtc_next_slot;
	if(slot < 0){
		/* 扇区写满: 整体擦除后从 0 号槽重新开始 */
		sf_sector_erase(ERASE_4K, RTC_FLASH_ADDR, 1);
		slot = 0;
	}

	memset(b, 0, sizeof(b));
	rtc_wr32(b, RTC_MAGIC);
	rtc_wr32(b + 4, rtc_last_seq + 1);
	b[8]  = (u8)(y & 0xff);
	b[9]  = (u8)(y >> 8);
	b[10] = (u8)mo;
	b[11] = (u8)d;
	b[12] = (u8)h;
	b[13] = (u8)mi;
	b[14] = (u8)s;
	b[15] = rtc_sum(b);

	sf_page_write(RTC_FLASH_ADDR + slot * RTC_SLOT_SIZE, b, RTC_SLOT_SIZE);
	sf_wait();
	fspi_exit();
	fspi_hw_leave(old);

	rtc_last_seq += 1;
	rtc_next_slot = slot + 1;
}

/******************************************************************************/


/* ---------------------------------------------------------------- OTA 升级
 * 槽布局: 64 字节 0x7051 头 + 镜像
 *   +0 magic/序列号   +4 镜像长度   +8 镜像 CRC   +28 版本号
 *
 * 协议 (特征 0000ff01 写入, 每包 <= 136 字节):
 *   0xA0 [3B保留] size(u32) version(u32) crc(u32)   开始: 选非活动槽并擦除
 *   0xA2 [7B保留] 128 字节   页前半
 *   0xA3 [7B保留] 128 字节   页后半 -> 组成 256 字节页
 *   0xA4 [3B保留] size(u32) crc(u32)                结束: 校验通过才提交并复位
 *
 * 关键: 第 0 页(含头)只在最后落盘。传输中断时槽内仍是 0xFF(序列号无效),
 *       引导器继续用旧槽, 不会变成砖。 */
int ota_state = 0;
static u8  ota_buf[256]   __attribute__((aligned(4)));   /* 当前页组装缓冲 */
static u8  ota_first[256] __attribute__((aligned(4)));   /* 第 0 页: 头 + 前 192 字节镜像, 最后才写 */
static int firm_addr;
static int firm_size;
static int firm_flag;
static u32 firm_ver;
static u32 firm_crc_host;
static u32 ota_crc;             /* 收到的镜像字节 CRC 累加 */
static u32 ota_got;             /* 已收到的镜像字节数 */

/* Cortex-M0 不支持非对齐访问: 蓝牙缓冲区的指针不保证 4 字节对齐, 必须逐字节拼 */
static u32 ota_rd32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1]<<8) | ((u32)p[2]<<16) | ((u32)p[3]<<24);
}


int ota_handle(u8 *buf, int len)
{
	int image_addr[2];
	int image_flag[2];
	int rc = 0;

	if(buf[0]==0xa0){
		/* ---- 升级开始 ---- */
		if(len < 16){
			printk("OTA: short start packet %d\n", len);
			return -1;
		}
		firm_size     = (int)ota_rd32(buf+4);
		firm_ver      = ota_rd32(buf+8);
		firm_crc_host = ota_rd32(buf+12);
		if(firm_size <= 0 || firm_size > 0x20000){
			printk("OTA: bad size %d\n", firm_size);
			return -1;
		}

		fspi_init();
		int id = sf_readid();
		printk("OTA start: size=%d ver=%08x crc=%08x flash=%04x\n",
		       firm_size, firm_ver, firm_crc_host, id);

		sf_read(0x38000, 16, ota_buf);
		image_addr[0] = (int)ota_rd32(ota_buf+4);
		image_addr[1] = (int)ota_rd32(ota_buf+8);
		if(image_addr[0] == image_addr[1] || image_addr[0] < 0x2000 || image_addr[1] < 0x2000){
			printk("OTA: bad product header %08x %08x\n", image_addr[0], image_addr[1]);
			fspi_exit();
			return -1;
		}

		/* 槽容量: 两槽间隔 */
		int cap = image_addr[1] - image_addr[0];
		if(cap < 0)
			cap = -cap;
		if(firm_size + 64 > cap){
			printk("OTA: too big %d > %d\n", firm_size + 64, cap);
			fspi_exit();
			return -1;
		}

		/* 读两个槽的头, 找当前活动槽 (序列号大的) */
		sf_read(image_addr[0], 32, ota_buf);
		sf_read(image_addr[1], 32, ota_buf+32);
		image_flag[0] = -1;
		image_flag[1] = -1;
		if(ota_buf[0]==0x70 && ota_buf[1]==0x51 && ota_buf[2]==0xaa)
			image_flag[0] = (signed char)ota_buf[3];
		if(ota_buf[32]==0x70 && ota_buf[33]==0x51 && ota_buf[34]==0xaa)
			image_flag[1] = (signed char)ota_buf[35];

		int active = (image_flag[0] >= image_flag[1]) ? 0 : 1;
		int new_id = active ^ 1;
		int nf = image_flag[active] + 1;
		if(nf <= 0 || nf > 0x7f)
			nf = 1;                 /* 序列号溢出回到 1, 避免 0xFF(无效) */
		firm_flag = nf;
		firm_addr = image_addr[new_id];

		printk("OTA active=%d flag=%d -> slot%d addr=%08x new_flag=%02x\n",
		       active, image_flag[active], new_id, firm_addr, firm_flag);
		printk("OTA erase %08x + %d\n", firm_addr, firm_size + 64);
		sf_erase(firm_addr, firm_size + 64, 1);

		arch_set_sleep_mode(ARCH_SLEEP_OFF);
		ota_crc = 0;
		ota_got = 0;
		ota_state = 1;

	}else if(buf[0]==0xa2){
		/* ---- 页前半 128 字节 ---- */
		if(ota_state <= 0 || len < 136)
			return -1;
		memcpy(ota_buf, buf+8, 128);

	}else if(buf[0]==0xa3){
		/* ---- 页后半 128 字节, 凑满一页 ---- */
		if(ota_state <= 0 || len < 136)
			return -1;
		memcpy(ota_buf+128, buf+8, 128);

		int page = ota_state - 1;
		if(page == 0){
			/* 第 0 页含 64 字节头, 只暂存, 等 0xA4 校验通过再写 */
			memcpy(ota_first, ota_buf, 256);
			u32 n = (firm_size < 192) ? (u32)firm_size : 192u;
			ota_crc = crc32(ota_crc, ota_buf+64, n);
			ota_got += n;
		}else{
			sf_page_write(firm_addr + page*256, ota_buf, 256);
			sf_wait();
			u32 n = 256;
			if(ota_got + n > (u32)firm_size)
				n = (u32)firm_size - ota_got;
			if(n){
				ota_crc = crc32(ota_crc, ota_buf, n);
				ota_got += n;
			}
		}
		ota_state++;

	}else if(buf[0]==0xa4){
		/* ---- 结束: 校验后提交 ---- */
		if(ota_state <= 0)
			return -1;

		printk("OTA end: got=%d/%d crc=%08x/%08x\n",
		       ota_got, firm_size, ota_crc, firm_crc_host);

		if(ota_got == (u32)firm_size && ota_crc == firm_crc_host){
			/* 改写第 0 页: 序列号 / 长度 / CRC / 版本 全部以此为准 */
			u32 *p32 = (u32*)ota_first;     /* ota_first 已 aligned(4) */
			p32[0] = ((u32)firm_flag << 24) | 0x00aa5170u;
			p32[1] = (u32)firm_size;
			p32[2] = firm_crc_host;
			p32[7] = firm_ver;

			sf_page_write(firm_addr, ota_first, 256);
			sf_wait();
			fspi_exit();
			ota_state = 0;
			printk("OTA commit OK, rebooting\n");
			arch_set_sleep_mode(ARCH_EXT_SLEEP_ON);
			// Remap addres 0x00 to ROM and force execution
			SetWord16(SYS_CTRL_REG, (GetWord16(SYS_CTRL_REG) & ~REMAP_ADR0) | SW_RESET );
		}else{
			/* 校验失败: 槽内第 0 页仍是 0xFF, 引导器继续用旧槽 */
			printk("OTA verify FAILED, keep old slot\n");
			ota_state = 0;
			fspi_exit();
			rc = -1;
		}
	}

	return rc;
}


/******************************************************************************/

