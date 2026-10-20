#include <cstring>
#include <set>
#include <thread>
#include <unistd.h>
#include "log.h"
#include "xdl.h"

extern "C" int DobbyHook(void *address, void *replace_func, void **origin_func);

typedef void (*HurtHp_t)(void *self, int damage, void *src, void *mi);
static HurtHp_t g_orig[3];
static thread_local int g_depth = 0;

template <int N>
static void hurt_hook(void *self, int damage, void *src, void *mi) {
    static int logged = 0;
    if (logged < 40) { logged++; LOGI("HurtHp[%d] damage=%d depth=%d", N, damage, g_depth); }
    int d = (g_depth == 0 && damage > 0) ? -damage : damage;
    g_depth++;
    g_orig[N](self, d, src, mi);
    g_depth--;
}
static void *const g_repl[3] = {(void *) hurt_hook<0>, (void *) hurt_hook<1>, (void *) hurt_hook<2>};

static void install(void *h) {
#define S(n) xdl_sym(h, n, nullptr)
    auto is_vm = (bool (*)(void *)) S("il2cpp_is_vm_thread");
    auto domain_get = (void *(*)()) S("il2cpp_domain_get");
    auto attach = (void *(*)(void *)) S("il2cpp_thread_attach");
    auto get_asms = (void **(*)(void *, size_t *)) S("il2cpp_domain_get_assemblies");
    auto get_image = (void *(*)(void *)) S("il2cpp_assembly_get_image");
    auto image_name = (const char *(*)(void *)) S("il2cpp_image_get_name");
    auto class_from_name = (void *(*)(void *, const char *, const char *)) S("il2cpp_class_from_name");
    auto method_by_name = (void *(*)(void *, const char *, int)) S("il2cpp_class_get_method_from_name");
#undef S
    if (!domain_get || !attach || !get_asms || !get_image || !image_name || !class_from_name || !method_by_name) {
        LOGE("hook: il2cpp api missing");
        return;
    }
    for (int i = 0; i < 180 && is_vm && !is_vm(nullptr); i++) sleep(1);
    void *domain = domain_get();
    attach(domain);

    void *image = nullptr;
    for (int i = 0; i < 60 && !image; i++) {
        size_t n = 0;
        void **asms = get_asms(domain, &n);
        for (size_t k = 0; k < n; k++) {
            void *img = get_image(asms[k]);
            if (img && !strcmp(image_name(img), "Assembly-CSharp.dll")) { image = img; break; }
        }
        if (!image) sleep(1);
    }
    if (!image) { LOGE("hook: Assembly-CSharp not found"); return; }

    const char *names[3] = {"RGController", "C12Controller", "C13Controller"};
    std::set<void *> done;
    int slot = 0;
    for (auto nm : names) {
        void *klass = class_from_name(image, "", nm);
        if (!klass) { LOGE("hook: class %s not found", nm); continue; }
        void *mi = method_by_name(klass, "HurtHp", 2);
        if (!mi) { LOGE("hook: HurtHp not found in %s", nm); continue; }
        void *addr = *(void **) mi;
        if (!addr || !done.insert(addr).second) continue;
        int rc = DobbyHook(addr, g_repl[slot], (void **) &g_orig[slot]);
        LOGI("hook: %s HurtHp @%p rc=%d", nm, addr, rc);
        slot++;
    }
}

void hook_start(void *handle) { std::thread(install, handle).detach(); }
