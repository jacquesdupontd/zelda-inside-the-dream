// Inside The Dream's recorder on Windows and Linux (28/10/2026, Guillaume: « une fonction record dans tous les ports,
// avec différentes qualités, je m'en servirai pour la vidéo de marketing », « jusqu'à la 4k ou 1440p »). As the Mac's
// (runtime/nvn/metal.m, 20/10): what the window shows - each image the first-person presenter sends to the screen
// (device.c wp_thread) - scaled by the GPU to the chosen size (Video size: the window's own, or 1080p / 1440p / 4K from
// its central 16:9), copied into host memory and piped as raw BGRA frames into ffmpeg (built with the game, LGPL:
// tools/build_deps_windows.sh; next to the executable), which encodes them: ProRes 422 HQ for editing, HEVC or H.264 by
// the GPU (AMF on AMD, Media Foundation otherwise; VAAPI on Linux). 60 frames a second: an image that comes late is
// written twice, so the video keeps the real time and the sound stays in step. The game's mixed sound goes alongside
// (float stereo 48 kHz, services/audio.c g_rec_audio_hook), and both are put into one .mov when the recording stops.
// Files: Videos/Inside The Dream/Inside The Dream <date> <time>.mov. F10 or the Video tab.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#include "vk.h"
#include "../../rt.h"
#include "../../paths.h"
#include "../../platform.h"
#include "../../record.h"
#include "../../settings.h"

#ifdef _WIN32
#define SEP_C '\\'
#else
#define SEP_C '/'
#endif
#define REC_SLOTS 6
static struct {
    VkImage img;
    VmaAllocation mem;
    VkImageView view;   // (the compute pass reads the scaled image through it)
    VkDescriptorSet set;
    VkBuffer buf;
    VmaAllocation bmem;
    void *map;
    int state;          // 0 free, 1 copied on the GPU (done when the presenter's timeline reaches tl)
    uint64_t seq, tl;
    double t;
} g_slot[REC_SLOTS];
static uint32_t g_w, g_h, g_made_w, g_made_h;  // the video's size; the slots' size (kept across recordings)
static VkFormat g_made_fmt;
static VkSemaphore g_tl;
static atomic_int g_on, g_want;     // recording; a start / stop asked (F10, the menu: done on the presenter's thread)
static FILE *g_pipe;
static FILE *volatile g_audio;
static uint64_t g_next_seq, g_write_seq, g_written, g_dropped;
static uint64_t g_presents, g_taken, g_filled;  // (the journal: images the presenter showed, game frames taken, gaps filled)
static double g_t0, g_started;
static char g_base[1200], g_final[1200], g_status[1400];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_writer;
static bool g_writer_up;

// ffmpeg started with its standard input our pipe (frames), its messages into <base>.ffmpeg.log. Windows: CreateProcess
// with no window - the game is a GUI program (RELEASE: no console), where _popen hangs or fails (Microsoft's own note;
// measured 28/10: ffmpeg never got a frame, and system() for the sound returned -2).
#ifdef _WIN32
static HANDLE g_proc;
static HANDLE log_handle(const char *log, SECURITY_ATTRIBUTES *sa) {
    HANDLE h = CreateFileA(log, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, sa, CREATE_ALWAYS, 0, NULL);
    return h != INVALID_HANDLE_VALUE ? h : NULL;
}
static FILE *spawn_writer(const char *cmdline, const char *log) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 22)) return NULL;
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    HANDLE lh = log_handle(log, &sa);
    STARTUPINFOA si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = rd, .hStdOutput = lh, .hStdError = lh};
    PROCESS_INFORMATION pi;
    char *cl = _strdup(cmdline);
    // (below normal: the encoder never takes the processor from the game)
    BOOL ok = CreateProcessA(NULL, cl, NULL, NULL, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi);
    free(cl);
    CloseHandle(rd);
    if (lh) CloseHandle(lh);
    if (!ok) { CloseHandle(wr); return NULL; }
    CloseHandle(pi.hThread);
    g_proc = pi.hProcess;
    int fd = _open_osfhandle((intptr_t)wr, _O_WRONLY | _O_BINARY);
    return fd >= 0 ? _fdopen(fd, "wb") : NULL;
}
static void close_writer(FILE *f) {
    fclose(f);
    if (g_proc) WaitForSingleObject(g_proc, 60000), CloseHandle(g_proc), g_proc = NULL;
}
static int run_wait(const char *cmdline, const char *log) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE lh = log_handle(log, &sa);
    STARTUPINFOA si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdOutput = lh, .hStdError = lh};
    PROCESS_INFORMATION pi;
    char *cl = _strdup(cmdline);
    BOOL ok = CreateProcessA(NULL, cl, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(cl);
    if (lh) CloseHandle(lh);
    if (!ok) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    return (int)code;
}
#else
static FILE *spawn_writer(const char *cmdline, const char *log) {
    char c[5200];
    snprintf(c, sizeof c, "%s 2>'%s'", cmdline, log);
    return popen(c, "w");
}
static void close_writer(FILE *f) { pclose(f); }
static int run_wait(const char *cmdline, const char *log) {
    char c[5200];
    snprintf(c, sizeof c, "%s 2>'%s'", cmdline, log);
    return system(c);
}
#endif

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void rec_toggle(void) { atomic_store(&g_want, 1); }
bool rec_active(void) { return atomic_load(&g_on) != 0; }
double rec_seconds(void) { return atomic_load(&g_on) ? now_s() - g_started : 0; }
const char *rec_status(void) { return g_status; }
static void folder(char *out, size_t n);
const char *rec_folder(void) { static char f[1100]; folder(f, sizeof f); return f; }

extern void (*g_rec_audio_hook)(const void *, int);  // services/audio.c: the mixed output (float stereo 48 kHz)
static void rec_audio(const void *d, int n) { FILE *f = g_audio; if (f) fwrite(d, 1, (size_t)n, f); }

static void mkdirs_(const char *path) {  // (each level; one that exists already is fine)
    char p[1200];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/' || *c == '\\') {
            char k = *c;
            *c = 0;
#ifdef _WIN32
            _mkdir(p);
#else
            mkdir(p, 0755);
#endif
            *c = k;
        }
#ifdef _WIN32
    _mkdir(p);
#else
    mkdir(p, 0755);
#endif
}

// Videos/Inside The Dream: Windows' Videos folder, Linux's XDG videos folder (user-dirs.dirs), else ~/Videos
static void folder(char *out, size_t n) {
    if (g_settings.rec_dir[0]) { snprintf(out, n, "%s", g_settings.rec_dir); return; }  // (the settings' rec_dir)
    if (getenv("LARECOMP_REC_DIR") && *getenv("LARECOMP_REC_DIR")) { snprintf(out, n, "%s", getenv("LARECOMP_REC_DIR")); return; }
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");
    snprintf(out, n, "%s\\Videos\\Inside The Dream", home && *home ? home : ".");
#else
    const char *home = getenv("HOME"), *cfg = getenv("XDG_CONFIG_HOME");
    char dirs[1100], videos[1100] = "";
    snprintf(dirs, sizeof dirs, "%s/user-dirs.dirs", cfg && *cfg ? cfg : home ? home : "/tmp");
    if (!(cfg && *cfg) && home) snprintf(dirs, sizeof dirs, "%s/.config/user-dirs.dirs", home);
    FILE *f = fopen(dirs, "r");
    if (f) {  // XDG_VIDEOS_DIR="$HOME/Videos"
        char line[1100];
        while (fgets(line, sizeof line, f)) {
            char *q = strstr(line, "XDG_VIDEOS_DIR=\"");
            if (!q || line[0] == '#') continue;
            q += 16;
            char *e = strchr(q, '"');
            if (e) *e = 0;
            if (!strncmp(q, "$HOME", 5)) snprintf(videos, sizeof videos, "%s%s", home ? home : "", q + 5);
            else snprintf(videos, sizeof videos, "%s", q);
        }
        fclose(f);
    }
    if (!*videos) snprintf(videos, sizeof videos, "%s/Videos", home && *home ? home : "/tmp");
    snprintf(out, n, "%s/Inside The Dream", videos);
#endif
}

// the encoder's part of ffmpeg's command line (Video quality)
static const char *encoder(void) {
#ifdef _WIN32
    bool amd = g_vk.props.vendorID == 0x1002;
#endif
    switch (g_settings.rec_quality) {
    case 1:  // HEVC by the GPU
#ifdef _WIN32
        return amd ? "-c:v hevc_amf -quality quality -rc cqp -qp_i 14 -qp_p 16 -pix_fmt nv12"  // (near what the screen shows)
                   : "-c:v hevc_mf -b:v 80M -pix_fmt nv12";
#else
        return "-vaapi_device /dev/dri/renderD128 -vf format=nv12,hwupload -c:v hevc_vaapi -b:v 80M";
#endif
    case 2:  // H.264 by the GPU
#ifdef _WIN32
        return amd ? "-c:v h264_amf -quality quality -rc cqp -qp_i 16 -qp_p 18 -pix_fmt nv12"
                   : "-c:v h264_mf -b:v 60M -pix_fmt nv12";
#else
        return "-vaapi_device /dev/dri/renderD128 -vf format=nv12,hwupload -c:v h264_vaapi -b:v 60M";
#endif
    default:  // ProRes 422 HQ, for editing (FFmpeg's own: on any computer)
        return "-c:v prores_ks -profile:v 3 -vendor apl0 -pix_fmt yuv422p10le";
    }
}

static bool ffmpeg_path(char *out, size_t n) {
#ifdef _WIN32
    return rt_resource("ffmpeg.exe", NULL, out, n);
#else
    if (rt_resource("ffmpeg", NULL, out, n)) return true;
    snprintf(out, n, "ffmpeg");  // (the system's, on Linux)
    return true;
#endif
}

static void *writer(void *arg) {
    (void)arg;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);  // (the game first)
#endif
    for (;;) {
        int slot = -1;
        pthread_mutex_lock(&g_mu);
        while (slot < 0) {
            for (int i = 0; i < REC_SLOTS; i++)
                if (g_slot[i].state == 1 && g_slot[i].seq == g_write_seq) slot = i;
            if (slot < 0) pthread_cond_wait(&g_cv, &g_mu);
        }
        pthread_mutex_unlock(&g_mu);
        uint64_t v = g_slot[slot].tl;  // (the presenter's submission that copied it: done)
        VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_tl, .pValues = &v};
        vkWaitSemaphores(g_vk.dev, &wi, UINT64_MAX);
        vmaInvalidateAllocation(g_vk.vma, g_slot[slot].bmem, 0, VK_WHOLE_SIZE);
        // Each game frame is one frame of the video (28/10: placed by the time it was taken, the game's own small
        // timing jitter plus the wait for a 165 Hz refresh put two in one place now and then - 26 doubled and 17
        // dropped in 11 s of turning). The clock only catches up with a real stall (two frames behind: the gap
        // filled with this image, the sound stays in step) or a game running ahead (three in front: this one left
        // out).
        double t = g_slot[slot].t;
        if (g_t0 == 0) g_t0 = t;
        double due = (t - g_t0) * 60.0 + 1;  // (frames due by now at 60 a second)
        uint64_t times = 1;
        if ((double)g_written + 1 < due - 2) times = (uint64_t)(due - g_written), g_filled += times - 1;
        else if ((double)g_written + 1 > due + 3) times = 0, g_dropped++;
        uint64_t target = g_written + times;
        while (g_written < target && g_pipe) {
            if (fwrite(g_slot[slot].map, 1, (size_t)g_w * g_h * 3 / 2, g_pipe) != (size_t)g_w * g_h * 3 / 2) {  // (ffmpeg stopped)
                snprintf(g_status, sizeof g_status, "The recording stopped: the encoder quit (Video quality: try ProRes).");
                close_writer(g_pipe), g_pipe = NULL;
                atomic_store(&g_want, 1);
                break;
            }
            g_written++;
        }
        pthread_mutex_lock(&g_mu);
        g_slot[slot].state = 0, g_write_seq++;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}

// The colour conversion on the GPU (28/10, Guillaume: « on perd des frames ça lag »): the window image, scaled by the
// blit, read by a compute pass that writes NV12 (BT.709, limited range: what HD video players expect) straight into
// the slot's host-visible buffer - 1.5 bytes a pixel instead of 4 to read back (5.5 MB a frame at 1440p, not 14.7), and
// no conversion in ffmpeg on the processor. Measured before: the game at 32-54 fps half of the time while recording
// 1440p, 60 before and after (Raccoon's Ryzen 5 5500X3D: 6 cores shared with ffmpeg's BGRA -> NV12 at 221 Mpx/s).
// Each invocation: 4x2 pixels - two words of luma, one of chroma (two U V pairs).
static const char *k_nv12_cs =
    "#version 450\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(binding = 0) uniform sampler2D src;\n"
    "layout(std430, binding = 1) writeonly buffer Out { uint data[]; };\n"
    "layout(push_constant) uniform PC { uvec2 size; } pc;\n"
    "float y_(vec3 c) { return 16.0 + 219.0 * dot(c, vec3(0.2126, 0.7152, 0.0722)); }\n"
    "void main() {\n"
    "    uvec2 b = gl_GlobalInvocationID.xy;\n"
    "    uint x0 = b.x * 4u, y0 = b.y * 2u;\n"
    "    if (x0 >= pc.size.x || y0 >= pc.size.y) return;\n"
    "    vec3 c[2][4];\n"
    "    for (int j = 0; j < 2; j++) for (int i = 0; i < 4; i++) c[j][i] = texelFetch(src, ivec2(x0 + uint(i), y0 + uint(j)), 0).rgb;\n"
    "    for (int j = 0; j < 2; j++) {\n"
    "        uint w = 0u;\n"
    "        for (int i = 0; i < 4; i++) w |= uint(clamp(y_(c[j][i]) + 0.5, 0.0, 255.0)) << (8u * uint(i));\n"
    "        data[((y0 + uint(j)) * pc.size.x + x0) / 4u] = w;\n"
    "    }\n"
    "    uint uv = 0u;\n"
    "    for (int k = 0; k < 2; k++) {\n"
    "        vec3 a = (c[0][2 * k] + c[0][2 * k + 1] + c[1][2 * k] + c[1][2 * k + 1]) * 0.25;\n"
    "        float yy = dot(a, vec3(0.2126, 0.7152, 0.0722));\n"
    "        float u = 128.0 + 224.0 * (a.b - yy) / 1.8556, v = 128.0 + 224.0 * (a.r - yy) / 1.5748;\n"
    "        uv |= (uint(clamp(u + 0.5, 0.0, 255.0)) | (uint(clamp(v + 0.5, 0.0, 255.0)) << 8u)) << (16u * uint(k));\n"
    "    }\n"
    "    data[(pc.size.x * pc.size.y + (y0 / 2u) * pc.size.x + x0) / 4u] = uv;\n"
    "}\n";
static VkPipeline g_nv12_pipe;
static VkPipelineLayout g_nv12_layout;
static VkDescriptorSetLayout g_nv12_dsl;
static VkDescriptorPool g_nv12_pool;
static VkSampler g_nv12_sampler;
static bool nv12_pass(void) {
    if (g_nv12_pipe) return true;
    VkShaderModule m = vk_compile_glsl(k_nv12_cs, 5 /* STAGE_COMPUTE */, "record_nv12.comp");
    if (!m) return false;
    VkDescriptorSetLayoutBinding bd[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bd};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &dl, NULL, &g_nv12_dsl));
    VkPushConstantRange pr = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 8};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_nv12_dsl,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &g_nv12_layout));
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = g_nv12_layout,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}};
    VkResult r = vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &ci, NULL, &g_nv12_pipe);
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    if (r != VK_SUCCESS) { g_nv12_pipe = VK_NULL_HANDLE; return false; }
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, REC_SLOTS}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, REC_SLOTS}};
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, .maxSets = REC_SLOTS,
                                     .poolSizeCount = 2, .pPoolSizes = ps};
    VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &pi, NULL, &g_nv12_pool));
    VkSamplerCreateInfo sc = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VK_CHECK(vkCreateSampler(g_vk.dev, &sc, NULL, &g_nv12_sampler));
    return true;
}

static void free_slots(void) {
    for (int i = 0; i < REC_SLOTS; i++) {
        if (g_slot[i].set) vkFreeDescriptorSets(g_vk.dev, g_nv12_pool, 1, &g_slot[i].set);
        if (g_slot[i].view) vkDestroyImageView(g_vk.dev, g_slot[i].view, NULL);
        if (g_slot[i].img) vmaDestroyImage(g_vk.vma, g_slot[i].img, g_slot[i].mem);
        if (g_slot[i].buf) vmaDestroyBuffer(g_vk.vma, g_slot[i].buf, g_slot[i].bmem);
        memset(&g_slot[i], 0, sizeof g_slot[i]);
    }
    g_made_w = g_made_h = 0;
}

static bool make_slots(void) {
    if (g_made_w == g_w && g_made_h == g_h && g_made_fmt == g_vk.sc_format) return true;
    vkDeviceWaitIdle(g_vk.dev);
    free_slots();
    if (!nv12_pass()) return false;
    for (int i = 0; i < REC_SLOTS; i++) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = g_vk.sc_format,
                                .extent = {g_w, g_h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(g_vk.vma, &ci, &ai, &g_slot[i].img, &g_slot[i].mem, NULL) != VK_SUCCESS) { free_slots(); return false; }
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_slot[i].img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = g_vk.sc_format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(g_vk.dev, &vi, NULL, &g_slot[i].view) != VK_SUCCESS) { free_slots(); return false; }
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)g_w * g_h * 3 / 2,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        VmaAllocationCreateInfo bai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                       .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo inf;
        if (vmaCreateBuffer(g_vk.vma, &bi, &bai, &g_slot[i].buf, &g_slot[i].bmem, &inf) != VK_SUCCESS) { free_slots(); return false; }
        g_slot[i].map = inf.pMappedData;
        VkDescriptorSetAllocateInfo da = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_nv12_pool,
                                          .descriptorSetCount = 1, .pSetLayouts = &g_nv12_dsl};
        if (vkAllocateDescriptorSets(g_vk.dev, &da, &g_slot[i].set) != VK_SUCCESS) { free_slots(); return false; }
        VkDescriptorImageInfo ii = {.sampler = g_nv12_sampler, .imageView = g_slot[i].view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo bf = {.buffer = g_slot[i].buf, .range = VK_WHOLE_SIZE};
        VkWriteDescriptorSet wr[2] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_slot[i].set, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_slot[i].set, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bf}};
        vkUpdateDescriptorSets(g_vk.dev, 2, wr, 0, NULL);
    }
    g_made_w = g_w, g_made_h = g_h, g_made_fmt = g_vk.sc_format;
    return true;
}

static void start(uint32_t sw, uint32_t sh) {
    static const uint32_t sizes[][2] = {{0, 0}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    int k = g_settings.rec_size < 0 || g_settings.rec_size > 3 ? 0 : g_settings.rec_size;
    g_w = k ? sizes[k][0] : (sw & ~3u), g_h = k ? sizes[k][1] : (sh & ~1u);  // (NV12 in 4x2 blocks)
    char ff[1100], dir[1100];
    if (!ffmpeg_path(ff, sizeof ff)) { snprintf(g_status, sizeof g_status, "No ffmpeg next to the game: it can't record."); return; }
    if (!make_slots()) { snprintf(g_status, sizeof g_status, "Not enough graphics memory to record at %ux%u.", g_w, g_h); return; }
    folder(dir, sizeof dir);
    mkdirs_(dir);
    time_t tt = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char stamp[64];
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H-%M-%S", &tm);
    snprintf(g_final, sizeof g_final, "%s%cInside The Dream %s.mov", dir, SEP_C, stamp);
    snprintf(g_base, sizeof g_base, "%s%c.recording %s", dir, SEP_C, stamp);
    bool bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
    char cmd[4000], log[1300];
    (void)bgr;  // (the compute pass reads the image through its format: channels in place either way)
    snprintf(cmd, sizeof cmd, "\"%s\" -loglevel error -y -f rawvideo -pix_fmt nv12 -color_range tv -colorspace bt709 -s %ux%u -r 60 "
             "-i pipe:0 %s -color_range tv -colorspace bt709 -color_primaries bt709 -color_trc bt709 \"%s.video.mov\"", ff, g_w, g_h,
             encoder(), g_base);
    snprintf(log, sizeof log, "%s.ffmpeg.log", g_base);
    g_pipe = spawn_writer(cmd, log);
    if (!g_pipe) { snprintf(g_status, sizeof g_status, "The encoder didn't start (%s).", ff); return; }
    char a[1300];
    snprintf(a, sizeof a, "%s.audio.f32", g_base);
    g_audio = fopen(a, "wb");
    g_t0 = 0, g_written = 0, g_dropped = 0, g_next_seq = g_write_seq = 0, g_presents = g_taken = g_filled = 0;
    g_started = now_s();
    g_rec_audio_hook = rec_audio;
    if (!g_writer_up) g_writer_up = pthread_create(&g_writer, NULL, writer, NULL) == 0;
    atomic_store(&g_on, 1);
    snprintf(g_status, sizeof g_status, "Recording %ux%u to %s", g_w, g_h, g_final);
    LOG("[rec] recording %ux%u (%s): %s\n", g_w, g_h, encoder(), g_final);
}

// the sound and the video put together, after (a thread: the game goes on; its own copy of the names - another
// recording may start meanwhile)
typedef struct { char cmd[5000], base[1200], final_[1200]; bool choppy; } Mux;
static void *mux(void *arg) {
    Mux *m = arg;
    char v[1300], a[1300], log[1300];
    snprintf(log, sizeof log, "%s.mux.log", m->base);
    int r = run_wait(m->cmd, log);
    snprintf(v, sizeof v, "%s.video.mov", m->base);
    snprintf(a, sizeof a, "%s.audio.f32", m->base);
    struct stat st;
    if (r == 0 && stat(m->final_, &st) == 0 && st.st_size > 0) {
        remove(v), remove(a), remove(log);
        snprintf(log, sizeof log, "%s.ffmpeg.log", m->base);
        remove(log);
        if (m->choppy)
            snprintf(g_status, sizeof g_status, "Saved, but choppy: the window was hidden or the screen asleep. %s", m->final_);
        else snprintf(g_status, sizeof g_status, "Saved: %s", m->final_);
        LOG("[rec] saved %s\n", m->final_);
    } else {
        snprintf(g_status, sizeof g_status, "The sound wasn't added (the video alone is in %s.video.mov).", m->base);
        LOG("[rec] mux failed (%d): %s\n", r, m->cmd);
    }
    free(m);
    return NULL;
}

static void stop(void) {
    atomic_store(&g_on, 0);
    g_rec_audio_hook = NULL;
    pthread_mutex_lock(&g_mu);
    while (g_write_seq != g_next_seq) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    struct timespec ts = {0, 20000000};
    nanosleep(&ts, NULL);  // (the sound thread's last write)
    FILE *a = g_audio;
    g_audio = NULL;
    if (a) fclose(a);
    if (g_pipe) close_writer(g_pipe), g_pipe = NULL;
    LOG("[rec] stopped: %llu frames (%.1f s), %llu late images dropped; %llu images shown, %llu game frames taken, %llu "
        "frames filled by repeating one\n", (unsigned long long)g_written, g_written / 60.0, (unsigned long long)g_dropped,
        (unsigned long long)g_presents, (unsigned long long)g_taken, (unsigned long long)g_filled);
    char ff[1100];
    ffmpeg_path(ff, sizeof ff);
    Mux *m = calloc(1, sizeof *m);
    if (!m) return;
    snprintf(m->base, sizeof m->base, "%s", g_base);
    snprintf(m->final_, sizeof m->final_, "%s", g_final);
    // (as long as the video: -shortest cut it to the sound, which can end sooner - the trailer's dive lost 2.4 s, 28/10)
    snprintf(m->cmd, sizeof m->cmd, "\"%s\" -loglevel error -y -i \"%s.video.mov\" -f f32le -ar 48000 -ac 2 -i \"%s.audio.f32\" "
             "-map 0:v -map 1:a -c:v copy -c:a pcm_s16le -t %.3f \"%s\"", ff, m->base, m->base, g_written / 60.0, m->final_);
    snprintf(g_status, sizeof g_status, "Saving the video...");
    // The window hardly shown - the screen asleep, the window minimized or covered: Windows then presents it a few
    // times a second, and each image it got stands in for the frames between (the trailer's takes, shot unattended
    // with Raccoon's screen asleep: 78 images shown in 12.7 s, 708 frames filled, 28/10). Said in the journal and in the
    // Video tab, not found by whoever watches the video.
    m->choppy = g_written > 60 && g_filled * 4 > g_written;
    if (m->choppy) LOG("[rec] the window was hardly shown (the screen asleep, or the window minimized or covered): choppy\n");
    pthread_t t;
    if (pthread_create(&t, NULL, mux, m) == 0) pthread_detach(t);
    else free(m);
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

// The presenter, each image it sends to the screen (in the command buffer that draws it, the window image then in
// COLOR_ATTACHMENT_OPTIMAL - and left so): start / stop when asked, and the copy into a free slot while recording.
// tl / value: the submission's timeline signal, which says when the copy is there.
void vk_rec_frame(VkCommandBuffer cb, VkImage sc, uint32_t sw, uint32_t sh, VkSemaphore tl, uint64_t value, bool new_frame) {
    {   // (test) LARECOMP_REC_TEST=<start s>:<length s>: a recording that starts and stops by itself, from the first image
        static double t_start = -1, t_len, t_first;
        static int phase;
        if (t_start < 0) {
            const char *e = getenv("LARECOMP_REC_TEST");
            t_start = 0;
            if (!e || sscanf(e, "%lf:%lf", &t_start, &t_len) != 2) t_start = 0, phase = 2;
            t_first = now_s();
        }
        double t = now_s() - t_first;
        if (phase == 0 && t >= t_start) phase = 1, atomic_store(&g_want, 1);
        else if (phase == 1 && t >= t_start + t_len && atomic_load(&g_on)) phase = 2, atomic_store(&g_want, 1);
    }
    {   // (test) LARECOMP_REC_AT=<frame>:<frames>,... : recordings at those game frames (the counter of LARECOMP_WARP_AT
        // and LARECOMP_SHOT_AT: scouting takes chained with warps in one run)
        extern uint64_t renderer_present_count(void);
        static const char *next;
        static int init;
        static long stop_at = -1;
        if (!init) init = 1, next = getenv("LARECOMP_REC_AT");
        long f = (long)renderer_present_count(), at, len;
        if (stop_at >= 0 && f >= stop_at && atomic_load(&g_on)) stop_at = -1, atomic_store(&g_want, 1);
        else if (stop_at < 0 && next && *next && sscanf(next, "%ld:%ld", &at, &len) == 2 && f >= at && !atomic_load(&g_on)) {
            stop_at = at + len, atomic_store(&g_want, 1);
            next = strchr(next, ',');
            if (next) next++;
        }
    }
    if (atomic_exchange(&g_want, 0)) {
        if (atomic_load(&g_on)) stop();
        else start(sw, sh);
    }
    if (!atomic_load(&g_on)) return;
    g_presents++;
    // One image per game frame - the first one the presenter shows of each (28/10, Guillaume: « je bouge pas mon écran
    // moi, ça doit se faire tout seul »): the video then has each game frame once, evenly, whatever the display's
    // rate. Taken by the clock at 60 a second, a 165 Hz display gave images 12 or 18 ms apart (165 / 60 = 2.75), and
    // copying every refresh read back 2.4 GB a second at 1440p. A game at 30 fills its frames twice (the writer).
    if (!new_frame) return;
    g_taken++;
    g_tl = tl;
    int slot = -1;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < REC_SLOTS && slot < 0; i++) if (g_slot[i].state == 0) slot = i;
    if (slot >= 0) g_slot[slot].state = 1, g_slot[slot].seq = g_next_seq++, g_slot[slot].tl = value, g_slot[slot].t = now_s();
    pthread_mutex_unlock(&g_mu);
    if (slot < 0) { g_dropped++; return; }
    // the window's central 16:9 for 1080p / 1440p / 4K, the whole window for its own size
    double cw = sw, ch = sh, cx = 0, cy = 0;
    if (g_settings.rec_size > 0) {
        if (sw * 9.0 > sh * 16.0) cw = sh * 16.0 / 9.0, cx = (sw - cw) / 2;
        else ch = sw * 9.0 / 16.0, cy = (sh - ch) / 2;
    }
    barrier(cb, sc, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cb, g_slot[slot].img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .srcOffsets = {{(int32_t)cx, (int32_t)cy, 0}, {(int32_t)(cx + cw), (int32_t)(cy + ch), 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .dstOffsets = {{0, 0, 0}, {(int32_t)g_w, (int32_t)g_h, 1}}};
    vkCmdBlitImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_slot[slot].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                   VK_FILTER_LINEAR);
    barrier(cb, g_slot[slot].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_nv12_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_nv12_layout, 0, 1, &g_slot[slot].set, 0, NULL);
    uint32_t size[2] = {g_w, g_h};
    vkCmdPushConstants(cb, g_nv12_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, size);
    vkCmdDispatch(cb, (g_w / 4 + 7) / 8, (g_h / 2 + 7) / 8, 1);
    VkBufferMemoryBarrier2 hb = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                 .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT, .buffer = g_slot[slot].buf, .size = VK_WHOLE_SIZE,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &hb};
    vkCmdPipelineBarrier2(cb, &di);
    barrier(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    pthread_mutex_lock(&g_mu);
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}
