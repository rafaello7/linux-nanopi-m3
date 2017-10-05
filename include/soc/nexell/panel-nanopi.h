#ifndef _PANEL_NANOPI_H
#define _PANEL_NANOPI_H

struct nanopi_panel_desc {
	char name[8];
	const char *i2c_touch_drv;	// touch sensor driver name for i2c bus
	int  i2c_touch_reg;			// i2c address of touch sensor
	unsigned onewireType;		// id reported by onewire channel
	unsigned bpc;				// max bits per color
	unsigned p_width;			// physical width, in millimeters
	unsigned p_height;			// physical height
	const struct drm_display_mode *mode;
};


/* returns type ID reported by panel on onewire bus
 */
int onewire_get_lcd_type(void);


/* Returns RGB panel currently connected, NULL when none
 */
const struct nanopi_panel_desc *nanopi_panelrgb_get_connected(void);


/* Returns true when touch sensor connected to RGB panel should be
 * handled by onewire protocol.
 *
 * onewireType parameter is the type reported by panel on onewire bus.
 */
bool nanopi_panelrgb_issensor_1wire(int onewireType);

#endif // _PANEL_NANOPI_H
