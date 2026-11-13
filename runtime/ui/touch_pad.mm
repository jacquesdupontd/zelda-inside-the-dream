// The on-screen controller drawn over the game (touch_pad.h): filled shapes, no outline, the pressed ones lit.
#include "imgui.h"
#include <pthread.h>
#include <string.h>
#include "touch_pad.h"

static pthread_mutex_t tp_lock = PTHREAD_MUTEX_INITIALIZER;
static TouchPadItem tp_items[TOUCH_PAD_MAX];
static int tp_n;
static volatile bool tp_shown;

extern "C" void touch_pad_set(const TouchPadItem *items, int n, bool shown) {
    if (n > TOUCH_PAD_MAX) n = TOUCH_PAD_MAX;
    pthread_mutex_lock(&tp_lock);
    memcpy(tp_items, items, sizeof *items * (size_t)n);
    tp_n = n;
    tp_shown = shown;
    pthread_mutex_unlock(&tp_lock);
}
extern "C" bool touch_pad_wanted(void) { return tp_shown; }

void touch_pad_draw(ImVec2 size, ImFont *font) {
    TouchPadItem it[TOUCH_PAD_MAX];
    pthread_mutex_lock(&tp_lock);
    int n = tp_n;
    memcpy(it, tp_items, sizeof *it * (size_t)n);
    pthread_mutex_unlock(&tp_lock);
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const ImU32 rest = IM_COL32(16, 18, 24, 92), lit = IM_COL32(236, 228, 210, 120);
    const ImU32 ink = IM_COL32(244, 240, 230, 190), ink_lit = IM_COL32(20, 20, 24, 230);
    for (int i = 0; i < n; i++) {
        const TouchPadItem &t = it[i];
        ImVec2 c(t.x * size.x, t.y * size.y);
        float r = t.r * size.y;
        bool d = t.down != 0;
        switch (t.shape) {
        case TP_STICK: {
            dl->AddCircleFilled(c, r, IM_COL32(16, 18, 24, d ? 80 : 56), 48);
            ImVec2 k(t.kx * size.x, t.ky * size.y);
            dl->AddCircleFilled(k, r * 0.46f, d ? lit : IM_COL32(236, 228, 210, 70), 40);
            break;
        }
        case TP_PILL: {
            float w = r * 2.5f;
            dl->AddRectFilled(ImVec2(c.x - w, c.y - r), ImVec2(c.x + w, c.y + r), d ? lit : rest, r);
            break;
        }
        case TP_MENU: {
            dl->AddCircleFilled(c, r, d ? lit : rest, 32);
            float w = r * 0.46f, g = r * 0.28f, th = r * 0.1f;
            for (int k = -1; k <= 1; k++)
                dl->AddRectFilled(ImVec2(c.x - w, c.y + k * g - th), ImVec2(c.x + w, c.y + k * g + th), d ? ink_lit : ink, th);
            break;
        }
        default: dl->AddCircleFilled(c, r, d ? lit : rest, 40); break;
        }
        if (t.label[0] && font) {
            float fs = (t.shape == TP_PILL ? r * 1.05f : r * (strlen(t.label) > 1 ? 0.72f : 0.95f));
            ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, t.label);
            dl->AddText(font, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), d ? ink_lit : ink, t.label);
        }
    }
}
