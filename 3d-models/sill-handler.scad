include <BOSL2/std.scad>
$fn = 30;

function size_max(a, b) = [for (i = [0:2]) max(a[i], b[i])];


shaft_d = 8;

// kp08 - bearing
kp08_length = 55;
kp08_width = 13;
kp08_height = 29;
kp08_hole_d = shaft_d;
kp08_hole_bottom = 15;

kp08_stand_height = 14;

// sk8 is placed on shaft tightly
sk8_height = 32.8;
sk8_length = 42;
sk8_width = 14;
sk8_hole_d = shaft_d;
sk8_hole_bottom = 20;
sk8_bot_hole_d = 5;
sk8_bot_hole_distance = 32;

kp08_sk8_side_distance = (kp08_length - sk8_length) / 2;
sk8_to_table_distance = kp08_stand_height - (sk8_hole_bottom - kp08_hole_bottom);

table_length = 300;
table_width = 400;
table_height = 25;

panel_height = 20;
panel_width = 150;

handWidth = 15;
handLength = 180;
handThickness = 2;

kp08_table_gap = 0;

panel_holder_box_width = 20;
panel_holder_box_length = handLength - (kp08_table_gap + kp08_sk8_side_distance + sk8_length);
echo("panel_holder_box_length", panel_holder_box_length);

panel_holder_box_height = sk8_to_table_distance - handThickness + table_height - panel_height;
echo("panel_holder_box_height", panel_holder_box_height);

panel_holder_box_hole_d = 4;
panel_holder_box_hole_1_position = 10;
panel_holder_box_hole_2_position = 80;


panel_holder_size = [panel_holder_box_length, panel_holder_box_width, panel_holder_box_height];

// Every anchor except CENTER lands on the true box faces; CENTER stays at
// the hole axis, which the geometry keeps at the origin.
// off = offset of the true box center from the hole axis (the origin).
function hole_anchor_override(size, off) =
	function(a) a == CTR ? undef
		: [[a.x*size.x/2 + off.x, a.y*size.y/2 + off.y, a.z*size.z/2 + off.z]];

module kp08(anchor = CENTER, spin = 0, orient = UP) {
	size = [kp08_length, kp08_width, kp08_height];
	attachable(anchor, spin, orient, size = size,
			override = hole_anchor_override(size, [0, 0, kp08_height/2 - kp08_hole_bottom])) {
		color("green", 0.4)
		down(kp08_hole_bottom)
		diff() {
			cuboid([kp08_length, kp08_width, kp08_height], anchor = BOT)
			tag("remove")
				position(BOT)
				up(kp08_hole_bottom)
				ycyl(d=kp08_hole_d, l=kp08_width+2);
		}
		children();
	}
}

module kpWithStand(anchor = CENTER, spin = 0, orient = UP) {
	standExtraSize = 0;
	h = kp08_height + kp08_stand_height;
	size = [kp08_length, kp08_width, h];
	attachable(anchor, spin, orient, size = size,
			override = hole_anchor_override(size, [0, 0, (kp08_height - kp08_stand_height)/2 - kp08_hole_bottom])) {
		kp08() {
			position(BOT)
			color("green", 0.7)
			cuboid([kp08_length+standExtraSize, kp08_width+standExtraSize, kp08_stand_height], anchor = TOP);
		}
		children();
	}
}

module sk8(anchor = CENTER, spin = 0, orient = UP) {
	size = [sk8_length, sk8_width, sk8_height];
	attachable(anchor, spin, orient, size = size,
			override = hole_anchor_override(size, [0, 0, sk8_height/2 - sk8_hole_bottom])) {
		color("yellow", 0.4)
		down(sk8_hole_bottom)
		diff() {
			cuboid([sk8_length, sk8_width, sk8_height], anchor = BOT)
			tag("remove")
				position(BOT)
				up(sk8_hole_bottom)
				ycyl(d=sk8_hole_d, l=sk8_width+2);
		}
		children();
	}
}

module panelHolder(anchor = CENTER, spin = 0, orient = UP) {
	attachable(anchor, spin, orient, size = panel_holder_size) {
		color("blue", 0.2)
		diff() {
			cuboid(panel_holder_size)
			tag("remove") {
				// holes for holder box and panel
				position(RIGHT) left(panel_holder_box_hole_1_position)
				cyl(d=panel_holder_box_hole_d, h = panel_holder_size[2] + 2);
				position(RIGHT) left(panel_holder_box_hole_2_position)
				cyl(d=panel_holder_box_hole_d, h = panel_holder_size[2] + 2);
			}
		}
		children();
	}
}

sk8WithHandSize = [handLength, handWidth, sk8_height + handThickness];
module sk8WithHand(anchor = CENTER, spin = 0, orient = UP) {
	h = sk8WithHandSize[2];
	size = [handLength, handWidth, h];
	// hand extends to the right of the sk8 and hangs handThickness below it
	off = [handLength/2 - sk8_length/2, 0, h/2 - sk8_hole_bottom - handThickness];
	// sk8 places inside L-shape angle, so the holes are shifted accordingly
	sk8_holes_shift = handThickness + sk8_width/2;
	attachable(anchor, spin, orient, size = size,
			override = hole_anchor_override(size, off)) {
		sk8() {
			position(BOT+LEFT)
			color("olivedrab", 0.7)
			diff() {
				cuboid([handLength, handWidth, handThickness], anchor = TOP+LEFT)
				tag("remove") {
					// holes for sk8 bot screws
					position(LEFT+BACK) right(sk8_length/2) fwd(sk8_holes_shift)
					xcopies(sk8_bot_hole_distance)
					cyl(d=sk8_bot_hole_d, h = handThickness + 2);

					// holes for holder box and panel
					position(RIGHT) left(panel_holder_box_hole_1_position)
					cyl(d=panel_holder_box_hole_d, h = handThickness + 2);
					position(RIGHT) left(panel_holder_box_hole_2_position)
					cyl(d=panel_holder_box_hole_d, h = handThickness + 2);
				}
			}
		}
		children();
	}
}


module sk8WithHandAndHolder(anchor = CENTER, spin = 0, orient = UP) {
	size = size_max(sk8WithHandSize, panel_holder_size);
	attachable(anchor, spin, orient, size = size) {
		sk8WithHand() {
			position(BOT+RIGHT)
			panelHolder(TOP+RIGHT);
		}
		children();
	}
}

module table(anchor = TOP, spin = 0, orient = UP) {
	attachable(anchor, spin, orient, size = [table_length, table_width, table_height]) {
		color("grey", 0.4)
		cuboid([table_length, table_width, table_height], rounding = 10, edges=[FWD+RIGHT, RIGHT+BACK, BACK+LEFT, LEFT+FWD]);
		children();
	}
}

module panel(anchor = TOP, spin = 0, orient = UP) {
	attachable(anchor, spin, orient, size = [panel_width, table_width, panel_height]) {
		color("slategrey", 0.4)
		cuboid([panel_width, table_width, panel_height], rounding = 1, edges=[FWD+RIGHT, RIGHT+BACK, BACK+LEFT, LEFT+FWD]);
		children();
	}
}

module kpWithShaft(length = table_width, anchor = CENTER, spin = 0, orient = UP) {
	h = kp08_height + kp08_stand_height;
	size = [kp08_length, length, h];
	attachable(anchor, spin, orient, size = size,
			override = hole_anchor_override(size, [0, 0, (kp08_height - kp08_stand_height)/2 - kp08_hole_bottom])) {
		union() {
			ycyl(d=shaft_d, l=length);
			ycopies(length-40)
			kpWithStand();
		}
		children();
	}
}

module tableWithShaft(gap = 0,anchor = CENTER, spin = 0, orient = UP) {
	attachable(anchor, spin, orient, size = [table_length, table_width, table_height]) {
		kpWithShaft() {
			position(BOT+RIGHT)
				right(gap)
				table(TOP+RIGHT);

		}
		children();
	}
}

module sk8Pair(spread = 300, anchor = CENTER, spin = 0, orient = UP) {
	h = sk8_height + handThickness;
	size = [handLength, spread + handWidth, h];
	off = [handLength/2 - sk8_length/2, 0, h/2 - sk8_hole_bottom - handThickness];
	attachable(anchor, spin, orient, size = size,
			override = hole_anchor_override(size, off)) {
		ycopies(spread)
			sk8WithHandAndHolder();
		children();
	}
}

module panelWithSk8(gap = 10, spread = 300, anchor = CENTER, spin = 0, orient = UP) {
	shift = sk8_length + kp08_sk8_side_distance + gap;
	attachable(anchor, spin, orient, size = [panel_width, table_width, panel_height]) {
		sk8Pair(spread) {
			position(BOT+LEFT)
				right(shift) down(panel_holder_box_height)
				panel(TOP+LEFT);
		}
		children();
	}
}




max_angle = -190;
// swings 0 -> max_angle -> 0 once per animation cycle
angle = max_angle * (1 - cos(360 * $t)) / 2;

// angle = 0;

yrot(angle)
	panelWithSk8(kp08_table_gap);

// up(130)
// !kp08();
// kpWithStand(BOT);
// kpWithShaft(anchor=BOT+RIGHT);
render() tableWithShaft(kp08_table_gap);

!panelHolder();

// print holes
/* 
!yflip_copy(20)
projection(cut = true) {
	up(21)
	sk8WithHand();
}
 */