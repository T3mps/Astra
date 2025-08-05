# Astra ECS

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![C++](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)](https://github.com/T3mps/Astra)

## 🌟 Overview

A modern Entity Component System reimagined for contemporary CPU architectures and cache hierarchies.

## Benchmark

```
=== ARCHETYPE ECS BENCHMARK ===
Testing multi-component iteration performance


--- 1000 Entities ---
Setup time: 0.5069 ms
Entities with Pos: 1000
Entities with Pos+Vel: 800
Entities with Pos+Vel+Health: 600
Entities with all 4 components: 400

Position iteration                 :      0.001 ms (stddev=   0.009, min=   0.000, max=   0.266)
  Per entity                       :      1.258 ns (stddev=   8.634, min=   0.400, max= 265.500)
Position + Velocity iteration      :      0.001 ms (stddev=   0.001, min=   0.000, max=   0.025)
  Per entity                       :      0.970 ns (stddev=   1.520, min=   0.500, max=  31.250)
Pos + Vel + Health iteration       :      0.000 ms (stddev=   0.000, min=   0.000, max=   0.009)
  Per entity                       :      0.528 ns (stddev=   0.494, min=   0.333, max=  15.833)
All 4 components iteration         :      0.000 ms (stddev=   0.000, min=   0.000, max=   0.011)
  Per entity                       :      0.796 ns (stddev=   0.942, min=   0.500, max=  28.750)

Random access (1000 entities):
  Time                             :      0.039 ms (stddev=   0.009, min=   0.036, max=   0.180)

--- 10000 Entities ---
Setup time: 1.728 ms
Entities with Pos: 10000
Entities with Pos+Vel: 8000
Entities with Pos+Vel+Health: 6000
Entities with all 4 components: 4000

Position iteration                 :      0.004 ms (stddev=   0.001, min=   0.004, max=   0.030)
  Per entity                       :      0.447 ns (stddev=   0.097, min=   0.430, max=   3.030)
Position + Velocity iteration      :      0.006 ms (stddev=   0.000, min=   0.004, max=   0.014)
  Per entity                       :      0.690 ns (stddev=   0.052, min=   0.537, max=   1.738)
Pos + Vel + Health iteration       :      0.003 ms (stddev=   0.001, min=   0.003, max=   0.019)
  Per entity                       :      0.465 ns (stddev=   0.101, min=   0.433, max=   3.183)
All 4 components iteration         :      0.003 ms (stddev=   0.008, min=   0.002, max=   0.128)
  Per entity                       :      0.724 ns (stddev=   2.001, min=   0.500, max=  32.000)

Random access (1000 entities):
  Time                             :      0.038 ms (stddev=   0.005, min=   0.036, max=   0.157)

--- 50000 Entities ---
Setup time: 6.983 ms
Entities with Pos: 50000
Entities with Pos+Vel: 40000
Entities with Pos+Vel+Health: 30000
Entities with all 4 components: 20000

Position iteration                 :      0.025 ms (stddev=   0.007, min=   0.022, max=   0.107)
  Per entity                       :      0.494 ns (stddev=   0.144, min=   0.436, max=   2.138)
Position + Velocity iteration      :      0.029 ms (stddev=   0.006, min=   0.028, max=   0.185)
  Per entity                       :      0.734 ns (stddev=   0.140, min=   0.690, max=   4.625)
Pos + Vel + Health iteration       :      0.018 ms (stddev=   0.006, min=   0.016, max=   0.127)
  Per entity                       :      0.587 ns (stddev=   0.203, min=   0.523, max=   4.217)
All 4 components iteration         :      0.012 ms (stddev=   0.003, min=   0.011, max=   0.113)
  Per entity                       :      0.589 ns (stddev=   0.170, min=   0.535, max=   5.665)

Random access (1000 entities):
  Time                             :      0.039 ms (stddev=   0.014, min=   0.036, max=   0.391)

--- 100000 Entities ---
Setup time: 13.452 ms
Entities with Pos: 100000
Entities with Pos+Vel: 80000
Entities with Pos+Vel+Health: 60000
Entities with all 4 components: 40000

Position iteration                 :      0.047 ms (stddev=   0.008, min=   0.044, max=   0.163)
  Per entity                       :      0.467 ns (stddev=   0.083, min=   0.437, max=   1.632)
Position + Velocity iteration      :      0.058 ms (stddev=   0.010, min=   0.047, max=   0.167)
  Per entity                       :      0.722 ns (stddev=   0.128, min=   0.583, max=   2.084)
Pos + Vel + Health iteration       :      0.034 ms (stddev=   0.004, min=   0.032, max=   0.110)
  Per entity                       :      0.568 ns (stddev=   0.065, min=   0.538, max=   1.827)
All 4 components iteration         :      0.033 ms (stddev=   0.011, min=   0.027, max=   0.197)
  Per entity                       :      0.817 ns (stddev=   0.280, min=   0.685, max=   4.933)

Random access (1000 entities):
  Time                             :      0.044 ms (stddev=   0.018, min=   0.037, max=   0.305)

--- 500000 Entities ---
Setup time: 96.015 ms
Entities with Pos: 500000
Entities with Pos+Vel: 400000
Entities with Pos+Vel+Health: 300000
Entities with all 4 components: 200000

Position iteration                 :      0.387 ms (stddev=   0.078, min=   0.298, max=   0.880)
  Per entity                       :      0.773 ns (stddev=   0.156, min=   0.595, max=   1.760)
Position + Velocity iteration      :      0.444 ms (stddev=   0.160, min=   0.339, max=   3.426)
  Per entity                       :      1.110 ns (stddev=   0.399, min=   0.847, max=   8.564)
Pos + Vel + Health iteration       :      0.327 ms (stddev=   0.143, min=   0.232, max=   2.128)
  Per entity                       :      1.091 ns (stddev=   0.478, min=   0.772, max=   7.093)
All 4 components iteration         :      0.248 ms (stddev=   0.217, min=   0.157, max=   3.546)
  Per entity                       :      1.239 ns (stddev=   1.083, min=   0.785, max=  17.730)

Random access (1000 entities):
  Time                             :      0.045 ms (stddev=   0.013, min=   0.042, max=   0.330)

--- 1000000 Entities ---
Setup time: 218.002 ms
Entities with Pos: 1000000
Entities with Pos+Vel: 800000
Entities with Pos+Vel+Health: 600000
Entities with all 4 components: 400000

Position iteration                 :      1.094 ms (stddev=   0.332, min=   0.833, max=   7.809)
  Per entity                       :      1.094 ns (stddev=   0.332, min=   0.833, max=   7.809)
Position + Velocity iteration      :      1.571 ms (stddev=   0.442, min=   1.127, max=   6.213)
  Per entity                       :      1.964 ns (stddev=   0.553, min=   1.409, max=   7.766)
Pos + Vel + Health iteration       :      1.137 ms (stddev=   0.182, min=   0.840, max=   2.450)
  Per entity                       :      1.894 ns (stddev=   0.303, min=   1.401, max=   4.083)
All 4 components iteration         :      0.769 ms (stddev=   0.428, min=   0.503, max=  12.526)
  Per entity                       :      1.923 ns (stddev=   1.070, min=   1.258, max=  31.316)

Random access (1000 entities):
  Time                             :      0.052 ms (stddev=   0.010, min=   0.047, max=   0.204)
```

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

---

<p align="center">
Made with ❤️ for the game development community<br/>
</p>
