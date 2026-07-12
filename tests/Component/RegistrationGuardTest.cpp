#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct alignas(128) OverAligned { float v[4]; }; }  // > CACHE_LINE_SIZE (64)

// The forceable arm of the over-alignment guard: ComponentRegistry must refuse
// (not silently accept) a component whose alignof() exceeds CACHE_LINE_SIZE,
// since chunk storage can only honor alignments up to CACHE_LINE_SIZE. Refusal
// goes through the same observable as the existing MAX_COMPONENTS guard: no
// descriptor is installed, so GetComponentDescriptor(id) stays nullptr.
TEST(RegistrationGuard, OverAlignedComponentIsRefusedNotMisaligned)
{
    Astra::Registry reg;
    auto* creg = reg.GetComponentRegistry();

    creg->RegisterComponent<OverAligned>();

    Astra::ComponentID id = Astra::TypeID<OverAligned>::Value();
    const Astra::ComponentDescriptor* desc = creg->GetComponentDescriptor(id);
    EXPECT_EQ(desc, nullptr);
}
