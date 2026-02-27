#pragma once

// Astra ECS - A high-performance Entity Component System library
// This header includes all Astra ECS headers in the correct dependency order

// Core headers - fundamental types and utilities
#include "Core/Base.hpp"
#include "Core/Platform.hpp"
#include "Core/Simd.hpp"
#include "Core/Version.hpp"

// Core utilities
#include "Core/Delegate.hpp"
#include "Core/Result.hpp"
#include "Core/Signal.hpp"
#include "Core/TypeID.hpp"

// Memory management
#include "Core/Memory.hpp"

// Container types
#include "Container/AlignedStorage.hpp"
#include "Container/Bitmap.hpp"
#include "Container/FlatMap.hpp"
#include "Container/FlatSet.hpp"
#include "Container/SmallVector.hpp"
#include "Container/Swiss.hpp"

// Entity system
#include "Entity/Entity.hpp"
#include "Entity/EntityIDStack.hpp"
#include "Entity/EntityManager.hpp"
#include "Entity/EntityTable.hpp"

// Component system
#include "Component/Component.hpp"
#include "Component/ComponentRegistry.hpp"

// Archetype system
#include "Archetype/Archetype.hpp"
#include "Archetype/ArchetypeChunkPool.hpp"
#include "Archetype/ArchetypeGraph.hpp"
#include "Archetype/ArchetypeManager.hpp"

// Registry and queries
#include "Registry/Query.hpp"
#include "Registry/Registry.hpp"
#include "Registry/Relations.hpp"
#include "Registry/RelationshipGraph.hpp"
#include "Registry/View.hpp"

// Command buffer system
#include "Commands/Command.hpp"  // Just for Command struct
#include "Commands/CommandBuffer.hpp"

// System support
#include "System/System.hpp"
#include "System/SystemExecutor.hpp"
#include "System/SystemMetadata.hpp"
#include "System/SystemScheduler.hpp"

// Serialization
#include "Serialization/BinaryArchive.hpp"
#include "Serialization/BinaryReader.hpp"
#include "Serialization/BinaryWriter.hpp"
#include "Serialization/Compression/Compression.hpp"
#include "Serialization/Compression/LZ4Decoder.hpp"
#include "Serialization/SerializationError.hpp"

// Reflection
#include "Reflection/Reflection.hpp"
