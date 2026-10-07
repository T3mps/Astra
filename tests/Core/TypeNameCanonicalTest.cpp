// Canonical, compiler-independent type names (Core/TypeNameCanonical.hpp).
//
// Two halves:
//   * RawSpellings: the exact T-slice each compiler prints for a representative
//     type set -- GCC 14.2 and Clang 19.1 (__PRETTY_FUNCTION__, Ubuntu 24.04),
//     MSVC 19.51 (__FUNCSIG__, VS 2026) -- fed through CanonicalizeTypeName.
//     All three must land on the same string. This runs everywhere, so a Linux
//     run proves the MSVC spellings canonicalize too.
//   * Live: TypeID<T>::Name() / Hash() on the compiler that built this test,
//     against the same canonical strings and golden hash values. The Windows
//     CI lane is what proves the MSVC half live.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "Astra/Core/TypeID.hpp"
#include "Astra/Registry/Registry.hpp"
#include "../TestComponents.hpp"

namespace
{
    std::string Canonical(std::string_view raw)
    {
        std::array<char, 512> buffer{};
        const size_t size = Astra::Detail::CanonicalizeTypeName(raw, buffer.data(), buffer.size());
        EXPECT_LE(size, buffer.size()) << raw;
        return std::string(buffer.data(), size < buffer.size() ? size : buffer.size());
    }

    struct Spellings
    {
        std::string_view gcc;
        std::string_view clang;
        std::string_view msvc;
        std::string_view canonical;
    };

    // T-slices exactly as printed (GCC's after the ';' tail cut).
    constexpr Spellings kPortable[] = {
        { "int", "int", "int", "int" },
        { "unsigned int", "unsigned int", "unsigned int", "unsigned int" },
        { "short int", "short", "short", "short" },
        { "short unsigned int", "unsigned short", "unsigned short", "unsigned short" },
        { "long int", "long", "long", "long" },
        { "long unsigned int", "unsigned long", "unsigned long", "unsigned long" },
        { "long long int", "long long", "__int64", "long long" },
        { "long long unsigned int", "unsigned long long", "unsigned __int64", "unsigned long long" },
        { "signed char", "signed char", "signed char", "signed char" },
        { "unsigned char", "unsigned char", "unsigned char", "unsigned char" },
        { "long double", "long double", "long double", "long double" },
        { "Position", "Position", "struct Position", "Position" },
        { "Klass", "Klass", "class Klass", "Klass" },
        { "Uni", "Uni", "union Uni", "Uni" },
        { "Color", "Color", "enum Color", "Color" },
        { "Ns::Inner::Deep", "Ns::Inner::Deep", "struct Ns::Inner::Deep", "Ns::Inner::Deep" },
        { "{anonymous}::Hidden", "(anonymous namespace)::Hidden", "struct `anonymous-namespace'::Hidden",
          "(anonymous namespace)::Hidden" },
        { "Tmpl<{anonymous}::Hidden>", "Tmpl<(anonymous namespace)::Hidden>",
          "struct Tmpl<struct `anonymous namespace'::Hidden>", "Tmpl<(anonymous namespace)::Hidden>" },
        { "Tmpl<Tmpl<{anonymous}::Hidden> >", "Tmpl<Tmpl<(anonymous namespace)::Hidden>>",
          "struct Tmpl<struct Tmpl<struct `anonymous namespace'::Hidden> >",
          "Tmpl<Tmpl<(anonymous namespace)::Hidden>>" },
        { "Tmpl<Klass>", "Tmpl<Klass>", "struct Tmpl<class Klass>", "Tmpl<Klass>" },
        { "Tmpl<Uni>", "Tmpl<Uni>", "struct Tmpl<union Uni>", "Tmpl<Uni>" },
        { "Tmpl<Color>", "Tmpl<Color>", "struct Tmpl<enum Color>", "Tmpl<Color>" },
        { "Pair<int, float>", "Pair<int, float>", "struct Pair<int,float>", "Pair<int,float>" },
        { "Pair<Tmpl<int>, Position>", "Pair<Tmpl<int>, Position>", "struct Pair<struct Tmpl<int>,struct Position>",
          "Pair<Tmpl<int>,Position>" },
        { "Tmpl<Tmpl<Tmpl<int> > >", "Tmpl<Tmpl<Tmpl<int>>>", "struct Tmpl<struct Tmpl<struct Tmpl<int> > >",
          "Tmpl<Tmpl<Tmpl<int>>>" },
        { "Pair<Ns::Player, Pair<int, Klass> >", "Pair<Ns::Player, Pair<int, Klass>>",
          "struct Pair<struct Ns::Player,struct Pair<int,class Klass> >", "Pair<Ns::Player,Pair<int,Klass>>" },
        { "IntN<-3>", "IntN<-3>", "struct IntN<-3>", "IntN<-3>" },
        { "ULLongN<5>", "ULLongN<5>", "struct ULLongN<5>", "ULLongN<5>" },
        { "BoolN<true>", "BoolN<true>", "struct BoolN<1>", "BoolN<1>" },
        { "CharN<'a'>", "CharN<'a'>", "struct CharN<97>", "CharN<97>" },
        { "CharN<'\\000'>", "CharN<'\\x00'>", "struct CharN<0>", "CharN<0>" },
        { "U8N<5>", "U8N<'\\x05'>", "struct U8N<5>", "U8N<5>" },
        { "Tmpl<int*>", "Tmpl<int *>", "struct Tmpl<int *>", "Tmpl<int*>" },
        { "Tmpl<const int*>", "Tmpl<const int *>", "struct Tmpl<int const *>", "Tmpl<const int*>" },
        { "Tmpl<int* const>", "Tmpl<int *const>", "struct Tmpl<int * const>", "Tmpl<int*const>" },
        { "Tmpl<const Position&>", "Tmpl<const Position &>", "struct Tmpl<struct Position const &>",
          "Tmpl<const Position&>" },
        { "Tmpl<int&&>", "Tmpl<int &&>", "struct Tmpl<int &&>", "Tmpl<int&&>" },
        { "Tmpl<int [4]>", "Tmpl<int[4]>", "struct Tmpl<int [4]>", "Tmpl<int[4]>" },
        { "Tmpl<void (*)(int)>", "Tmpl<void (*)(int)>", "struct Tmpl<void (__cdecl*)(int)>", "Tmpl<void(*)(int)>" },
        { "Tmpl<long long int>", "Tmpl<long long>", "struct Tmpl<__int64>", "Tmpl<long long>" },
        { "Tmpl<long long unsigned int>", "Tmpl<unsigned long long>", "struct Tmpl<unsigned __int64>",
          "Tmpl<unsigned long long>" },
        { "Tmpl<const volatile int*>", "Tmpl<const volatile int *>", "struct Tmpl<int const volatile *>",
          "Tmpl<const volatile int*>" },
        { "Tmpl<const long long unsigned int*>", "Tmpl<const unsigned long long *>",
          "struct Tmpl<unsigned __int64 const *>", "Tmpl<const unsigned long long*>" },
        { "Tmpl<const Tmpl<int>*>", "Tmpl<const Tmpl<int> *>", "struct Tmpl<struct Tmpl<int> const *>",
          "Tmpl<const Tmpl<int>*>" },
        { "Tmpl<int Klass::*>", "Tmpl<int Klass::*>", "struct Tmpl<int Klass::*>", "Tmpl<int Klass::*>" },
        { "Tmpl<void (Klass::*)(int) const>", "Tmpl<void (Klass::*)(int) const>",
          "struct Tmpl<void (__cdecl Klass::*)(int)const >", "Tmpl<void(Klass::*)(int)const>" },
        { "Tmpl<const char* const*>", "Tmpl<const char *const *>", "struct Tmpl<char const * const *>",
          "Tmpl<const char*const*>" },
        { "Pair<const Ns::Player*, volatile short unsigned int&>", "Pair<const Ns::Player *, volatile unsigned short &>",
          "struct Pair<struct Ns::Player const *,unsigned short volatile &>",
          "Pair<const Ns::Player*,volatile unsigned short&>" },
        { "std::array<int, 3>", "std::array<int, 3>", "class std::array<int,3>", "std::array<int,3>" },
        { "std::pair<int, float>", "std::pair<int, float>", "struct std::pair<int,float>", "std::pair<int,float>" },
    };
}

TEST(TypeNameCanonical, EveryCompilerSpellingCanonicalizesToOneName)
{
    for (const Spellings& s : kPortable)
    {
        EXPECT_EQ(Canonical(s.gcc), s.canonical) << "gcc: " << s.gcc;
        EXPECT_EQ(Canonical(s.clang), s.canonical) << "clang: " << s.clang;
        EXPECT_EQ(Canonical(s.msvc), s.canonical) << "msvc: " << s.msvc;
    }
}

TEST(TypeNameCanonical, CanonicalFormIsAFixedPoint)
{
    for (const Spellings& s : kPortable)
        EXPECT_EQ(Canonical(s.canonical), s.canonical);
}

TEST(TypeNameCanonical, StandardLibraryInlineNamespacesAreDropped)
{
    // libstdc++ (GCC) vs libc++ vs the Clang-on-libstdc++ spelling.
    EXPECT_EQ(Canonical("std::__cxx11::basic_string<char>"), "std::basic_string<char>");
    EXPECT_EQ(Canonical("std::__1::basic_string<char>"), "std::basic_string<char>");
    EXPECT_EQ(Canonical("Tmpl<std::__cxx11::basic_string<char> >"), "Tmpl<std::basic_string<char>>");
    // Only directly under std::, never a user namespace that happens to be named so.
    EXPECT_EQ(Canonical("Game::__1::Thing"), "Game::__1::Thing");
}

// Apple Clang prints __PRETTY_FUNCTION__ like upstream Clang, over libc++ instead
// of libstdc++: standard-library names may carry libc++'s versioned inline
// namespace (std::__1::). Both spellings land on the shared canonical form.
TEST(TypeNameCanonical, AppleClangLibcxxSpellingsCanonicalize)
{
    EXPECT_EQ(Canonical("std::__1::array<int, 3>"), "std::array<int,3>");
    EXPECT_EQ(Canonical("std::array<int, 3>"), "std::array<int,3>");
    EXPECT_EQ(Canonical("std::__1::pair<int, float>"), "std::pair<int,float>");
    EXPECT_EQ(Canonical("Tmpl<std::__1::pair<int, float> >"), "Tmpl<std::pair<int,float>>");
    EXPECT_EQ(Canonical("Pair<std::__1::array<int, 3>, std::__1::pair<int, float>>"),
              "Pair<std::array<int,3>,std::pair<int,float>>");
}

TEST(TypeNameCanonical, IntegerLiteralSuffixesAreDropped)
{
    EXPECT_EQ(Canonical("N<5U>"), "N<5>");
    EXPECT_EQ(Canonical("N<5UL>"), "N<5>");
    EXPECT_EQ(Canonical("N<5ull>"), "N<5>");
    EXPECT_EQ(Canonical("N<-5LL>"), "N<-5>");
    EXPECT_EQ(Canonical("Vec3<int>"), "Vec3<int>");     // identifiers ending in a digit are untouched
}

TEST(TypeNameCanonical, OtherCallingConventionsAreKept)
{
    // Only the default (__cdecl) is implicit on every compiler; others are
    // distinct function types and must not collapse onto it.
    EXPECT_EQ(Canonical("void (__vectorcall*)(int)"), "void(__vectorcall*)(int)");
    EXPECT_NE(Canonical("void (__vectorcall*)(int)"), Canonical("void (__cdecl*)(int)"));
}

// The documented limits: these differ in CONTENT between MSVC and GCC/Clang, so
// no formatting rule can unify them. Pinned so a change in either direction is
// noticed (and TypeNameCanonical.hpp updated).
TEST(TypeNameCanonical, DocumentedNonPortableSpellingsStayDistinct)
{
    // Defaulted template arguments: MSVC prints them, GCC/Clang omit them.
    EXPECT_EQ(Canonical("std::vector<int>"), "std::vector<int>");
    EXPECT_EQ(Canonical("class std::vector<int,class std::allocator<int> >"), "std::vector<int,std::allocator<int>>");
    EXPECT_EQ(Canonical("class std::basic_string<char,struct std::char_traits<char>,class std::allocator<char> >"),
              "std::basic_string<char,std::char_traits<char>,std::allocator<char>>");
    // Enumerator arguments: GCC/Clang print the enumerator, MSVC its value.
    EXPECT_EQ(Canonical("ColorN<Color::Red>"), "ColorN<Color::Red>");
    EXPECT_EQ(Canonical("struct ColorN<1>"), "ColorN<1>");
    // Unsigned 64-bit arguments above INT64_MAX: MSVC prints them signed.
    EXPECT_EQ(Canonical("ULLongN<18446744073709551615>"), "ULLongN<18446744073709551615>");
    EXPECT_EQ(Canonical("struct ULLongN<-1>"), "ULLongN<-1>");
}

TEST(TypeNameCanonical, OverlongResultReportsItsFullLength)
{
    std::array<char, 4> tiny{};
    const size_t size = Astra::Detail::CanonicalizeTypeName("struct Ns::Inner::Deep", tiny.data(), tiny.size());
    EXPECT_EQ(size, std::string_view("Ns::Inner::Deep").size());
}

// ---------------------------------------------------------------------------
// Live half: what TypeID<T> yields on the compiler that built this binary.
// ---------------------------------------------------------------------------

namespace CanonProbe
{
    struct Position { float x, y, z; };
    class Klass { public: int a; };
    union Uni { int i; float f; };
    enum class Color : int { Red = 1, Green = 2 };
    namespace Inner { struct Deep { int d; }; }

    template<typename T> struct Tmpl { T v; };
    template<typename A, typename B> struct Pair { A a; B b; };
    template<int N> struct IntN {};
    template<unsigned long long N> struct ULLongN {};
    template<bool B> struct BoolN {};
    template<char C> struct CharN {};
    template<std::uint8_t N> struct U8N {};
}

namespace
{
    struct CanonHidden { int h; };
}

namespace
{
    template<typename T>
    void ExpectLive(std::string_view expected)
    {
        EXPECT_EQ(Astra::TypeID<T>::Name(), expected);
        EXPECT_EQ(Astra::TypeID<T>::Hash(), Astra::Detail::XXHash::XXHash64(expected)) << expected;
    }
}

TEST(TypeNameCanonical, LiveNamesAreCanonical)
{
    using namespace CanonProbe;
    ExpectLive<int>("int");
    ExpectLive<unsigned short>("unsigned short");
    ExpectLive<unsigned long long>("unsigned long long");
    ExpectLive<long long>("long long");
    ExpectLive<Position>("CanonProbe::Position");
    ExpectLive<Klass>("CanonProbe::Klass");
    ExpectLive<Uni>("CanonProbe::Uni");
    ExpectLive<Color>("CanonProbe::Color");
    ExpectLive<Inner::Deep>("CanonProbe::Inner::Deep");
    ExpectLive<CanonHidden>("(anonymous namespace)::CanonHidden");
    ExpectLive<Tmpl<CanonHidden>>("CanonProbe::Tmpl<(anonymous namespace)::CanonHidden>");
    ExpectLive<Tmpl<Klass>>("CanonProbe::Tmpl<CanonProbe::Klass>");
    ExpectLive<Pair<int, float>>("CanonProbe::Pair<int,float>");
    ExpectLive<Pair<Tmpl<int>, Position>>("CanonProbe::Pair<CanonProbe::Tmpl<int>,CanonProbe::Position>");
    ExpectLive<Tmpl<Tmpl<Tmpl<int>>>>("CanonProbe::Tmpl<CanonProbe::Tmpl<CanonProbe::Tmpl<int>>>");
    ExpectLive<IntN<-3>>("CanonProbe::IntN<-3>");
    ExpectLive<ULLongN<5>>("CanonProbe::ULLongN<5>");
    ExpectLive<BoolN<true>>("CanonProbe::BoolN<1>");
    ExpectLive<CharN<'a'>>("CanonProbe::CharN<97>");
    ExpectLive<U8N<5>>("CanonProbe::U8N<5>");
    ExpectLive<Tmpl<const int*>>("CanonProbe::Tmpl<const int*>");
    ExpectLive<Tmpl<int* const>>("CanonProbe::Tmpl<int*const>");
    ExpectLive<Tmpl<const Position&>>("CanonProbe::Tmpl<const CanonProbe::Position&>");
    ExpectLive<Tmpl<const unsigned long long*>>("CanonProbe::Tmpl<const unsigned long long*>");
    ExpectLive<Tmpl<int[4]>>("CanonProbe::Tmpl<int[4]>");
    ExpectLive<Tmpl<void (*)(int)>>("CanonProbe::Tmpl<void(*)(int)>");
    ExpectLive<Tmpl<void (Klass::*)(int) const>>("CanonProbe::Tmpl<void(CanonProbe::Klass::*)(int)const>");
    ExpectLive<std::array<int, 3>>("std::array<int,3>");
    ExpectLive<std::pair<int, float>>("std::pair<int,float>");
}

TEST(TypeNameCanonical, LiveNamesAreCompileTimeConstants)
{
    static_assert(Astra::TypeID<CanonProbe::Pair<int, float>>::Name() == "CanonProbe::Pair<int,float>");
    static_assert(Astra::TypeID<CanonProbe::Position>::Hash()
                  == Astra::Detail::XXHash::XXHash64("CanonProbe::Position"));
    SUCCEED();
}

// std::int64_t is a different fundamental type per platform ABI, not a
// spelling difference: long on LP64 Linux, but long long on LLP64 Windows AND
// on Darwin, whose <stdint.h> declares it long long even though long is also
// 64 bits there.
TEST(TypeNameCanonical, Int64AliasFollowsThePlatformAbi)
{
#if defined(__APPLE__) || defined(_WIN32)
    constexpr std::string_view expected = "long long";
#else
    constexpr std::string_view expected = sizeof(long) == 8 ? "long" : "long long";
#endif
    EXPECT_EQ(Astra::TypeID<std::int64_t>::Name(), expected);
}

// Golden hashes: the serialization identity an archive written by one compiler
// carries into a process built by another. Equal on MSVC, GCC, Clang and
// Apple Clang.
TEST(TypeNameCanonical, GoldenHashesMatchAcrossCompilers)
{
    using namespace CanonProbe;
    EXPECT_EQ(Astra::TypeID<Position>::Hash(), 0x928324569DE07252ULL);
    EXPECT_EQ(Astra::TypeID<Tmpl<Klass>>::Hash(), 0xF6CFFE5842C515D3ULL);
    EXPECT_EQ((Astra::TypeID<Pair<Tmpl<int>, Position>>::Hash()), 0x94ACFAB39682C268ULL);
    EXPECT_EQ(Astra::TypeID<Tmpl<const unsigned long long*>>::Hash(), 0x105C888008BF79A6ULL);
    EXPECT_EQ(Astra::TypeID<BoolN<true>>::Hash(), 0x2589163FA309882CULL);
    EXPECT_EQ(Astra::TypeID<Astra::Test::Position>::Hash(), 0xD1BCE3829ECD62BFULL);
}

// THE PIN: canonical name AND its hash, literal, for every portable shape --
// fundamentals, namespaced and anonymous-namespace classes, enums, nested
// templates, integral/bool/char template arguments, cv/ref/array/function and
// member-function pointers, and the std types whose names agree across
// libstdc++, libc++ and MSVC's STL. Every CI toolchain (MSVC, GCC 14, Clang 19,
// Apple Clang) must produce exactly these values: an archive written on one
// platform is read by its type hashes on another (Arcane ships Windows, Linux
// and macOS builds against the same data). A failure here means a cross-platform
// identity break -- fix the canonicalizer, never the table.
namespace
{
    struct PinnedIdentity
    {
        std::string_view name;
        std::uint64_t hash;
        std::string_view liveName;
        std::uint64_t liveHash;
    };

    template<typename T>
    constexpr PinnedIdentity Pin(std::string_view name, std::uint64_t hash) noexcept
    {
        return { name, hash, Astra::TypeID<T>::Name(), Astra::TypeID<T>::Hash() };
    }

    const char* ToolchainName() noexcept
    {
#if defined(_MSC_VER) && !defined(__clang__)
        return "MSVC";
#elif defined(__apple_build_version__)
        return "Apple Clang";
#elif defined(__clang__)
        return "Clang";
#elif defined(__GNUC__)
        return "GCC";
#else
        return "unknown";
#endif
    }
}

TEST(TypeNameCanonical, NamesAndHashesArePinnedOnEveryToolchain)
{
    using namespace CanonProbe;
    const PinnedIdentity pins[] = {
        Pin<int>("int", 0x60F24396F77057C3ULL),
        Pin<unsigned long long>("unsigned long long", 0x2009B33FB5EA5E61ULL),
        Pin<Position>("CanonProbe::Position", 0x928324569DE07252ULL),
        Pin<Color>("CanonProbe::Color", 0xC3FAD440F50FC7FAULL),
        Pin<CanonHidden>("(anonymous namespace)::CanonHidden", 0x6E76F93AAEE00983ULL),
        Pin<Tmpl<CanonHidden>>("CanonProbe::Tmpl<(anonymous namespace)::CanonHidden>", 0x183BC801FAAC7799ULL),
        Pin<Pair<int, float>>("CanonProbe::Pair<int,float>", 0x2A0BC05789A16277ULL),
        Pin<IntN<-3>>("CanonProbe::IntN<-3>", 0xD43CADB5D84E3F48ULL),
        Pin<BoolN<true>>("CanonProbe::BoolN<1>", 0x2589163FA309882CULL),
        Pin<CharN<'a'>>("CanonProbe::CharN<97>", 0xE48D826AEB67B3DFULL),
        Pin<Tmpl<int* const>>("CanonProbe::Tmpl<int*const>", 0x3F7C5DD641F40488ULL),
        Pin<Tmpl<const Position&>>("CanonProbe::Tmpl<const CanonProbe::Position&>", 0xD054E092B55B416BULL),
        Pin<Tmpl<int[4]>>("CanonProbe::Tmpl<int[4]>", 0x0499B52A8803DD36ULL),
        Pin<Tmpl<void (*)(int)>>("CanonProbe::Tmpl<void(*)(int)>", 0xF1C63D793CB357C1ULL),
        Pin<Tmpl<void (Klass::*)(int) const>>("CanonProbe::Tmpl<void(CanonProbe::Klass::*)(int)const>",
                                              0x5C71BEFFCDCF7BB5ULL),
        Pin<std::array<int, 3>>("std::array<int,3>", 0xCB04EE78D83FE909ULL),
        Pin<std::pair<int, float>>("std::pair<int,float>", 0x3B1695DDD566A283ULL),
    };

    SCOPED_TRACE(ToolchainName());
    for (const PinnedIdentity& pin : pins)
    {
        EXPECT_EQ(pin.liveName, pin.name);
        EXPECT_EQ(pin.liveHash, pin.hash) << pin.name;
        // The table itself is self-consistent: the literal is the name's hash.
        EXPECT_EQ(Astra::Detail::XXHash::XXHash64(pin.name), pin.hash) << pin.name;
    }
}

// Name-keyed lookups take the canonical spelling a user would write.
TEST(TypeNameCanonical, ComponentLookupByCanonicalName)
{
    Astra::Registry registry;
    registry.GetComponentRegistry()->RegisterComponents<Astra::Test::Position>();
    const Astra::Entity entity = registry.CreateEntity();
    registry.EmplaceComponent<Astra::Test::Position>(entity, 1.0f, 2.0f, 3.0f);

    void* byName = registry.GetComponentByName(entity, "Astra::Test::Position");
    ASSERT_NE(byName, nullptr);
    EXPECT_EQ(static_cast<Astra::Test::Position*>(byName)->x, 1.0f);
}
