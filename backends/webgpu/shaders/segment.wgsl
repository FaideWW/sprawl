struct Uniforms {
    viewport: vec2f,
    _pad: vec2f,
}

struct Point {
    x: f32,
    y: f32,
    p: f32,
}

struct Stroke {
    color: vec4f,
    offset: vec2f,
    k: f32,
    radius: f32,
    first_point: u32,
    points_count: u32,
    subdivisions: u32,
}

const MIN_RADIUS = 0.5;

@group(0) @binding(0)
var<uniform> u: Uniforms;
@group(0) @binding(1)
var<storage, read> points: array<Point>;
@group(0) @binding(2)
var<storage, read> strokes: array<Stroke>;

struct VsOut {
    @builtin(position) pos: vec4f,
    @interpolate(flat)
    @location(0) a: vec3f,
    @interpolate(flat)
    @location(1) b: vec3f,
    @interpolate(flat)
    @location(2) color: vec4f,
    @interpolate(flat)
    @location(3) radius: f32,
}

fn knot(a: vec2f, b: vec2f, fallback: f32) -> f32 {
    let d = sqrt(length(b - a));
    return select(d, fallback, d < 1e-4);
}

fn catmull_rom(p0: vec2f, p1: vec2f, p2: vec2f, p3: vec2f, u: f32) -> vec2f {
    let d1 = knot(p1, p2, 1.0);
    let d0 = knot(p0, p1, d1);
    let d2 = knot(p2, p3, d1);

    let m1 = ((p1 - p0) / d0 - (p2 - p0) / (d0 + d1) + (p2 - p1) / d1) * d1;
    let m2 = ((p2 - p1) / d1 - (p3 - p1) / (d1 + d2) + (p3 - p2) / d2) * d1;
    let u2 = u * u;
    let u3 = u2 * u;

    return (2.0 * u3 - 3.0 * u2 + 1.0) * p1 + (u3 - 2.0 * u2 + u) * m1 + (-2.0 * u3 + 3.0 * u2) * p2
        + (u3 - u2) * m2;
}

fn xy(i: u32) -> vec2f {
    return vec2f(points[i].x, points[i].y);
}

// each stroke segment has a bounding box represented as a quad. the vertex shader 
// takes each vertex of the bounding box quad, converts the local coordinates into 
// worldspace coordinates using the stroke's origin and scale (k), adds 1px of 
// padding to prevent clipping during anti-aliasing, and returns the clipspace 
// coordinate of that vertex. this is repeated `stroke.subdivisions` times for each
// stroke segment
@vertex
fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VsOut {
    // vi = stroke * 4 + corner
    let s = strokes[vi >> 2u];
    let corner = vi & 3u;

    let radius = max(s.radius, MIN_RADIUS);
    let fade = min(s.radius / MIN_RADIUS, 1.0);

    // local_instance = span * subdivisions + sub_segment
    let local_instance = ii - s.first_point;
    let j = local_instance % s.subdivisions;

    let i1 = s.first_point + local_instance / s.subdivisions;
    let i2 = i1 + 1u;

    let i0 = max(i1, s.first_point + 1u) - 1u;
    let last = s.first_point + s.points_count - 1u;
    let i3 = min(i2 + 1u, last);

    let u0 = f32(j) / f32(s.subdivisions);
    let u1 = f32(j + 1u) / f32(s.subdivisions);

    let a = catmull_rom(xy(i0), xy(i1), xy(i2), xy(i3), u0) * s.k + s.offset;
    let b = catmull_rom(xy(i0), xy(i1), xy(i2), xy(i3), u1) * s.k + s.offset;

    let ab = b - a;
    let len = length(ab);
    let dir = select(vec2f(1.0, 0.0), ab / len, len > 0.0);
    let n = vec2f(-dir.y, dir.x);

    // resolves to quad corners based on vi: (-1,-1) (1,-1) (-1, 1) (1, 1)
    let x = f32(corner & 1u) * 2.0 - 1.0;
    let y = f32(corner >> 1u) * 2.0 - 1.0;

    //let pressure1 = pb.p;
    //let pressure2 = pc.p;

    //let r = (pressure1 + (pressure2 - pressure1) * (f32(j+0.5)/f32(s.subdivisions))) * s.radius;
    //let radius = max(r, MIN_RADIUS);
    //let fade = min(r / MIN_RADIUS, 1.0);

    let pad = radius + 1.0;
    let p = select(a, b, x > 0.0) + dir * (x * pad) + n * (y * pad);

    let clip = p / u.viewport * 2.0 - 1.0;

    var o: VsOut;
    o.pos = vec4f(clip.x, -clip.y, 0.0, 1.0);
    o.a = vec3f(a, mix(points[i1].p, points[i2].p, u0));
    o.b = vec3f(b, mix(points[i1].p, points[i2].p, u1));
    o.color = vec4f(s.color.rgb, s.color.a * fade);
    o.radius = radius;
    return o;
}

@fragment
fn fs(in: VsOut) -> @location(0) vec4f {
    let pa = in.pos.xy - in.a.xy;
    let ba = in.b.xy - in.a.xy;
    // project the point onto the segment, clamped to the ends to find the closest point
    let h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-8), 0.0, 1.0);

    // compute signed distance from closest point and return a coverage value for anti-aliasing
    let d = length(pa - ba * h) - in.radius;
    let cov = clamp(0.5 - d, 0.0, 1.0);

    // premultiply the color output by its coverage
    return vec4f(in.color.rgb, 1.0) * (in.color.a * cov);
}
