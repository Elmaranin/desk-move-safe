include <BOSL2/std.scad>
$fn = 130;

width = 25;
height = 40;
thickness = 3;

hole = 2.5;

diff() {
    cuboid([thickness, width, height], rounding = 3, edges=[FWD+TOP, TOP+BACK, BACK+BOTTOM, BOTTOM+FWD])

    tag("remove")
    position(BOT)
        ycopies(16) up(5)
        xcyl(l = thickness+2, d = hole);
}
