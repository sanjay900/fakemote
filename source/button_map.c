#include "button_map.h"
#include "globals.h"

void bm_map_wiimote(
	/* Inputs */
	int num_buttons, u32 buttons,
	/* Mapping tables */
	const u16 *wiimote_button_map,
	/* Outputs */
	u16 *wiimote_buttons)
{
	for (int i = 0; i < num_buttons; i++) {
		if (buttons & 1)
			*wiimote_buttons |= wiimote_button_map[i];
		buttons >>= 1;
	}
}

void bm_map_nunchuk(
	/* Inputs */
	int num_buttons, u32 buttons,
	int num_analog_axis, const u8 *analog_axis,
	u16 ax, u16 ay, u16 az,
	/* Mapping tables */
	const u8 *nunchuk_button_map,
	const u8 *nunchuk_analog_axis_map,
	/* Outputs */
	struct wiimote_extension_data_format_nunchuk_t *nunchuk)
{
	u8 nunchuk_buttons = 0;
	u8 nunchuk_analog_axis[BM_NUNCHUK_ANALOG_AXIS__NUM] = {0};

	for (int i = 0; i < num_buttons; i++) {
		if (buttons & 1)
			nunchuk_buttons |= nunchuk_button_map[i];
		buttons >>= 1;
	}

	for (int i = 0; i < num_analog_axis; i++) {
		if (nunchuk_analog_axis_map[i])
			nunchuk_analog_axis[nunchuk_analog_axis_map[i] - 1] = analog_axis[i];
	}

	bm_nunchuk_format(nunchuk, nunchuk_buttons, nunchuk_analog_axis, ax, ay, az);
}

void bm_map_classic(
	/* Inputs */
	int num_buttons, u32 buttons,
	int num_analog_axis, const u8 *analog_axis,
	/* Mapping tables */
	const u16 *classic_button_map,
	const u8 *classic_analog_axis_map,
	/* Outputs */
	struct wiimote_extension_data_format_classic_t *classic)
{
	u16 classic_buttons = 0;
	u8 classic_analog_axis[BM_CLASSIC_ANALOG_AXIS__NUM] = {0};

	for (int i = 0; i < num_buttons; i++) {
		if (buttons & 1)
			classic_buttons |= classic_button_map[i];
		buttons >>= 1;
	}

	for (int i = 0; i < num_analog_axis; i++) {
		if (classic_analog_axis_map[i])
			classic_analog_axis[classic_analog_axis_map[i] - 1] = analog_axis[i];
	}

	bm_classic_format(classic, classic_buttons, classic_analog_axis);
}

void bm_calculate_ir(
	/* Inputs */
	int num_coordinates, const u16 *x, const u16 *y,
	u16 max_x, u16 max_y,
	/* Outputs */
	struct ir_dot_t ir_dots[static IR_MAX_DOTS])
{
	struct ir_dot_t dot;
	s16 vert_offset = g_sensor_bar_position_top ? IR_VERTICAL_OFFSET : -IR_VERTICAL_OFFSET;

	/* TODO: For now we only care about 1 reported coordinate... */
	if (num_coordinates == 0) {
		ir_dots[0].x = 1023;
		ir_dots[0].y = 1023;
		ir_dots[1].x = 1023;
		ir_dots[1].y = 1023;
		return;
	}

	dot.x = IR_DOT_CENTER_MIN_X + (x[0] * (IR_DOT_CENTER_MAX_X - IR_DOT_CENTER_MIN_X)) / max_x;
	dot.y = IR_DOT_CENTER_MIN_Y + (y[0] * (IR_DOT_CENTER_MAX_Y - IR_DOT_CENTER_MIN_Y)) / max_y;

	ir_dots[0].x = (IR_HIGH_X - dot.x) - IR_HORIZONTAL_OFFSET;
	ir_dots[0].y = dot.y + vert_offset;
	ir_dots[1].x = (IR_HIGH_X - dot.x) + IR_HORIZONTAL_OFFSET;
	ir_dots[1].y = dot.y + vert_offset;
}
