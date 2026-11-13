// GLSL -> SPIR-V (shaderc), descriptor layouts from the SPIR-V itself, translated NVN programs, built-in shaders.
#include <pthread.h>
#include <shaderc/shaderc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "shaders.h"
#include "xr_depth_ray.h"
#include "../../../third_party/sgsr2/shaders.inc"
#include "../screen_filter.h"

void vk_shaders_init(void) {  // (compilers are per thread: check shaderc works at all)
    shaderc_compiler_t c = shaderc_compiler_initialize();
    if (!c) FATAL("shaderc_compiler_initialize failed\n");
    shaderc_compiler_release(c);
}

static shaderc_shader_kind kind_of(int stage) {
    return stage == STAGE_VERTEX ? shaderc_vertex_shader : stage == STAGE_FRAGMENT ? shaderc_fragment_shader : shaderc_compute_shader;
}

// SPIR-V disk cache: cache/spirv/<hash of stage + source>.spv, so a known shader costs a file read instead of a
// GLSL compile (the driver's pipeline cache only covers the step after this one).
static uint64_t src_hash(const char *src, int stage) {  // (beta 6: the optimised ones apart, vk_glsl_to_spirv)
    uint64_t h = (0xcbf29ce484222325ull ^ (uint64_t)stage) + 4;  // + version: no_contraction, and our built-ins (FP / XR)
    for (const char *p = src; *p; p++) { h ^= (uint8_t)*p; h *= 0x100000001B3ull; }
    return h;
}

static uint32_t *cache_load(uint64_t h, size_t *nwords) {
    char path[64];
    snprintf(path, sizeof path, "cache/spirv/%016llx.spv", (unsigned long long)h);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *d = n > 20 && n % 4 == 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (d && d[0] != 0x07230203) { free(d); d = NULL; }
    if (d) *nwords = (size_t)n / 4;
    return d;
}

static void cache_store(uint64_t h, const uint32_t *spv, size_t nwords) {
    static int made;
    if (!made) { made = 1; mkdir("cache", 0755); mkdir("cache/spirv", 0755); }
    char path[64], tmp[80];
    snprintf(path, sizeof path, "cache/spirv/%016llx.spv", (unsigned long long)h);
    snprintf(tmp, sizeof tmp, "%s.%llx", path, (unsigned long long)(uintptr_t)&made ^ (unsigned long long)(uintptr_t)spv);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(spv, 4, nwords, f) == nwords;
    fclose(f);
    if (!ok || rename(tmp, path) != 0) remove(tmp);  // (another thread may have stored it first)
}

// Vertex shaders: every float add/sub/mul/div/fma gets NoContraction, so no compiler fuses or splits them. Depth
// pre-pass and colour pass are different shaders computing the same position, and the colour pass tests EQUAL:
// any difference in fusing leaves holes (foliage flickered black). GLSL's `precise gl_Position` only covers what
// glslang traces back from the position, which the translated register code defeats.
static bool float_arith(const uint32_t *w, uint32_t wc, uint32_t op) {
    switch (op) {
    case 127: case 129: case 131: case 133: case 136:  // FNegate FAdd FSub FMul FDiv
    case 142: case 143: case 144: case 145: case 146: case 148:  // vector/matrix products, Dot
        return true;
    case 12: return wc >= 5 && w[4] == 50;  // ExtInst GLSL.std.450 Fma
    default: return false;
    }
}

static uint32_t *no_contraction(uint32_t *spv, size_t *nwords) {
    enum { OP_DECORATE = 71, DEC_NO_CONTRACTION = 42 };
    size_t n = *nwords, insert = 0, count = 0;
    uint32_t bound = spv[3];
    uint8_t *done = calloc(bound, 1);  // already decorated (glslang does it for what it traces from precise)
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (!insert && op >= 19 && op <= 39) insert = i;  // first type declaration: end of the annotations
        if (op == OP_DECORATE && wc >= 3 && spv[i + 2] == DEC_NO_CONTRACTION && spv[i + 1] < bound) done[spv[i + 1]] = 1;
        i += wc;
    }
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) count++;
        i += wc;
    }
    if (!insert || !count) { free(done); return spv; }
    uint32_t *out = malloc((n + count * 3) * 4);
    memcpy(out, spv, insert * 4);
    size_t o = insert;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) {
            done[spv[i + 2]] = 1;
            out[o++] = 3u << 16 | OP_DECORATE;
            out[o++] = spv[i + 2];  // result id
            out[o++] = DEC_NO_CONTRACTION;
        }
        i += wc;
    }
    memcpy(out + o, spv + insert, (n - insert) * 4);
    *nwords = o + (n - insert);
    free(done);
    free(spv);
    return out;
}

// SPIR-V words of src, or NULL (error logged with the source). *nwords set on success. Thread-safe: one shaderc
// compiler per thread (a shared one behind a lock serialised all the pipeline workers).
// No spirv-opt by default: it rewrote the depth pre-pass and colour-pass vertex shaders differently enough that
// their depths stopped matching (EQUAL test: black holes in foliage), and it was most of the compile time; the
// driver optimises anyway. LARECOMP_VK_OPT=1 turns it back on (experiments).
// Mali (ARM's driver): fragment and compute shaders are run through spirv-opt (performance). The translated pc-loop
// shaders (a switch on a guest pc inside a loop, every guest register a function-scope variable, the SSY / PBK
// stacks as dynamically indexed arrays) compiled at -O0 are pathological for Mali's compiler: the light-list loop
// of the interiors' main pass (Marin & Tarin's house: one 1280x720 pass, 48 draws) took ~400-800 ms of GPU a frame
// on a Mali-G610 (1.5 fps), 30 fps with it optimised. Vertex shaders stay -O0 (the depth pre-pass / EQUAL reason
// above). LARECOMP_VK_OPT_FS=0/1 forces it off / on for any GPU; the optimised SPIR-V has its own cache entries.
static bool opt_stage(int stage) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LARECOMP_VK_OPT_FS");
        on = e ? atoi(e) != 0 : g_vk.props.vendorID == 0x13B5;  // (ARM)
        if (on) LOG("[vk] fragment / compute shaders optimised (spirv-opt)%s\n", e ? " (LARECOMP_VK_OPT_FS)" : ": Mali");
    }
    return getenv("LARECOMP_VK_OPT") || (on && stage != STAGE_VERTEX);
}

uint32_t *vk_glsl_to_spirv(const char *src, int stage, const char *name, size_t *nwords) {
    bool opt = opt_stage(stage);
    uint64_t h = src_hash(src, stage) ^ (opt ? 0x9e3779b97f4a7c15ull : 0);
    static int nocache = -1;
    if (nocache < 0) nocache = getenv("LARECOMP_VK_NO_SPIRV_CACHE") != NULL;
    uint32_t *out = nocache ? NULL : cache_load(h, nwords);
    if (out) return out;
    static __thread shaderc_compiler_t comp;
    if (!comp) comp = shaderc_compiler_initialize();
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_optimization_level(o, opt ? shaderc_optimization_level_performance
                                                                                  : shaderc_optimization_level_zero);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(comp, src, strlen(src), kind_of(stage), name, "main", o);
    shaderc_compile_options_release(o);
    if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
        static int nlog;
        if (nlog++ < 20) LOG("[shader] %s: GLSL compile failed:\n%s\n%s\n", name, shaderc_result_get_error_message(r), src);
    } else {
        size_t n = shaderc_result_get_length(r);
        out = malloc(n);
        memcpy(out, shaderc_result_get_bytes(r), n);
        *nwords = n / 4;
        if (stage == STAGE_VERTEX) out = no_contraction(out, nwords);
        if (!nocache) cache_store(h, out, *nwords);
    }
    shaderc_result_release(r);
    return out;
}

VkShaderModule vk_module(const uint32_t *spv, size_t nwords) {
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nwords * 4, .pCode = spv};
    VkShaderModule m;
    return vkCreateShaderModule(g_vk.dev, &ci, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

VkShaderModule vk_compile_glsl(const char *src, int stage, const char *name) {
    size_t n;
    uint32_t *spv = vk_glsl_to_spirv(src, stage, name, &n);
    if (!spv) return VK_NULL_HANDLE;
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    return m;
}

// ================================================================== reflection
// Just enough SPIR-V parsing to list the descriptor bindings a module declares (set, binding, type).
enum { OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLER = 26, OP_TYPE_SAMPLED_IMAGE = 27, OP_TYPE_ARRAY = 28, OP_TYPE_RUNTIME_ARRAY = 29,
       OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_VARIABLE = 59, OP_DECORATE = 71 };
enum { DEC_BLOCK = 2, DEC_BUFFER_BLOCK = 3, DEC_BINDING = 33, DEC_SET = 34 };
enum { SC_UNIFORM_CONSTANT = 0, SC_UNIFORM = 2, SC_STORAGE_BUFFER = 12 };

int vk_reflect(const uint32_t *spv, size_t n, VkBindingInfo *out, int max) {
    if (n < 5 || spv[0] != 0x07230203) return 0;
    uint32_t bound = spv[3];
    int32_t *binding = malloc(bound * sizeof *binding), *set = malloc(bound * sizeof *set);
    uint8_t *flags = calloc(bound, 1);    // 1 Block, 2 BufferBlock
    uint32_t *op = calloc(bound, sizeof *op), *ref = calloc(bound, sizeof *ref), *sc = calloc(bound, sizeof *sc);
    for (uint32_t i = 0; i < bound; i++) binding[i] = set[i] = -1;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, o = spv[i] & 0xFFFF;
        if (!wc) break;
        const uint32_t *w = spv + i;
        if (o == OP_DECORATE && wc >= 3 && w[1] < bound) {
            if (w[2] == DEC_BINDING && wc >= 4) binding[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_SET && wc >= 4) set[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_BLOCK) flags[w[1]] |= 1;
            else if (w[2] == DEC_BUFFER_BLOCK) flags[w[1]] |= 2;
        } else if ((o == OP_TYPE_IMAGE || o == OP_TYPE_SAMPLER || o == OP_TYPE_SAMPLED_IMAGE || o == OP_TYPE_STRUCT) && wc >= 2 && w[1] < bound) {
            op[w[1]] = o;
        } else if ((o == OP_TYPE_ARRAY || o == OP_TYPE_RUNTIME_ARRAY) && wc >= 3 && w[1] < bound) {
            op[w[1]] = o;
            ref[w[1]] = w[2];
        } else if (o == OP_TYPE_POINTER && wc >= 4 && w[1] < bound) {
            op[w[1]] = o;
            sc[w[1]] = w[2];
            ref[w[1]] = w[3];
        } else if (o == OP_VARIABLE && wc >= 4 && w[2] < bound && w[1] < bound) {
            op[w[2]] = o;
            ref[w[2]] = w[1];  // pointer type
            sc[w[2]] = w[3];
        }
        i += wc;
    }
    int cnt = 0;
    for (uint32_t id = 0; id < bound && cnt < max; id++) {
        if (op[id] != OP_VARIABLE || binding[id] < 0) continue;
        uint32_t t = ref[ref[id]];  // pointee
        while (t < bound && (op[t] == OP_TYPE_ARRAY || op[t] == OP_TYPE_RUNTIME_ARRAY)) t = ref[t];
        VkDescriptorType dt;
        if (t >= bound) continue;
        if (op[t] == OP_TYPE_SAMPLED_IMAGE) dt = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        else if (op[t] == OP_TYPE_IMAGE) dt = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        else if (op[t] == OP_TYPE_SAMPLER) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
        else if (op[t] == OP_TYPE_STRUCT && (sc[id] == SC_STORAGE_BUFFER || (flags[t] & 2))) dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (op[t] == OP_TYPE_STRUCT && sc[id] == SC_UNIFORM) dt = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else continue;
        out[cnt++] = (VkBindingInfo){(uint32_t)(set[id] < 0 ? 0 : set[id]), (uint32_t)binding[id], dt};
    }
    free(binding); free(set); free(flags); free(op); free(ref); free(sc);
    return cnt;
}

VkDescriptorSetLayout vk_set_layout(const VkBindingInfo *b, int n, uint32_t set, VkShaderStageFlags stages) {
    VkDescriptorSetLayoutBinding lb[96];
    uint32_t k = 0;
    for (int i = 0; i < n && k < 96; i++)
        if (b[i].set == set) lb[k++] = (VkDescriptorSetLayoutBinding){b[i].binding, b[i].type, 1, stages, NULL};
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = k, .pBindings = lb};
    VkDescriptorSetLayout l;
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

// ================================================================== built-in shaders
static const char *builtin_src(const char *name, int *stage) {
    if (!strcmp(name, "sgsr2_clear.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\nlayout(location=0) out vec4 o; void main(){o=vec4(0);}\n";
    }
    if (!strcmp(name, "sgsr2_upscale_hq.frag")) {
        *stage = STAGE_FRAGMENT;
        return sgsr2_upscale_hq_src;
    }
    if (!strcmp(name, "sgsr2_convert.frag") || !strcmp(name, "sgsr2_upscale.frag")) {
        *stage = STAGE_FRAGMENT;
        return !strcmp(name, "sgsr2_convert.frag") ? sgsr2_convert_src : sgsr2_upscale_src;
    }
    if (!strcmp(name, "present.vert")) {
        *stage = STAGE_VERTEX;
        // pv[0].x: flip (lower-left origin), pv[0].y: the swapchain's pre-rotation in quarter turns (device.c
        // vk_sc_rect: 1 -> (-y, x), 3 -> (y, -x)), pv[1]: uv crop (origin, size). Fullscreen triangle; Vulkan NDC y
        // points down.
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 a; vec4 crop; } pc;\n"
               "layout(location = 0) out vec2 uv;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  vec2 c = p * 2.0 - 1.0;\n"
               "  if (pc.a.y > 0.5) c = pc.a.y < 2.0 ? vec2(-c.y, c.x) : vec2(c.y, -c.x);\n"
               "  gl_Position = vec4(c, 0.0, 1.0);\n"
               "  vec2 u = vec2(p.x, pc.a.x > 0.5 ? 1.0 - p.y : p.y);\n"
               "  uv = pc.crop.xy + u * pc.crop.zw;\n"
               "}\n";
    }
    // present.frag with FSR 1's RCAS sharpen folded in: when the present draws EASU's output 1:1 (the whole image, its
    // size in the window), the sharpen reads EASU's image here instead of a pass of its own - one full-window image less
    // written and read back. Same math as rcas.frag (sharpness 0.2 stops).
    if (!strcmp(name, "present_rcas.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform mediump sampler2D t;\n"
               "layout(push_constant) uniform F { layout(offset = 32) vec4 pf; };\n"  // (the screen filter)
               "layout(location = 0) in vec2 uv;\n"
               "layout(location = 0) out vec4 o;\n"
               "ivec2 sz;\n"
               "mediump vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), sz - 1), 0).rgb; }\n"
               "mediump float lum(mediump vec3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
               "void main() {\n"
               "  sz = textureSize(t, 0);\n"
               "  ivec2 ip = ivec2(floor(uv * vec2(sz)));\n"
               "  mediump vec3 b = tap(ip + ivec2(0, -1)), d = tap(ip + ivec2(-1, 0)), e = tap(ip), f = tap(ip + ivec2(1, 0)), h = tap(ip + ivec2(0, 1));\n"
               "  mediump float bL = lum(b), dL = lum(d), eL = lum(e), fL = lum(f), hL = lum(h);\n"
               "  float nz = 0.25 * (bL + dL + fL + hL) - eL;\n"
               "  float rng = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);\n"
               "  nz = 1.0 - 0.5 * clamp(abs(nz) / max(rng, 1.0 / 65536.0), 0.0, 1.0);\n"
               "  vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
               "  vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0 / 65536.0));\n"
               "  vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));\n"
               "  vec3 lobeRGB = max(-hitMin, hitMax);\n"
               "  float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * exp2(-0.2) * nz;\n"
               "  vec3 c = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);\n"
               SCREEN_FILTER_SRC("vec3")
               "  o = vec4(c, 1.0);\n"
               "}\n";
    }
    if (!strcmp(name, "present.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform F { layout(offset = 32) vec4 pf; };\n"  // (the screen filter)
               "layout(location = 0) in vec2 uv;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec3 c = texture(t, uv).rgb;\n"
               SCREEN_FILTER_SRC("vec3")
               "  o = vec4(c, 1.0);\n"
               "}\n";
    }
    // ---- decoded video, NV12 -> RGBA (exec.c vk_convert_nv12): limited range BT.601, as swscale did (the movies
    // declare no matrix). Fullscreen triangle; Vulkan NDC y points down, so uv = p.
    if (!strcmp(name, "nv12.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "layout(location = 0) out vec2 uv;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "  uv = p;\n"
               "}\n";
    }
    if (!strcmp(name, "nv12.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D ty;\n"
               "layout(set = 0, binding = 1) uniform sampler2D tuv;\n"
               "layout(location = 0) in vec2 uv;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  float y = (texture(ty, uv).r - 16.0 / 255.0) * (255.0 / 219.0);\n"
               "  vec2 c = (texture(tuv, uv).rg - 128.0 / 255.0) * (255.0 / 224.0);\n"
               "  o = vec4(clamp(vec3(y + 1.402 * c.y, y - 0.344136 * c.x - 0.714136 * c.y, y + 1.772 * c.x), 0.0, 1.0), 1.0);\n"
               "}\n";
    }
    // A texture copy by a draw (exec.c copy_by_draw): the destination rectangle's pixel -> its place in the source
    // rectangle (a: the rectangle's corner and 1 / its size; b: the source's corner and size, in its level's UV)
    if (!strcmp(name, "copy_scaled.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D src;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() { o = textureLod(src, pc.b.xy + (gl_FragCoord.xy - pc.a.xy) * pc.a.zw * pc.b.zw, 0.0); }\n";
    }
    // ---- post passes before the present (Video tab "Anti-aliasing" / "Upscaler"): exec.c vk_present_draw.
    // Each draws a fullscreen triangle into an image the size of its output; push constants: two vec4.
    // Depth copy that upsamples (exec.c depth_upsample_draw): bilinear depth from the source region. a.xy = the
    // destination rectangle's origin, a.zw = source texels per destination pixel; b.xy / b.zw = the source region's
    // first / last texel. Texel fetches: depth formats need no linear-filter support this way.
    if (!strcmp(name, "depthup.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } p;\n"
               "void main() {\n"
               "  vec2 s = p.b.xy + (gl_FragCoord.xy - p.a.xy) * p.a.zw - 0.5;\n"
               "  vec2 fl = floor(s);\n"
               "  vec2 f = s - fl;\n"
               "  ivec2 lo = ivec2(p.b.xy), hi = ivec2(p.b.zw);\n"
               "  ivec2 i0 = clamp(ivec2(fl), lo, hi), i1 = clamp(ivec2(fl) + ivec2(1), lo, hi);\n"
               "  float d00 = texelFetch(t, i0, 0).r;\n"
               "  float d10 = texelFetch(t, ivec2(i1.x, i0.y), 0).r;\n"
               "  float d01 = texelFetch(t, ivec2(i0.x, i1.y), 0).r;\n"
               "  float d11 = texelFetch(t, i1, 0).r;\n"
               "  gl_FragDepth = mix(mix(d00, d10, f.x), mix(d01, d11, f.x), f.y);\n"
               "}\n";
    }
    if (!strcmp(name, "post.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "}\n";
    }
    // A headset's eye made from the frame (android/xr_app.c LARECOMP_XR_STEREO=2, exec.c vk_xr_eye_warp): each output
    // pixel's source in the frame for a point at infinity (a: x = a.x + a.y * px, y = a.z + a.w * py), moved by the
    // eye's offset over the distance of what is there (b.x: the shift in x per 1 / distance). The first surface along
    // the eye's ray is searched from near (1 / distance 5: 0.2 game unit, half a metre - nearer, Link's own hands and
    // shield are farther) to far in 12 steps, then refined twice (24 steps and 3 refinements: 7.6 ms of the Quest's GPU
    // with the copy, 21/10). The scene's depth at binding 1, read through the present's crop (c) and flip (b.w & 4); distance =
    // b.z / (depth + b.y), depth 0..1 (b.w & 1) or -1..1. b.w & 2: the depth drawn instead (test).
    // A maximum inverse depth per tile (two reductions by eight). Each UNORM8
    // result is rounded UP with an extra quantum: it may include extra geometry,
    // but cannot underestimate a surface along a ray. The warp skips only sample
    // positions above this bound, keeping the original search grid and bracket.
    // Mode 2 retains every row, avoiding foreground on neighbouring rows.
    if (!strcmp(name, "xr_depth_bound.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set=0,binding=0) uniform sampler2D dep;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location=0) out vec4 o;\n"
               "void main() {\n"
               " int bh=int(pc.b.x); ivec2 sz=textureSize(dep,0), p=ivec2(gl_FragCoord.xy)*ivec2(8,bh); float hi=0.0;\n"
               " for(int y=0;y<bh;y++) for(int x=0;x<8;x++) {\n"
               "  float z=texelFetch(dep,min(p+ivec2(x,y),sz-1),0).r;\n"
               "  if(pc.a.x<0.5) { if(pc.a.w<0.5) z=z*2.0-1.0; z=clamp((z+pc.a.y)/pc.a.z,0.0,5.0)/5.0; }\n"
               "  hi=max(hi,z);\n"
               " }\n"
               " o=vec4(min(1.0,(ceil(hi*255.0)+1.0)/255.0),0,0,1);\n"
               "}\n";
    }
    if (!strcmp(name, "xr_warp.frag")) {
        *stage = STAGE_FRAGMENT;
        return
               "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D col;\n"
               "layout(set = 0, binding = 1) uniform sampler2D dep;\n"
               "layout(set = 0, binding = 2) uniform sampler2D bounds;\n"
               "layout(set = 0, binding = 3) uniform sampler2D hcol;\n"  // (what he holds in a layer of its own, pc.b.w 256)
               "layout(set = 0, binding = 4) uniform sampler2D hdep;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; vec4 d; vec4 e; vec4 f; vec4 g; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "#define XR_RAY_BOUNDS\n"
               "#define XR_HELD_LAYER\n"
               XR_DEPTH_RAY_GLSL
               "float rawz(float u, float v) { int f = int(pc.b.w); return texture(dep, pc.c.xy + vec2(u, (f & 4) != 0 ? 1.0 - v : v) * pc.c.zw).r; }\n"
               // what he holds: drawn into the first d.w of the depth range (exec.c FP_HELD_Z) - its own depth, scaled:
               // its inverse distance found again (-1 elsewhere); held_t: its surface along the eye's ray, from its own
               // depth alone (nearest 1/12 of a unit, to 2 units), 0 if none - each eye sees it where it is
               "bool split() { return (int(pc.b.w) & 256) != 0; }\n"
               "vec2 tq(float u, float v) { int f = int(pc.b.w); return pc.c.xy + vec2(u, (f & 4) != 0 ? 1.0 - v : v) * pc.c.zw; }\n"
               "float rawh(float u, float v) { return split() ? texture(hdep, tq(u, v)).r : rawz(u, v); }\n"
               "vec3 tmap(vec3 c) {\n"  // (its layer is the scene target's: linear there for houses and dungeons)
               "  if ((int(pc.b.w) & 512) == 0) return c;\n"
               // (linear: the game's tonemap, read at its draw - exec.c g_tm - then sRGB as the window's bytes)
               "  if (pc.e.w < 0.5) return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));\n"
               "  vec3 x = max(c * pc.e.x - 0.004, 0.0);\n"
               "  vec3 l = pow(clamp(x * (pc.e.y * x + 0.5) / (x * (pc.e.y * x + 1.7) + pc.e.z), 0.0, 1.0), vec3(2.2));\n"
               "  return mix(l * 12.92, 1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), l));\n"
               "}\n"
               "vec3 heldc(float u, float v) { return split() ? tmap(texture(hcol, tq(u, v)).rgb) : texture(col, vec2(u, v)).rgb; }\n"
               // an eye each (pc.b.w 2048, exec.c draw_): this eye's half (f.x), as it is - its four nearest texels
               // weighed as a bilinear look would, those of his only (depth below d.x, a colour drawn): its colour and
               // how much of the pixel it covers
               "vec4 held_eye(float u, float v) {\n"
               "  vec2 q = tq(u, v), sz = vec2(textureSize(hdep, 0));\n"
               "  float hw = sz.x * 0.5, x0 = pc.f.x * hw;\n"
               "  vec2 p = vec2(x0 + q.x * hw, q.y * sz.y) - 0.5, b = floor(p), w = p - b;\n"
               "  float a = 0.0; vec3 c = vec3(0.0);\n"
               "  for (int j = 0; j < 4; j++) {\n"
               "    vec2 d = vec2(float(j & 1), float(j >> 1));\n"
               "    ivec2 i = ivec2(clamp(b.x + d.x, x0, x0 + hw - 1.0), clamp(b.y + d.y, 0.0, sz.y - 1.0));\n"
               "    float wt = mix(1.0 - w.x, w.x, d.x) * mix(1.0 - w.y, w.y, d.y);\n"
               "    vec3 cc = texelFetch(hcol, i, 0).rgb;\n"
               "    if (texelFetch(hdep, i, 0).r < pc.d.x && dot(cc, vec3(1.0)) > 0.0) { a += wt; c += wt * cc; }\n"
               "  }\n"
               "  return a > 0.0 ? vec4(tmap(c / a), a) : vec4(0.0);\n"
               "}\n"
               "float idh(float u, float v) {\n"
               "  float z = rawh(u, v);\n"
               "  if (z >= pc.d.x) return -1.0;\n"
               "  z /= pc.d.w;\n"
               "  if ((int(pc.b.w) & 1) == 0) z = z * 2.0 - 1.0;\n"
               "  return max((z + pc.b.y) / pc.b.z, 0.0);\n"
               "}\n"
               "float held_t(float u0, float v, float K) {\n"
               "  const float TH = 12.0, TL = 0.25; const int N = 40;\n"  // (to 4 units: his body from behind is ~1.9 away)
               "  int f = int(pc.b.w);\n"
               "  if (split()) {\n"  // (its own layer: 16 looks along the ray's span - the bounds are the scene's)
               "    bool any = false;\n"
               "    for (int i = 0; i <= 15 && !any; i++) any = rawh(u0 + K * (TL + (TH - TL) * float(i) / 15.0), v) < pc.d.x;\n"
               "    if (!any) return 0.0;\n"
               "  } else if ((f & 8) != 0) {\n"  // (none on the ray's span of the depth's bounds - every held pixel's above bt there)
               "    vec2 sz = vec2(textureSize(dep, 0));\n"
               "    float yy = pc.c.y + ((f & 4) != 0 ? 1.0 - v : v) * pc.c.w;\n"
               "    int row = int(clamp(floor(yy * sz.y), 0.0, sz.y - 1.0)) / ((f & 32) != 0 ? 1 : 64);\n"
               "    float xa = (pc.c.x + (u0 + K * TL) * pc.c.z) * sz.x, xb = (pc.c.x + (u0 + K * TH) * pc.c.z) * sz.x;\n"
               "    int lo = int(clamp(floor(min(xa, xb)), 0.0, sz.x - 1.0)) / 64, hi = int(clamp(floor(max(xa, xb)), 0.0, sz.x - 1.0)) / 64;\n"
               "    float zt = (f & 1) == 0 ? pc.d.x * 2.0 - 1.0 : pc.d.x, bt = clamp((zt + pc.b.y) / pc.b.z, 0.0, 5.0) / 5.0, bound = 0.0;\n"
               "    for (int x = lo; x <= hi; x++) bound = max(bound, texelFetch(bounds, ivec2(x, row), 0).r);\n"
               "    if (bound < bt) return 0.0;\n"
               "  }\n"
               "  float tp = TH, dp = idh(u0 + K * tp, v) - tp;\n"
               "  if (dp >= 0.0) return tp;\n"
               "  for (int i = 1; i <= N; i++) {\n"
               "    float tc = TH - (TH - TL) * float(i) / float(N), dc = idh(u0 + K * tc, v) - tc;\n"
               "    if (dc >= 0.0) {\n"
               "      for (int j = 0; j < 5; j++) { float tm = 0.5 * (tp + tc), dm = idh(u0 + K * tm, v) - tm; if (dm >= 0.0) tc = tm; else tp = tm; }\n"
               "      return tc;\n"
               "    }\n"
               "    tp = tc, dp = dc;\n"
               "  }\n"
               "  return 0.0;\n"
               "}\n"
               "void main() {\n"
               "  if ((int(pc.b.w) & 4096) != 0) { o = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"  // (the view held: black)
               "  float u0 = pc.a.x + pc.a.y * gl_FragCoord.x, v = pc.a.z + pc.a.w * gl_FragCoord.y;\n"
               "  float K = pc.b.x, t = xr_surface(u0, v, pc.b.x);\n"
               "  if ((int(pc.b.w) & 2) != 0) {\n"  // (the depth shown: grey 1 / distance over 4; what he holds red, green 0.25 and blue 0.5)
               "    float hd = pc.d.z > 0.5 ? idh(u0, v) : -1.0;\n"
               "    o = hd >= 0.0 ? vec4(clamp(hd * 0.25, 0.0, 1.0), 0.25, 0.5, 1.0) : vec4(vec3(clamp(idist(u0, v) * 0.25, 0.0, 1.0)), 1.0); return;\n"
               "  }\n"
               "  float us = u0 + K * t;\n"
               "  if ((int(pc.b.w) & 2048) != 0) {\n"  // (an eye each: the scene - none of his in it - and his over it)
               "    vec4 h = held_eye(u0, v);\n"
               // (no more the fade fill caught in draw_ - its colour, white at a door, turned the sword white)
               // (and the fades the game makes otherwise - at the doors it is none of the above: the final image itself,
               //  16 points across it, its brightest and its darkest; black: his, drawn over it, goes black with it -
               //  the sword and shield floated lit in the black, 27/10; the consultant's idea, Gemini)
               "    if (h.a > 0.0) {\n"
               "      float pk = 0.0, fl = 1.0;\n"
               "      for (int i = 0; i < 16; i++) {\n"
               "        vec3 s = texture(col, vec2(0.15 + 0.2333 * float(i & 3), 0.15 + 0.2333 * float(i >> 2))).rgb;\n"
               "        pk = max(pk, max(s.r, max(s.g, s.b))), fl = min(fl, min(s.r, min(s.g, s.b)));\n"
               "      }\n"
               // (hidden, not tinted: tinted white they flashed white at a door - he would rather not see them then)
               "      h.a *= smoothstep(0.0, 0.06, pk) * (1.0 - smoothstep(0.9, 0.99, fl));\n"
               "    }\n"
               "    o = vec4(mix(texture(col, vec2(us, v)).rgb, h.rgb, h.a), 1.0); return;\n"
               "  }\n"
               "  if (pc.d.z > 0.5) {\n"  // (what he holds: found with its own depth; the scene behind it from its edge)
               "    float th = held_t(u0, v, K);\n"
               "    if (th > 0.0) { o = vec4(heldc(u0 + K * th, v), 1.0); return; }\n"
               "    if (!split() && rawz(us, v) < pc.d.x) {\n"
               "      float px = 2.0 / float(textureSize(col, 0).x), found = us;\n"
               "      for (int i = 1; i <= 64; i++) {\n"
               "        if (rawz(us + float(i) * px, v) >= pc.d.x) { found = us + float(i) * px; break; }\n"
               "        if (rawz(us - float(i) * px, v) >= pc.d.x) { found = us - float(i) * px; break; }\n"
               "      }\n"
               "      us = found;\n"
               "    }\n"
               "  }\n"
               "  o = vec4(texture(col, vec2(us, v)).rgb, 1.0);\n"
               "}\n";
    }
    // AppSW (android/xr_app.c LARECOMP_XR_APPSW, exec.c vk_xr_appsw): an eye's motion vectors and depth, at the runtime's
    // size. The first surface along each pixel's ray is found as xr_warp.frag finds it (a, b, c the same, a's steps for
    // this size); the point there, in the eye's frame (e: the eye's tangents left, right, up, down; distance 1 / t, game
    // units), is taken back to where it was in the previous image - the body's motion only, the head's being the
    // headset's: Q = M (P + O) + T - O (m0..m2: M's rows and T; O = (z.x, 0, 0) the eye's offset from the head's centre).
    // Out: CurrNDC - PrevNDC (Vulkan's, y down; z.w = 1: y flipped), and the depth 0..1 of the game's own projection
    // (distance = b.z / (depth + b.y), b.w & 1: 0..1, else -1..1).
    if (!strcmp(name, "xr_appsw.frag")) {
        *stage = STAGE_FRAGMENT;
        return
               "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D col;\n"
               "layout(set = 0, binding = 1) uniform sampler2D dep;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; vec4 m0; vec4 m1; vec4 m2; vec4 e; vec4 z; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "layout(set = 0, binding = 2) uniform sampler2D bounds;\n"
               "#define XR_RAY_BOUNDS\n"
               XR_DEPTH_RAY_GLSL
               "float depth01(float d) {\n"
               "  float z = pc.b.z / d - pc.b.y;\n"
               "  if ((int(pc.b.w) & 1) == 0) z = z * 0.5 + 0.5;\n"
               "  return clamp(z, 0.0, 1.0);\n"
               "}\n"
               "void main() {\n"
               "  float u0 = pc.a.x + pc.a.y * gl_FragCoord.x, v = pc.a.z + pc.a.w * gl_FragCoord.y;\n"
               "  float K = pc.b.x, t = K != 0.0 ? xr_surface(u0, v, K) : idist(u0, v);\n"
               "  float D = t > 1e-4 ? 1.0 / t : 1e4;\n"
               "  vec2 f = gl_FragCoord.xy * pc.z.yz;\n"
               "  vec2 tc = vec2(mix(pc.e.x, pc.e.y, f.x), mix(pc.e.z, pc.e.w, f.y));\n"
               "  vec3 P = vec3(tc * D, -D), O = vec3(pc.z.x, 0.0, 0.0);\n"
               "  vec3 Q = mat3(pc.m0.x, pc.m1.x, pc.m2.x, pc.m0.y, pc.m1.y, pc.m2.y, pc.m0.z, pc.m1.z, pc.m2.z) * (P + O)\n"
               "         + vec3(pc.m0.w, pc.m1.w, pc.m2.w) - O;\n"
               "  vec3 mv = vec3(0.0);\n"
               "  if (Q.z < -1e-3) {\n"
               "    vec2 tq = Q.xy / -Q.z;\n"
               "    mv.xy = 2.0 * (tc - tq) / vec2(pc.e.y - pc.e.x, pc.e.w - pc.e.z);\n"
               "    mv.z = depth01(D) - depth01(-Q.z);\n"
               "  }\n"
               "  o = vec4(mv.x, mv.y * pc.z.w, mv.z, 0.0);\n"
               "  gl_FragDepth = depth01(D);\n"
               "}\n";
    }
    // First person at the display's rate (device.c, the presenter): the newest composed window image s turned from its
    // frame's view to the moment's. Each pixel of the game image's place (vr, in pixels) is a direction in the moment's
    // view (n: its place in the game's view - crop: the part of it the image shows - times the field's tangents), seen
    // from the frame's view through M (rows m0..m2: the frame's camera axes dotted with the moment's), projected back:
    // the source pixel, held inside the image's place. Outside the place (bars) and when misc2.x = 0: s as it is.
    // Overscan: the frame's view is drawn wider than the screen (firstperson.c): a pixel's direction is taken with the
    // screen's tangents (misc2.zw), found in the frame with its own (misc.xy) - the edge the view turns toward exists;
    // nid: where the pixel is in the frame, unturned (what he holds).
    // Moved as well (tr.w): the moment's eye stands at tr from the frame's (the frame's axes); along the moment's ray
    // from there, the point the frame's depth puts at the same distance - found by going back and forth between the
    // ray and the depth up to three times (the depth z: binding 2, its part df.xy). The depth read is the nearest of
    // the scene's in a 5x5 texel neighbourhood (probe): thin edges and what borders them move with their object instead
    // of hesitating between it and what is behind (the treetops' twigs trembled as he turned, Guillaume, 22/10); the
    // far plane (sky) is turned only. What he holds (depth below df.z, exec.c FP_HELD_Z) is kept where it is on screen
    // with a 2-pixel border (its smoothed edge went with the scene: a slight blur round the sword), and a scene pixel
    // the turn takes from under it is taken at its edge, toward this pixel. df.w: 1 = the depth over the image, 2 = alone.
    if (!strcmp(name, "fpwarp.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D s;\n"
               "layout(set = 0, binding = 1) uniform sampler2D t;\n"
               "layout(set = 0, binding = 2) uniform sampler2D z;\n"
               "layout(push_constant) uniform P { vec4 m0, m1, m2, vr, crop, misc, misc2, tr, dp, df; } w;\n"
               "ivec2 ztex(vec2 nn) {\n"
               "  ivec2 zs = textureSize(z, 0);\n"
               "  vec2 gg = vec2(nn.x + 1.0, 1.0 - nn.y) * 0.5;\n"
               "  vec2 du = vec2(gg.x, w.dp.w > 0.5 ? gg.y : 1.0 - gg.y) * w.df.xy;\n"
               "  return clamp(ivec2(du * vec2(zs)), ivec2(0), zs - 1);\n"
               "}\n"
               "vec2 probe(vec2 nn) {\n"
               "  ivec2 zs = textureSize(z, 0), c = ztex(nn);\n"
               "  float best = 1e9, held = 0.0;\n"
               "  for (int j = -1; j <= 1; j++)\n"
               "    for (int i = -1; i <= 1; i++) {\n"
               "      float z0 = texelFetch(z, clamp(c + ivec2(i, j) * 2, ivec2(0), zs - 1), 0).r;\n"
               "      if (z0 < w.df.z) { held = 1.0; continue; }\n"
               "      if (z0 >= 0.99999) continue;\n"
               "      best = min(best, w.dp.z > 0.5 ? w.dp.y / (z0 + w.dp.x) : w.dp.y / (z0 * 2.0 - 1.0 + w.dp.x));\n"
               "    }\n"
               "  return vec2(best < 1e8 ? best : -1.0, held);\n"
               "}\n"
               "layout(location = 0) out vec4 o;\n"
               "vec2 win_uv(vec2 nn) {\n"
               "  vec2 size = w.misc.zw, g2 = vec2(nn.x + 1.0, 1.0 - nn.y) * 0.5, q2 = (g2 - w.crop.xy) / w.crop.zw;\n"
               "  vec2 lo = (w.vr.xy + 0.5) / size, hi = (w.vr.xy + w.vr.zw - 0.5) / size;\n"
               "  return clamp((w.vr.xy + q2 * w.vr.zw) / size, lo, hi);\n"
               "}\n"
               "void main() {\n"
               "  vec2 px = gl_FragCoord.xy, size = w.misc.zw, uv = px / size;\n"
               "  vec2 q = (px - w.vr.xy) / w.vr.zw;\n"
               "  vec3 S = texture(s, uv).rgb;\n"
               "  bool sep = w.misc2.y > 0.5;\n"
               "  if (sep && any(greaterThan(abs(S - texture(t, uv).rgb), vec3(0.02)))) { o = vec4(w.df.w > 2.5 ? mix(S, vec3(1.0, 0.0, 1.0), 0.7) : S, 1.0); return; }\n"
               "  if (w.misc2.x > 0.5 && all(greaterThanEqual(q, vec2(0.0))) && all(lessThanEqual(q, vec2(1.0)))) {\n"
               "    vec2 g = w.crop.xy + q * w.crop.zw, n = vec2(g.x * 2.0 - 1.0, 1.0 - g.y * 2.0);\n"
               "    vec3 dc = vec3(n.x * w.misc2.z, n.y * w.misc2.w, 1.0);\n"
               "    vec2 nid = n * w.misc2.zw / w.misc.xy;\n"
               "    vec3 d = vec3(dot(w.m0.xyz, dc), dot(w.m1.xyz, dc), dot(w.m2.xyz, dc));\n"
               "    if (d.z > 1e-4) {\n"
               "      vec2 n2 = vec2(d.x / d.z / w.misc.x, d.y / d.z / w.misc.y);\n"
               "      bool depth = w.df.z > 0.0;\n"
               "      if (depth && w.df.w > 0.5 && w.df.w < 2.5) {\n"
               "        vec2 e = probe(n2);\n"
               "        vec3 c = e.y > 0.5 ? vec3(0.0, 1.0, 0.0) : e.x < 0.0 ? vec3(0.0, 0.0, 1.0) : w.df.w > 1.5 ? vec3(1.0 - clamp(e.x / 40.0, 0.0, 1.0)) : vec3(fract(e.x), 0.0, 0.0);\n"
               "        o = vec4(w.df.w > 1.5 ? c : mix(S, c, 0.6), 1.0);\n"
               "        return;\n"
               "      }\n"
               "      if (depth && probe(nid).y > 0.5) { vec2 ui = win_uv(nid); o = vec4(sep ? texture(t, ui).rgb : texture(s, ui).rgb, 1.0); return; }\n"
               "      if (depth && w.tr.w > 0.5) {\n"
               "        vec2 nn = n2;\n"
               "        for (int i = 0; i < 3; i++) {\n"
               "          float e = probe(nn).x;\n"
               "          if (e < 0.0) break;\n"
               "          vec3 P = w.tr.xyz + d * ((e - w.tr.z) / d.z);\n"
               "          if (P.z < 1e-3) break;\n"
               "          vec2 nx = vec2(P.x / P.z / w.misc.x, P.y / P.z / w.misc.y);\n"
               "          bool still = all(lessThan(abs(nx - nn), vec2(1e-4)));\n"
               "          nn = nx;\n"
               "          if (still) break;\n"
               "        }\n"
               "        n2 = nn;\n"
               "      }\n"
               "      if (depth && probe(n2).y > 0.5) {\n"
               "        vec2 a = n2;\n"
               "        for (int i = 1; i <= 12; i++) { vec2 c = mix(a, nid, float(i) / 12.0); if (probe(c).y < 0.5) { n2 = c; break; } }\n"
               "      }\n"
               "      vec2 u2 = win_uv(n2);\n"
               "      S = sep ? texture(t, u2).rgb : texture(s, u2).rgb;\n"
               "    }\n"
               "  }\n"
               "  o = vec4(S, 1.0);\n"
               "}\n";
    }
    // SMAA 1x (third_party/smaa, runtime/nvn/smaa_data.c): the reference SMAA.hlsl in its GLSL mode, High preset,
    // with an entry point per pass. a = SMAA_RT_METRICS (1 / size, size). The vertex-stage offset helpers run per
    // pixel here. Luma edges (t0 = colour) -> blending weights (t0 = edges, t1 = area, t2 = search) -> neighbourhood
    // blending (t0 = colour, t1 = weights). Vulkan's texture origin is top-left, as SMAA expects (no flips).
    if (!strncmp(name, "smaa_", 5)) {
        *stage = STAGE_FRAGMENT;
        static char *src[3];
        int k = !strcmp(name, "smaa_edge.frag") ? 0 : !strcmp(name, "smaa_weight.frag") ? 1 : !strcmp(name, "smaa_blend.frag") ? 2 : -1;
        if (k < 0) return NULL;
        if (!src[k]) {
            extern const char smaa_hlsl_src[];
            static const char *head =
                "#version 450\n"
                "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
                "#define SMAA_RT_METRICS pc.a\n"
                "#define SMAA_GLSL_4\n";
            const char *quality = getenv("LARECOMP_SMAA_ULTRA");
            const char *preset = quality && *quality == '1' ? "#define SMAA_PRESET_ULTRA\n" : "#define SMAA_PRESET_HIGH\n";
            static const char *mains[3] = {
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy;\n"
                "  vec4 off[3];\n"
                "  SMAAEdgeDetectionVS(uv, off);\n"
                "  o = vec4(SMAALumaEdgeDetectionPS(uv, off, t0), 0.0, 0.0);\n"
                "}\n",
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
                "layout(set = 0, binding = 2) uniform sampler2D t2;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy, pix;\n"
                "  vec4 off[3];\n"
                "  SMAABlendingWeightCalculationVS(uv, pix, off);\n"
                "  o = SMAABlendingWeightCalculationPS(uv, pix, off, t0, t1, t2, vec4(0.0));\n"
                "}\n",
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy;\n"
                "  vec4 off;\n"
                "  SMAANeighborhoodBlendingVS(uv, off);\n"
                "  o = vec4(SMAANeighborhoodBlendingPS(uv, off, t0, t1).rgb, 1.0);\n"
                "}\n"};
            size_t n = strlen(head) + strlen(preset) + strlen(smaa_hlsl_src) + strlen(mains[k]) + 1;
            src[k] = malloc(n);
            snprintf(src[k], n, "%s%s%s%s", head, preset, smaa_hlsl_src, mains[k]);
        }
        return src[k];
    }
    // AMD FidelityFX Super Resolution 1 (MIT), EASU: edge-adaptive upscale. a = (in / out scale xy, offset zw),
    // b.xy = input size. 12 taps around the source pixel, direction / edge length from luma, Lanczos-2-like
    // weights stretched along the edge, clamped to the nearest 4 (no ringing).
    if (!strcmp(name, "easu.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform mediump sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "mediump vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }\n"
               "mediump float lum(mediump vec3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
               "void setf(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {\n"
               "  float dx = lD - lB, dy = lE - lA;\n"
               "  float lx = clamp(abs(dx) / max(max(abs(lD - lC), abs(lC - lB)), 1.0 / 65536.0), 0.0, 1.0);\n"
               "  float ly = clamp(abs(dy) / max(max(abs(lE - lC), abs(lC - lA)), 1.0 / 65536.0), 0.0, 1.0);\n"
               "  dir += vec2(dx, dy) * w;\n"
               "  len += (lx * lx + ly * ly) * w;\n"
               "}\n"
               "void tapf(inout mediump vec3 ac, inout mediump float aw, mediump vec2 off, mediump vec2 dir, mediump vec2 len2,\n"
               "          mediump float lob, mediump float clp, mediump vec3 c) {\n"
               "  mediump vec2 v = vec2(dot(off, dir), dot(off, vec2(-dir.y, dir.x))) * len2;\n"
               "  mediump float d2 = min(dot(v, v), clp);\n"
               "  mediump float wb = 0.4 * d2 - 1.0, wa = lob * d2 - 1.0;\n"
               "  wb *= wb; wa *= wa;\n"
               "  mediump float w = (1.5625 * wb - 0.5625) * wa;\n"
               "  ac += c * w; aw += w;\n"
               "}\n"
               "void main() {\n"
               "  vec2 pp = floor(gl_FragCoord.xy) * pc.a.xy + pc.a.zw;\n"
               "  vec2 fpf = floor(pp); pp -= fpf;\n"
               "  ivec2 fp = ivec2(fpf);\n"
               "  //    b c\n"
               "  //  e f g h\n"
               "  //  i j k l\n"
               "  //    n o\n"
               "  mediump vec3 bC = tap(fp + ivec2(0, -1)), cC = tap(fp + ivec2(1, -1));\n"
               "  mediump vec3 eC = tap(fp + ivec2(-1, 0)), fC = tap(fp), gC = tap(fp + ivec2(1, 0)), hC = tap(fp + ivec2(2, 0));\n"
               "  mediump vec3 iC = tap(fp + ivec2(-1, 1)), jC = tap(fp + ivec2(0, 1)), kC = tap(fp + ivec2(1, 1)), lC = tap(fp + ivec2(2, 1));\n"
               "  mediump vec3 nC = tap(fp + ivec2(0, 2)), oC = tap(fp + ivec2(1, 2));\n"
               "  mediump float bL = lum(bC), cL = lum(cC), eL = lum(eC), fL = lum(fC), gL = lum(gC), hL = lum(hC);\n"
               "  mediump float iL = lum(iC), jL = lum(jC), kL = lum(kC), lL = lum(lC), nL = lum(nC), oL = lum(oC);\n"
               "  vec2 dir = vec2(0.0);\n"
               "  float len = 0.0;\n"
               "  setf(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
               "  setf(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);\n"
               "  setf(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);\n"
               "  setf(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);\n"
               "  float dr = dot(dir, dir);\n"
               "  bool zro = dr < 1.0 / 32768.0;\n"
               "  dir = zro ? vec2(1.0, 0.0) : dir * inversesqrt(dr);\n"
               "  len = len * 0.5; len *= len;\n"
               "  float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n"
               "  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
               "  float lob = 0.5 - 0.29 * len, clp = 1.0 / lob;\n"
               "  mediump vec3 mn = min(min(fC, gC), min(jC, kC)), mx = max(max(fC, gC), max(jC, kC));\n"
               "  mediump vec3 ac = vec3(0.0);\n"
               "  mediump float aw = 0.0;\n"
               "  tapf(ac, aw, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, bC);\n"
               "  tapf(ac, aw, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, cC);\n"
               "  tapf(ac, aw, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, iC);\n"
               "  tapf(ac, aw, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, jC);\n"
               "  tapf(ac, aw, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, fC);\n"
               "  tapf(ac, aw, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, eC);\n"
               "  tapf(ac, aw, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, kC);\n"
               "  tapf(ac, aw, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, lC);\n"
               "  tapf(ac, aw, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, hC);\n"
               "  tapf(ac, aw, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, gC);\n"
               "  tapf(ac, aw, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, oC);\n"
               "  tapf(ac, aw, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, nC);\n"
               "  o = vec4(min(mx, max(mn, ac / aw)), 1.0);\n"
               "}\n";
    }
    // FSR 1 RCAS: contrast-adaptive sharpening of the EASU output (same size). a.x = sharpness (exp2(-stops)),
    // b.xy = image size. The lobe is limited so the result stays inside the neighbours' range; noise is damped.
    if (!strcmp(name, "rcas.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform mediump sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "mediump vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }\n"
               "mediump float lum(mediump vec3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
               "void main() {\n"
               "  ivec2 ip = ivec2(gl_FragCoord.xy);\n"
               "  //    b\n"
               "  //  d e f\n"
               "  //    h\n"
               "  mediump vec3 b = tap(ip + ivec2(0, -1)), d = tap(ip + ivec2(-1, 0)), e = tap(ip), f = tap(ip + ivec2(1, 0)), h = tap(ip + ivec2(0, 1));\n"
               "  mediump float bL = lum(b), dL = lum(d), eL = lum(e), fL = lum(f), hL = lum(h);\n"
               "  float nz = 0.25 * (bL + dL + fL + hL) - eL;\n"
               "  float rng = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);\n"
               "  nz = 1.0 - 0.5 * clamp(abs(nz) / max(rng, 1.0 / 65536.0), 0.0, 1.0);\n"
               "  vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
               "  vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0 / 65536.0));\n"
               "  vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));\n"
               "  vec3 lobeRGB = max(-hitMin, hitMax);\n"
               "  float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * pc.a.x * nz;\n"
               "  o = vec4((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 1.0);\n"
               "}\n";
    }
    if (!strcmp(name, "clear.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, pc.d.x, 1.0);\n"
               "}\n";
    }
    // clear.frag.<f|i|u><index>: writes the push-constant colour to colour target <index>
    if (!strncmp(name, "clear.frag.", 11)) {
        *stage = STAGE_FRAGMENT;
        static __thread char buf[512];
        char k = name[11];
        int idx = atoi(name + 12);
        const char *ty = k == 'i' ? "ivec4" : k == 'u' ? "uvec4" : "vec4";
        const char *cv = k == 'i' ? "floatBitsToInt(pc.c)" : k == 'u' ? "floatBitsToUint(pc.c)" : "pc.c";
        snprintf(buf, sizeof buf,
                 "#version 450\n"
                 "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
                 "layout(location = %d) out %s o;\n"
                 "void main() { o = %s; }\n", idx, ty, cv);
        return buf;
    }
    if (!strcmp(name, "clear.frag.none")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\nvoid main() {}\n";
    }
    return NULL;
}

VkShaderModule vk_builtin_shader(const char *name) {
    static struct { char name[32]; VkShaderModule m; } cache[64];
    static int n;
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].name, name)) return cache[i].m;
    int stage = 0;
    const char *src = builtin_src(name, &stage);
    if (!src) FATAL("no built-in shader %s\n", name);
    VkShaderModule m = vk_compile_glsl(src, stage, name);
    if (!m) FATAL("built-in shader %s failed to compile\n", name);
    if (n < 64) { snprintf(cache[n].name, sizeof cache[n].name, "%s", name); cache[n++].m = m; }
    return m;
}
