include <BOSL2/std.scad>
$fn = 130;

shaft_d = 8.1;
magnet_d = 4.1;

thickness = .8;
length = 5;

diff() {
	cyl(l = length, d = shaft_d + 2*thickness)

	tag("remove") {
		position(BOT)
			up(thickness)
			cyl(l = length, d = shaft_d, anchor = BOT);

		position(BOT)
			up(thickness)
			cyl(l = length, d = magnet_d);
	}
}
