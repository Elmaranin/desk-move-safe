# SillHandler — native, skeleton-driven Fusion 360 recreation of
# 3d-models/sill-handler.scad
#
# Top-down master-sketch ("skeleton") structure:
#   * A root-level 'Skeleton' sketch holds every part profile (side view),
#     fully dimensioned; all fx parameter bindings live ONLY there.
#   * Each part is its own top-level component (Table, Panel, Shaft, KP08s,
#     SK8s) whose sketches contain nothing but projections of the Skeleton —
#     components never reference each other and can be restructured freely.
#   * The assembly is expressed with rigid groups + one revolute joint, not
#     with parent/child coupling.
#
# Parametric: user parameters (Modify > Change Parameters) drive the
# Skeleton dimensions, extrude extents, and fillet radii; derived values
# (hand_out_of_table, kp08_spread, ...) are stored as formulas, mirroring
# the OpenSCAD source. Change a value and the whole model rebuilds.
#
# Orientation: Y-up (Fusion default). The tabletop is horizontal, the shaft
# and bearing-hole axis is the global Z axis through the origin, and the
# panel folds away underneath. All dimensions in mm.
#
# Run via Utilities > ADD-INS > Scripts and Add-Ins (Shift+S).

import math
import traceback

import adsk.core
import adsk.fusion

# --- numeric mirrors of the parameters (mm), used to draw the initial
# geometry; the authoritative values live in the user parameters below ----
shaft_d = 8

kp08_length = 55
kp08_width = 13
kp08_height = 29
kp08_hole_bottom = 15
kp08_stand_height = 14

sk8_height = 32.8
sk8_length = 42
sk8_width = 14
sk8_hole_bottom = 20

kp08_sk8_side_distance = (kp08_length - sk8_length) / 2
sk8_to_table_distance = kp08_stand_height - (sk8_hole_bottom - kp08_hole_bottom)

table_length = 300
table_width = 400
table_height = 25

panel_height = 16
panel_width = 150

hand_width = 15
hand_length = 180
hand_thickness = 2
panel_holder_width = 20

kp08_table_gap = 25

hand_out_of_table = hand_length - (kp08_table_gap + kp08_sk8_side_distance + sk8_length)
sk8_hand_to_panel_distance = sk8_to_table_distance - hand_thickness + table_height - panel_height

max_swing_deg = 190  # max_angle in the OpenSCAD animation

# user parameters: (name, expression, comment); order matters — derived
# parameters may only reference ones defined above them
USER_PARAMS = [
    ('shaft_d', '8 mm', 'shaft / bearing hole diameter'),
    ('kp08_length', '55 mm', 'KP08 pillow block length'),
    ('kp08_width', '13 mm', 'KP08 pillow block width'),
    ('kp08_height', '29 mm', 'KP08 pillow block height'),
    ('kp08_hole_bottom', '15 mm', 'KP08 hole center above block bottom'),
    ('kp08_stand_height', '14 mm', 'printed stand under each KP08'),
    ('sk8_length', '42 mm', 'SK8 shaft support length'),
    ('sk8_width', '14 mm', 'SK8 shaft support width'),
    ('sk8_height', '32.8 mm', 'SK8 shaft support height'),
    ('sk8_hole_bottom', '20 mm', 'SK8 hole center above block bottom'),
    ('table_length', '300 mm', 'table top length'),
    ('table_width', '400 mm', 'table top width (also shaft length)'),
    ('table_height', '25 mm', 'table top thickness'),
    ('panel_width', '150 mm', 'flip panel width'),
    ('panel_height', '16 mm', 'flip panel thickness'),
    ('hand_length', '180 mm', 'swing hand length'),
    ('hand_width', '15 mm', 'swing hand width'),
    ('hand_thickness', '2 mm', 'swing hand thickness'),
    ('panel_holder_width', '20 mm', 'panel holder width'),
    ('kp08_table_gap', '25 mm', 'gap between KP08 and table edge'),
    ('table_rounding', '10 mm', 'table corner rounding'),
    ('panel_rounding', '1 mm', 'panel corner rounding'),
    ('sk8_spread', '300 mm', 'distance between the two SK8 centers'),
    ('kp08_spread', 'table_width - 40 mm', 'distance between the two KP08 centers'),
    ('kp08_sk8_side_distance', '(kp08_length - sk8_length) / 2', 'derived'),
    ('sk8_to_table_distance', 'kp08_stand_height - (sk8_hole_bottom - kp08_hole_bottom)', 'derived'),
    ('hand_out_of_table', 'hand_length - (kp08_table_gap + kp08_sk8_side_distance + sk8_length)', 'derived'),
    ('sk8_hand_to_panel_distance', 'sk8_to_table_distance - hand_thickness + table_height - panel_height', 'derived'),
]

NEW_BODY = adsk.fusion.FeatureOperations.NewBodyFeatureOperation
HORIZONTAL = adsk.fusion.DimensionOrientations.HorizontalDimensionOrientation
VERTICAL = adsk.fusion.DimensionOrientations.VerticalDimensionOrientation


def mm(v):
    return v / 10.0  # the Fusion API works in cm internally


def m2s(sketch, u, v):
    # model (X, Y) mm -> sketch space point; all sketches lie on XY planes
    # (u = along table length, v = vertical/up)
    return sketch.modelToSketchSpace(adsk.core.Point3D.create(mm(u), mm(v), 0))


def u_maps_to_sketch_x(sketch):
    # which sketch axis does model X land on? (avoids relying on plane
    # orientation conventions)
    o = sketch.modelToSketchSpace(adsk.core.Point3D.create(0, 0, 0))
    p = sketch.modelToSketchSpace(adsk.core.Point3D.create(1, 0, 0))
    return abs(p.x - o.x) >= abs(p.y - o.y)


def text_point(sketch_point, dx, dy):
    g = sketch_point.geometry
    return adsk.core.Point3D.create(g.x + dx, g.y + dy, 0)


def param_rect(sketch, u1, v1, u2, v2, w_expr, h_expr, ou_expr, ov_expr):
    """Rectangle from model-XY corners (mm), fully dimensioned: width/height
    plus the (u1, v1) corner's distances from the origin, each bound to a
    parameter expression (all magnitudes, so pass positive expressions).
    Returns the four lines for projection into part sketches."""
    lines = sketch.sketchCurves.sketchLines.addTwoPointRectangle(
        m2s(sketch, u1, v1), m2s(sketch, u2, v2))
    a = lines.item(0).startSketchPoint      # the (u1, v1) corner
    c = lines.item(1).endSketchPoint        # the (u2, v2) corner
    if u_maps_to_sketch_x(sketch):
        u_ori, v_ori = HORIZONTAL, VERTICAL
    else:
        u_ori, v_ori = VERTICAL, HORIZONTAL
    dims = sketch.sketchDimensions
    d = dims.addDistanceDimension(a, c, u_ori, text_point(c, 0.8, 0.8))
    d.parameter.expression = w_expr
    d = dims.addDistanceDimension(a, c, v_ori, text_point(c, -0.8, -0.8))
    d.parameter.expression = h_expr
    d = dims.addDistanceDimension(sketch.originPoint, a, u_ori, text_point(a, 0.8, -0.8))
    d.parameter.expression = ou_expr
    d = dims.addDistanceDimension(sketch.originPoint, a, v_ori, text_point(a, -0.8, 0.8))
    d.parameter.expression = ov_expr
    return [lines.item(i) for i in range(lines.count)]


def param_circle_at_origin(sketch, r, d_expr):
    c = m2s(sketch, 0, 0)
    circle = sketch.sketchCurves.sketchCircles.addByCenterRadius(c, mm(r))
    sketch.geometricConstraints.addCoincident(circle.centerSketchPoint, sketch.originPoint)
    d = sketch.sketchDimensions.addDiameterDimension(
        circle, adsk.core.Point3D.create(c.x + 1, c.y + 1, 0))
    d.parameter.expression = d_expr
    return circle


def new_component(parent, name):
    occ = parent.occurrences.addNewComponent(adsk.core.Matrix3D.create())
    occ.component.name = name
    return occ


def project_into(comp, entities):
    """A part sketch that contains nothing but projections of Skeleton
    entities — the only link a component has to the outside world."""
    sk = comp.sketches.add(comp.xYConstructionPlane)
    for e in entities:
        sk.project(e)
    return sk


def profile_with_loops(sketch, loop_count):
    # a rectangle-with-hole sketch yields two profiles; the 2-loop one is
    # the block with the bore already subtracted
    for i in range(sketch.profiles.count):
        p = sketch.profiles.item(i)
        if p.profileLoops.count == loop_count:
            return p
    return sketch.profiles.item(0)


def sym_extrude(comp, profile, width_expr, name):
    # symmetric about the sketch plane — immune to plane-normal direction
    extrudes = comp.features.extrudeFeatures
    inp = extrudes.createInput(profile, NEW_BODY)
    inp.setSymmetricExtent(adsk.core.ValueInput.createByString(width_expr), True)
    body = extrudes.add(inp).bodies.item(0)
    body.name = name
    return body


def offset_extrude(comp, profile, offset_expr, width_expr, name):
    # one part of a symmetric pair: start at a parametric offset from the
    # sketch plane, extrude one width along the plane normal; the +/- pair
    # of offsets keeps the pair symmetric whatever the normal's sign is
    extrudes = comp.features.extrudeFeatures
    inp = extrudes.createInput(profile, NEW_BODY)
    inp.startExtent = adsk.fusion.OffsetStartDefinition.create(
        adsk.core.ValueInput.createByString(offset_expr))
    inp.setOneSideExtent(
        adsk.fusion.DistanceExtentDefinition.create(
            adsk.core.ValueInput.createByString(width_expr)),
        adsk.fusion.ExtentDirections.PositiveExtentDirection)
    body = extrudes.add(inp).bodies.item(0)
    body.name = name
    return body


def pair_extrude(comp, profile, spread, width, name):
    offset_extrude(comp, profile, '{} / 2 - {} / 2'.format(spread, width),
                   width, name + '-1')
    return offset_extrude(comp, profile, '-{} / 2 - {} / 2'.format(spread, width),
                          width, name + '-2')


def fillet_vertical_edges(comp, body, radius_expr):
    # rounds the edges running along Y (the up axis) — the plan-view corners
    edges = adsk.core.ObjectCollection.create()
    for i in range(body.edges.count):
        e = body.edges.item(i)
        if not adsk.core.Line3D.cast(e.geometry):
            continue
        sp, ep = e.startVertex.geometry, e.endVertex.geometry
        if abs(ep.x - sp.x) < 1e-6 and abs(ep.z - sp.z) < 1e-6:
            edges.add(e)
    inp = comp.features.filletFeatures.createInput()
    inp.edgeSetInputs.addConstantRadiusEdgeSet(
        edges, adsk.core.ValueInput.createByString(radius_expr), True)
    comp.features.filletFeatures.add(inp)


def rigid_group(root, occurrences):
    coll = adsk.core.ObjectCollection.create()
    for o in occurrences:
        coll.add(o)
    return root.rigidGroups.add(coll, True)


def run(context):
    ui = None
    try:
        app = adsk.core.Application.get()
        ui = app.userInterface
        app.documents.add(adsk.core.DocumentTypes.FusionDesignDocumentType)
        design = adsk.fusion.Design.cast(app.activeProduct)
        design.designType = adsk.fusion.DesignTypes.ParametricDesignType
        root = design.rootComponent

        for name, expr, comment in USER_PARAMS:
            design.userParameters.add(
                name, adsk.core.ValueInput.createByString(expr), 'mm', comment)

        # --- the Skeleton: one root sketch, side view (XY plane), holding
        # every profile and every fx binding --------------------------------
        table_u_max = kp08_length / 2 + kp08_table_gap
        table_v_max = -(kp08_hole_bottom + kp08_stand_height)
        panel_v_max = -(sk8_hole_bottom + hand_thickness + sk8_hand_to_panel_distance)
        hand_u_min = -sk8_length / 2
        hand_u_max = hand_u_min + hand_length
        hand_v_max = -sk8_hole_bottom

        skeleton = root.sketches.add(root.xYConstructionPlane)
        skeleton.name = 'Skeleton'

        skel_table = param_rect(
            skeleton, table_u_max - table_length, table_v_max - table_height,
            table_u_max, table_v_max,
            'table_length', 'table_height',
            'table_length - kp08_length / 2 - kp08_table_gap',
            'kp08_hole_bottom + kp08_stand_height + table_height')
        skel_panel = param_rect(
            skeleton, table_u_max, panel_v_max - panel_height,
            table_u_max + panel_width, panel_v_max,
            'panel_width', 'panel_height',
            'kp08_length / 2 + kp08_table_gap',
            'sk8_hole_bottom + hand_thickness + sk8_hand_to_panel_distance + panel_height')
        skel_kp08 = param_rect(
            skeleton, -kp08_length / 2, -kp08_hole_bottom,
            kp08_length / 2, kp08_height - kp08_hole_bottom,
            'kp08_length', 'kp08_height',
            'kp08_length / 2', 'kp08_hole_bottom')
        skel_stand = param_rect(
            skeleton, -kp08_length / 2, -kp08_hole_bottom - kp08_stand_height,
            kp08_length / 2, -kp08_hole_bottom,
            'kp08_length', 'kp08_stand_height',
            'kp08_length / 2', 'kp08_hole_bottom + kp08_stand_height')
        skel_sk8 = param_rect(
            skeleton, -sk8_length / 2, -sk8_hole_bottom,
            sk8_length / 2, sk8_height - sk8_hole_bottom,
            'sk8_length', 'sk8_height',
            'sk8_length / 2', 'sk8_hole_bottom')
        skel_hand = param_rect(
            skeleton, hand_u_min, hand_v_max - hand_thickness,
            hand_u_max, hand_v_max,
            'hand_length', 'hand_thickness',
            'sk8_length / 2', 'sk8_hole_bottom + hand_thickness')
        skel_holder = param_rect(
            skeleton, hand_u_max - hand_out_of_table,
            hand_v_max - hand_thickness - sk8_hand_to_panel_distance,
            hand_u_max, hand_v_max - hand_thickness,
            'hand_out_of_table', 'sk8_hand_to_panel_distance',
            'hand_length - sk8_length / 2 - hand_out_of_table',
            'sk8_hole_bottom + hand_thickness + sk8_hand_to_panel_distance')
        # one shared circle: the shaft OD and both bearing bores
        skel_hole = [param_circle_at_origin(skeleton, shaft_d / 2, 'shaft_d')]

        # --- parts: each component references ONLY the Skeleton ------------
        table_occ = new_component(root, 'Table')
        sk = project_into(table_occ.component, skel_table)
        body = sym_extrude(table_occ.component, sk.profiles.item(0),
                           'table_width', 'table')
        fillet_vertical_edges(table_occ.component, body, 'table_rounding')

        panel_occ = new_component(root, 'Panel')
        sk = project_into(panel_occ.component, skel_panel)
        body = sym_extrude(panel_occ.component, sk.profiles.item(0),
                           'table_width', 'panel')
        fillet_vertical_edges(panel_occ.component, body, 'panel_rounding')

        shaft_occ = new_component(root, 'Shaft')
        sk = project_into(shaft_occ.component, skel_hole)
        shaft_body = sym_extrude(shaft_occ.component, sk.profiles.item(0),
                                 'table_width', 'shaft')

        kp_occ = new_component(root, 'KP08s')
        kp = kp_occ.component
        sk = project_into(kp, skel_kp08 + skel_hole)
        pair_extrude(kp, profile_with_loops(sk, 2), 'kp08_spread', 'kp08_width', 'kp08')
        sk = project_into(kp, skel_stand)
        pair_extrude(kp, sk.profiles.item(0), 'kp08_spread', 'kp08_width', 'stand')

        sk_occ = new_component(root, 'SK8s')
        s8 = sk_occ.component
        sk = project_into(s8, skel_sk8 + skel_hole)
        pair_extrude(s8, profile_with_loops(sk, 2), 'sk8_spread', 'sk8_width', 'sk8')
        sk = project_into(s8, skel_hand)
        pair_extrude(s8, sk.profiles.item(0), 'sk8_spread', 'hand_width', 'hand')
        sk = project_into(s8, skel_holder)
        pair_extrude(s8, sk.profiles.item(0), 'sk8_spread', 'panel_holder_width', 'panelHolder')

        # --- assembly: rigid groups + one revolute joint, no parent/child
        # coupling ----------------------------------------------------------
        table_occ.isGrounded = True
        rigid_group(root, [table_occ, kp_occ])
        rigid_group(root, [shaft_occ, sk_occ, panel_occ])

        shaft_face = None
        for i in range(shaft_body.faces.count):
            f = shaft_body.faces.item(i)
            if f.geometry.surfaceType == adsk.core.SurfaceTypes.CylinderSurfaceType:
                shaft_face = f
                break
        face_proxy = shaft_face.createForAssemblyContext(shaft_occ)
        geo = adsk.fusion.JointGeometry.createByNonPlanarFace(
            face_proxy, adsk.fusion.JointKeyPointTypes.MiddleKeyPoint)
        joint_input = root.asBuiltJoints.createInput(shaft_occ, table_occ, geo)
        joint_input.setAsRevoluteJointMotion(adsk.fusion.JointDirections.ZAxisJointDirection)
        joint = root.asBuiltJoints.add(joint_input)
        joint.name = 'PanelSwing'
        try:
            limits = joint.jointMotion.rotationLimits
            limits.isMinimumValueEnabled = True
            limits.minimumValue = -math.radians(max_swing_deg)
            limits.isMaximumValueEnabled = True
            limits.maximumValue = 0.0
        except Exception:
            pass  # limits are a convenience; the joint still works without them

        app.activeViewport.fit()
        ui.messageBox(
            'Sill handler assembly created (skeleton method).\n'
            'All dimensions live in the root "Skeleton" sketch and in\n'
            'Modify > Change Parameters — edit either and the model rebuilds.\n'
            'hand_out_of_table = {} mm\n'
            'sk8_hand_to_panel_distance = {} mm'.format(
                hand_out_of_table, sk8_hand_to_panel_distance))
    except Exception:
        if ui:
            ui.messageBox('Failed:\n{}'.format(traceback.format_exc()))
