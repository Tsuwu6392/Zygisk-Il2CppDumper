#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include "log.h"
#include "xdl.h"

#if defined(__aarch64__)
extern "C" void A64HookFunction(void *symbol, void *replace, void **result);
#endif

// ---------------------------------------------------------
// Config
//   ds_hooks.txt : one "name=0" per line disables that hook group (read once
//                  at startup, restart the game after editing).
//                  Missing file or unlisted name = enabled.
//   sk_cd.txt    : cooldown factor 0.0 - 1.0 (re-read every second).
//   Names: hp shield delaydead mall gem energy cooldown items dizzy
//          seasoncoin autoattack
// ---------------------------------------------------------
#define CFG_DIR "/storage/emulated/0/Android/data/com.ChillyRoom.DungeonShooter/files/"

static std::set<std::string> g_off;

static void load_cfg() {
    FILE *f = fopen(CFG_DIR "ds_hooks.txt", "r");
    if (!f) { LOGE("cfg: cannot open %sds_hooks.txt (all hooks enabled)", CFG_DIR); return; }
    char k[64];
    int v;
    while (fscanf(f, " %63[^=]=%d", k, &v) == 2) {
        if (!v) { g_off.insert(k); LOGI("cfg: %s disabled", k); }
    }
    fclose(f);
}

static bool on(const char *k) { return g_off.find(k) == g_off.end(); }

static float get_cd_factor() {
    static float s_factor = 0.15f;
    static time_t s_last_check = 0;
    time_t now = time(nullptr);
    if (now != s_last_check) {
        s_last_check = now;
        FILE *f = fopen(CFG_DIR "sk_cd.txt", "r");
        if (f) {
            float val = 0.0f;
            if (fscanf(f, "%f", &val) == 1 && val >= 0.0f && val <= 1.0f)
                s_factor = val;
            fclose(f);
        }
    }
    return s_factor;
}

// ---------------------------------------------------------
// 1. HurtHp Hooks (God Mode)
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
// 2. VoidShield Hooks (Infinite Shield + No Instadeath)
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
// 3. Mall Hooks (Free In-Game Purchases)
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
    static int logged = 0;
    if (logged < 20) { logged++; LOGI("HasEnoughCurrency bypassed -> true"); }
    return true;
}

// ---------------------------------------------------------
// 4. RGSaveManager Hooks (Free Gem Purchases Outside Dungeon)
// ---------------------------------------------------------
typedef void (*ConsumeGem_t)(void *self, int amount, bool unk1, bool unk2, void *mi);
static ConsumeGem_t g_consumegem_orig;

static void consumegem_hook(void *self, int amount, bool unk1, bool unk2, void *mi) {
    static int logged = 0;
    if (logged < 20) { logged++; LOGI("ConsumeGem intercepted! Original cost: %d -> Forcing 0", amount); }
    g_consumegem_orig(self, 0, unk1, unk2, mi);
}

// ---------------------------------------------------------
// 5. Energy Hook (RoleAttributePlayer.set_energy)
//    Once energy drops to ENERGY_REFILL or below, set it back to max.
//    No max bump: the game keeps its own limit anyway.
// ---------------------------------------------------------
static const int ENERGY_REFILL = 100;

typedef void (*SetEnergy_t)(void *self, int value, void *mi);
static SetEnergy_t g_setenergy_orig;

static void setenergy_hook(void *self, int value, void *mi) {
    static int logged = 0;
    g_setenergy_orig(self, value, mi);
    if (!self) return;

    int *p_max = (int *)((char *)self + 0x178);
    int *p_cur = (int *)((char *)self + 0x17c);
    int max = *p_max;
    int cur = *p_cur;
    if (max > 0 && cur <= ENERGY_REFILL) {
        *p_cur = max;
        if (logged < 20) { logged++; LOGI("energy: refilled %d -> %d", cur, max); }
    }
}

// ---------------------------------------------------------
// 6. Skill Cooldown Hooks (SkillInfo)
// ---------------------------------------------------------
typedef float (*GetFinalCd_t)(void *self, void *mi);
static GetFinalCd_t g_getfinalcd_orig;

static float getfinalcd_hook(void *self, void *mi) {
    static int logged = 0;
    float raw = g_getfinalcd_orig(self, mi);
    float factor = get_cd_factor();
    float result = raw * factor;
    if (logged < 20) { logged++; LOGI("SkillInfo.get_FinalCooldown: %.2f -> %.2f (factor=%.2f)", raw, result, factor); }
    return result;
}

typedef float (*CalcFinalCd_t)(void *self, float cd, void *mi);
static CalcFinalCd_t g_calcfinalcd_orig;

static float calcfinalcd_hook(void *self, float cd, void *mi) {
    static int logged = 0;
    float raw = g_calcfinalcd_orig(self, cd, mi);
    float factor = get_cd_factor();
    float result = raw * factor;
    if (logged < 20) { logged++; LOGI("SkillInfo.CalculateFinalCooldown: %.2f -> %.2f (factor=%.2f)", raw, result, factor); }
    return result;
}

// ---------------------------------------------------------
// 7. No Change: Materials, Seeds, Vouchers, Fish Chips (ItemData)
// ---------------------------------------------------------
typedef void (*ConsumeMat3_t)(void *self, void *itemName, int count, bool needRecord, void *mi);
static ConsumeMat3_t g_consumemat3_orig;
static void consumemat3_hook(void *self, void *itemName, int count, bool needRecord, void *mi) {
    static int logged = 0;
    if (logged < 20) { logged++; LOGI("ConsumeMaterial(3) blocked: count=%d", count); }
}

typedef void (*ConsumeMat6_t)(void *self, void *itemName, int count,
                               bool save, bool needRecord, bool fromActivity,
                               void *trackReason, void *mi);
static ConsumeMat6_t g_consumemat6_orig;
static void consumemat6_hook(void *self, void *itemName, int count,
                              bool save, bool needRecord, bool fromActivity,
                              void *trackReason, void *mi) {
    static int logged = 0;
    if (logged < 20) { logged++; LOGI("ConsumeMaterial(6) blocked: count=%d", count); }
}

typedef void (*ConsumeSeed_t)(void *self, void *itemName, void *mi);
static ConsumeSeed_t g_consumeseed_orig;
static void consumeseed_hook(void *self, void *itemName, void *mi) {
    static int logged = 0;
    if (logged < 20) { logged++; LOGI("ConsumeSeed blocked"); }
}

typedef void (*ConsumeTicket_t)(void *self, void *itemName, int count, void *mi);
static ConsumeTicket_t g_consumeticket_orig;
static void consumeticket_hook(void *self, void *itemName, int count, void *mi) {
    static int logged = 0;
    if (logged < 20) { logged++; LOGI("ConsumeTokenTicket blocked: count=%d", count); }
}

typedef void (*ConsumeFishCount_t)(void *self, int count, int obtainType, bool save, void *mi);
static ConsumeFishCount_t g_consumefishcount_orig;
static void consumefishcount_hook(void *self, int count, int obtainType, bool save, void *mi) {
    static int logged = 0;
    if (logged < 20) { logged++; LOGI("ConsumeFishCount blocked: count=%d", count); }
}

// ---------------------------------------------------------
// 8. No Dizzy (DamageCarrierHelper.ProcessPlayerDizzy)
// ---------------------------------------------------------
typedef float (*ProcessPlayerDizzy_t)(void *controller, float dizzyTime, void *buff, void *mi);
static ProcessPlayerDizzy_t g_processplayerdizzy_orig;

static float processplayerdizzy_hook(void *controller, float dizzyTime, void *buff, void *mi) {
    static int logged = 0;
    if (logged < 20) { logged++; LOGI("ProcessPlayerDizzy blocked: dizzyTime=%.2f", dizzyTime); }
    return 0.0f;
}

// ---------------------------------------------------------
// 9. Season Coin: No Deduct + Always Affordable
//    WindowBuyOrWatchAdDialog (RGScript.UI)
// ---------------------------------------------------------
typedef void (*DoConsumeSeasonCoin_t)(void *self, void *mi);
static DoConsumeSeasonCoin_t g_doconsumeseasoncoin_orig;
static void doconsumeseasoncoin_hook(void *self, void *mi) {
    static int logged = 0;
    if (logged < 20) { logged++; LOGI("DoConsumeSeasonCoin blocked: balance preserved"); }
}

typedef void (*DoConsumeGem2_t)(void *self, void *mi);
static DoConsumeGem2_t g_doconsumegem2_orig;
static void doconsumegem2_hook(void *self, void *mi) {
    static int logged = 0;
    if (logged < 20) { logged++; LOGI("DoConsumeGem (dialog) blocked"); }
}

typedef bool (*IsAffordable_t)(void *self, void *mi);
static IsAffordable_t g_isaffordable_orig;
static bool isaffordable_hook(void *self, void *mi) {
    static int logged = 0;
    if (logged < 20) { logged++; LOGI("IsAffordable bypassed -> true"); }
    return true;
}

// ---------------------------------------------------------
// 10. get_AutoAttack Always True (RGWeapon)
//     Global fully automatic for all weapons, no gadget needed.
// ---------------------------------------------------------
typedef bool (*GetAutoAttack_t)(void *self, void *mi);
static GetAutoAttack_t g_getautoattack_orig;
static bool getautoattack_hook(void *self, void *mi) {
    return true;
}

// ---------------------------------------------------------
// 11. Installer
// ---------------------------------------------------------
typedef void *(*ClassFromName_t)(void *, const char *, const char *);
typedef void *(*MethodByName_t)(void *, const char *, int);
static ClassFromName_t class_from_name;
static MethodByName_t  method_by_name;

// Resolve class + method by name, then inline-hook it. Logs on every miss and install.
static bool hook(void *img, const char *ns, const char *cls, const char *m, int argc,
                 void *repl, void **orig) {
    void *k = class_from_name(img, ns, cls);
    if (!k) { LOGE("hook: class %s not found", cls); return false; }
    void *mi = method_by_name(k, m, argc);
    void *addr = mi ? *(void **) mi : nullptr;
    if (!addr) { LOGE("hook: %s.%s(%d) not found", cls, m, argc); return false; }
#if defined(__aarch64__)
    A64HookFunction(addr, repl, orig);
    LOGI("hook: %s.%s @%p installed", cls, m, addr);
    return true;
#else
    return false;
#endif
}

static void install(void *h) {
#define S(n) xdl_sym(h, n, nullptr)
    auto is_vm           = (bool  (*)(void *))              S("il2cpp_is_vm_thread");
    auto domain_get      = (void *(*)())                    S("il2cpp_domain_get");
    auto attach          = (void *(*)(void *))              S("il2cpp_thread_attach");
    auto get_asms        = (void **(*)(void *, size_t *))   S("il2cpp_domain_get_assemblies");
    auto get_image       = (void *(*)(void *))              S("il2cpp_assembly_get_image");
    auto image_name      = (const char *(*)(void *))        S("il2cpp_image_get_name");
    class_from_name      = (ClassFromName_t)                S("il2cpp_class_from_name");
    method_by_name       = (MethodByName_t)                 S("il2cpp_class_get_method_from_name");
#undef S

    if (!domain_get || !attach || !get_asms || !get_image || !image_name || !class_from_name || !method_by_name) {
        LOGE("hook: il2cpp api missing"); return;
    }
    load_cfg();

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

    // --- hp: HurtHp (base + subclass overrides, deduped by address) ---
    if (on("hp")) {
        const char *hurtnames[3] = {"RGController", "C12Controller", "C13Controller"};
        std::set<void *> done;
        int slot = 0;
        for (auto nm : hurtnames) {
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
    }

    // --- shield: infinite void shield ---
    if (on("shield"))
        hook(image, "TalentImpl", "BuffImplVoidShield", "OnBeforePlayerConsumeVoidShield", 1,
             (void *) consume_hook, (void **) &g_consume_orig);

    // --- delaydead: block the second-life countdown ---
    if (on("delaydead"))
        hook(image, "TalentImpl", "BuffImplVoidShield", "AddDelayDeadBuff", 1,
             (void *) adddead_hook, (void **) &g_adddead_orig);

    // --- mall: free in-game purchases ---
    if (on("mall")) {
        hook(image, "RGScript.Data.Mall", "MallData", "BuyItem", 2,
             (void *) buyitem_hook, (void **) &g_buyitem_orig);
        hook(image, "RGScript.Data.Mall", "MallData", "HasEnoughCurrency", 1,
             (void *) hasenough_hook, (void **) &g_hasenough_orig);
    }

    // --- gem: free gem purchases outside dungeon ---
    if (on("gem"))
        hook(image, "", "RGSaveManager", "ConsumeGem", 3,
             (void *) consumegem_hook, (void **) &g_consumegem_orig);

    // --- energy: refill to max once it drops to ENERGY_REFILL ---
    if (on("energy"))
        hook(image, "", "RoleAttributePlayer", "set_energy", 1,
             (void *) setenergy_hook, (void **) &g_setenergy_orig);

    // --- cooldown: scaled by sk_cd.txt ---
    if (on("cooldown")) {
        hook(image, "", "SkillInfo", "get_FinalCooldown", 0,
             (void *) getfinalcd_hook, (void **) &g_getfinalcd_orig);
        hook(image, "", "SkillInfo", "CalculateFinalCooldown", 1,
             (void *) calcfinalcd_hook, (void **) &g_calcfinalcd_orig);
    }

    // --- items: materials, seeds, vouchers, fish chips never consumed ---
    if (on("items")) {
        hook(image, "", "ItemData", "ConsumeMaterial", 3,
             (void *) consumemat3_hook, (void **) &g_consumemat3_orig);
        hook(image, "", "ItemData", "ConsumeMaterial", 6,
             (void *) consumemat6_hook, (void **) &g_consumemat6_orig);
        hook(image, "", "ItemData", "ConsumeSeed", 1,
             (void *) consumeseed_hook, (void **) &g_consumeseed_orig);
        hook(image, "", "ItemData", "ConsumeTokenTicket", 2,
             (void *) consumeticket_hook, (void **) &g_consumeticket_orig);
        hook(image, "", "ItemData", "ConsumeFishCount", 3,
             (void *) consumefishcount_hook, (void **) &g_consumefishcount_orig);
    }

    // --- dizzy: no dizzy ---
    if (on("dizzy"))
        hook(image, "", "DamageCarrierHelper", "ProcessPlayerDizzy", 3,
             (void *) processplayerdizzy_hook, (void **) &g_processplayerdizzy_orig);

    // --- seasoncoin: no deduct + always affordable ---
    if (on("seasoncoin")) {
        hook(image, "RGScript.UI", "WindowBuyOrWatchAdDialog", "DoConsumeSeasonCoin", 0,
             (void *) doconsumeseasoncoin_hook, (void **) &g_doconsumeseasoncoin_orig);
        hook(image, "RGScript.UI", "WindowBuyOrWatchAdDialog", "DoConsumeGem", 0,
             (void *) doconsumegem2_hook, (void **) &g_doconsumegem2_orig);
        hook(image, "RGScript.UI", "WindowBuyOrWatchAdDialog", "IsAffordable", 0,
             (void *) isaffordable_hook, (void **) &g_isaffordable_orig);
    }

    // --- autoattack: all weapons fully automatic ---
    if (on("autoattack"))
        hook(image, "", "RGWeapon", "get_AutoAttack", 0,
             (void *) getautoattack_hook, (void **) &g_getautoattack_orig);
}

void hook_start(void *handle) { std::thread(install, handle).detach(); }
