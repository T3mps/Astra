#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace
{
    // Test type with a plain field, a const field, and a differently-typed field,
    // used to exercise FieldInfo's type-mismatch and const-field guards.
    // Copy/move construction is explicitly disabled: a struct with a const member
    // has no copy-assignment operator, which would otherwise make
    // Detail::TypeMetaBuilder<RFields>'s constructor (which builds a copyAssign/
    // moveAssign std::function whenever is_copy_constructible_v<T> is true) fail
    // to compile. Deleting the copy constructor makes is_copy_constructible_v
    // (and, transitively, is_move_constructible_v) false, so that guarded block
    // is skipped entirely - unrelated to the FieldInfo guards under test here.
    struct RFields
    {
        int a = 5;
        const int b = 9;
        float c = 1.5f;

        RFields() = default;
        RFields(const RFields&) = delete;
    };
}

// Reflect the test type - must be outside anonymous namespace for static initialization
// (mirrors tests/Reflection/ReflectionTest.cpp's registration pattern).
ASTRA_REFLECT_TYPE(RFields)
    ASTRA_REFLECT_FIELD(RFields, a)
    ASTRA_REFLECT_FIELD(RFields, b)
    ASTRA_REFLECT_FIELD(RFields, c)
ASTRA_END_REFLECT_TYPE()

TEST(FieldAccessorSafety, WrongTypeGetReturnsDefaultNotGarbage)
{
    using namespace Astra;

    RFields obj;
    const TypeMeta* meta = GetMeta<RFields>();
    ASSERT_NE(meta, nullptr);

    const FieldInfo* fa = meta->GetField("a");           // int field
    ASSERT_NE(fa, nullptr);

    // Ask for the wrong type: must not read a differently-sized object.
    double wrong = fa->Get<double>(&obj);
    EXPECT_EQ(wrong, double{});                            // value-initialized, not garbage
}

TEST(FieldAccessorSafety, SetOnConstFieldIsNoOp)
{
    using namespace Astra;

    RFields obj;
    const TypeMeta* meta = GetMeta<RFields>();
    ASSERT_NE(meta, nullptr);

    const FieldInfo* fb = meta->GetField("b");           // const int field
    ASSERT_NE(fb, nullptr);

    fb->Set<int>(&obj, 123);                               // must NOT terminate; must be a no-op
    EXPECT_EQ(obj.b, 9);
}

TEST(FieldAccessorSafety, GetPtrOnConstFieldReturnsNull)
{
    using namespace Astra;

    RFields obj;
    const TypeMeta* meta = GetMeta<RFields>();
    ASSERT_NE(meta, nullptr);

    const FieldInfo* fb = meta->GetField("b");           // const int field
    ASSERT_NE(fb, nullptr);

    EXPECT_EQ(fb->GetPtr<int>(&obj), nullptr);             // no writable pointer into a const field
}
