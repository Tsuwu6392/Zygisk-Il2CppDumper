#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <thread>
#include <unistd.h>
#include "log.h"
#include "xdl.h"

#if defined(__aarch64__)
extern "C" void A64HookFunction(void *symbol, void *replace, void **result);
#endif

// ---------------------------------------------------------
// Live Config Reader: /data/local/tmp/sk_cd.txt
// ---------------------------------------------------------
static float get_cd_factor() {
    static float s_factor = 0.15f;
    static time_t s_last_check = 0;
    time_t now = time(nullptr);
    if (now != s_last_check) {
        s_last_check = now;
        FILE *f = fopen("/data/local/tmp/sk_cd.txt", "r");
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
    LOGI("HasEnoughCurrency bypassed -> true");
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
// 5. Energy Hooks (RoleAttributePlayer)
// ---------------------------------------------------------
static const int ENERGY_BONUS  = 1000;
static const int ENERGY_REFILL = 100;

typedef void (*SetUpChar_t)(void *self, void *mi);
static SetUpChar_t g_setupchar_orig;

static void setupchar_hook(void *self, void *mi) {
    g_setupchar_orig(self, mi);
    if (!self) return;
    int *p_max = (int *)((char *)self + 0x178);
    int cur_max = *p_max;
    if (cur_max > 0 && cur_max < 10000) {
        *p_max = cur_max + ENERGY_BONUS;
        LOGI("energy: SetUpChar max %d -> %d", cur_max, *p_max);
    }
}

typedef void (*SetEnergy_t)(void *self, int value, void *mi);
static SetEnergy_t g_setenergy_orig;

static void setenergy_hook(void *self, int value, void *mi) {
    static int logged = 0;
    if (!self) { g_setenergy_orig(self, value, mi); return; }

    int *p_max = (int *)((char *)self + 0x178);
    int *p_cur = (int *)((char *)self + 0x17c);

    int cur_max = *p_max;
    if (cur_max > 0 && cur_max < 10000) {
        *p_max = cur_max + ENERGY_BONUS;
        if (logged < 20) { logged++; LOGI("energy: set_energy re-bumped max %d -> %d", cur_max, *p_max); }
    }

    g_setenergy_orig(self, value, mi);

    int after = *p_cur;
    int new_max = *p_max;
    if (after <= ENERGY_REFILL && new_max > 0) {
        *p_cur = new_max;
        if (logged < 20) { logged++; LOGI("energy: refilled %d -> %d (max=%d)", after, *p_cur, new_max); }
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
        LOGE("hook: il2cpp api missing"); return;
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

#if defined(__aarch64__)
    // --- MallData ---
    {
        void *klass = class_from_name(image, "RGScript.Data.Mall", "MallData");
        if (!klass) { LOGE("hook: MallData not found"); } else {
            void *mi = method_by_name(klass, "BuyItem", 2);
            void *addr = mi ? *(void **) mi : nullptr;
            if (addr) { A64HookFunction(addr, (void *) buyitem_hook, (void **) &g_buyitem_orig); LOGI("hook: MallData.BuyItem @%p installed", addr); }
            else { LOGE("hook: BuyItem not found"); }

            void *mi_he = method_by_name(klass, "HasEnoughCurrency", 1);
            void *addr_he = mi_he ? *(void **) mi_he : nullptr;
            if (addr_he) { A64HookFunction(addr_he, (void *) hasenough_hook, (void **) &g_hasenough_orig); LOGI("hook: HasEnoughCurrency @%p installed", addr_he); }
            else { LOGE("hook: HasEnoughCurrency not found"); }
        }
    }

    // --- RGSaveManager ConsumeGem ---
    {
        void *klass = class_from_name(image, "", "RGSaveManager");
        if (!klass) { LOGE("hook: RGSaveManager not found"); } else {
            void *mi = method_by_name(klass, "ConsumeGem", 3);
            void *addr = mi ? *(void **) mi : nullptr;
            if (addr) { A64HookFunction(addr, (void *) consumegem_hook, (void **) &g_consumegem_orig); LOGI("hook: RGSaveManager.ConsumeGem @%p installed", addr); }
            else { LOGE("hook: ConsumeGem not found"); }
        }
    }

    // --- TalentImpl VoidShield + AddDelayDeadBuff ---
    {
        void *klass = class_from_name(image, "TalentImpl", "BuffImplVoidShield");
        if (!klass) { LOGE("hook: TalentImpl.BuffImplVoidShield not found"); } else {
            void *mi = method_by_name(klass, "OnBeforePlayerConsumeVoidShield", 1);
            void *addr = mi ? *(void **) mi : nullptr;
            if (addr) { A64HookFunction(addr, (void *) consume_hook, (void **) &g_consume_orig); LOGI("hook: VoidShield consume @%p installed", addr); }
            else { LOGE("hook: OnBeforePlayerConsumeVoidShield not found"); }

            void *mi2 = method_by_name(klass, "AddDelayDeadBuff", 1);
            void *addr2 = mi2 ? *(void **) mi2 : nullptr;
            if (addr2) { A64HookFunction(addr2, (void *) adddead_hook, (void **) &g_adddead_orig); LOGI("hook: AddDelayDeadBuff @%p installed", addr2); }
            else { LOGE("hook: AddDelayDeadBuff not found"); }
        }
    }

    // --- RoleAttributePlayer: SetUpChar + set_energy ---
    {
        void *klass = class_from_name(image, "", "RoleAttributePlayer");
        if (!klass) { LOGE("hook: RoleAttributePlayer not found"); } else {
            void *mi0 = method_by_name(klass, "SetUpChar", 0);
            void *addr0 = mi0 ? *(void **) mi0 : nullptr;
            if (addr0) { A64HookFunction(addr0, (void *) setupchar_hook, (void **) &g_setupchar_orig); LOGI("hook: RoleAttributePlayer.SetUpChar @%p installed", addr0); }
            else { LOGE("hook: SetUpChar not found in RoleAttributePlayer"); }

            void *mi1 = method_by_name(klass, "set_energy", 1);
            void *addr1 = mi1 ? *(void **) mi1 : nullptr;
            if (addr1) { A64HookFunction(addr1, (void *) setenergy_hook, (void **) &g_setenergy_orig); LOGI("hook: RoleAttributePlayer.set_energy @%p installed", addr1); }
            else { LOGE("hook: set_energy not found in RoleAttributePlayer"); }
        }
    }

    // --- SkillInfo: get_FinalCooldown + CalculateFinalCooldown ---
    {
        void *klass = class_from_name(image, "", "SkillInfo");
        if (!klass) { LOGE("hook: SkillInfo not found"); } else {
            void *mi_final = method_by_name(klass, "get_FinalCooldown", 0);
            void *addr_final = mi_final ? *(void **) mi_final : nullptr;
            if (addr_final) { A64HookFunction(addr_final, (void *) getfinalcd_hook, (void **) &g_getfinalcd_orig); LOGI("hook: SkillInfo.get_FinalCooldown @%p installed", addr_final); }
            else { LOGE("hook: get_FinalCooldown not found in SkillInfo"); }

            void *mi_calc = method_by_name(klass, "CalculateFinalCooldown", 1);
            void *addr_calc = mi_calc ? *(void **) mi_calc : nullptr;
            if (addr_calc) { A64HookFunction(addr_calc, (void *) calcfinalcd_hook, (void **) &g_calcfinalcd_orig); LOGI("hook: SkillInfo.CalculateFinalCooldown @%p installed", addr_calc); }
            else { LOGE("hook: CalculateFinalCooldown not found in SkillInfo"); }
        }
    }

    // --- ItemData: Materials, Seeds, Vouchers, Fish Chips (All No-Op) ---
    {
        void *klass = class_from_name(image, "", "ItemData");
        if (!klass) { LOGE("hook: ItemData not found"); } else {
            void *mi3 = method_by_name(klass, "ConsumeMaterial", 3);
            void *addr3 = mi3 ? *(void **) mi3 : nullptr;
            if (addr3) { A64HookFunction(addr3, (void *) consumemat3_hook, (void **) &g_consumemat3_orig); LOGI("hook: ItemData.ConsumeMaterial(3) @%p installed", addr3); }
            else { LOGE("hook: ConsumeMaterial(3) not found in ItemData"); }

            void *mi6 = method_by_name(klass, "ConsumeMaterial", 6);
            void *addr6 = mi6 ? *(void **) mi6 : nullptr;
            if (addr6) { A64HookFunction(addr6, (void *) consumemat6_hook, (void **) &g_consumemat6_orig); LOGI("hook: ItemData.ConsumeMaterial(6) @%p installed", addr6); }
            else { LOGE("hook: ConsumeMaterial(6) not found in ItemData"); }

            void *mi_seed = method_by_name(klass, "ConsumeSeed", 1);
            void *addr_seed = mi_seed ? *(void **) mi_seed : nullptr;
            if (addr_seed) { A64HookFunction(addr_seed, (void *) consumeseed_hook, (void **) &g_consumeseed_orig); LOGI("hook: ItemData.ConsumeSeed @%p installed", addr_seed); }
            else { LOGE("hook: ConsumeSeed not found in ItemData"); }

            void *mi_ticket = method_by_name(klass, "ConsumeTokenTicket", 2);
            void *addr_ticket = mi_ticket ? *(void **) mi_ticket : nullptr;
            if (addr_ticket) { A64HookFunction(addr_ticket, (void *) consumeticket_hook, (void **) &g_consumeticket_orig); LOGI("hook: ItemData.ConsumeTokenTicket @%p installed", addr_ticket); }
            else { LOGE("hook: ConsumeTokenTicket not found in ItemData"); }

            void *mi_fish = method_by_name(klass, "ConsumeFishCount", 3);
            void *addr_fish = mi_fish ? *(void **) mi_fish : nullptr;
            if (addr_fish) { A64HookFunction(addr_fish, (void *) consumefishcount_hook, (void **) &g_consumefishcount_orig); LOGI("hook: ItemData.ConsumeFishCount @%p installed", addr_fish); }
            else { LOGE("hook: ConsumeFishCount not found in ItemData"); }
        }
    }

    // --- DamageCarrierHelper: No Dizzy ---
    {
        void *klass = class_from_name(image, "", "DamageCarrierHelper");
        if (!klass) { LOGE("hook: DamageCarrierHelper not found"); } else {
            void *mi = method_by_name(klass, "ProcessPlayerDizzy", 3);
            void *addr = mi ? *(void **) mi : nullptr;
            if (addr) { A64HookFunction(addr, (void *) processplayerdizzy_hook, (void **) &g_processplayerdizzy_orig); LOGI("hook: DamageCarrierHelper.ProcessPlayerDizzy @%p installed", addr); }
            else { LOGE("hook: ProcessPlayerDizzy not found in DamageCarrierHelper"); }
        }
    }

    // --- WindowBuyOrWatchAdDialog: Season Coin No Deduct + Always Affordable ---
    {
        void *klass = class_from_name(image, "RGScript.UI", "WindowBuyOrWatchAdDialog");
        if (!klass) { LOGE("hook: WindowBuyOrWatchAdDialog not found"); } else {
            void *mi_sc = method_by_name(klass, "DoConsumeSeasonCoin", 0);
            void *addr_sc = mi_sc ? *(void **) mi_sc : nullptr;
            if (addr_sc) { A64HookFunction(addr_sc, (void *) doconsumeseasoncoin_hook, (void **) &g_doconsumeseasoncoin_orig); LOGI("hook: DoConsumeSeasonCoin @%p installed", addr_sc); }
            else { LOGE("hook: DoConsumeSeasonCoin not found"); }

            void *mi_gem = method_by_name(klass, "DoConsumeGem", 0);
            void *addr_gem = mi_gem ? *(void **) mi_gem : nullptr;
            if (addr_gem) { A64HookFunction(addr_gem, (void *) doconsumegem2_hook, (void **) &g_doconsumegem2_orig); LOGI("hook: DoConsumeGem (dialog) @%p installed", addr_gem); }
            else { LOGE("hook: DoConsumeGem (dialog) not found"); }

            void *mi_af = method_by_name(klass, "IsAffordable", 0);
            void *addr_af = mi_af ? *(void **) mi_af : nullptr;
            if (addr_af) { A64HookFunction(addr_af, (void *) isaffordable_hook, (void **) &g_isaffordable_orig); LOGI("hook: IsAffordable @%p installed", addr_af); }
            else { LOGE("hook: IsAffordable not found"); }
        }
    }

    // --- RGWeapon: get_AutoAttack Always True ---
    {
        void *klass = class_from_name(image, "", "RGWeapon");
        if (!klass) { LOGE("hook: RGWeapon not found"); } else {
            void *mi = method_by_name(klass, "get_AutoAttack", 0);
            void *addr = mi ? *(void **) mi : nullptr;
            if (addr) { A64HookFunction(addr, (void *) getautoattack_hook, (void **) &g_getautoattack_orig); LOGI("hook: RGWeapon.get_AutoAttack @%p installed", addr); }
            else { LOGE("hook: get_AutoAttack not found in RGWeapon"); }
        }
    }

#endif
}

void hook_start(void *handle) { std::thread(install, handle).detach(); }
