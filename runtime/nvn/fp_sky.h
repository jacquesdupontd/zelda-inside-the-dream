// The first-person sky pass's pixel shader (runtime/hle/firstperson.c fp_sky_params), one source for both renderers:
// runtime/nvn/renderer.m compiles it as MSL, runtime/nvn/vulkan/exec.c as GLSL. Seen from above the island covers the
// whole screen, so the game never draws a sky; right after the scene's composite into the window image, the pixels
// where the scene's depth is still at its far plane get a sky and the island's ground (nothing of the game is invented:
// no castle, no mountains - Guillaume, 20/10), the edge of what the game draws fades into them; indoors a ceiling.
// FP_SKY_BODY is written in what the two languages share; each prelude defines the rest: float2..4 and uint4 (GLSL:
// vec2..4, uvec4), saturate, fmod, DEPTH (the scene depth at a screen position), INOUT3 (a float3 the function sets: MSL thread float3 &, GLSL
// inout vec3), KP / KA (the parameter block k: an argument in MSL, a uniform block in GLSL). Conditions are bools
// (GLSL takes no int as one); a pixel left alone comes back with alpha -1, discarded by the wrapper. The window image
// is stored upside down: its first row is the bottom of the screen.
// k.dsc.xy: the part of the scene depth texture its contents occupy (the dynamic resolution draws into the top-left part
// of screen-sized targets): read as a whole, the far plane was found in the empty part - a sky drawn into a box there,
// none where it belonged, the sky black (the merge of peiikai's beta_5, 21/10).
#pragma once

#define FP_SKY_BODY \
    "float sq(float v) { return v * v; }\n" \
    "float hash12(float2 p) { float3 p3 = fract(float3(p.x, p.y, p.x) * 0.1031); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.x + p3.y) * p3.z); }\n" \
    "float vnoise(float2 p) { float2 i = floor(p), f = fract(p); float2 u = f * f * (3.0 - 2.0 * f);\n" \
    "  return mix(mix(hash12(i), hash12(i + float2(1.0, 0.0)), u.x), mix(hash12(i + float2(0.0, 1.0)), hash12(i + float2(1.0, 1.0)), u.x), u.y); }\n" \
    "float fbm(float2 p) { float s = 0.0, a = 0.5; for (int i = 0; i < 5; i++) { s += a * vnoise(p); p = p * 2.03 + float2(17.0, 9.0); a *= 0.5; } return s; }\n" \
    "// masonry or boards: blocks of size (world units, along the surface), each its own tone, a joint between\n" \
    "float3 blocks(float2 uv, float2 size, float3 base) {\n" \
    "  float2 b = uv / size; float row = floor(b.y); b.x += (row - 2.0 * floor(row * 0.5)) * 0.5;\n" \
    "  float2 f = fract(b), id = floor(b);\n" \
    "  float edge = min(min(f.x, 1.0 - f.x) * size.x, min(f.y, 1.0 - f.y) * size.y);\n" \
    "  float tone = (0.7 + 0.6 * hash12(id)) * (0.8 + 0.4 * vnoise(uv * 3.0));\n" \
    "  return base * mix(0.3, tone, smoothstep(0.02, 0.07, edge)); }\n" \
    "// The south wall of a house, which the view from above never needed (Link comes in by it): a real plane 1.0 past\n" \
    "// the mat where he came in - planks from the floor up (one material: a stone base and a board under the planks read\n" \
    "// as two walls not holding together, Guillaume 20/10), the door cut in it, a dark wooden frame, the day outside. One\n" \
    "// wall for every house: mirroring each room's north wall stood its furniture by the door and, in the shop, its\n" \
    "// counter flickering through the wall (Guillaume, 20/10). What the game draws before the plane stays (dpix: how far\n" \
    "// the game's pixel is, 1e9 for none). b: door x, mat z, half width, height; cl: floor y, ceiling height.\n" \
    "#define ROOF_RISE 1.2\n" \
    "float4 south_wall(float3 eye, float3 w, float dpix, float4 b, float4 cl) {\n" \
    "  float zw = b.y + 1.0;\n" \
    "  if (b.z <= 0.0 || w.z < 1e-4 || eye.z >= zw) return float4(0.0, 0.0, 0.0, -1.0);\n" \
    "  float t = (zw - eye.z) / w.z;\n" \
    "  if (dpix < t - 0.05) return float4(0.0, 0.0, 0.0, -1.0);  // (something of the room stands before it)\n" \
    "  float3 h = eye + w * t;\n" \
    "  float y = h.y - cl.x, dx = abs(h.x - b.x), hw = b.z, dh = b.w, fr = 0.13;\n" \
    "  if (y < -0.05 || y > cl.y + ROOF_RISE * max(0.0, 1.0 - abs(h.x - b.x) / 7.5)) return float4(0.0, 0.0, 0.0, -1.0);  // (up to the roof)\n" \
    "  if (dx < hw && y < dh) {  // the opening: the day outside, the lit ground low, the sky above\n" \
    "    float3 day = mix(float3(0.95, 0.90, 0.72), float3(0.55, 0.74, 0.98), saturate((y - 0.3) / max(dh - 0.3, 0.1)));\n" \
    "    return float4(day, 1.0); }\n" \
    "  float3 col;\n" \
    "  if (dx < hw + fr && y < dh + fr) col = float3(0.20, 0.11, 0.05) * (0.7 + 0.4 * smoothstep(0.0, fr, min(hw + fr - dx, dh + fr - y)));\n" \
    "  else col = blocks(float2(h.x, y), float2(3.1, 0.34), float3(0.55, 0.36, 0.18));  // (planks)\n" \
    "  return float4(col * (0.45 + 0.55 * exp(-t * 0.06)), 1.0); }\n" \
    "// how far (along the view axis) the game's pixel at screen position q is, from its depth zq\n" \
    "float vdist(float2 q, float zq KP) { return k.g.w > 0.5 ? k.g.y / (zq + k.g.x) : k.g.y / (zq * 2.0 - 1.0 + k.g.x); }\n" \
    "float4 sky_px(float2 uv, float z0 KP) {\n" \
    "  if (k.o.w > 0.5) return float4(z0, fract(z0 * 64.0), uv.y, 1.0);  // (LARECOMP_FP_SKY_Z: the scene depth it reads)\n" \
    "  if (k.pal.w > 0.001) return float4(0.0, 0.0, 0.0, k.pal.w);  // (a fall into a hole: the dark, firstperson.c fall_fade)\n" \
    "  float2 ndc = uv * 2.0 - 1.0;\n" \
    "  float3 v = float3((ndc.x + k.o.x) / k.f.x, (ndc.y + k.o.y) / k.f.y, -1.0);  // (o: a headset eye's off-centre lens)\n" \
    "  float3 w = normalize(float3(dot(k.r0.xyz, v), dot(k.r1.xyz, v), dot(k.r2.xyz, v)));\n" \
    "  float3 zen = float3(0.10, 0.30, 0.78), hor = float3(0.46, 0.64, 0.88), sea = float3(0.02, 0.20, 0.36);  // (clear air, as Breath of the Wild's: no white)\n" \
    "  bool indoor = k.f.w > 0.5;\n" \
    "  if (indoor) { zen = float3(0.0008, 0.0007, 0.0009); hor = float3(0.006, 0.0045, 0.0045); sea = hor; }\n" \
    "  float3 eye = float3(k.r0.w, k.r1.w, k.r2.w);\n" \
    "  uint bits[8] = {k.m0.x, k.m0.y, k.m0.z, k.m0.w, k.m1.x, k.m1.y, k.m1.z, k.m1.w};  // (the cells drawn: 16 x 16 bits)\n" \
    "  #define CELL(cc, rr) ((cc) >= 0 && (cc) < 16 && (rr) >= 0 && (rr) < 16 && ((bits[((rr) * 16 + (cc)) >> 5] >> uint(((rr) * 16 + (cc)) & 31)) & 1u) != 0u)\n" \
    "  #define DRAWN(px, pz) CELL(int(floor((px) / 15.0)), int(floor((pz) / 12.0)))\n" \
    "  bool far_on = !indoor && k.b.w > 0.5;\n" \
    "  if (!indoor && z0 >= 0.99999 && abs(w.y) < 0.45) {  // an opening in something near - a door, a window, a hollow:\n" \
    "    // the game drew nothing inside, and the sky or the ground showed through (« des trous », Guillaume 20/10). Near\n" \
    "    // pixels on all four sides, all as far as one another - a lintel, a threshold, two jambs: the dark. (Above and\n" \
    "    // below only took the sky between a statue's head and the ring over it for a door: black saw teeth, 20/10)\n" \
    "    // (and an opening of some size: across, the sides at least two steps apart - a gap between leaves, near on all\n" \
    "    //  four sides at the first step, went black: black dots over the foliage at times, 20/10)\n" \
    "    float4 dn = float4(-1.0); float wide = 0.0;\n" \
    "    for (int sd = 0; sd < 4; sd++) {\n" \
    "      float2 st = sd == 0 ? float2(0.0, 0.03) : sd == 1 ? float2(0.0, -0.03) : sd == 2 ? float2(0.03, 0.0) : float2(-0.03, 0.0);\n" \
    "      for (int i = 1; i <= 6; i++) { float2 q = uv + st * float(i); if (q.x <= 0.0 || q.x >= 1.0 || q.y <= 0.0 || q.y >= 1.0) break;\n" \
    "        float zq = DEPTH(q); if (zq < 0.99999) { dn[sd] = vdist(q, zq KA); wide += float(i); break; } }\n" \
    "      if (dn[sd] < 0.0 || dn[sd] > 9.0) break;\n" \
    "    }\n" \
    "    if (wide >= 8.0 && min(min(dn.x, dn.y), min(dn.z, dn.w)) > 0.0 && max(max(dn.x, dn.y), max(dn.z, dn.w)) - min(min(dn.x, dn.y), min(dn.z, dn.w)) < 0.7)\n" \
    "      return float4(0.010, 0.009, 0.011, 1.0);\n" \
    "  }\n" \
    "  float zv = k.g.w > 0.5 ? -k.g.y / (z0 + k.g.x) : -k.g.y / (z0 * 2.0 - 1.0 + k.g.x);  // view-space z (negative)\n" \
    "  float3 pv = float3(v.xy * -zv, zv);\n" \
    "  float3 pw = float3(dot(k.r0.xyz, pv), dot(k.r1.xyz, pv), dot(k.r2.xyz, pv)) + float3(k.r0.w, k.r1.w, k.r2.w);\n" \
    "  if (k.o.z > 0.5) {  // the motion vector (ndc, this frame minus the last), as a colour: 0.5 grey = still\n" \
    "    float4 q = z0 >= 0.99999 ? float4(w, 0.0) : float4(pw, 1.0);  // (the sky: a direction, at infinity)\n" \
    "    float4 c1 = float4(dot(k.pv0, q), dot(k.pv1, q), dot(k.pv2, q), dot(k.pv3, q));\n" \
    "    float2 mv = c1.w > 0.0 ? ndc - c1.xy / c1.w : float2(0.0);\n" \
    "    return float4(saturate(0.5 + mv.x * 4.0), saturate(0.5 + mv.y * 4.0), 0.5, 1.0); }\n" \
    "  float haze = 1.0;\n" \
    "  if (z0 < 0.99999) {  // a pixel of the game: kept (out at once) unless it is at the edge of the cells drawn\n" \
    "  if (indoor) {  // a house: its south wall over what lies past it (south_wall), the rest left alone\n" \
    "    if (k.cl.w > 1.5) { float4 sw = south_wall(eye, w, length(pw - eye), k.b, k.cl); if (sw.w > 0.0) return sw; }\n" \
    "    return float4(0.0, 0.0, 0.0, -1.0);  // (nothing: the wrapper discards)\n" \
    "  }\n" \
    "  if (dot(pw - eye, pw - eye) < 900.0) return float4(0.0, 0.0, 0.0, -1.0);  // (nearer than 30: never veiled, out at once)\n" \
    "  int ci = int(floor(pw.x / 15.0)), ri = int(floor(pw.z / 12.0));\n" \
    "  float lx = pw.x - float(ci) * 15.0, lz = pw.z - float(ri) * 12.0, edge = 1e9;\n" \
    "  if ((k.m0.x | k.m0.y | k.m0.z | k.m0.w | k.m1.x | k.m1.y | k.m1.z | k.m1.w) == 0u) return float4(0.0, 0.0, 0.0, -1.0);  // (no cell drawn yet - loading, shaders compiling: no haze over everything)\n" \
    "  if (!CELL(ci, ri)) edge = 0.0;\n" \
    "  else {\n" \
    "    if (ci > 0 && !CELL(ci - 1, ri)) edge = min(edge, lx);  // (past the map's edge: the sea, nothing to fade to)\n" \
    "    if (ci < 15 && !CELL(ci + 1, ri)) edge = min(edge, 15.0 - lx);\n" \
    "    if (ri > 0 && !CELL(ci, ri - 1)) edge = min(edge, lz);\n" \
    "    if (ri < 15 && !CELL(ci, ri + 1)) edge = min(edge, 12.0 - lz);\n" \
    "  }\n" \
    "  haze = 1.0 - smoothstep(0.0, k.g.z, edge);\n" \
    "  // (only far off: what he has before him is never veiled - the sea, a model of its own over cells never\n" \
    "  //  drawn, went under a sand-coloured fog by the beach, Guillaume 20/10)\n" \
    "  haze *= smoothstep(30.0, 60.0, length(pw - eye));\n" \
    "  // (and only below the horizon, where the ground goes on into the island's far ground: a wall standing against the\n" \
    "  //  sky went sky-coloured, ghosts of stone over the forest - « des bouts de textures », Guillaume 20/10)\n" \
    "  haze *= 1.0 - smoothstep(-0.01, 0.03, w.y);\n" \
    "  if (haze <= 0.0) return float4(0.0, 0.0, 0.0, -1.0);\n" \
    "  }\n" \
    "  float3 c = w.y >= 0.0 ? mix(hor, zen, pow(saturate(w.y * 1.4), 0.6)) : mix(hor, sea, saturate(-w.y * 6.0));\n" \
    "  if (!indoor && w.y > 0.005) {  // clouds: a layer 140 above the island, lit from above\n" \
    "    float2 q = (eye.xz + w.xz * (140.0 / w.y)) * 0.0055;\n" \
    "    float cov = smoothstep(0.50, 0.80, fbm(q)) * smoothstep(0.005, 0.14, w.y);\n" \
    "    float3 cl = mix(float3(0.58, 0.63, 0.72), float3(1.0, 1.0, 0.98), smoothstep(0.45, 0.85, fbm(q * 1.9 + 3.7)));\n" \
    "    c = mix(c, cl, cov * 0.9);\n" \
    "  }\n" \
    "  if (far_on && w.y < -0.0005) {  // the island's ground past what the game draws (not the sea): grass, beaches at its edge\n" \
    "    float t = -1.2 / w.y;  // (at his feet's height: the island seen from where he stands)\n" \
    "    float3 g = eye + w * t;\n" \
    "    float shore = min(min(g.x, 240.0 - g.x), min(g.z, 192.0 - g.z));\n" \
    "    if (shore > 0.0) {  // meadows and woods (two noises), sand along the coast; a light veil with distance\n" \
    "      float n = fbm(g.xz * 0.045), m = fbm(g.xz * 0.23 + 7.0);\n" \
    "      float3 land = mix(mix(float3(0.15, 0.33, 0.06), float3(0.26, 0.45, 0.10), m), mix(float3(0.035, 0.13, 0.035), float3(0.07, 0.20, 0.05), m), smoothstep(0.54, 0.66, n));\n" \
    "      c = mix(mix(float3(0.66, 0.58, 0.36), land, smoothstep(3.0, 8.0, shore)), hor, saturate(t / 480.0) * 0.45);\n" \
    "    }\n" \
    "  }\n" \
    "  float3 sky = c;\n" \
    "  if (indoor && z0 >= 0.99999 && k.cl.w > 1.5 && k.cl.z < 0.5) { float4 sw = south_wall(eye, w, 1e9, k.b, k.cl); if (sw.w > 0.0) return sw; }\n" \
    "  if (indoor && k.cl.z > 0.5 && z0 >= 0.99999) {  // a ceiling over rooms the view from above never closed\n" \
    "    bool wood = k.cl.w > 1.5;  // (a house: plaster and planks; else stone)\n" \
    "    // a house's rooms stand on the game's grid (cells 15 x 12; the door in the middle of the south side): the south\n" \
    "    // wall and the roof stop half a cell out on each side and one cell further north - past that they ran on to the\n" \
    "    // horizon; at one cell exactly, the bigger houses (Papahl's) went black past it (Guillaume, 20/10)\n" \
    "    float cx0 = floor(k.b.x / 15.0) * 15.0 - 7.5, cz0 = floor(k.b.y / 12.0) * 12.0 - 12.0;\n" \
    "    if (wood && k.b.z > 0.0 && w.z > 1e-4) {\n" \
    "      float4 sw = south_wall(eye, w, 1e9, k.b, k.cl);\n" \
    "      if (sw.w > 0.0) {\n" \
    "        float hx = eye.x + w.x * (k.b.y + 1.0 - eye.z) / w.z;\n" \
    "        return hx > cx0 && hx < cx0 + 30.0 ? sw : float4(0.010, 0.009, 0.011, 1.0);\n" \
    "      }\n" \
    "    }\n" \
    "    float top = k.cl.x + k.cl.y, dist = 1e9;\n" \
    "    float3 col = k.pal.xyz;\n" \
    "    float tc = w.y > 0.001 ? (top - eye.y) / w.y : 1e9;\n" \
    "    if (wood) {  // a house: a pitched roof, on the walls' tops at the sides and ROOF_RISE higher along the middle\n" \
    "      // (flat on the tops it was too low, « un peu trop bas », and higher it left a gap over them - Guillaume, 20/10)\n" \
    "      float s = ROOF_RISE / 7.5, mid = k.b.x, r = top + ROOF_RISE;\n" \
    "      float t1 = (r - s * mid + s * eye.x - eye.y) / (w.y - s * w.x), t2 = (r + s * mid - s * eye.x - eye.y) / (w.y + s * w.x);\n" \
    "      float x1 = eye.x + w.x * t1, x2 = eye.x + w.x * t2;\n" \
    "      float tf = tc, xf = eye.x + w.x * tf;  // (past 7.5 from the ridge, flat at the eaves: the wider houses)\n" \
    "      tc = 1e9;\n" \
    "      if (t1 > 0.0 && x1 <= mid && x1 >= mid - 7.5) tc = t1;\n" \
    "      if (t2 > 0.0 && x2 >= mid && x2 <= mid + 7.5) tc = min(tc, t2);\n" \
    "      if (tf < 1e8 && abs(xf - mid) > 7.5) tc = min(tc, tf);\n" \
    "    }\n" \
    "    if (wood && tc < 1e8) {\n" \
    "      float3 pc = eye + w * tc;\n" \
    "      if (pc.x < cx0 || pc.x > cx0 + 30.0 || pc.z < cz0 || pc.z > k.b.y + 1.0) return float4(0.010, 0.009, 0.011, 1.0);\n" \
    "    }\n" \
    "    // (the walls are not carried up, nor darkened above their tops: a texture carried up swam - « la texture de mur en\n" \
    "    //  infini » - and the dark drew their jagged tops as a black pagoda, a saw-toothed column in caves; Guillaume\n" \
    "    //  20/10. The ceiling goes on behind them)\n" \
    "    if (dist > 1e8 && tc < 1e8) {  // the ceiling: slabs in caves and dungeons, boards in houses\n" \
    "      float3 p = eye + w * tc;\n" \
    "      col = blocks(p.xz, wood ? float2(0.45, 4.0) : float2(1.8, 1.8), wood ? float3(0.26, 0.14, 0.06) : k.pal.xyz * 0.7);\n" \
    "      dist = tc;\n" \
    "    }\n" \
    "    if (dist < 1e8) return float4(col * (0.35 + 0.65 * exp(-dist * 0.06)), 1.0);  // (darker away from him: a torch's reach)\n" \
    "  }\n" \
    "  if (z0 >= 0.99999) return float4(sky, 1.0);\n" \
    "  return float4(c, haze); }\n"

// MSL (runtime/nvn/renderer.m): a full-screen triangle, the scene depth at texture 0, k at buffer 0.
#define FP_SKY_MSL \
    "#include <metal_stdlib>\nusing namespace metal;\n" \
    "struct V { float4 pos [[position]]; float2 uv; };\n" \
    "vertex V vs(uint vid [[vertex_id]]) {\n" \
    "  float2 p = float2((vid << 1) & 2, vid & 2); V o; o.pos = float4(p * 2.0 - 1.0, 0, 1); o.uv = float2(p.x, 1.0 - p.y);\n" \
    "  return o; }\n" \
    "struct K { float4 r0, r1, r2, f, g, b; uint4 m0, m1; float4 o, pv0, pv1, pv2, pv3, cl, pal, dsc; };\n" \
    "#define INOUT3 thread float3 &\n#define KP , constant K &k, depth2d<float> d, sampler s\n#define KA , k, d, s\n" \
    "#define DEPTH(q) d.sample(s, float2((q).x, fmod(k.f.z, 2.0) > 0.5 ? 1.0 - (q).y : (q).y) * k.dsc.xy)\n" \
    FP_SKY_BODY \
    "fragment float4 fs(V in [[stage_in]], depth2d<float> d [[texture(0)]], sampler s [[sampler(0)]],\n" \
    "                   constant K &k [[buffer(0)]]) {\n" \
    "  float2 duv = float2(in.uv.x, fmod(k.f.z, 2.0) > 0.5 ? 1.0 - in.uv.y : in.uv.y) * k.dsc.xy;  // (k.f.z: 1 = depth upside down, +2 = test colours)\n" \
    "  float4 r = sky_px(in.uv, d.sample(s, duv) KA);\n" \
    "  if (r.w < 0.0) discard_fragment();\n" \
    "  return r; }\n"

// GLSL (runtime/nvn/vulkan/exec.c): the scene depth at binding 0, k at binding 1, 1 / the pass's size pushed.
#define FP_SKY_GLSL_VS \
    "#version 450\n" \
    "void main() { vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n"
#define FP_SKY_GLSL_FS \
    "#version 450\n" \
    "#define float2 vec2\n#define float3 vec3\n#define float4 vec4\n#define uint4 uvec4\n" \
    "#define saturate(x) clamp((x), 0.0, 1.0)\n#define fmod mod\n" \
    "#define INOUT3 inout vec3\n#define KP\n#define KA\n" \
    "layout(set = 0, binding = 0) uniform sampler2D dtex;\n" \
    "layout(set = 0, binding = 1, std140) uniform KB { vec4 r0, r1, r2, f, g, b; uvec4 m0, m1; vec4 o, pv0, pv1, pv2, pv3, cl, pal, dsc; } k;\n" \
    "#define DEPTH(q) texture(dtex, vec2((q).x, mod(k.f.z, 2.0) > 0.5 ? 1.0 - (q).y : (q).y) * k.dsc.xy).r\n" \
    "layout(push_constant) uniform PC { vec2 inv; } pc;\n" \
    "layout(location = 0) out vec4 fp_out;\n" \
    FP_SKY_BODY \
    "void main() {\n" \
    "  vec2 uv = gl_FragCoord.xy * pc.inv;  // (the pass's rows as stored: the first is the bottom of the screen, as in MSL)\n" \
    "  vec2 duv = vec2(uv.x, mod(k.f.z, 2.0) > 0.5 ? 1.0 - uv.y : uv.y) * k.dsc.xy;\n" \
    "  vec4 r = sky_px(uv, texture(dtex, duv).r);\n" \
    "  if (r.w < 0.0) discard;\n" \
    "  fp_out = r; }\n"
