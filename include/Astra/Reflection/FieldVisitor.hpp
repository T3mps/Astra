#pragma once

#include "FieldInfo.hpp"

namespace Astra
{
    // Format-agnostic field visitor. An end user (e.g. the Arcane engine)
    // implements this to drive ANY serialization format by walking reflection.
    // Astra ships NO format backend beyond the built-in binary path -- a JSON,
    // protobuf, or editor backend lives entirely in the consumer.
    class IFieldVisitor
    {
    public:
        virtual ~IFieldVisitor() = default;

        // Invoked once per serializable reflected field of a component instance.
        // `instance` is the component base pointer; read or write the field via
        // field.GetAny(instance) / field.SetAny(instance, value), the typed
        // field.Get<T>/Set<T>, or field.GetPtr<T>(instance). For a nested
        // reflected struct, look its type up by field.typeHash in MetaRegistry
        // and recurse (the consumer owns recursion policy and POD-math fallbacks).
        virtual void Visit(const FieldInfo& field, void* instance) = 0;

        // Direction hint so a single visitor type can serve read and write.
        ASTRA_NODISCARD virtual bool IsWriting() const noexcept = 0;
    };
}
