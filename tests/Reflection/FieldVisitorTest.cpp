#include <gtest/gtest.h>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Reflection/Reflection.hpp>
#include <Astra/Reflection/FieldVisitor.hpp>

#include <any>
#include <map>
#include <string>
#include <vector>

namespace
{
    struct Stats
    {
        int   hp = 0;
        float speed = 0.0f;
        bool  dead = false;
        int   derived = 0;   // opted OUT of serialization below
    };

    struct NoReflect   // registered as a component but never reflected
    {
        int x = 0;
    };

    class RecordingVisitor : public Astra::IFieldVisitor
    {
    public:
        std::vector<std::string> names;
        void Visit(const Astra::FieldInfo& field, void* /*instance*/) override
        {
            names.emplace_back(field.name);
        }
        bool IsWriting() const noexcept override { return true; }
    };

    class MapWriteVisitor : public Astra::IFieldVisitor
    {
    public:
        std::map<std::string, std::any>& out;
        explicit MapWriteVisitor(std::map<std::string, std::any>& o) : out(o) {}
        void Visit(const Astra::FieldInfo& field, void* instance) override
        {
            out[std::string(field.name)] = field.GetAny(instance);
        }
        bool IsWriting() const noexcept override { return true; }
    };

    class MapReadVisitor : public Astra::IFieldVisitor
    {
    public:
        const std::map<std::string, std::any>& in;
        explicit MapReadVisitor(const std::map<std::string, std::any>& i) : in(i) {}
        void Visit(const Astra::FieldInfo& field, void* instance) override
        {
            auto it = in.find(std::string(field.name));
            if (it != in.end())
                field.SetAny(instance, it->second);
        }
        bool IsWriting() const noexcept override { return false; }
    };
}

ASTRA_REFLECT_TYPE(Stats)
    ASTRA_REFLECT_FIELD(Stats, hp)
    ASTRA_REFLECT_FIELD(Stats, speed)
    ASTRA_REFLECT_FIELD(Stats, dead)
    ASTRA_REFLECT_FIELD(Stats, derived)
        ASTRA_REFLECT_ATTR(Serializable, false)
ASTRA_END_REFLECT_TYPE()

TEST(FieldVisitor, EnumeratesSerializableFieldsInOrder)
{
    Astra::ComponentRegistry creg;
    creg.RegisterComponent<Stats>();
    const auto* desc = creg.GetComponentDescriptor(Astra::TypeID<Stats>::Value());
    ASSERT_NE(desc, nullptr);
    ASSERT_NE(desc->visitFields, nullptr);

    Stats s{};
    RecordingVisitor rec;
    desc->visitFields(&s, rec);

    ASSERT_EQ(rec.names.size(), 3u);
    EXPECT_EQ(rec.names[0], "hp");
    EXPECT_EQ(rec.names[1], "speed");
    EXPECT_EQ(rec.names[2], "dead");
}

TEST(FieldVisitor, RoundTripsThroughAFormatAgnosticMap)
{
    Astra::ComponentRegistry creg;
    creg.RegisterComponent<Stats>();
    const auto* desc = creg.GetComponentDescriptor(Astra::TypeID<Stats>::Value());
    ASSERT_NE(desc, nullptr);

    Stats a{};
    a.hp = 42; a.speed = 3.5f; a.dead = true; a.derived = 99;

    std::map<std::string, std::any> blob;
    MapWriteVisitor w(blob);
    desc->visitFields(&a, w);

    Stats b{};
    MapReadVisitor r(blob);
    desc->visitFields(&b, r);

    EXPECT_EQ(b.hp, 42);
    EXPECT_FLOAT_EQ(b.speed, 3.5f);
    EXPECT_TRUE(b.dead);
    EXPECT_EQ(b.derived, 0);
}

TEST(FieldVisitor, UnreflectedComponentHasNullSlot)
{
    Astra::ComponentRegistry creg;
    creg.RegisterComponent<NoReflect>();
    const auto* desc = creg.GetComponentDescriptor(Astra::TypeID<NoReflect>::Value());
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->visitFields, nullptr);
}
