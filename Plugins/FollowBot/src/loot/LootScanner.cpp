// LootScanner.cpp - the PluginSDK side of the loot scanner: reads the inner
// item of a ground WorldItem and asks the host price list. Everything else is
// the pure core in LootScanCore.h.
// FB_TESTS_EXCLUDE: adapter (includes PluginSDK.h); the unit tests link their own LootScanner::Update.
#include "LootScanner.h"

#include "../../../../POEFixer/plugin_sdk/PluginSDK.h"

#include "../common/Strings.h"
#include "LootScanCore.h"

namespace fb {

namespace {

class SdkLootHost final : public ILootHost {
public:
    explicit SdkLootHost(const PluginSDK::Context& sdk) : m_sdk(sdk) {}

    bool ResolveItem(uintptr_t worldItemAddr, ResolvedItem& out) override {
        if (worldItemAddr == 0) return false;
        const std::optional<PluginSDK::Entity> inner = m_sdk.Entities.GetWorldItemInner(worldItemAddr);
        if (!inner || !inner->IsValid || inner->Address == 0) return false;
        out.path = WideToUtf8(inner->Path);
        if (out.path.empty()) return false;
        const PluginSDK::ComponentAddresses& c = inner->Components;
        if (c.Base != 0) {
            const PluginSDK::Base b = m_sdk.Components.ReadBase(c.Base);
            if (b.Valid) {
                out.w = b.Width;
                out.h = b.Height;
                out.baseName = b.BaseTypeName;
            }
        }
        if (out.baseName.empty()) out.baseName = m_sdk.Inventory.ReadItemBaseTypeName(inner->Address);
        // Stackables (currency, runes, ...) carry no Mods component: rarity normal.
        out.rarity = c.Mods != 0 ? m_sdk.Inventory.ReadItemRarity(inner->Address) : 0;
        if (out.rarity == 3) out.uniqueName = m_sdk.Inventory.ReadItemUniqueName(inner->Address);
        if (c.Stack != 0) {
            const PluginSDK::Stack st = m_sdk.Components.ReadStack(c.Stack);
            if (st.Valid) {
                out.stack = st.CurrentSize > 0 ? st.CurrentSize : 1;
                out.maxStack = st.MaxSize;
            }
        } else {
            out.maxStack = 1;
        }
        return true;
    }

    bool LookupPrice(const std::string& name, PriceQuote& out) override {
        const PluginSDK::PriceResult r = m_sdk.Prices.LookupPrice(name);
        if (!r.found) return false;
        out.found = true;
        out.chaos = r.chaos;
        out.divine = r.divine;
        out.exalt = r.exalt;
        out.category = r.category;
        return true;
    }

    uint64_t PriceRevision() override { return m_sdk.Prices.GetRevision(); }

private:
    const PluginSDK::Context& m_sdk;
};

}  // namespace

void LootScanner::Update(BotContext& ctx) {
    if (!ctx.sdk) return;
    SdkLootHost host(*ctx.sdk);
    UpdateWith(ctx, host);
}

}  // namespace fb
