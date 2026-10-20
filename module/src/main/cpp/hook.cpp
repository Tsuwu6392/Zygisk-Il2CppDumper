#include <cstring>
#include <set>
#include <thread>
#include <unistd.h>
#include "log.h"
#include "xdl.h"

#if defined(__aarch64__)
extern "C" void A64HookFunction(void *symbol, void *replace, void **result);
#endif

// ---------------------------------------------------------
// 1. HurtHp Hooks
// ---------------------------------------------------------
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

// ---------------------------------------------------------
// 2. VoidShield Hooks
// ---------------------------------------------------------
typedef bool (*Consume_t)(void *self, void *ev, void *mi);
static Consume_t g_consume_orig;

static bool consume_hook(void *self, void *ev, void *mi) {
    static int logged = 0;
    int before = self ? *(int *)((char *)self + 0x18) : 0;
    bool r = g_consume_orig(self, ev, mi);
    if (self && before > 0) {
        int after = *(int *)((char *)self + 0x18);
        if (logged < 20) { logged++; LOGI("VoidShield consume: count %d -> %d", before, after); }
        *(int *)((char *)self + 0x18) = before;
    }
    return r;
}

typedef void (*AddDead_t)(void *self, void *src, void *mi);
static AddDead_t g_adddead_orig;

static void adddead_hook(void *self, void *src, void *mi) {
    static int logged = 0;
    if (logged < 20) { logged++; LOGI("AddDelayDeadBuff blocked"); }
}

// ---------------------------------------------------------
// 3. Mall Hooks (Price = 0, Currency = True)
// ---------------------------------------------------------
typedef void (*BuyItem_t)(void *self, void *mallItem, void *onDone, void *mi);
static BuyItem_t g_buyitem_orig;

static void buyitem_hook(void *self, void *mallItem, void *onDone, void *mi) {
    static int logged = 0;
    if (logged < 20) { logged++; LOGI("BuyItem hit! Forcing price to 0."); }
    if (mallItem) {
        *(int *)((char *)mallItem + 0x24) = 0;
        *(int *)((char *)mallItem + 0x28) = 0;
    }
    g_buyitem_orig(self, mallItem, onDone, mi);
}

typedef bool (*HasEnough_t)(void *mallItem, void *mi);
static HasEnough_t g_hasenough_orig;

static bool hasenough_hook(void *mallItem, void *mi) {
    LOGI("HasEnoughCurrency bypassed -> true");
    return true;
}

// ---------------------------------------------------------
// 4. RGSaveManager Hooks (Outside Shop / Batch)
// ---------------------------------------------------------
typedef void (*ConsumeGem_t)(void *self, int amount, bool unk1, bool unk2, void *mi);
static ConsumeGem_t g_consumegem_orig;

static void consumegem_hook(void *self, int amount, bool unk1, bool unk2, void *mi) {
    static int logged = 0;
    if (logged < 20) { logged++; LOGI("ConsumeGem intercepted! Original cost: %d -> Forcing 0", amount); }
    g_consumegem_orig(self, 0, unk1, unk2, mi);
}

// ---------------------------------------------------------
// 5. Energy Hooks (RoleAttributePlayer)
//
//    Fields:
//      max_energy : Int32 @ +0x178
//      _energy    : Int32 @ +0x17c
//
//    set_energy hook:
//      a) bump max_energy by +1000 once (idempotent, natural base < 10000)
//      b) call original
//      c) if _energy <= 100, refill to max_energy
// ---------------------------------------------------------
static const int ENERGY_BONUS  = 1000;
static const int ENERGY_REFILL = 100;

typedef void (*SetEnergy_t)(void *self, int value, void *mi);
static SetEnergy_t g_setenergy_orig;

static void setenergy_hook(void *self, int value, void *mi) {
    static int logged = 0;
    if (!self) { g_setenergy_orig(self, value, mi); return; }

    int *p_max = (int *)((char *)self + 0x178);
    int *p_cur = (int *)((char *)self + 0x17c);

    // a) Apply +1000 max bonus idempotently
    int cur_max = *p_max;
    if (cur_max > 0 && cur_max < 10000) {
        *p_max = cur_max + ENERGY_BONUS;
        if (logged < 20) { logged++; LOGI("energy: max bumped %d -> %d", cur_max, *p_max); }
    }

    // b) Call original setter
    g_setenergy_orig(self, value, mi);

    // c) Refill if at or below threshold
    int after = *p_cur;
    int new_max = *p_max;
    if (after <= ENERGY_REFILL && new_max > 0) {
        *p_cur = new_max;
        if (logged < 20) { logged++; LOGI("energy: refilled %d -> %d (max=%d)", after, *p_cur, new_max); }
    }
}

// ---------------------------------------------------------
// 6. Skill Cooldown Hook (RoleAttributePlayer)
//
//    get_skillCd() returns Single — the remaining cooldown time
//    the game reads to decide when the skill is ready.
//    Return 10% of the original value = 10x faster cooldown.
//
//    argc = 0 (no explicit parameters; self + MethodInfo* are implicit)
// ---------------------------------------------------------
typedef float (*GetSkillCd_t)(void *self, void *mi);
static GetSkillCd_t g_getskillcd_orig;

static float getskillcd_hook(void *self, void *mi) {
    static int logged = 0;
    float orig = g_getskillcd_orig(self, mi);
    float reduced = orig * 0.1f;
    if (logged < 20 && orig > 0.0f) {
        logged++;
        LOGI("skillCd: %.3f -> %.3f", orig, reduced);
    }
    return reduced;
}

// ---------------------------------------------------------
// 7. Installer
// ---------------------------------------------------------
static void install(void *h) {
#define S(n) xdl_sym(h, n, nullptr)
    auto is_vm           = (bool  (*)(void *))              S("il2cpp_is_vm_thread");
    auto domain_get      = (void *(*)())                    S("il2cpp_domain_get");
    auto attach          = (void *(*)(void *))              S("il2cpp_thread_attach");
    auto get_asms        = (void **(*)(void *, size_t *))   S("il2cpp_domain_get_assemblies");
    auto get_image       = (void *(*)(void *))              S("il2cpp_assembly_get_image");
    auto image_name      = (const char *(*)(void *))        S("il2cpp_image_get_name");
    auto class_from_name = (void *(*)(void *, const char *, const char *)) S("il2cpp_class_from_name");
    auto method_by_name  = (void *(*)(void *, const char *, int))          S("il2cpp_class_get_method_from_name");
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

    // --- HurtHp ---
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
#if defined(__aarch64__)
        A64HookFunction(addr, g_repl[slot], (void **) &g_orig[slot]);
        LOGI("hook: %s HurtHp @%p installed", nm, addr);
        slot++;
#endif
    }

#if defined(__aarch64__)
    // --- MallData ---
    {
        void *klass = class_from_name(image, "RGScript.Data.Mall", "MallData");
        if (!klass) {
            LOGE("hook: MallData not found");
        } else {
            void *mi = method_by_name(klass, "BuyItem", 2);
            void *addr = mi ? *(void **) mi : nullptr;
            if (addr) {
                A64HookFunction(addr, (void *) buyitem_hook, (void **) &g_buyitem_orig);
                LOGI("hook: MallData.BuyItem @%p installed", addr);
            } else { LOGE("hook: BuyItem not found"); }

            void *mi_he = method_by_name(klass, "HasEnoughCurrency", 1);
            void *addr_he = mi_he ? *(void **) mi_he : nullptr;
            if (addr_he) {
                A64HookFunction(addr_he, (void *) hasenough_hook, (void **) &g_hasenough_orig);
                LOGI("hook: HasEnoughCurrency @%p installed", addr_he);
            } else { LOGE("hook: HasEnoughCurrency not found"); }
        }
    }

    // --- RGSaveManager ConsumeGem ---
    {
        void *klass = class_from_name(image, "", "RGSaveManager");
        if (!klass) {
            LOGE("hook: RGSaveManager not found");
        } else {
            void *mi = method_by_name(klass, "ConsumeGem", 3);
            void *addr = mi ? *(void **) mi : nullptr;
            if (addr) {
                A64HookFunction(addr, (void *) consumegem_hook, (void **) &g_consumegem_orig);
                LOGI("hook: RGSaveManager.ConsumeGem @%p installed", addr);
            } else { LOGE("hook: ConsumeGem not found"); }
        }
    }

    // --- TalentImpl VoidShield + AddDelayDeadBuff ---
    {
        void *klass = class_from_name(image, "TalentImpl", "BuffImplVoidShield");
        if (!klass) {
            LOGE("hook: TalentImpl.BuffImplVoidShield not found");
        } else {
            void *mi = method_by_name(klass, "OnBeforePlayerConsumeVoidShield", 1);
            void *addr = mi ? *(void **) mi : nullptr;
            if (addr) {
                A64HookFunction(addr, (void *) consume_hook, (void **) &g_consume_orig);
                LOGI("hook: VoidShield consume @%p installed", addr);
            } else { LOGE("hook: OnBeforePlayerConsumeVoidShield not found"); }

            void *mi2 = method_by_name(klass, "AddDelayDeadBuff", 1);
            void *addr2 = mi2 ? *(void **) mi2 : nullptr;
            if (addr2) {
                A64HookFunction(addr2, (void *) adddead_hook, (void **) &g_adddead_orig);
                LOGI("hook: AddDelayDeadBuff @%p installed", addr2);
            } else { LOGE("hook: AddDelayDeadBuff not found"); }
        }
    }

    // --- RoleAttributePlayer: set_energy + get_skillCd ---
    {
        void *klass = class_from_name(image, "", "RoleAttributePlayer");
        if (!klass) {
            LOGE("hook: RoleAttributePlayer not found");
        } else {
            // set_energy — argc 1 (the Int32 value parameter)
            void *mi = method_by_name(klass, "set_energy", 1);
            void *addr = mi ? *(void **) mi : nullptr;
            if (addr) {
                A64HookFunction(addr, (void *) setenergy_hook, (void **) &g_setenergy_orig);
                LOGI("hook: RoleAttributePlayer.set_energy @%p installed", addr);
            } else { LOGE("hook: set_energy not found in RoleAttributePlayer"); }

            // get_skillCd — argc 0 (no explicit parameters)
            void *mi2 = method_by_name(klass, "get_skillCd", 0);
            void *addr2 = mi2 ? *(void **) mi2 : nullptr;
            if (addr2) {
                A64HookFunction(addr2, (void *) getskillcd_hook, (void **) &g_getskillcd_orig);
                LOGI("hook: RoleAttributePlayer.get_skillCd @%p installed", addr2);
            } else { LOGE("hook: get_skillCd not found in RoleAttributePlayer"); }
        }
    }
#endif
}

void hook_start(void *handle) { std::thread(install, handle).detach(); }
