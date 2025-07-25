//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0

/**
 * This is a minimal catch_user_config.hpp for use with premake5.
 * For CMake builds, this file would be autogenerated from catch_user_config.hpp.in
 */

#ifndef CATCH_USER_CONFIG_HPP_INCLUDED
#define CATCH_USER_CONFIG_HPP_INCLUDED

// Enable fast compile mode to speed up compilation
#define CATCH_CONFIG_FAST_COMPILE

// Default reporter
#define CATCH_CONFIG_DEFAULT_REPORTER "console"

// Console width
#define CATCH_CONFIG_CONSOLE_WIDTH 80

// Platform-specific settings
#ifdef _WIN32
    #define CATCH_CONFIG_COLOUR_WIN32
    #define CATCH_CONFIG_WINDOWS_SEH
#endif

// Enable wchar support
#define CATCH_CONFIG_WCHAR

// Enable C++17 features
#define CATCH_CONFIG_CPP17_OPTIONAL
#define CATCH_CONFIG_CPP17_STRING_VIEW
#define CATCH_CONFIG_CPP17_VARIANT
#define CATCH_CONFIG_CPP17_BYTE
#define CATCH_CONFIG_CPP17_UNCAUGHT_EXCEPTIONS

// Enable all string makers
#define CATCH_CONFIG_ENABLE_ALL_STRINGMAKERS
#define CATCH_CONFIG_ENABLE_OPTIONAL_STRINGMAKER
#define CATCH_CONFIG_ENABLE_PAIR_STRINGMAKER
#define CATCH_CONFIG_ENABLE_TUPLE_STRINGMAKER
#define CATCH_CONFIG_ENABLE_VARIANT_STRINGMAKER

#endif // CATCH_USER_CONFIG_HPP_INCLUDED