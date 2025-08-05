# Astra ECS

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![C++](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)](https://github.com/T3mps/Astra)

A modern Entity Component System reimagined for contemporary CPU architectures and cache hierarchies.

## 🌟 Overview

Astra is a high-performance Entity Component System that breaks from 15+ years of established ECS patterns. Instead of traditional sparse-set implementations with multiple indirections, Astra uses a SwissTable-inspired `FlatMap` for direct entity-to-component mapping.

```cpp
// Traditional ECS: Entity → Sparse Array → Dense Array → Component
auto* component = sparse[entity] != INVALID ? &dense[sparse[entity]] : nullptr;

// Astra: Entity → Component (direct mapping)
auto* component = pool.TryGet(entity);
```

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

---

<p align="center">
Made with ❤️ for the game development community<br/>
</p>
