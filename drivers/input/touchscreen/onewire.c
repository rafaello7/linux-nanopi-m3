/* Onewire protocol support for touch panels from FriendlyARM
 * Based on FriendlyARM driver for 3.x kernel.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/backlight.h>
#include <soc/nexell/panel-nanopi.h>


enum {
	REQ_KEY		= 0x30,
	REQ_TS		= 0x40,
	REQ_INFO	= 0x60,
	REQ_BLINIT	= 0x7f,
};

/* timer register */
#define REG_TCFG0	0x00
#define REG_TCFG1	0x04
#define REG_TCON	0x08
#define REG_TCNTB0	0x0C
#define REG_TCMPB0	0x10
#define REG_CSTAT	0x44

#define TCON_BIT_AUTO (1 << 3)
#define TCON_BIT_INVT (1 << 2)
#define TCON_BIT_UP (1 << 1)
#define TCON_BIT_RUN (1 << 0)
#define TCFG0_BIT_CH(ch) (ch == 0 || ch == 1 ? 0 : 8)
#define TCFG1_BIT_CH(ch) (ch * 4)
#define TCON_BIT_CH(ch) (ch ? ch * 4 + 4 : 0)
#define TINT_CSTAT_BIT_CH(ch) (ch + 5)
#define TINT_CSTAT_MASK (0x1F)
#define TIMER_TCNT_OFFS (0xC)


enum OneWireState {
	IDLE,
	START,
	REQUEST,
	WAITING,
	RESPONSE,
};


struct onewire_device {
	struct device *dev;			// platform device
	u32 irq_timer;				// hardware timer number used for irq's
	struct gpio_desc *gpiod;	// onewire gpio
	void __iomem *regs;			// timer registers
	struct backlight_device *bl;
	unsigned rate25Hz;			// timer rate for 25 Hz
	unsigned rate9600Hz;		// timer rate for 9600 Hz
	enum OneWireState state;
	unsigned total_received;
	bool backlight_init_success;
	bool has_key_data;
	bool has_ts_data;
	unsigned char backlight_req;	// pending backlight brightness change
									// request; 0 for none
	unsigned io_bit_count;
	u8 io_data[4];					// data being sent or received
	unsigned char one_wire_request; // request (being) sent (if non-IDLE state)
	bool isTouchDown;				// last reported touch state: down/up
};

static int lcd_type;
static DECLARE_WAIT_QUEUE_HEAD(onewire_waitqueue);

int onewire_get_lcd_type(void)
{
	wait_event(onewire_waitqueue, lcd_type);
	return lcd_type;
}

/* Set timer clock as:  pclk / (2^mux * scl)
 * mux: 0 .. 4
 * scl: 1 .. 256
 */
static void timer_clock(struct onewire_device *onew, int mux, int scl)
{
	unsigned ch = onew->irq_timer;
	u32 val = readl(onew->regs + REG_TCFG0) & ~(0xFF << TCFG0_BIT_CH(ch));

	writel(val | ((scl - 1) << TCFG0_BIT_CH(ch)), onew->regs + REG_TCFG0);
	val = readl(onew->regs + REG_TCFG1) & ~(0xF << TCFG1_BIT_CH(ch));
	writel(val | (mux << TCFG1_BIT_CH(ch)), onew->regs + REG_TCFG1);
}

/* Set timer counter
 */
static void timer_count(struct onewire_device *onew, unsigned cnt)
{
	unsigned ch = onew->irq_timer;

	writel((cnt - 1), onew->regs + REG_TCNTB0 + (TIMER_TCNT_OFFS * ch));
	writel((cnt - 1), onew->regs + REG_TCMPB0 + (TIMER_TCNT_OFFS * ch));
}

/* Starts timer countdown
 */
static void timer_start(struct onewire_device *onew, bool irqon, bool repeat)
{
	unsigned ch = onew->irq_timer;
	int on = irqon ? 1 : 0;
	u32 val;

	val = readl(onew->regs + REG_CSTAT) & ~(TINT_CSTAT_MASK << 5 | 0x1 << ch);
	writel(val | (0x1 << TINT_CSTAT_BIT_CH(ch) | on << ch),
	       onew->regs + REG_CSTAT);
	val = readl(onew->regs + REG_TCON) & ~(0xE << TCON_BIT_CH(ch));
	writel(val | (TCON_BIT_UP << TCON_BIT_CH(ch)), onew->regs + REG_TCON);

	val &= ~(TCON_BIT_UP << TCON_BIT_CH(ch));
	if( repeat )
		val |= TCON_BIT_AUTO << TCON_BIT_CH(ch);
	else
		val &= ~(TCON_BIT_AUTO << TCON_BIT_CH(ch));
	val |= TCON_BIT_RUN << TCON_BIT_CH(ch);
	writel(val, onew->regs + REG_TCON);
}

/* Stops timer countdown
 */
static void timer_stop(struct onewire_device *onew)
{
	unsigned ch = onew->irq_timer;
	u32 val;

	val = readl(onew->regs + REG_CSTAT) & ~(TINT_CSTAT_MASK << 5 | 0x1 << ch);
	writel(val | 1 << TINT_CSTAT_BIT_CH(ch), onew->regs + REG_CSTAT);
	val = readl(onew->regs + REG_TCON) & ~(TCON_BIT_RUN << TCON_BIT_CH(ch));
	writel(val, onew->regs + REG_TCON);
}

/* Clear timer interrupt state
 */
static void timer_clear_irq(struct onewire_device *onew)
{
	u32 val;

	val = readl(onew->regs + REG_CSTAT) & ~(TINT_CSTAT_MASK << 5);
	val |= (0x1 << TINT_CSTAT_BIT_CH(onew->irq_timer));
	writel(val, onew->regs + REG_CSTAT);
}

static u8 crc8sum(u8 *pdata, unsigned nbytes)
{
	// msb, polynomial == 7
	static const u8 t[] = { 0x7, 0xe, 0x1c, 0x38, 0x70, 0xe0, 0xc7, 0x89 };
	unsigned crc = 0xac;

	while( nbytes-- ) {
		unsigned i, m = crc ^ *pdata++;

		crc = 0;
		for(i = 0; m; ++i ) {
			if( m & 1 )
				crc ^= t[i];
			m >>= 1;
		}
	}
	return crc;
}

static int onew_backlight_update_status(struct backlight_device *bl)
{
	struct onewire_device *onew = bl_get_data(bl);
	int brightness = bl->props.brightness;

	if (bl->props.power != FB_BLANK_UNBLANK ||
			bl->props.fb_blank != FB_BLANK_UNBLANK ||
			bl->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK))
		brightness = 0;

	onew->backlight_req = 0x80 + brightness;
	return 0;
}

static const struct backlight_ops onewire_backlight_ops = {
	.options	= BL_CORE_SUSPENDRESUME,
	.update_status	= onew_backlight_update_status,
};

static void notify_info_data(struct onewire_device *onew)
{
	dev_info(onew->dev, "lcd type: %d, year: %d, week: %d\n",
			onew->io_data[0], onew->io_data[1], onew->io_data[2]);
	if( onew->io_data[0] != 255 ) {
		lcd_type = onew->io_data[0];
		if( lcd_type == 24 )
			onew->has_key_data = true;
		wake_up_all(&onewire_waitqueue);
		if( nanopi_panelrgb_issensor_1wire(lcd_type) )
			onew->has_ts_data = true;
	}
}

static void notify_bl_data(struct onewire_device *onew)
{
	struct backlight_properties props;

	dev_info(onew->dev, "backlight data: %x,%x,%x\n",
			onew->io_data[0], onew->io_data[1], onew->io_data[2]);
	onew->backlight_init_success = true;

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_PLATFORM;
	props.brightness = 96;
	props.max_brightness = 127;
	onew->bl = devm_backlight_device_register(onew->dev, dev_name(onew->dev),
					onew->dev, onew, &onewire_backlight_ops, &props);
	if( IS_ERR(onew->bl) ) {
		dev_err(onew->dev, "failed to register backlight: %ld\n",
				PTR_ERR(onew->bl));
		onew->bl = NULL;
	}else{
		dev_info(onew->dev, "added backlight device");
	}
}

static void ts_if_report_key(int key)
{
	pr_info("onewire received key %d\n", key);
}

static void notify_ts_data(struct onewire_device *onew)
{
	unsigned x, y, down;

	x = ((onew->io_data[0] & 0xf0) << 4) + onew->io_data[1];
	y = ((onew->io_data[0] &  0xf) << 8) + onew->io_data[2];
	down = (x != 0xFFF) && (y != 0xFFF);
	if( down ) {
		pr_info("onewire touch (%d, %d)\n", x, y);
		onew->isTouchDown = true;
	}else if( onew->isTouchDown ) {
		pr_info("onewire fingers up\n");
		onew->isTouchDown = false;
	}
}

static bool start_one_wire_session(struct onewire_device *onew)
{
	unsigned char req;

	if( lcd_type == 0 ) {
		req = REQ_INFO;
	} else if (!onew->backlight_init_success) {
		req = REQ_BLINIT;
	} else if (onew->backlight_req) {
		req = onew->backlight_req;
		onew->backlight_req = 0;
	} else if (onew->has_key_data) {
		req = REQ_KEY;
	} else if (onew->has_ts_data) {
		req = REQ_TS;
	} else {
		return false;
	}

	// prepare data for transfering
	onew->io_data[0] = req;
	onew->io_data[1] = crc8sum(&req, 1);
	onew->one_wire_request = req;
	return true;
}

static unsigned total_error;

static irqreturn_t onewire_irq_threaded_handler(int irq, void *data)
{
	struct onewire_device *onew = platform_get_drvdata(data);

	++onew->total_received;
	if( crc8sum(onew->io_data, 4) == 0 ) {
		switch( onew->one_wire_request ) {
			case REQ_INFO:
				notify_info_data(onew);
				break;
			case REQ_KEY:
				ts_if_report_key(onew->io_data[2]);
				break;
			case REQ_TS:
				notify_ts_data(onew);
				break;
			case REQ_BLINIT:
				notify_bl_data(onew);
				break;
		}
	}else{ // CRC mismatch
		++total_error;
	}
	if( lcd_type == 0 && onew->total_received > 15 ) {
		dev_info(onew->dev, "no panel\n");
		lcd_type = -1;
		wake_up_all(&onewire_waitqueue);
	}
	if( lcd_type >= 0 ) {
		timer_count(onew, onew->rate25Hz);
		timer_start(onew, true, false);
	}
	return IRQ_HANDLED;
}

static irqreturn_t onewire_irq_handler(int irq, void *data)
{
	struct onewire_device *onew = platform_get_drvdata(data);

	timer_clear_irq(onew);

	onew->io_bit_count--;
	switch( onew->state ) {
	case IDLE:
		timer_stop(onew);
		if( start_one_wire_session(onew) ) {
			// init transfer and start timer
			gpiod_direction_output(onew->gpiod, 0);
			onew->io_bit_count = 1;
			onew->state = START;
			timer_count(onew, onew->rate9600Hz);
			timer_start(onew, true, true);
		}else{
			timer_count(onew, onew->rate25Hz);
			timer_start(onew, true, false);
		}
		break;
	case START:
		if (onew->io_bit_count == 0) {
			onew->io_bit_count = 16;
			onew->state = REQUEST;
		}
		break;

	case REQUEST:
		// Send a bit
		gpiod_set_value(onew->gpiod,
				onew->io_data[onew->io_bit_count < 8] &
				(1 << (onew->io_bit_count & 0x7)));
		if (onew->io_bit_count == 0) {
			onew->io_bit_count = 2;
			onew->state = WAITING;
		}
		break;

	case WAITING:
		if (onew->io_bit_count == 0) {
			onew->io_bit_count = 32;
			onew->io_data[0] = onew->io_data[1] = 0;
			onew->io_data[2] = onew->io_data[3] = 0;
			onew->state = RESPONSE;
		}
		if (onew->io_bit_count == 1) {
			gpiod_direction_input(onew->gpiod);
			gpiod_set_value(onew->gpiod, 1);
		}
		break;

	default:	// RESPONSE
		// Get a bit
		onew->io_data[3 - (onew->io_bit_count >> 3)] |=
			gpiod_get_value(onew->gpiod) ? 1 << (onew->io_bit_count & 0x7) : 0;
		if (onew->io_bit_count == 0) {
			gpiod_direction_output(onew->gpiod, 1);
			timer_stop(onew);
			onew->state = IDLE;
			return IRQ_WAKE_THREAD;
		}
		break;
	}
	return IRQ_HANDLED;
}

static int onewire_probe(struct platform_device *pdev)
{
	struct onewire_device *onew;
	int irq, err;
	struct resource *res;
	struct clk *pclk;
	unsigned rate;

	onew = devm_kzalloc(&pdev->dev, sizeof(struct onewire_device),
			GFP_KERNEL);
	if( onew == NULL )
		return -ENOMEM;
	err = of_property_read_u32(pdev->dev.of_node, "irq-timer",
			&onew->irq_timer);
	if( err ) {
		dev_err(&pdev->dev, "OF property irq-timer missing");
		return err;
	}
	onew->gpiod = devm_gpiod_get(&pdev->dev, "channel", GPIOD_ASIS);
	if( IS_ERR(onew->gpiod) ) {
		dev_err(&pdev->dev, "unable to get gpio: %ld\n",
				PTR_ERR(onew->gpiod));
		return PTR_ERR(onew->gpiod);
	}
	gpiod_direction_output(onew->gpiod, 1);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if( res == NULL ) {
		dev_err(&pdev->dev, "failed to get registers base address\n");
		return -ENXIO;
	}
	onew->regs = devm_ioremap_resource(&pdev->dev, res);
	if( onew->regs == NULL ) {
		dev_err(&pdev->dev, "failed to iomap registers\n");
		return -EIO;
	}
	pclk = clk_get(NULL, "sys-bpclk");
	if( IS_ERR(pclk) ) {
		dev_err(&pdev->dev, "get pclk error %ld\n", PTR_ERR(pclk));
		return PTR_ERR(pclk);
	}
	rate = clk_get_rate(pclk);
	clk_put(pclk);
	irq = platform_get_irq(pdev, 0);
	if( irq < 0 ) {
		dev_err(&pdev->dev, "unable to get irq: %d\n", irq);
		return irq;
	}
	onew->dev = &pdev->dev;
	platform_set_drvdata(pdev, onew);
	timer_stop(onew);
	onew->state = IDLE;
	err = devm_request_threaded_irq(&pdev->dev, irq, onewire_irq_handler,
			onewire_irq_threaded_handler, 0, "onewire", pdev);
	if( err ) {
		dev_err(&pdev->dev, "failed to request irq: %d\n", err);
		return err;
	}
	dev_info(&pdev->dev, "probe success\n");
	// divide pclk by 22; for pclk frequency 200 MHz the calculated
	// rate9600Hz is 947 giving 9599.69 Hz
	timer_clock(onew, 1, 11);
	onew->rate25Hz = (rate + 11 * 25) / (22 * 25);
	onew->rate9600Hz = (rate + 11 * 9600) / (22 * 9600);
	onewire_irq_handler(irq, pdev);
	return 0;
}

static int onewire_remove(struct platform_device *pdev)
{
	struct onewire_device *onew = platform_get_drvdata(pdev);

	timer_stop(onew);
	gpiod_set_value(onew->gpiod, 0);
	dev_info(&pdev->dev, "removed\n");
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id onewire_of_match[] = {
	{ .compatible = "friendlyarm,onewire" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, onewire_of_match);
#endif

struct platform_driver onewire_platform_driver = {
	.probe = onewire_probe,
	.remove = onewire_remove,
	.driver = {
		.name = "onewire",
		.of_match_table = of_match_ptr(onewire_of_match),
	},
};

static int __init onewire_init(void)
{
	return platform_driver_register(&onewire_platform_driver);
}

static void __exit onewire_exit(void)
{
	platform_driver_unregister(&onewire_platform_driver);
}

/* early init needed for possible LCD panel detection */
subsys_initcall(onewire_init);
module_exit(onewire_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rafaello7 <fatwildcat@gmail.com>");
MODULE_DESCRIPTION("FriendlyArm onewire protocol driver");
