# Astra ECS

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![C++](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)](https://github.com/T3mps/Astra)

## 🌟 Overview

A modern Entity Component System reimagined for contemporary CPU architectures and cache hierarchies.

## Benchmark

```
=== ASTRA ECS BENCHMARK ===
Testing multi-component iteration performance


--- 1000 Entities ---
Setup time: 0.4252 ms
Entities with Pos: 1000
Entities with Pos+Vel: 800
Entities with Pos+Vel+Health: 600
Entities with all 4 components: 400

Position iteration                 :      0.000 ms (stddev=   0.000, min=   0.000, max=   0.006)
  Per entity                       :      0.459 ns (stddev=   0.180, min=   0.400, max=   5.900)
Position + Velocity iteration      :      0.001 ms (stddev=   0.000, min=   0.000, max=   0.007)
  Per entity                       :      0.662 ns (stddev=   0.257, min=   0.500, max=   8.375)
Pos + Vel + Health iteration       :      0.000 ms (stddev=   0.000, min=   0.000, max=   0.006)
  Per entity                       :      0.493 ns (stddev=   0.306, min=   0.333, max=  10.000)
All 4 components iteration         :      0.000 ms (stddev=   0.000, min=   0.000, max=   0.000)
  Per entity                       :      0.581 ns (stddev=   0.117, min=   0.500, max=   0.750)

Random access (1000 entities):
  Time                             :      0.037 ms (stddev=   0.004, min=   0.035, max=   0.104)

--- 10000 Entities ---
Setup time: 1.563 ms
Entities with Pos: 10000
Entities with Pos+Vel: 8000
Entities with Pos+Vel+Health: 6000
Entities with all 4 components: 4000

Position iteration                 :      0.004 ms (stddev=   0.001, min=   0.004, max=   0.031)
  Per entity                       :      0.447 ns (stddev=   0.147, min=   0.420, max=   3.090)
Position + Velocity iteration      :      0.004 ms (stddev=   0.000, min=   0.004, max=   0.009)
  Per entity                       :      0.537 ns (stddev=   0.025, min=   0.525, max=   1.188)
Pos + Vel + Health iteration       :      0.003 ms (stddev=   0.000, min=   0.003, max=   0.009)
  Per entity                       :      0.462 ns (stddev=   0.050, min=   0.450, max=   1.583)
All 4 components iteration         :      0.002 ms (stddev=   0.000, min=   0.002, max=   0.009)
  Per entity                       :      0.525 ns (stddev=   0.065, min=   0.500, max=   2.300)

Random access (1000 entities):
  Time                             :      0.038 ms (stddev=   0.005, min=   0.036, max=   0.133)

--- 50000 Entities ---
Setup time: 6.997 ms
Entities with Pos: 50000
Entities with Pos+Vel: 40000
Entities with Pos+Vel+Health: 30000
Entities with all 4 components: 20000

Position iteration                 :      0.023 ms (stddev=   0.002, min=   0.022, max=   0.061)
  Per entity                       :      0.453 ns (stddev=   0.041, min=   0.442, max=   1.224)
Position + Velocity iteration      :      0.027 ms (stddev=   0.007, min=   0.023, max=   0.166)
  Per entity                       :      0.681 ns (stddev=   0.164, min=   0.580, max=   4.147)
Pos + Vel + Health iteration       :      0.017 ms (stddev=   0.007, min=   0.015, max=   0.157)
  Per entity                       :      0.579 ns (stddev=   0.229, min=   0.513, max=   5.247)
All 4 components iteration         :      0.013 ms (stddev=   0.006, min=   0.011, max=   0.114)
  Per entity                       :      0.636 ns (stddev=   0.276, min=   0.530, max=   5.685)

Random access (1000 entities):
  Time                             :      0.039 ms (stddev=   0.008, min=   0.037, max=   0.188)

--- 100000 Entities ---
Setup time: 13.690 ms
Entities with Pos: 100000
Entities with Pos+Vel: 80000
Entities with Pos+Vel+Health: 60000
Entities with all 4 components: 40000

Position iteration                 :      0.049 ms (stddev=   0.015, min=   0.044, max=   0.338)
  Per entity                       :      0.487 ns (stddev=   0.145, min=   0.438, max=   3.384)
Position + Velocity iteration      :      0.050 ms (stddev=   0.008, min=   0.046, max=   0.231)
  Per entity                       :      0.627 ns (stddev=   0.104, min=   0.581, max=   2.882)
Pos + Vel + Health iteration       :      0.039 ms (stddev=   0.018, min=   0.032, max=   0.291)
  Per entity                       :      0.643 ns (stddev=   0.303, min=   0.538, max=   4.852)
All 4 components iteration         :      0.030 ms (stddev=   0.002, min=   0.029, max=   0.089)
  Per entity                       :      0.756 ns (stddev=   0.060, min=   0.725, max=   2.232)

Random access (1000 entities):
  Time                             :      0.040 ms (stddev=   0.021, min=   0.037, max=   0.669)

--- 500000 Entities ---
Setup time: 94.866 ms
Entities with Pos: 500000
Entities with Pos+Vel: 400000
Entities with Pos+Vel+Health: 300000
Entities with all 4 components: 200000

Position iteration                 :      0.339 ms (stddev=   0.045, min=   0.293, max=   0.754)
  Per entity                       :      0.679 ns (stddev=   0.091, min=   0.586, max=   1.507)
Position + Velocity iteration      :      0.460 ms (stddev=   0.251, min=   0.335, max=   7.471)
  Per entity                       :      1.151 ns (stddev=   0.628, min=   0.838, max=  18.678)
Pos + Vel + Health iteration       :      0.334 ms (stddev=   0.096, min=   0.241, max=   1.341)
  Per entity                       :      1.115 ns (stddev=   0.320, min=   0.804, max=   4.469)
All 4 components iteration         :      0.245 ms (stddev=   0.083, min=   0.164, max=   0.707)
  Per entity                       :      1.227 ns (stddev=   0.416, min=   0.818, max=   3.534)

Random access (1000 entities):
  Time                             :      0.049 ms (stddev=   0.011, min=   0.043, max=   0.189)

--- 1000000 Entities ---
Setup time: 188.677 ms
Entities with Pos: 1000000
Entities with Pos+Vel: 800000
Entities with Pos+Vel+Health: 600000
Entities with all 4 components: 400000

Position iteration                 :      0.925 ms (stddev=   0.200, min=   0.736, max=   2.067)
  Per entity                       :      0.925 ns (stddev=   0.200, min=   0.736, max=   2.067)
Position + Velocity iteration      :      1.296 ms (stddev=   0.241, min=   0.961, max=   2.870)
  Per entity                       :      1.620 ns (stddev=   0.301, min=   1.202, max=   3.587)
Pos + Vel + Health iteration       :      0.975 ms (stddev=   0.167, min=   0.738, max=   1.998)
  Per entity                       :      1.625 ns (stddev=   0.278, min=   1.230, max=   3.329)
All 4 components iteration         :      0.711 ms (stddev=   0.179, min=   0.515, max=   2.508)
  Per entity                       :      1.778 ns (stddev=   0.447, min=   1.286, max=   6.270)

Random access (1000 entities):
  Time                             :      0.049 ms (stddev=   0.008, min=   0.046, max=   0.174)
```

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

---

<p align="center">
Made with ❤️ for the game development community<br/>
</p>
