#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/iio/consumer.h>
#include <linux/hwmon.h>


struct nanopi_thermistor_device {
	struct iio_channel *therm_adc;
	struct device *hwmon_dev;
};

/* Thermistor temperature is calculated from thermistor resistance with
 * respect to two nearest resistance values: one lower and one higher.
 *
 * Temperature calculation formula:
 *
 *					T1 * T2 * log2(R1/R2)
 *	T =	---------------------------------------------------
 *		T1 * log2(R1) - T2 * log2(R2) + (T2 - T1) * log2(R)
 *
 * where:
 *		T1, T2	- reference temperatures
 *		R1		- thermistor resistance at T1
 *		R2		- thermistor resistance at T2
 *		R		- measured resistance
 *		T		- temperature being calculated
 *		log2(n)	- logarithm at base 2
 */

/* Reference resistances with pre-calculated values for temperature formula.
 *	Rprev	- resistance from previous row (mOhm)
 *	Rcur	- resistance from current row  (mOhm)
 *	Tprew	- temperature for previous row (mK)
 *	Tcur	- temperature for current row  (mK)
 */
static const struct {
	unsigned mohm;		/* thermistor resistance (mOhm) */
	unsigned cnum;		/* 2^32 * (log2(Rprev) - log2(Rcur)) */
	unsigned cdenom;	/* 2^48 * (log2(Rprev) / Tcur - log2(Rcur) / Tprev) */
	unsigned f;			/* 2^48 * (Tprev - Tcur) / (Tprev * Tcur) */
} mohm2temp[] = {
	{ 189204600,           0,          0,          0 },		/* -40 */
	{ 145024000,  1647787795, -233745012,   25346838 },		/* -35 */
	{ 112004800,  1600867930, -218394555,   24304401 },		/* -30 */
	{  87133800,  1555878504, -204331715,   23324977 },		/* -25 */
	{  68260000,  1512653326, -191444168,   22403588 },		/* -20 */
	{  53834600,  1471055118, -179627764,   21535736 },		/* -15 */
	{  42733400,  1430952525, -168790799,   20717354 },		/* -10 */
	{  34133800,  1392269797, -158839901,   19944751 },		/*  -5 */
	{  27429900,  1354854610, -149711764,   19214575 },		/*   0 */
	{  22171800,  1318655735, -141327720,   18523776 },		/*   5 */
	{  18023400,  1283571192, -133630102,   17869572 },		/*  10 */
	{  14731900,  1249524882, -126562571,   17249424 },		/*  15 */
	{  12106000,  1216426020, -120078852,   16661008 },		/*  20 */
	{  10000000,  1184218072, -114130761,   16102195 },		/*  25 */
	{   8302200,  1152917003, -108659623,   15571032 },		/*  30 */
	{   6926800,  1122295002, -103665081,   15065725 },		/*  35 */
	{   5807100,  1092515732,  -99065950,   14584623 },		/*  40 */
	{   4891300,  1063433580,  -94851850,   14126203 },		/*  45 */
	{   4139000,  1034810969,  -91033798,   13689062 },		/*  50 */
	{   3518100,  1007108659,  -87482425,   13271903 },		/*  55 */
	{   3003600,   979698336,  -84293403,   12873527 },		/*  60 */
	{   2575400,   953036810,  -81346055,   12492822 },		/*  65 */
	{   2212700,   940549134,  -76013040,   12128759 },		/*  70 */
	{   1917600,   886936723,  -78909246,   11780382 },		/*  75 */
	{   1664900,   875599038,  -74081352,   11446802 },		/*  80 */
	{   1451300,   850789018,  -72080121,   11127193 },		/*  85 */
	{   1270100,   826367517,  -70277052,   10820785 },		/*  90 */
	{   1115900,   802017895,  -68711577,   10526862 },		/*  95 */
	{    984200,   778180754,  -67287444,   10244754 },		/* 100 */
	{    871300,   754977826,  -65968962,    9973836 },		/* 105 */
	{    774300,   731333721,  -64929971,    9713524 },		/* 110 */
	{    690500,   709748618,  -63726441,    9463273 },		/* 115 */
	{    618100,   686340710,  -63009676,    9222569 },		/* 120 */
	{    555300,   663885417,  -62296769,    8990933 },		/* 125 */
};

/* Calculates f * log2(val)
 */
static int ilog2mult(int val, int f)
{
	enum { POW2_30 = 1073741824 }; /* 2^30 */
	long long n = val, res = 0, add = f;

	/* loop invariant: res + add * log2(n) == f * log2(val) */
	while( add > 1 ) {
		if( n < POW2_30 ) {
			n *= n;
			add /= 2;
		}else{
			res += add;
			n /= 2;
		}
	}
	/* add == 1; adding log2(n) */
	while( n ) {
		++res;
		n /= 2;
	}
	return res;
}

/* Calculates thermistor temperature based on ADC value
 */
static long adcval_to_temperature(int adcval)
{
	int i;
	long long uvolt, mohm;

	if( adcval <= 0 || adcval >= 4096 ) /* should never occur */
		return 0;
	uvolt = 1800000LL * adcval / 4096;	// voltage measured by ADC (0V .. 1.8V)
	mohm =  4700000LL * uvolt / (1800000 - uvolt); // thermistor resistance
	i = 1;
	while(i < ARRAY_SIZE(mohm2temp)-1 && mohm2temp[i].mohm >= mohm)
		++i;
	return 0x10000LL * mohm2temp[i].cnum /
		(mohm2temp[i].cdenom + ilog2mult(mohm, mohm2temp[i].f)) - 273150;
}

static umode_t nanopi_thermistor_is_visible(const void *drvdata,
		enum hwmon_sensor_types type, u32 attr, int channel)
{
	return S_IRUGO;
}

static int nanopi_thermistor_read(struct device *dev,
		enum hwmon_sensor_types type, u32 attr, int channel, long *val)
{
	int ret = 0, adcval;
	struct nanopi_thermistor_device *nthdev;

	nthdev = dev_get_drvdata(dev);
	switch( attr ) {
	case hwmon_temp_input:
		ret = iio_read_channel_processed(nthdev->therm_adc, &adcval);
		*val = adcval_to_temperature(adcval);
		break;
	case hwmon_temp_max:
		*val = 85000;
		break;
	}
	return 0;
}

static const struct hwmon_ops nanopi_thermistor_hwmon_ops = {
	.is_visible	= nanopi_thermistor_is_visible,
	.read		= nanopi_thermistor_read,
};

static const u32 nanopi_thermistor_config[] = {
	HWMON_T_INPUT | HWMON_T_MAX,
	0
};

static const struct hwmon_channel_info nanopi_thermistor_channel_info = {
	.type = hwmon_temp,
	.config = nanopi_thermistor_config
};

static const struct hwmon_channel_info *nanopi_thermistor_channel_info_tab[] = {
	&nanopi_thermistor_channel_info,
	NULL
};

static const struct hwmon_chip_info nanopi_thermistor_chip_info = {
	.ops = &nanopi_thermistor_hwmon_ops,
	.info = nanopi_thermistor_channel_info_tab
};

static int nanopi_thermistor_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct nanopi_thermistor_device *nthdev;

	nthdev = devm_kzalloc(&pdev->dev, sizeof(struct nanopi_thermistor_device),
			GFP_KERNEL);
	if( nthdev == NULL )
		return -ENOMEM;
	nthdev->therm_adc = devm_iio_channel_get(&pdev->dev, "nanopi-thermistor");
	if( IS_ERR(nthdev->therm_adc) ) {
		if (PTR_ERR(nthdev->therm_adc) == -ENODEV)
			return -EPROBE_DEFER;
		return PTR_ERR(nthdev->therm_adc);
	}
	nthdev->hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev, 
			"nanopi_thermistor", nthdev, &nanopi_thermistor_chip_info, NULL);
	if( IS_ERR(nthdev->hwmon_dev) ) {
		ret = PTR_ERR(nthdev->hwmon_dev);
		dev_err(&pdev->dev, "hwmon registration error %d\n", ret);
	}else{
		dev_info(&pdev->dev, "registered device\n");
		//platform_set_drvdata(pdev, nthdev);
	}
	return ret;
}

static int nanopi_thermistor_remove(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "unregistered device\n");
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id nanopi_thermistor_of_match[] = {
	{ .compatible = "friendlyarm,nanopi-thermistor" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, nanopi_thermistor_of_match);
#endif

static struct platform_driver nanopi_thermistor_platform_driver = {
	.probe = nanopi_thermistor_probe,
	.remove = nanopi_thermistor_remove,
	.driver = {
		.name = "nanopi-thermistor",
		.of_match_table = of_match_ptr(nanopi_thermistor_of_match),
	},
};

static int __init nanopi_thermistor_init(void)
{
	int ret = platform_driver_register(&nanopi_thermistor_platform_driver);
	pr_info("nanopi-thermistor: registered platform driver\n");
	return ret;
}

static void __exit nanopi_thermistor_exit(void)
{
	platform_driver_unregister(&nanopi_thermistor_platform_driver);
	pr_info("nanopi-thermistor: unregistered platform driver\n");
}

module_init(nanopi_thermistor_init);
module_exit(nanopi_thermistor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rafaello7 <fatwildcat@gmail.com>");
MODULE_DESCRIPTION("NanoPi M3 temperature read from thermistor connected to s5p6818 ADC");
