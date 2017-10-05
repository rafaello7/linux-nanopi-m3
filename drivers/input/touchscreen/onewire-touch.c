/* Registers appropriate touchscreen input device for NanoPi M3
 * based on LCD panel type connected to RGB connector.
 * The panel type is reported by LCD panel on onewire channel.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/of_irq.h>
#include <soc/nexell/panel-nanopi.h>


static int onewire_touch_probe(struct platform_device *pdev)
{
	struct i2c_client *touchsensor;
	struct i2c_board_info bdi;
	struct i2c_adapter *adap;
	struct device_node *i2c_bus_np;
	const struct nanopi_panel_desc *panel_desc;

	panel_desc = nanopi_panelrgb_get_connected();
	if( panel_desc == NULL || !panel_desc->i2c_touch_drv ||
			!strcmp(panel_desc->i2c_touch_drv, "onewire"))
		return -ENODEV;
	memset(&bdi, 0, sizeof(bdi));
	strlcpy(bdi.type, panel_desc->i2c_touch_drv, sizeof(bdi.type));
	bdi.addr = panel_desc->i2c_touch_reg;
	bdi.irq = platform_get_irq(pdev, 0);
	if( bdi.irq < 0 ) {
		dev_err(&pdev->dev, "unable to get irq: %d\n", bdi.irq);
		return bdi.irq;
	}
	i2c_bus_np = of_parse_phandle(pdev->dev.of_node, "i2c-bus", 0);
	if( i2c_bus_np == NULL ) {
		dev_err(&pdev->dev, "no i2c-bus property\n");
		return -EINVAL;
	}
	adap = of_find_i2c_adapter_by_node(i2c_bus_np);
	of_node_put(i2c_bus_np);
	if( adap == NULL ) {
		dev_err(&pdev->dev, "i2c-bus for touch sensor not found\n");
		return -EPROBE_DEFER;
	}
	touchsensor = i2c_new_device(adap, &bdi);
	put_device(&adap->dev);
	if( touchsensor == NULL ) {
		dev_err(&pdev->dev, "touch sensor registration error\n");
		return -ENXIO;
	}
	platform_set_drvdata(pdev, touchsensor);
	dev_info(&pdev->dev, "probe success\n");
	return 0;
}

static int onewire_touch_remove(struct platform_device *pdev)
{
	struct i2c_client *touchsensor = platform_get_drvdata(pdev);

	i2c_unregister_device(touchsensor);
	dev_info(&pdev->dev, "removed\n");
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id onewire_touch_of_match[] = {
	{ .compatible = "friendlyarm,onewire-touch" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, onewire_touch_of_match);
#endif

struct platform_driver onewire_touch_platform_driver = {
	.probe = onewire_touch_probe,
	.remove = onewire_touch_remove,
	.driver = {
		.name = "onewire-touch",
		.of_match_table = of_match_ptr(onewire_touch_of_match),
	},
};

static int __init onewire_touch_init(void)
{
	return platform_driver_register(&onewire_touch_platform_driver);
}

static void __exit onewire_touch_exit(void)
{
	platform_driver_unregister(&onewire_touch_platform_driver);
}

module_init(onewire_touch_init);
module_exit(onewire_touch_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rafaello7 <fatwildcat@gmail.com>");
MODULE_DESCRIPTION("FriendlyArm onewire touch sensor driver");
