struct Uniforms {
    viewport: vec2f,
    _pad: vec2f,
};

struct Point {
    x: f32,
    y: f32,
    p: f32,
};

struct Stroke {
    color: vec4f,
    offset: vec2f,
    k: f32,
    radius: f32,
    first_point: u32,
    points_count: u32,
}

@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var<storage, read> points: array<Point>;
@group(0) @binding(2) var<storage, read> strokes: array<Stroke>;

struct VsOut {
    @builtin(position) pos: vec4f, 
    @location(0) @interpolate(flat) a: vec3f,
    @location(1) @interpolate(flat) b: vec3f,
    @location(2) @interpolate(flat) color: vec4f,
    @location(3) @interpolate(flat) radius: f32,
};

// each stroke segment has a bounding box represented as a quad. the vertex shader 
// takes each vertex of the bounding box quad, adds 1px of padding to prevent clipping 
// during anti-aliasing, and then returns the clipspace coordinate of that vertex 
@vertex
fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VsOut {
    // vi = stroke * 4 + corner
    let s = strokes[vi >> 2u]; 
    let corner = vi & 3u;

    let pa = points[ii];
    let pb = points[ii + 1u];
    let a = vec2f(pa.x, pa.y) * s.k + s.offset;
    let b = vec2f(pb.x, pb.y) * s.k + s.offset;
    let ab = b - a;
    let len = length(ab);
    let dir = select(vec2f(1.0, 0.0), ab / len, len > 0.0);
    let n = vec2f(-dir.y, dir.x);

    // resolves to quad corners based on vi: (-1,-1) (1,-1) (-1, 1) (1, 1)
    let x = f32(corner & 1u) * 2.0 - 1.0;
    let y = f32(corner >> 1u) * 2.0 - 1.0;

    let pad = s.radius + 1.0;
    let p = select(a, b, x > 0.0) + dir * (x * pad) + n * (y * pad);

    let clip = p / u.viewport * 2.0 - 1.0;
    var o: VsOut;
    o.pos = vec4f(clip.x, -clip.y, 0.0, 1.0);
    o.a = vec3f(a, pa.p);
    o.b = vec3f(b, pb.p);
    o.color = s.color;
    o.radius = s.radius;
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
