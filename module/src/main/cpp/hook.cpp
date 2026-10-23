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
//          seasoncoin autoattack crit shop goldarmor
// ---------------------------------------------------------
#define CFG_DIR "/storage/emulated/0/Android/data/com.ChillyRoom.DungeonShooter/files/"

// ---------------------------------------------------------
// File log: everything LOGI/LOGE prints is also appended to
// CFG_DIR/ds_log.txt (truncated on every game start), so it can be read
// from Termux without logcat:  cat <CFG_DIR>ds_log.txt
// ---------------------------------------------------------
#include <cstdarg>
#if __has_include(<android/log.h>)
#include <android/log.h>
#define DS_HAVE_ALOG 1
#endif

static void ds_log(int err, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
#ifdef DS_HAVE_ALOG
    va_list ap2;
    va_copy(ap2, ap);
    __android_log_vprint(err ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, "DSHook", fmt, ap2);
    va_end(ap2);
#endif
    FILE *f = fopen(CFG_DIR "ds_log.txt", "a");
    if (f) {
        fputs(err ? "E " : "I ", f);
        vfprintf(f, fmt, ap);
        fputc('\n', f);
        fclose(f);
    }
    va_end(ap);
}
#undef LOGI
#undef LOGE
#define LOGI(...) ds_log(0, __VA_ARGS__)
#define LOGE(...) ds_log(1, __VA_ARGS__)

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
// 10. get_AutoAttack: true while in battle (RGWeapon)
//     Outside battle, returns the game's own value so a real
//     Fully Automatic Device still works.
//     [0] get_AutoAttack (public)  [1] get__autoAttack (protected)
// ---------------------------------------------------------
typedef bool (*GetAutoAttack_t)(void *self, void *mi);
static GetAutoAttack_t g_autoattack_orig[2];

typedef bool (*InBattle_t)(void *mi);
static InBattle_t g_inbattle = nullptr;
static void *g_inbattle_mi = nullptr;

template <int N>
static bool autoattack_hook(void *self, void *mi) {
    static int logged = 0;
    bool orig = g_autoattack_orig[N] ? g_autoattack_orig[N](self, mi) : false;
    bool r = g_inbattle ? (orig || g_inbattle(g_inbattle_mi)) : true;
    if (logged < 10) { logged++; LOGI("autoattack[%d] orig=%d -> %d", N, orig, r); }
    return r;
}

typedef bool (*HasTags_t)(void *self, void *mi);
static HasTags_t g_hastags_orig;

static bool hastags_hook(void *self, void *mi) {
    static int logged = 0;
    bool orig = g_hastags_orig ? g_hastags_orig(self, mi) : false;
    bool r = orig || (g_inbattle && g_inbattle(g_inbattle_mi));
    if (logged < 10) { logged++; LOGI("HasAutoAttackTags orig=%d -> %d", orig, r); }
    return r;
}

typedef void (*M12SetAttack_t)(void *self, bool v, bool hand, void *mi);
static M12SetAttack_t g_m12setattack_orig;

static void m12setattack_hook(void *self, bool v, bool hand, void *mi) {
    static int logged = 0;
    bool inb = g_inbattle && g_inbattle(g_inbattle_mi);
    if (logged < 20) { logged++; LOGI("M12.SetAttack v=%d hand=%d inbattle=%d", v, hand, inb); }
    g_m12setattack_orig(self, inb ? true : v, hand, mi);
}

// ---------------------------------------------------------
// 11. Always Crit (not tied to the Precise Strike buff)
//     DamageTrigger.HandleCritical is the roll. Trigger fields:
//     critical @0x40, camp @0x64. Camp.Character == 1.
//     DealDamage is a fallback: forces isCrit for player-camp damage.
// ---------------------------------------------------------
static const int CAMP_CHARACTER = 1;

typedef void (*HandleCritical_t)(void *self, void *mi);
static HandleCritical_t g_handlecrit_orig;

static void handlecrit_hook(void *self, void *mi) {
    static int logged = 0;
    if (self) {
        int camp = *(int *)((char *)self + 0x64);
        if (camp == CAMP_CHARACTER) {
            int *p_crit = (int *)((char *)self + 0x40);
            if (logged < 20) { logged++; LOGI("HandleCritical: camp=%d critical %d -> 10000", camp, *p_crit); }
            *p_crit = 10000;
        }
    }
    g_handlecrit_orig(self, mi);
}

struct V3 { float x, y, z; };
typedef bool (*DealDamage_t)(void *carrier, int damage, bool isCrit, V3 repel, void *target,
                             void *sourceObj, void *weapon, int sourceCamp, int damageType,
                             int elementalType, int subType, void *mi);
static DealDamage_t g_dealdamage_orig;

static bool dealdamage_hook(void *carrier, int damage, bool isCrit, V3 repel, void *target,
                            void *sourceObj, void *weapon, int sourceCamp, int damageType,
                            int elementalType, int subType, void *mi) {
    static int logged = 0;
    bool crit = (sourceCamp == CAMP_CHARACTER) ? true : isCrit;
    if (logged < 20) { logged++; LOGI("DealDamage: camp=%d isCrit=%d -> %d dmg=%d", sourceCamp, isCrit, crit, damage); }
    return g_dealdamage_orig(carrier, damage, crit, repel, target, sourceObj, weapon,
                             sourceCamp, damageType, elementalType, subType, mi);
}

// ---------------------------------------------------------
// 12. Free Shopping (not tied to the On Sales! buff)
//     BuffStackCfgShopDiscount.GetFinalPriceFactor() is static, no args.
//     Price multiplier forced to 0. Original value is logged: if it reads
//     1.00 without the buff and 0.50 with it, 0 = free is correct.
// ---------------------------------------------------------
typedef float (*ShopFactor_t)(void *mi);
static ShopFactor_t g_shopfactor_orig;

static float shopfactor_hook(void *mi) {
    static int logged = 0;
    float raw = g_shopfactor_orig ? g_shopfactor_orig(mi) : 1.0f;
    if (logged < 20) { logged++; LOGI("ShopDiscount.GetFinalPriceFactor: %.2f -> 0.00", raw); }
    return 0.0f;
}

// ---------------------------------------------------------
// 13. Golden Armor+ (not tied to the buff)
//     Every GOLD_STEP coins owned = +1 max HP and +1 max armor, no cap.
//     Coin count is captured from CoinChangeEvent.curCoinVal (@0x10) in
//     RoleAttributePlayer.OnCoinChange and from UpdateArmor(coinVal).
//     The bonus is added in the max HP / max armor getters.
// ---------------------------------------------------------
static const int GOLD_STEP = 10;
static volatile int g_coin = 0;

static int gold_bonus() { int c = g_coin; return c > 0 ? c / GOLD_STEP : 0; }

typedef void (*OnCoinChange_t)(void *self, void *ev, void *mi);
static OnCoinChange_t g_oncoin_orig;

static void oncoin_hook(void *self, void *ev, void *mi) {
    static int logged = 0;
    if (ev) {
        int cur = *(int *)((char *)ev + 0x10);
        if (cur >= 0) g_coin = cur;
        if (logged < 20) { logged++; LOGI("OnCoinChange: curCoinVal=%d (+%d)", cur, gold_bonus()); }
    }
    g_oncoin_orig(self, ev, mi);
}

typedef void (*UpdateArmorInt_t)(void *self, int coinVal, void *mi);
static UpdateArmorInt_t g_updarmor_orig;

static void updarmor_hook(void *self, int coinVal, void *mi) {
    static int logged = 0;
    if (coinVal >= 0) g_coin = coinVal;
    if (logged < 20) { logged++; LOGI("UpdateArmor(coinVal=%d)", coinVal); }
    g_updarmor_orig(self, coinVal, mi);
}

typedef int (*GetInt_t)(void *self, void *mi);
static GetInt_t g_getmaxhp_orig;
static GetInt_t g_getmaxarmor_orig;

static int getmaxhp_hook(void *self, void *mi) {
    static int logged = 0;
    int r = g_getmaxhp_orig(self, mi);
    int b = gold_bonus();
    if (b > 0 && logged < 20) { logged++; LOGI("max_hp %d -> %d", r, r + b); }
    return r + b;
}

static int getmaxarmor_hook(void *self, void *mi) {
    static int logged = 0;
    int r = g_getmaxarmor_orig(self, mi);
    int b = gold_bonus();
    if (r <= 0 || b <= 0) return r;   // characters without armor stay without
    if (logged < 20) { logged++; LOGI("max_armor %d -> %d", r, r + b); }
    return r + b;
}

// ---------------------------------------------------------
// 14. Installer
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
    { FILE *lf = fopen(CFG_DIR "ds_log.txt", "w"); if (lf) fclose(lf); }
    LOGI("install: start");
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
    {
        void *gu = class_from_name(image, "", "GameUtil");
        void *imi = gu ? method_by_name(gu, "InBattleState", 0) : nullptr;
        if (imi && *(void **) imi) {
            g_inbattle_mi = imi;
            g_inbattle = (InBattle_t) *(void **) imi;
            LOGI("InBattleState resolved @%p", (void *) g_inbattle);
        } else LOGE("InBattleState not found, autoattack falls back to always-true");
    }

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
    if (on("autoattack")) {
        hook(image, "", "RGWeapon", "get_AutoAttack", 0,
             (void *) autoattack_hook<0>, (void **) &g_autoattack_orig[0]);
        hook(image, "", "RGWeapon", "get__autoAttack", 0,
             (void *) autoattack_hook<1>, (void **) &g_autoattack_orig[1]);
        hook(image, "", "RGWeapon", "get_HasAutoAttackTags", 0,
             (void *) hastags_hook, (void **) &g_hastags_orig);
        hook(image, "", "MythicWeapon12", "SetAttack", 2,
             (void *) m12setattack_hook, (void **) &g_m12setattack_orig);
    }

    // --- crit: always crit for player-camp damage ---
    if (on("crit")) {
        hook(image, "", "DamageTrigger", "HandleCritical", 0,
             (void *) handlecrit_hook, (void **) &g_handlecrit_orig);
        hook(image, "", "DamageCarrierHelper", "DealDamage", 11,
             (void *) dealdamage_hook, (void **) &g_dealdamage_orig);
    }

    // --- shop: price factor forced to 0 ---
    if (on("shop"))
        hook(image, "RGScript.Battle", "BuffStackCfgShopDiscount", "GetFinalPriceFactor", 0,
             (void *) shopfactor_hook, (void **) &g_shopfactor_orig);

    // --- goldarmor: +1 max HP and +1 max armor per GOLD_STEP coins ---
    if (on("goldarmor")) {
        hook(image, "", "RoleAttributePlayer", "OnCoinChange", 1,
             (void *) oncoin_hook, (void **) &g_oncoin_orig);
        hook(image, "", "RoleAttributePlayer", "UpdateArmor", 1,
             (void *) updarmor_hook, (void **) &g_updarmor_orig);
        hook(image, "", "RoleAttributePlayer", "get_max_hp", 0,
             (void *) getmaxhp_hook, (void **) &g_getmaxhp_orig);
        hook(image, "", "RoleAttributePlayer", "get_Max_armor", 0,
             (void *) getmaxarmor_hook, (void **) &g_getmaxarmor_orig);
    }
}

void hook_start(void *handle) { std::thread(install, handle).detach(); }
