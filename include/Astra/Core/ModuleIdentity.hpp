#pragma once

#include <cstdint>

namespace Astra
{
    // Whether the calling module's image can ever be unmapped (spec
    // 2026-09-09 §3.1). Declared once, at SetTypeContext, before any
    // registration in that module. Resident => every meta binder this module
    // pushes is PINNED (never removed at zero refs), so registry-less
    // GetMeta/GetByName keep resolving after the module's last handle is
    // gone. Transient (default) => a binder leaves at zero refs, which is
    // exactly today's plugin behaviour. A module that declares Resident and
    // then unmaps leaves pinned binders whose content dangles -- the same
    // misuse class as a leaked handle; Astra cannot detect either.
    enum class ModuleResidency : uint8_t { Transient, Resident };

    // Identity of the calling EXE/DLL image: the address of a per-module
    // inline-function-local static (the same mechanism as the per-module
    // TypeContext slot). A reloaded DLL lands at a new base address and so
    // gets a NEW token -- that is what makes load-before-unload reload fall
    // out with no special handling. POSIX caveat (shared with the context
    // slot): default-visibility DSOs may coalesce the static, merging two
    // modules into one token. That only over-holds; it never erases early.
    using ModuleToken = const void*;

    namespace Detail
    {
        struct ModuleIdentity
        {
            ModuleToken     token     = nullptr;
            ModuleResidency residency = ModuleResidency::Transient;
        };

        // Per-module slot. The token is the slot's own address: unique per
        // image, stable for the image's life, never null.
        inline ModuleIdentity& CurrentModuleIdentity() noexcept
        {
            static ModuleIdentity s_identity{ &s_identity, ModuleResidency::Transient };
            return s_identity;
        }

        // Test seam: swaps the per-module identity for a scope so one test
        // binary can play several modules (an "engine" and a "plugin" with
        // distinct tokens and residencies). Restores the previous identity
        // on exit. Not for production use; production identity comes from
        // the image itself.
        struct ScopedModuleIdentity
        {
            ScopedModuleIdentity(ModuleToken token, ModuleResidency residency)
                : m_saved(CurrentModuleIdentity())
            {
                CurrentModuleIdentity() = ModuleIdentity{ token, residency };
            }
            ~ScopedModuleIdentity() { CurrentModuleIdentity() = m_saved; }
            ScopedModuleIdentity(const ScopedModuleIdentity&) = delete;
            ScopedModuleIdentity& operator=(const ScopedModuleIdentity&) = delete;

        private:
            ModuleIdentity m_saved;
        };
    }
}
