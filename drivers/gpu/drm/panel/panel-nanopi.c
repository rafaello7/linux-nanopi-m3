#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <drm/drmP.h>
#include <drm/drm_panel.h>
#include <video/videomode.h>


static char connected_rgb[8], connected_lvds[8];
module_param_string(rgb, connected_rgb, sizeof(connected_rgb), 0444);
module_param_string(lvds, connected_lvds, sizeof(connected_lvds), 0444);


struct panel_desc {
	char name[8];
	unsigned bpc;		// max bits per color
	unsigned p_width;	// physical width, in millimeters
	unsigned p_height;	// physical height
	const struct drm_display_mode mode;
};

struct panel_nanopi {
	struct drm_panel base;
	const struct panel_desc *desc;
};

static inline struct panel_nanopi *to_panel_nanopi(struct drm_panel *panel)
{
	return container_of(panel, struct panel_nanopi, base);
}

static int panel_nanopi_get_modes(struct drm_panel *panel)
{
	struct panel_nanopi *p = to_panel_nanopi(panel);
	struct drm_connector *connector = panel->connector;
	struct drm_device *drm = panel->drm;
	struct drm_display_mode *mode;
	const struct drm_display_mode *m = &p->desc->mode;

	if( !p->desc )
		return 0;
	mode = drm_mode_duplicate(drm, m);
	if( !mode ) {
		dev_err(drm->dev, "failed to add mode %ux%u@%u\n",
			m->hdisplay, m->vdisplay, m->vrefresh);
		return 0;
	}
	mode->type |= DRM_MODE_TYPE_DRIVER;
	mode->type |= DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);
	connector->display_info.bpc = p->desc->bpc;
	connector->display_info.width_mm = p->desc->p_width;
	connector->display_info.height_mm = p->desc->p_height;
	return 1;
}

int panel_nanopi_disable(struct drm_panel *panel)
{
	return 0;
}

int panel_nanopi_unprepare(struct drm_panel *panel)
{
	return 0;
}

int panel_nanopi_prepare(struct drm_panel *panel)
{
	return 0;
}

int panel_nanopi_enable(struct drm_panel *panel)
{
	return 0;
}

static const struct drm_panel_funcs panel_nanopi_funcs = {
	.get_modes = panel_nanopi_get_modes,
	.prepare = panel_nanopi_prepare,
	.unprepare = panel_nanopi_unprepare,
	.enable = panel_nanopi_enable,
	.disable = panel_nanopi_disable
};

static const struct panel_desc nanopi_panels[] = {
	{
		.name = "hd101",
		.bpc  = 8,
		.p_width = 218,
		.p_height = 136,
		.mode = {
			.clock = 66670,
			.hdisplay = 1280,
			.hsync_start = 1280 + 16,
			.hsync_end = 1280 + 16 + 30,
			.htotal = 1280 + 16 + 30 + 16,
			.vdisplay = 800,
			.vsync_start = 800 + 8,
			.vsync_end = 800 + 8 + 12,
			.vtotal = 800 + 8 + 12 + 8,
			.vrefresh = 60,
			.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
					DRM_MODE_FLAG_NCSYNC
		}
	},{
		.name = "hd101b",
		.bpc  = 8,
		.p_width = 218,
		.p_height = 136,
		.mode = {
			.clock = 66670,
			.hdisplay = 1280,
			.hsync_start = 1280 + 16,
			.hsync_end = 1280 + 16 + 30,
			.htotal = 1280 + 16 + 30 + 16,
			.vdisplay = 800,
			.vsync_start = 800 + 8,
			.vsync_end = 800 + 8 + 12,
			.vtotal = 800 + 8 + 12 + 8,
			.vrefresh = 60,
			.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
					DRM_MODE_FLAG_NCSYNC
		}
	},{
		.name = "hd700",
		.bpc  = 8,
		.p_width = 94,
		.p_height = 151,
		.mode = {
			.clock = 67184,
			.hdisplay = 800,
			.hsync_start = 800 + 20,
			.hsync_end = 800 + 20 + 24,
			.htotal = 800 + 20 + 24 + 20,
			.vdisplay = 1280,
			.vsync_start = 1280 + 4,
			.vsync_end = 1280 + 4 + 8,
			.vtotal = 1280 + 4 + 8 + 4,
			.vrefresh = 60,
			.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
					DRM_MODE_FLAG_NCSYNC
		}
	},{
		.name = "hd702",
		.bpc  = 8,
		.p_width = 94,
		.p_height = 151,
		.mode = {
			.clock = 67184,
			.hdisplay = 800,
			.hsync_start = 800 + 20,
			.hsync_end = 800 + 20 + 24,
			.htotal = 800 + 20 + 24 + 20,
			.vdisplay = 1280,
			.vsync_start = 1280 + 4,
			.vsync_end = 1280 + 4 + 8,
			.vtotal = 1280 + 4 + 8 + 4,
			.vrefresh = 60,
			.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
					DRM_MODE_FLAG_NCSYNC
		}
	},{
		.name = "s70",
		.bpc  = 8,
		.p_width = 155,
		.p_height = 93,
		.mode = {
			.clock = 28630,
			.hdisplay = 800,
			.hsync_start = 800 + 48,
			.hsync_end = 800 + 48 + 10,
			.htotal = 800 + 48 + 10 + 36,
			.vdisplay = 480,
			.vsync_start = 480 + 22,
			.vsync_end = 480 + 22 + 8,
			.vtotal = 480 + 22 + 8 + 15,
			.vrefresh = 61,
		}
	},{
		.name = "s702",
		.bpc  = 8,
		.p_width = 155,
		.p_height = 93,
		.mode = {
			.clock = 28502,
			.hdisplay = 800,
			.hsync_start = 800 + 44,
			.hsync_end = 800 + 44 + 20,
			.htotal = 800 + 44 + 20 + 26,
			.vdisplay = 480,
			.vsync_start = 480 + 22,
			.vsync_end = 480 + 22 + 8,
			.vtotal = 480 + 22 + 8 + 15,
			.vrefresh = 61,
			.flags = DRM_MODE_FLAG_NCSYNC
		}
	},{
		.name = "s70d",
		.bpc  = 8,
		.p_width = 155,
		.p_height = 93,
		.mode = {
			.clock = 31531,
			.hdisplay = 800,
			.hsync_start = 800 + 80,
			.hsync_end = 800 + 80 + 10,
			.htotal = 800 + 80 + 10 + 78,
			.vdisplay = 480,
			.vsync_start = 480 + 22,
			.vsync_end = 480 + 22 + 8,
			.vtotal = 480 + 22 + 8 + 24,
			.vrefresh = 61,
		}
	},{
		.name = "x710",
		.bpc  = 8,
		.p_width = 154,
		.p_height = 90,
		.mode = {
			.clock = 49971,
			.hdisplay = 1024,
			.hsync_start = 1024 + 84,
			.hsync_end = 1024 + 84 + 88,
			.htotal = 1024 + 84 + 88 + 84,
			.vdisplay = 600,
			.vsync_start = 600 + 10,
			.vsync_end = 600 + 10 + 20,
			.vtotal = 600 + 10 + 20 + 10,
			.vrefresh = 61,
		}
	},{
		.name = "s430",
		.bpc  = 8,
		.p_width = 108,
		.p_height = 64,
		.mode = {
			.clock = 28492,
			.hdisplay = 480,
			.hsync_start = 480 + 64,
			.hsync_end = 480 + 64 + 16,
			.htotal = 480 + 64 + 16 + 0,
			.vdisplay = 800,
			.vsync_start = 800 + 32,
			.vsync_end = 800 + 32 + 16,
			.vtotal = 800 + 32 + 16 + 0,
			.vrefresh = 60,
			.flags = DRM_MODE_FLAG_NCSYNC
		}
	},{
		.name = "h43",
		.bpc  = 8,
		.p_width = 96,
		.p_height = 54,
		.mode = {
			.clock = 9933,
			.hdisplay = 480,
			.hsync_start = 480 + 5,
			.hsync_end = 480 + 5 + 2,
			.htotal = 480 + 5 + 2 + 40,
			.vdisplay = 272,
			.vsync_start = 272 + 8,
			.vsync_end = 272 + 8 + 2,
			.vtotal = 272 + 8 + 2 + 8,
			.vrefresh = 65,
		}
	},{
		.name = "p43",
		.bpc  = 8,
		.p_width = 96,
		.p_height = 54,
		.mode = {
			.clock = 9968,
			.hdisplay = 480,
			.hsync_start = 480 + 5,
			.hsync_end = 480 + 5 + 2,
			.htotal = 480 + 5 + 2 + 40,
			.vdisplay = 272,
			.vsync_start = 272 + 8,
			.vsync_end = 272 + 8 + 2,
			.vtotal = 272 + 8 + 2 + 9,
			.vrefresh = 65,
			.flags = DRM_MODE_FLAG_NCSYNC
		}
	},{
		.name = "w35",
		.bpc  = 6,
		.p_width = 70,
		.p_height = 52,
		.mode = {
			.clock = 6726,
			.hdisplay = 320,
			.hsync_start = 320 + 4,
			.hsync_end = 320 + 4 + 4,
			.htotal = 320 + 4 + 4 + 70,
			.vdisplay = 240,
			.vsync_start = 240 + 4,
			.vsync_end = 240 + 4 + 4,
			.vtotal = 240 + 4 + 4 + 12,
			.vrefresh = 65,
			.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
					DRM_MODE_FLAG_NCSYNC
		}
	},{
		.name = "atops",
		.bpc  = 6,
		.p_width = 155,
		.p_height = 93,
		.mode = {
			.clock = 34539,
			.hdisplay = 800,
			.hsync_start = 800 + 210,
			.hsync_end = 800 + 210 + 20,
			.htotal = 800 + 210 + 20 + 46,
			.vdisplay = 480,
			.vsync_start = 480 + 22,
			.vsync_end = 480 + 22 + 10,
			.vtotal = 480 + 22 + 10 + 23,
			.vrefresh = 60,
		}
	},
};

static int panel_nanopi_platform_probe(struct platform_device *pdev)
{
	int i, err;
	struct panel_nanopi *panel;
	const struct panel_desc *desc = NULL;
	bool isLvds;
	const char *panelName;

	isLvds = of_property_read_bool(pdev->dev.of_node, "lvds");
	panelName = isLvds ? connected_lvds : connected_rgb;
	if( panelName[0] ) {
		for(i = 0; i < ARRAY_SIZE(nanopi_panels) && desc == NULL; ++i) {
			if( !strcmp(nanopi_panels[i].name, panelName) )
				desc = nanopi_panels + i;
		}
		if( desc == NULL ) {
			dev_err(&pdev->dev, "unknown panel \"%s\"\n", panelName);
			dev_info(&pdev->dev, "available panels:");
			for(i = 0; i < ARRAY_SIZE(nanopi_panels); ++i)
				pr_cont(" %s", nanopi_panels[i].name);
			pr_cont("\n");
		}
	}
	if( desc == NULL )
		return -ENODEV;
	panel = devm_kzalloc(&pdev->dev, sizeof(*panel), GFP_KERNEL);
	if (!panel)
		return -ENOMEM;
	panel->desc = desc;
	drm_panel_init(&panel->base);
	panel->base.dev = &pdev->dev;
	panel->base.funcs = &panel_nanopi_funcs;

	err = drm_panel_add(&panel->base);
	if( err < 0 )
		return err;
	dev_set_drvdata(&pdev->dev, panel);
	dev_info(&pdev->dev, "added %s panel for %s\n", isLvds ? "lvds" : "rgb",
			panelName);
	return 0;
}

static int panel_nanopi_platform_remove(struct platform_device *pdev)
{
	struct panel_nanopi *panel = dev_get_drvdata(&pdev->dev);

	drm_panel_detach(&panel->base);
	drm_panel_remove(&panel->base);
	return 0;
}

static const struct of_device_id platform_of_match[] = {
	{
		.compatible = "nanopi,nano-panel",
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, platform_of_match);

static struct platform_driver panel_nanopi_platform_driver = {
	.driver = {
		.name = "panel-nanopi",
		.of_match_table = platform_of_match,
	},
	.probe = panel_nanopi_platform_probe,
	.remove = panel_nanopi_platform_remove,
};

static int __init panel_nanopi_init(void)
{
	int err;

	err = platform_driver_register(&panel_nanopi_platform_driver);
	if (err < 0)
		return err;

	return 0;
}
module_init(panel_nanopi_init);

static void __exit panel_nanopi_exit(void)
{
	platform_driver_unregister(&panel_nanopi_platform_driver);
}
module_exit(panel_nanopi_exit);

MODULE_AUTHOR("Rafaello7 <fatwildcat@gmail.com>");
MODULE_DESCRIPTION("DRM driver for NanoPi M3 panel");
MODULE_LICENSE("GPL");
