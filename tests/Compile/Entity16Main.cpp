// Compile check: 16-bit entity configuration (8-bit ID / 8-bit version).
#define ASTRA_ENTITY_BITS 16
#define ASTRA_ENTITY_VERSION_BITS 8
#include <Astra/Astra.hpp>

int main()
{
    Astra::Registry reg;
    struct P { float x; };
    auto e = reg.CreateEntity<P>();
    return reg.IsValid(e) ? 0 : 1;
}
