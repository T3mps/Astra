// Compile check: 64-bit entity configuration (32-bit version).
#define ASTRA_ENTITY_BITS 64
#define ASTRA_ENTITY_VERSION_BITS 32
#include <Astra/Astra.hpp>

int main()
{
    Astra::Registry reg;
    struct P { float x; };
    auto e = reg.CreateEntity<P>();
    return reg.IsValid(e) ? 0 : 1;
}
