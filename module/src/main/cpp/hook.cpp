#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/mman.h>
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
//          seasoncoin autoattack crit shop goldarmor lockedlevel saleday
// ---------------------------------------------------------
#define CFG_DIR "/storage/emulated/0/Android/data/com.ChillyRoom.DungeonShooter/files/"

// ---------------------------------------------------------
// File log: everything LOGI/LOGE prints is also appended to two files
// (truncated on every game start): CFG_DIR/ds_log.txt and the game's
// internal data dir /data/data/<pkg>/ds_log.txt. Read from Termux with:
//   su -c 'cat /data/data/com.ChillyRoom.DungeonShooter/ds_log.txt'
// ---------------------------------------------------------
#include <cstdarg>
#if __has_include(<android/log.h>)
#include <android/log.h>
#define DS_HAVE_ALOG 1
#endif

#define LOG_PATH_EXT CFG_DIR "ds_log.txt"
#define LOG_PATH_INT "/data/data/com.ChillyRoom.DungeonShooter/ds_log.txt"

static void ds_log_file(const char *path, int err, const char *fmt, va_list ap) {
    FILE *f = fopen(path, "a");
    if (!f) return;
    fputs(err ? "E " : "I ", f);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fclose(f);
}

static void ds_log(int err, const char *fmt, ...) {
    va_list ap;
#ifdef DS_HAVE_ALOG
    va_start(ap, fmt);
    __android_log_vprint(err ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, "DSHook", fmt, ap);
    va_end(ap);
#endif
    va_start(ap, fmt);
    ds_log_file(LOG_PATH_EXT, err, fmt, ap);
    va_end(ap);
    va_start(ap, fmt);
    ds_log_file(LOG_PATH_INT, err, fmt, ap);
    va_end(ap);
}
#undef LOGI
#undef LOGE
#define LOGI(...) ds_log(0, __VA_ARGS__)
#define LOGE(...) ds_log(1, __VA_ARGS__)

static std::set<std::string> g_off;
static std::map<std::string, int> g_val;

static void load_cfg() {
    FILE *f = fopen(CFG_DIR "ds_hooks.txt", "r");
    if (!f) { LOGE("cfg: cannot open %sds_hooks.txt (all hooks enabled)", CFG_DIR); return; }
    char k[64];
    int v;
    while (fscanf(f, " %63[^=]=%d", k, &v) == 2) {
        g_val[k] = v;
        if (!v) { g_off.insert(k); LOGI("cfg: %s disabled", k); }
    }
    fclose(f);
}

static bool on(const char *k) { return g_off.find(k) == g_off.end(); }

// Optional stage number for a group: "name=N" in ds_hooks.txt (0 = off as before).
// Missing entry = def. Used to bisect which step of a group breaks something.
static int stage(const char *k, int def) {
    auto it = g_val.find(k);
    return it == g_val.end() ? def : it->second;
}

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
// Field offsets. Each default is the value from the current dump; the
// installer overwrites it with the offset looked up BY NAME at runtime
// (il2cpp_class_get_field_from_name + il2cpp_field_get_offset), so a game
// update that shifts a field cannot silently corrupt memory. If the lookup
// fails, the default is kept and an E line is logged.
// ---------------------------------------------------------
static int g_off_itemprice  = 0x24;   // MallItem.itemPrice
static int g_off_finalprice = 0x28;   // MallItem.finalPrice
static int g_off_maxenergy  = 0x178;  // RoleAttributePlayer.max_energy
static int g_off_energy     = 0x17c;  // RoleAttributePlayer._energy
static int g_off_critical   = 0x40;   // DamageTrigger.critical
static int g_off_camp       = 0x64;   // DamageTrigger.camp
static int g_off_curcoin    = 0x10;   // CoinChangeEvent.curCoinVal

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

// The game keeps the real count in runtime data, keyed by _netId (@0x20).
// SyncShieldCountFromRuntimeData reads it and overwrites _shieldCount (@0x18);
// SaveShieldCount writes @0x18 into it. Restoring only the field gets undone
// by the next sync, so after restoring we call SaveShieldCount to put the
// restored count back into the runtime data.
typedef void (*SaveShield_t)(void *self, void *mi);
static SaveShield_t g_saveshield = nullptr;
static int g_shield_off = 0x18;   // _shieldCount, resolved by name at install
static void *g_saveshield_mi = nullptr;

static bool consume_hook(void *self, void *ev, void *mi) {
    static int logged = 0;
    int before = self ? *(int *)((char *)self + g_shield_off) : 0;
    bool r = g_consume_orig(self, ev, mi);
    if (self && before > 0) {
        int after = *(int *)((char *)self + g_shield_off);
        *(int *)((char *)self + g_shield_off) = before;
        if (after != before && g_saveshield) g_saveshield(self, g_saveshield_mi);
        if (logged < 20) { logged++; LOGI("VoidShield consume: count %d -> %d, restored to %d (saved=%d)", before, after, before, (int) (g_saveshield != nullptr)); }
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
// BuyItem returns MallPayResult. R16 passes back whatever the original returned
// in x0/x1 (covers enum, reference and small struct), so the result is never lost.
struct R16 { uint64_t a, b; };
typedef R16 (*BuyItem_t)(void *self, void *mallItem, void *onDone, void *mi);
static BuyItem_t g_buyitem_orig;

static R16 buyitem_hook(void *self, void *mallItem, void *onDone, void *mi) {
    static int logged = 0;
    if (logged < 20) { logged++; LOGI("BuyItem hit! Forcing price to 0."); }
    if (mallItem) {
        *(int *)((char *)mallItem + g_off_itemprice) = 0;
        *(int *)((char *)mallItem + g_off_finalprice) = 0;
    }
    return g_buyitem_orig(self, mallItem, onDone, mi);
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
typedef bool (*ConsumeGem_t)(void *self, int amount, bool unk1, bool unk2, void *mi);
static ConsumeGem_t g_consumegem_orig;

// ConsumeGem returns Boolean (success); the caller checks it.
static bool consumegem_hook(void *self, int amount, bool unk1, bool unk2, void *mi) {
    static int logged = 0;
    if (logged < 20) { logged++; LOGI("ConsumeGem intercepted! Original cost: %d -> Forcing 0", amount); }
    g_consumegem_orig(self, 0, unk1, unk2, mi);
    return true;
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

    int *p_max = (int *)((char *)self + g_off_maxenergy);
    int *p_cur = (int *)((char *)self + g_off_energy);
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
// get_FinalCooldown calls CalculateFinalCooldown inside (the log showed the
// raw value of the first equal to the hooked result of the second), so scaling
// both would apply the factor twice. A thread-local flag makes it apply once.
static thread_local bool g_cd_applied = false;

typedef float (*GetFinalCd_t)(void *self, void *mi);
static GetFinalCd_t g_getfinalcd_orig;

static float getfinalcd_hook(void *self, void *mi) {
    static int logged = 0;
    g_cd_applied = false;
    float raw = g_getfinalcd_orig(self, mi);
    float factor = get_cd_factor();
    float result = g_cd_applied ? raw : raw * factor;
    if (logged < 20) { logged++; LOGI("SkillInfo.get_FinalCooldown: %.2f -> %.2f (factor=%.2f, inner=%d)", raw, result, factor, (int) g_cd_applied); }
    g_cd_applied = false;
    return result;
}

typedef float (*CalcFinalCd_t)(void *self, float cd, void *mi);
static CalcFinalCd_t g_calcfinalcd_orig;

static float calcfinalcd_hook(void *self, float cd, void *mi) {
    static int logged = 0;
    float raw = g_calcfinalcd_orig(self, cd, mi);
    float factor = get_cd_factor();
    float result = raw * factor;
    g_cd_applied = true;
    if (logged < 20) { logged++; LOGI("SkillInfo.CalculateFinalCooldown: %.2f -> %.2f (factor=%.2f)", raw, result, factor); }
    return result;
}

// ---------------------------------------------------------
// 7. No Change: Materials, Seeds, Vouchers, Fish Chips (ItemData)
// ---------------------------------------------------------
// ConsumeMaterial has 4 overloads (string x2, Dictionary x2); all are blocked.
static void consumemat_hook(void *self) {
    static int logged = 0;
    if (logged < 20) { logged++; LOGI("ConsumeMaterial blocked"); }
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
//     DamageTrigger.HandleCritical is the roll. Trigger fields
//     critical and camp are resolved by name (g_off_critical / g_off_camp).
//     Camp.Character == 1.
//     DealDamage is a fallback: forces isCrit for player-camp damage.
// ---------------------------------------------------------
static const int CAMP_CHARACTER = 1;

typedef void (*HandleCritical_t)(void *self, void *mi);
static HandleCritical_t g_handlecrit_orig;

static void handlecrit_hook(void *self, void *mi) {
    static int logged = 0;
    if (self) {
        int camp = *(int *)((char *)self + g_off_camp);
        if (camp == CAMP_CHARACTER) {
            int *p_crit = (int *)((char *)self + g_off_critical);
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
// 12. Free Shopping (not tied to the buff)
//     Price code does: if (BattleData.HasBuff(ExpertShop=10))
//         price = ceil(price * 0.5 * DataUtil.CalBuffExpertShopExtraRate(..)).
//     1) CalBuffExpertShopExtraRate (called only from price code) returns 0.
//     2) BattleData.HasBuff(10) returns true, but only when its caller sits
//        inside one of the price functions, before their rate call. Every
//        other HasBuff call anywhere in the game gets the real answer.
//     Everything is resolved by class/method name at runtime (no RVAs), so
//     it survives updates. A site is used only if a bl to the helper is
//     found in its first 0x400 bytes; the found offsets are logged.
// ---------------------------------------------------------
static const int BUFF_EXPERTSHOP = 10;

// Price functions, found by name. Each is scanned at install for its first bl
// to the rate helper; HasBuff(10) is forced only for callers before that bl.
struct PriceName { const char *cls; const char *m; };
static const PriceName k_sites[] = {
    {"ItemSellerDoctor",       "GetItemValue"},
    {"ItemSellPlaceAttrbute",  "GetItemValue"},
    {"ItemSellPlaceBallBuff",  "GetItemValue"},
    {"ObjectBuffman",          "Start"},
    {"ObjectSlotInGame",       "Start"},
    {"ObjectVendingMachine",   "Start"},
    {"RGContainer",            "GetItemValue"},
    {"RGContainer",            "CalculateItemValue"},
    {"TalkMercenaryCoinRobot", "GetItemValue"},
    {"TalkStatue",             "Start"},
    {"ContainerItemBase",      "GetItemValue"},
    {"GunCoin",                "DoConsume"},
};
static uintptr_t g_pr_lo[16], g_pr_hi[16];
static int g_pr_n = 0;

static uintptr_t bl_target(uintptr_t at) {
    uint32_t ins = *(volatile uint32_t *) at;
    if ((ins & 0xFC000000u) != 0x94000000u) return 0;
    int32_t imm = (int32_t)((ins & 0x03FFFFFFu) << 6) >> 6;
    return at + (intptr_t) imm * 4;
}

// Price code ends with  cmp Wn,#1 ; csinc Wd,Wn,wzr,gt  = max(1, price).
// That clamp makes the cheapest possible price 1 (2 after the factor-106
// doubling), so a rate of 0 alone is not free. The csinc is replaced by
// mov Wd,Wn so 0 passes through.
static bool patch_u32(uintptr_t at, uint32_t val) {
    uintptr_t pg = at & ~(uintptr_t) 4095;
    if (mprotect((void *) pg, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return false;
    *(volatile uint32_t *) at = val;
    __builtin___clear_cache((char *) at, (char *) at + 4);
    mprotect((void *) pg, 4096, PROT_READ | PROT_EXEC);
    return *(volatile uint32_t *) at == val;
}

static bool patch_clamp(uintptr_t rate_call, uintptr_t *where) {
    for (uintptr_t a = rate_call + 8; a < rate_call + 0x100; a += 4) {
        uint32_t ins  = *(volatile uint32_t *) a;
        uint32_t prev = *(volatile uint32_t *) (a - 4);
        if ((ins & 0xFFFFFC00u) == 0x1A9FC400u &&            // csinc Wd, Wn, wzr, gt
            (prev & 0xFFFFFC1Fu) == 0x7100041Fu &&           // cmp Wn, #1
            ((prev >> 5) & 31) == ((ins >> 5) & 31)) {
            uint32_t rn = (ins >> 5) & 31, rd = ins & 31;
            *where = a;
            return patch_u32(a, 0x2A0003E0u | (rn << 16) | rd);   // mov Wd, Wn
        }
    }
    return false;
}

typedef float (*RateFn_t)(float discount, void *mi);
static RateFn_t g_ratefn_orig;

static float ratefn_hook(float discount, void *mi) {
    static int logged = 0;
    if (logged < 20) { logged++; LOGI("CalBuffExpertShopExtraRate(%.2f) -> 0.00", discount); }
    return 0.0f;
}

typedef bool (*HasBuff_t)(void *self, int buff, void *mi);
static HasBuff_t g_hasbuff_orig;

static int g_site_hits[16];

__attribute__((noinline))
static bool hasbuff_hook(void *self, int buff, void *mi) {
    bool r = g_hasbuff_orig(self, buff, mi);
    if (buff != BUFF_EXPERTSHOP || r) return r;
    uintptr_t ra = (uintptr_t) __builtin_return_address(0);
    for (int i = 0; i < g_pr_n; i++) {
        if (ra >= g_pr_lo[i] && ra < g_pr_hi[i]) {
            if (g_site_hits[i] < 3) { g_site_hits[i]++; LOGI("HasBuff(10) -> true at price site %d (ra=%p)", i, (void *) ra); }
            return true;
        }
    }
    return r;
}

// ---------------------------------------------------------
// 13. Golden Armor+ (not tied to the buff)
//     Every GOLD_STEP coins owned = +1 max HP and +1 max armor, no cap.
//     Coin count is captured from CoinChangeEvent.curCoinVal (g_off_curcoin) in
//     RoleAttributePlayer.OnCoinChange and from UpdateArmor(coinVal).
//     The bonus is added in RoleAttributeProxy.get_max_hp / get_MaxArmor
//     (the single source for 330 / 37 callers). Both must return Int32;
//     the installer checks the return type before hooking.
// ---------------------------------------------------------
static const int GOLD_STEP = 10;
static volatile int g_coin = 0;

static int gold_bonus() { int c = g_coin; return c > 0 ? c / GOLD_STEP : 0; }

typedef void (*OnCoinChange_t)(void *self, void *ev, void *mi);
static OnCoinChange_t g_oncoin_orig;

static void oncoin_hook(void *self, void *ev, void *mi) {
    static int logged = 0;
    if (ev) {
        int cur = *(int *)((char *)ev + g_off_curcoin);
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
// 14. Level up locked heroes (e.g. trial-ticket heroes)
//     RGSaveManager.HeroLevelUp / ChooseHeroModel.OnUpgradeButtonClicked have
//     no unlock check (verified in the disassembly). The only gate is
//     ChooseHeroView.RefreshUnlockArea(ChooseHeroData), which ends with
//         _upgradeButton.gameObject.SetActive(
//             data.unlocked && data.level < data.maxLevel && !otherButton.activeSelf)
//     compiled as
//         ldrb w8,[x19,#unlocked] ; mov x21,x0 ; tbz w8,#0,<w1=0>
//         ldp  w8,w9,[x19,#12]    ; cmp w8,w9  ; b.ge <w1=0>
//         ... otherButton.get_gameObject / get_activeSelf ...
//         mvn  w8,w0 ; and w1,w8,#1
//     Hooking that function broke the hero screen (hook never logged a hit),
//     so it is NOT hooked. Two instructions are patched in place instead
//     (same technique as the shop clamp):
//       tbz  -> nop          (locked heroes no longer take the w1=0 path)
//       and  -> mov w1,#1    (ignore the other-button check; the level < max
//                             check stays, so maxed heroes still hide it)
//     Both are found by pattern inside the function, anchored on the
//     ChooseHeroData.unlocked offset resolved by name.
//
//     BISECT RESULT: stage 1 ok, stage 2 (method lookup) breaks. The patch
//     code was never the cause. The stages below split the lookup further.
// ---------------------------------------------------------
static int g_off_cd_unlocked = 0x4;   // ChooseHeroData.unlocked (raw struct offset; il2cpp reports +0x10)

// mode 1 = only tbz->nop, mode 2 = tbz->nop and and->mov w1,#1
static bool patch_lockedlevel(uintptr_t fn, int unlocked_off, int mode, uintptr_t *at_tbz, uintptr_t *at_and) {
    // ldrb w8, [x19, #unlocked_off]
    uint32_t ldrb = 0x39400000u | ((uint32_t) unlocked_off << 10) | (19u << 5) | 8u;
    for (uintptr_t a = fn; a < fn + 0x800; a += 4) {
        if (*(volatile uint32_t *) a != ldrb) continue;
        uintptr_t tbz = a + 8;                                        // ldrb ; mov x21,x0 ; tbz
        uint32_t t = *(volatile uint32_t *) tbz;
        if ((t & 0xFFF8001Fu) != 0x36000008u) continue;               // tbz w8, #0, ...
        for (uintptr_t b = tbz + 4; b < tbz + 0x60; b += 4) {
            if (*(volatile uint32_t *) b != 0x2A2003E8u) continue;    // mvn w8, w0
            if (*(volatile uint32_t *) (b + 4) != 0x12000101u) continue;  // and w1, w8, #1
            uintptr_t pand = b + 4;
            uint32_t old_and = 0x12000101u;
            if (mode >= 2 && !patch_u32(pand, 0x52800021u)) return false;   // mov w1, #1
            if (!patch_u32(tbz, 0xD503201Fu)) {                              // nop
                if (mode >= 2) patch_u32(pand, old_and);
                return false;
            }
            *at_tbz = tbz;
            *at_and = (mode >= 2) ? pand : 0;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------
// 15. Sales Day challenge always active
//     Sales Day is emBattleFactor 316. BattleData.get_IsSaleDay is
//         if (get_IsNormalMode()) return CompareFactor(316); else false
//     and its callers (map structure generation, chest/special room
//     instantiation, GetSpecialRoomCount, the Lua wrapper) all go through
//     it. CompareFactor(factor) is the single per-factor check, so it is
//     hooked: for factor 316 it returns true whenever IsNormalMode is true
//     (the same gate the game uses). Every other factor (Bad Luck = 103
//     and the rest) gets the real answer, so no other challenge changes.
//     Each distinct return address that asks for 316 is logged once, which
//     shows who reads the factor.
// ---------------------------------------------------------
static const int FACTOR_SALEDAY = 316;

typedef bool (*CompareFactor_t)(void *self, int factor, void *mi);
static CompareFactor_t g_comparefactor_orig;

typedef bool (*IsNormalMode_t)(void *self, void *mi);
static IsNormalMode_t g_isnormal = nullptr;
static void *g_isnormal_mi = nullptr;

static thread_local bool g_cf_busy = false;
static uintptr_t g_cf_seen[24];
static int g_cf_nseen = 0;

__attribute__((noinline))
static bool comparefactor_hook(void *self, int factor, void *mi) {
    bool r = g_comparefactor_orig(self, factor, mi);
    if (factor != FACTOR_SALEDAY || r || !self || g_cf_busy) return r;
    g_cf_busy = true;
    bool normal = g_isnormal ? g_isnormal(self, g_isnormal_mi) : true;
    g_cf_busy = false;
    if (!normal) return r;
    uintptr_t ra = (uintptr_t) __builtin_return_address(0);
    bool seen = false;
    for (int i = 0; i < g_cf_nseen; i++) if (g_cf_seen[i] == ra) { seen = true; break; }
    if (!seen && g_cf_nseen < 24) {
        g_cf_seen[g_cf_nseen++] = ra;
        LOGI("saleday: CompareFactor(316) false -> true (ra=%p, site #%d)", (void *) ra, g_cf_nseen);
    }
    return true;
}

// ---------------------------------------------------------
// 16. Installer
// ---------------------------------------------------------
typedef void *(*ClassFromName_t)(void *, const char *, const char *);
typedef void *(*MethodByName_t)(void *, const char *, int);
static ClassFromName_t class_from_name;
static MethodByName_t  method_by_name;

// Resolve class + method by name, then inline-hook it. Logs on every miss and install.
// Class lookup: namespace "" first, then any namespace by plain name.
static size_t (*image_class_count)(void *) = nullptr;
static void *(*image_get_class)(void *, size_t) = nullptr;
static const char *(*class_get_name)(void *) = nullptr;
static void *(*class_get_methods)(void *, void **) = nullptr;
static const char *(*method_get_name)(void *) = nullptr;

static const char *(*class_get_ns)(void *) = nullptr;
static void *(*asm_get_image)(void *) = nullptr;
static const char *(*img_get_name)(void *) = nullptr;
static void **g_asms = nullptr;
static size_t g_nasms = 0;

static void *scan_image(void *img, const char *name) {
    if (!image_class_count || !image_get_class || !class_get_name) return nullptr;
    size_t n = image_class_count(img);
    for (size_t i = 0; i < n; i++) {
        void *c = image_get_class(img, i);
        if (c && !strcmp(class_get_name(c), name)) return c;
    }
    return nullptr;
}

// Class lookup: namespace "" in img, then by plain name in img, then in every assembly.
static void *find_class(void *img, const char *name) {
    void *k = class_from_name(img, "", name);
    if (k) return k;
    k = scan_image(img, name);
    for (size_t i = 0; !k && i < g_nasms && asm_get_image; i++) {
        void *im = asm_get_image(g_asms[i]);
        if (im && im != img) k = scan_image(im, name);
    }
    if (k) LOGI("find_class: %s found by scan (ns='%s')", name, class_get_ns ? class_get_ns(k) : "?");
    else LOGE("find_class: %s not found in any assembly", name);
    return k;
}

static uintptr_t method_addr(void *img, const char *cls, const char *m, int argc) {
    void *k = find_class(img, cls);
    void *mi = k ? method_by_name(k, m, argc) : nullptr;
    return mi ? (uintptr_t) *(void **) mi : 0;
}

// Hook EVERY overload of cls.m (method_by_name only returns one per argc).
// Only for replacements that ignore their arguments and return void.
static void hook_all_overloads(void *img, const char *cls, const char *m, void *repl) {
    static void *dummy[16];
    static int nd = 0;
    void *k = find_class(img, cls);
    if (!k || !class_get_methods || !method_get_name) { LOGE("hook: %s overloads unavailable", cls); return; }
    std::set<void *> done;
    void *iter = nullptr;
    while (void *mi = class_get_methods(k, &iter)) {
        if (strcmp(method_get_name(mi), m) != 0) continue;
        void *addr = *(void **) mi;
        if (!addr || !done.insert(addr).second || nd >= 16) continue;
#if defined(__aarch64__)
        A64HookFunction(addr, repl, &dummy[nd++]);
        LOGI("hook: %s.%s overload @%p installed", cls, m, addr);
#endif
    }
}

static void *(*class_get_field)(void *, const char *) = nullptr;
static int (*field_get_offset)(void *) = nullptr;
static void *(*method_ret_type)(void *) = nullptr;
static int (*type_get_type)(void *) = nullptr;
static const int IL2CPP_TYPE_I4 = 0x08;

// Look up an instance field offset by class + field name. Keeps `fallback`
// (and logs an E line) if the class or field cannot be found.
static int resolve_off(void *img, const char *cls, const char *fld, int fallback) {
    void *k = find_class(img, cls);
    void *f = (k && class_get_field && field_get_offset) ? class_get_field(k, fld) : nullptr;
    int off = f ? field_get_offset(f) : -1;
    if (off > 0 && off < 0x1000) {
        if (off == fallback) LOGI("field: %s.%s offset 0x%x (by name)", cls, fld, off);
        else LOGI("field: %s.%s offset 0x%x (by name, CHANGED from old 0x%x)", cls, fld, off, fallback);
        return off;
    }
    LOGE("field: %s.%s not found, keeping fallback 0x%x", cls, fld, fallback);
    return fallback;
}

static bool hook(void *img, const char *ns, const char *cls, const char *m, int argc,
                 void *repl, void **orig, int ret_type = -1) {
    void *k = class_from_name(img, ns, cls);
    if (!k && !*ns) k = find_class(img, cls);
    if (!k) { LOGE("hook: class %s not found", cls); return false; }
    void *mi = method_by_name(k, m, argc);
    void *addr = mi ? *(void **) mi : nullptr;
    if (!addr) { LOGE("hook: %s.%s(%d) not found", cls, m, argc); return false; }
    if (ret_type >= 0) {
        if (!method_ret_type || !type_get_type) { LOGE("hook: %s.%s skipped, return-type API missing", cls, m); return false; }
        int rt = type_get_type(method_ret_type(mi));
        if (rt != ret_type) { LOGE("hook: %s.%s skipped, return type 0x%x != 0x%x", cls, m, rt, ret_type); return false; }
    }
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
    method_ret_type      = (void *(*)(void *))              S("il2cpp_method_get_return_type");
    type_get_type        = (int (*)(void *))                S("il2cpp_type_get_type");
    image_class_count    = (size_t (*)(void *))             S("il2cpp_image_get_class_count");
    image_get_class      = (void *(*)(void *, size_t))      S("il2cpp_image_get_class");
    class_get_name       = (const char *(*)(void *))        S("il2cpp_class_get_name");
    class_get_methods    = (void *(*)(void *, void **))     S("il2cpp_class_get_methods");
    method_get_name      = (const char *(*)(void *))        S("il2cpp_method_get_name");
    class_get_ns         = (const char *(*)(void *))        S("il2cpp_class_get_namespace");
    class_get_field      = (void *(*)(void *, const char *)) S("il2cpp_class_get_field_from_name");
    field_get_offset     = (int (*)(void *))                S("il2cpp_field_get_offset");
    asm_get_image        = get_image;
    img_get_name         = image_name;
#undef S

    if (!domain_get || !attach || !get_asms || !get_image || !image_name || !class_from_name || !method_by_name) {
        LOGE("hook: il2cpp api missing"); return;
    }
    LOGI("install: start");
    load_cfg();

    for (int i = 0; i < 180 && is_vm && !is_vm(nullptr); i++) sleep(1);
    void *domain = domain_get();
    attach(domain);

    { size_t n0 = 0; void **a0 = get_asms(domain, &n0); g_asms = a0; g_nasms = n0; }
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

    // --- field offsets, resolved by name before any hook that reads them ---
    if (on("mall")) {
        g_off_itemprice  = resolve_off(image, "MallItem", "itemPrice", g_off_itemprice);
        g_off_finalprice = resolve_off(image, "MallItem", "finalPrice", g_off_finalprice);
    }
    if (on("energy")) {
        g_off_maxenergy = resolve_off(image, "RoleAttributePlayer", "max_energy", g_off_maxenergy);
        g_off_energy    = resolve_off(image, "RoleAttributePlayer", "_energy", g_off_energy);
    }
    if (on("crit")) {
        g_off_critical = resolve_off(image, "DamageTrigger", "critical", g_off_critical);
        g_off_camp     = resolve_off(image, "DamageTrigger", "camp", g_off_camp);
    }
    if (on("goldarmor"))
        g_off_curcoin = resolve_off(image, "CoinChangeEvent", "curCoinVal", g_off_curcoin);

    // --- lockedlevel: show the upgrade button for locked heroes (in-place patch, no hook) ---
    // Stages (ds_hooks.txt "lockedlevel=N"; default 1 = known safe), for bisecting a UI break.
    // Known so far: 0 normal, 1 normal, 2 (old numbering: class + method lookup) BREAKS.
    //   1 = resolve ChooseHeroData.unlocked only
    //   2 = + find_class(ChooseHeroView) only, no method lookup
    //   3 = + method_by_name(RefreshUnlockArea) via class_get_methods iteration
    //       (no method_by_name call, to tell the two lookups apart)
    //   4 = same as 3 but via method_by_name
    //   5 = + patch tbz only
    //   6 = + patch the other-button check (full feature)
    if (on("lockedlevel")) {
        int st = stage("lockedlevel", 1);
        LOGI("lockedlevel: stage %d", st);
        int o = resolve_off(image, "ChooseHeroData", "unlocked", g_off_cd_unlocked + 0x10);
        g_off_cd_unlocked = o >= 0x10 ? o - 0x10 : o;
        LOGI("lockedlevel: stage 1 done (unlocked raw off 0x%x)", g_off_cd_unlocked);
        void *kv = nullptr;
        uintptr_t fn = 0;
        if (st >= 2) {
            kv = find_class(image, "ChooseHeroView");
            LOGI("lockedlevel: stage 2 done, ChooseHeroView class=%p", kv);
        }
        if (st >= 3 && kv) {
            if (st == 3) {
                // iterate the method list instead of calling method_by_name
                if (class_get_methods && method_get_name) {
                    void *iter = nullptr;
                    while (void *mi = class_get_methods(kv, &iter)) {
                        if (strcmp(method_get_name(mi), "RefreshUnlockArea") == 0 && *(void **) mi) {
                            fn = (uintptr_t) *(void **) mi;
                            break;
                        }
                    }
                } else LOGE("lockedlevel: class_get_methods API missing");
            } else {
                void *mi = method_by_name(kv, "RefreshUnlockArea", 1);
                fn = mi ? (uintptr_t) *(void **) mi : 0;
            }
            if (fn) LOGI("lockedlevel: stage %d done, RefreshUnlockArea @%p", st >= 4 ? 4 : 3, (void *) fn);
            else LOGE("lockedlevel: RefreshUnlockArea not found, skipped");
        }
        if (st >= 5 && fn) {
            uintptr_t at_tbz = 0, at_and = 0;
            if (patch_lockedlevel(fn, g_off_cd_unlocked, st >= 6 ? 2 : 1, &at_tbz, &at_and))
                LOGI("lockedlevel: patched RefreshUnlockArea @%p: tbz->nop at +0x%x, and at +0x%x (0 = not patched), unlocked off 0x%x",
                     (void *) fn, (unsigned) (at_tbz - fn), at_and ? (unsigned) (at_and - fn) : 0u, g_off_cd_unlocked);
            else
                LOGE("lockedlevel: RefreshUnlockArea @%p pattern not found / patch failed, skipped", (void *) fn);
        }
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
    if (on("shield")) {
        void *vk = class_from_name(image, "TalentImpl", "BuffImplVoidShield");
        if (!vk) vk = find_class(image, "BuffImplVoidShield");
        if (vk && class_get_field && field_get_offset) {
            void *fld = class_get_field(vk, "_shieldCount");
            int off = fld ? field_get_offset(fld) : -1;
            if (off > 0 && off < 0x200) { g_shield_off = off; LOGI("shield: _shieldCount offset 0x%x (by name)", off); }
            else LOGE("shield: _shieldCount field not found, using 0x%x", g_shield_off);
        }
        void *smi = vk ? method_by_name(vk, "SaveShieldCount", 0) : nullptr;
        if (smi && *(void **) smi) {
            g_saveshield = (SaveShield_t) *(void **) smi;
            g_saveshield_mi = smi;
            LOGI("shield: SaveShieldCount resolved @%p", (void *) g_saveshield);
        } else LOGE("shield: SaveShieldCount not found, count will not persist");
        hook(image, "TalentImpl", "BuffImplVoidShield", "OnBeforePlayerConsumeVoidShield", 1,
             (void *) consume_hook, (void **) &g_consume_orig);
    }

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
        hook_all_overloads(image, "ItemData", "ConsumeMaterial", (void *) consumemat_hook);
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

    // --- shop: HasBuff(10) true inside price code + rate helper 0 ---
    if (on("shop")) {
        uintptr_t rate    = method_addr(image, "DataUtil", "CalBuffExpertShopExtraRate", 1);
        uintptr_t hasbuff = method_addr(image, "BattleData", "HasBuff", 1);
        if (!rate || !hasbuff) {
            LOGE("shop: rate=%p hasbuff=%p, not found, skipped", (void *) rate, (void *) hasbuff);
        } else {
            for (const PriceName &pn : k_sites) {
                uintptr_t fn = method_addr(image, pn.cls, pn.m, 0);
                if (!fn) { LOGE("shop: %s.%s not found", pn.cls, pn.m); continue; }
                uintptr_t off = 0;
                for (uintptr_t o = 4; o < 0x400; o += 4)
                    if (bl_target(fn + o) == rate) { off = o; break; }
                if (!off || g_pr_n >= 16) { LOGE("shop: %s.%s has no rate call, skipped", pn.cls, pn.m); continue; }
                g_pr_lo[g_pr_n] = fn;
                g_pr_hi[g_pr_n] = fn + off;
                g_pr_n++;
                LOGI("shop: %s.%s rate call at +0x%x", pn.cls, pn.m, (unsigned) off);
                uintptr_t at = 0;
                if (patch_clamp(fn + off, &at)) LOGI("shop: %s.%s min-price clamp removed at +0x%x", pn.cls, pn.m, (unsigned) (at - fn));
                else LOGE("shop: %s.%s min-price clamp NOT found/patched", pn.cls, pn.m);
            }
            LOGI("shop: %d price sites found", g_pr_n);
#if defined(__aarch64__)
            if (g_pr_n > 0) {
                A64HookFunction((void *) rate, (void *) ratefn_hook, (void **) &g_ratefn_orig);
                A64HookFunction((void *) hasbuff, (void *) hasbuff_hook, (void **) &g_hasbuff_orig);
                LOGI("shop: hooks installed (rate=%p hasbuff=%p)", (void *) rate, (void *) hasbuff);
            }
#endif
        }
    }

    // --- goldarmor: +1 max HP and +1 max armor per GOLD_STEP coins (proxy getters only) ---
    if (on("goldarmor")) {
        hook(image, "", "RoleAttributePlayer", "OnCoinChange", 1,
             (void *) oncoin_hook, (void **) &g_oncoin_orig);
        hook(image, "", "RoleAttributePlayer", "UpdateArmor", 1,
             (void *) updarmor_hook, (void **) &g_updarmor_orig);
        hook(image, "", "RoleAttributeProxy", "get_max_hp", 0,
             (void *) getmaxhp_hook, (void **) &g_getmaxhp_orig, IL2CPP_TYPE_I4);
        hook(image, "", "RoleAttributeProxy", "get_MaxArmor", 0,
             (void *) getmaxarmor_hook, (void **) &g_getmaxarmor_orig, IL2CPP_TYPE_I4);
    }

    // --- saleday: Sales Day challenge (factor 316) always active ---
    if (on("saleday")) {
        void *bk = find_class(image, "BattleData");
        void *nmi = bk ? method_by_name(bk, "get_IsNormalMode", 0) : nullptr;
        if (nmi && *(void **) nmi) {
            g_isnormal = (IsNormalMode_t) *(void **) nmi;
            g_isnormal_mi = nmi;
            LOGI("saleday: get_IsNormalMode resolved @%p", (void *) g_isnormal);
        } else LOGE("saleday: BattleData.get_IsNormalMode not found, gate skipped");
        hook(image, "", "BattleData", "CompareFactor", 1,
             (void *) comparefactor_hook, (void **) &g_comparefactor_orig);
    }
}

void hook_start(void *handle) {
    // fresh log files on every game start, then note that we got here at all
    { FILE *f = fopen(LOG_PATH_EXT, "w"); if (f) fclose(f); }
    { FILE *f = fopen(LOG_PATH_INT, "w"); if (f) fclose(f); }
    LOGI("hook_start: module entered, pid=%d", (int) getpid());
    std::thread(install, handle).detach();
}
