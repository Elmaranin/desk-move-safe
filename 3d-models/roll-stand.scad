include <BOSL2/std.scad>
$fn = 130;

height = 14;
length = 55;
width = 13;

hole = 5;

diff() {
    cuboid([length, width, height], rounding = 3, edges=[FWD+RIGHT, RIGHT+BACK, BACK+LEFT, LEFT+FWD]);

    tag("remove")
    xcopies(42)
        cyl(l = height+2, d = hole);
}
