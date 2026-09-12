// FieldInfo's std::vector element access (2026-09-11, for Arcane's reflection
// -> JSON container branch and Inspector list editor): a consumer holding only
// a FieldInfo and the CONTAINING instance can size, grow, address, erase and
// insert elements type-erased, and resolve the element's own TypeMeta.
#include <gtest/gtest.h>

#include <Astra/Reflection/Reflection.hpp>

#include <string_view>
#include <vector>

namespace
{
    struct Slot { int id = 0; float weight = 1.0f; };
    struct Bag
    {
        int               tag = 0;
        std::vector<Slot> slots;
        std::vector<int>  counts;
    };
}

ASTRA_REFLECT_TYPE(Slot)
    ASTRA_REFLECT_FIELD(Slot, id)
    ASTRA_REFLECT_FIELD(Slot, weight)
ASTRA_END_REFLECT_TYPE()

ASTRA_REFLECT_TYPE(Bag)
    ASTRA_REFLECT_FIELD(Bag, tag)
    ASTRA_REFLECT_FIELD(Bag, slots)
    ASTRA_REFLECT_FIELD(Bag, counts)
ASTRA_END_REFLECT_TYPE()

namespace
{
    const Astra::FieldInfo* Field(std::string_view name)
    {
        const Astra::TypeMeta* meta = Astra::GetMeta<Bag>();
        if (!meta) return nullptr;
        for (const Astra::FieldInfo& f : meta->fields)
            if (f.name == name) return &f;
        return nullptr;
    }
}

TEST(FieldInfoVector, ElementMetadataIsDerivedForVectorFieldsOnly)
{
    const Astra::FieldInfo* slots = Field("slots");
    ASSERT_NE(slots, nullptr);
    ASSERT_TRUE(slots->isVector);
    EXPECT_EQ(slots->elementTypeHash, Astra::TypeID<Slot>::Hash());
    EXPECT_EQ(slots->elementSize, sizeof(Slot));
    EXPECT_NE(Astra::GetMeta(slots->elementTypeHash), nullptr);   // the element's own TypeMeta resolves
    EXPECT_TRUE(static_cast<bool>(slots->vectorSize));
    EXPECT_TRUE(static_cast<bool>(slots->vectorResize));
    EXPECT_TRUE(static_cast<bool>(slots->vectorElement));
    EXPECT_TRUE(static_cast<bool>(slots->vectorErase));
    EXPECT_TRUE(static_cast<bool>(slots->vectorInsert));

    const Astra::FieldInfo* tag = Field("tag");
    ASSERT_NE(tag, nullptr);
    EXPECT_FALSE(tag->isVector);
    EXPECT_EQ(tag->elementTypeHash, 0u);
    EXPECT_EQ(tag->elementSize, 0u);
    EXPECT_FALSE(static_cast<bool>(tag->vectorSize));
}

TEST(FieldInfoVector, AccessorsOperateOnTheContainingInstance)
{
    const Astra::FieldInfo* slots = Field("slots");
    ASSERT_NE(slots, nullptr);
    Bag bag;
    EXPECT_EQ(slots->vectorSize(&bag), 0u);

    slots->vectorResize(&bag, 2);                       // growth default-constructs
    ASSERT_EQ(bag.slots.size(), 2u);
    EXPECT_EQ(bag.slots[1].id, 0);

    static_cast<Slot*>(slots->vectorElement(&bag, 1))->id = 7;
    EXPECT_EQ(bag.slots[1].id, 7);
    EXPECT_EQ(slots->vectorElement(&bag, 2), nullptr);  // past the end

    slots->vectorInsert(&bag, 0);                       // default element BEFORE index 0
    ASSERT_EQ(bag.slots.size(), 3u);
    EXPECT_EQ(bag.slots[0].id, 0);
    EXPECT_EQ(bag.slots[2].id, 7);

    slots->vectorInsert(&bag, 99);                      // i >= size appends
    EXPECT_EQ(bag.slots.size(), 4u);

    slots->vectorErase(&bag, 0);
    ASSERT_EQ(bag.slots.size(), 3u);
    EXPECT_EQ(bag.slots[1].id, 7);

    slots->vectorErase(&bag, 99);                       // out of range: no-op
    EXPECT_EQ(bag.slots.size(), 3u);

    slots->vectorResize(&bag, 0);
    EXPECT_EQ(slots->vectorSize(&bag), 0u);
}

TEST(FieldInfoVector, ScalarElementsGetTheSameAccessors)
{
    const Astra::FieldInfo* counts = Field("counts");
    ASSERT_NE(counts, nullptr);
    EXPECT_EQ(counts->elementTypeHash, Astra::TypeID<int>::Hash());
    EXPECT_EQ(counts->elementSize, sizeof(int));
    Bag bag;
    counts->vectorResize(&bag, 1);
    *static_cast<int*>(counts->vectorElement(&bag, 0)) = 42;
    EXPECT_EQ(bag.counts[0], 42);
}
