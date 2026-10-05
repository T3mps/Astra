#pragma once

#include <array>
#include <cstddef>
#include <string_view>

// Canonical, compiler-independent spelling of a C++ type name.
//
// TypeID<T>::Name() is derived from the compiler's pretty-function string
// (__FUNCSIG__ on MSVC, __PRETTY_FUNCTION__ on GCC and Clang), and
// TypeID<T>::Hash() -- the identity binary archives persist -- is the hash of
// that name. The three compilers spell the same type differently, so the raw
// slice is rewritten into one canonical form before it is hashed:
//
//   - no elaborated-type keywords:   MSVC "struct Tmpl<class Klass>" -> "Tmpl<Klass>"
//   - whitespace only where two identifiers would otherwise fuse:
//       "Pair<int, Tmpl<int> >" / "Pair<int,Tmpl<int> >" -> "Pair<int,Tmpl<int>>",
//       "int *" / "int*" -> "int*", "int [4]" -> "int[4]"
//   - integer types in one spelling: GCC "long unsigned int" and MSVC
//       "unsigned __int64" -> "unsigned long" / "unsigned long long"
//   - cv-qualifiers in front of the type they qualify: MSVC "int const *" -> "const int*"
//   - one anonymous-namespace spelling: GCC "{anonymous}", MSVC
//       "`anonymous namespace'" -> "(anonymous namespace)" (Clang's own spelling)
//   - no default calling convention: MSVC "void (__cdecl*)(int)" -> "void(*)(int)"
//   - no standard-library inline namespace: "std::__cxx11::" / "std::__1::" -> "std::"
//   - non-type template arguments as plain integers: "true"/"false" -> "1"/"0",
//       character literals ('a', '\x05', '\000') -> their value, and no
//       integer-literal suffixes ("5UL" -> "5"); MSVC already prints all of
//       these as plain integers
//
// The canonical form is identical on MSVC, GCC and Clang for: fundamental
// types; classes, structs, unions and enums at any namespace depth (anonymous
// namespaces included); class templates over those, nested to any depth;
// pointers, references, arrays, const/volatile, function and member pointers;
// and integral, bool and char non-type template arguments in the range
// 0..INT64_MAX (char: 0..127).
//
// It is NOT identical, because the compilers print different CONTENT rather
// than different formatting, for:
//   - class templates with defaulted template arguments as MSVC prints them:
//       MSVC writes the defaults out ("std::vector<int,std::allocator<int>>"),
//       GCC and Clang omit them ("std::vector<int>") -- this covers most
//       standard-library types (std::string, std::vector, std::map, ...);
//   - enumerator non-type template arguments: GCC/Clang print "Color::Red",
//       MSVC prints the underlying value;
//   - unsigned 64-bit non-type arguments above INT64_MAX (MSVC prints them
//       as negative) and char arguments outside 0..127 (signedness);
//   - fixed-width aliases whose underlying type differs by data model:
//       std::int64_t is "long" on LP64 Linux/macOS but "long long" on LLP64
//       Windows -- a distinct type, not a spelling difference;
//   - lambdas and function-local types, whose names are compiler-invented.
// Types in those groups hash consistently within one compiler, but not
// across compilers.
namespace Astra::Detail
{
    namespace TypeNameCanon
    {
        constexpr bool IsDigit(char c) noexcept { return c >= '0' && c <= '9'; }

        constexpr bool IsIdent(char c) noexcept
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || IsDigit(c) || c == '_';
        }

        constexpr bool IsSpace(char c) noexcept
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }

        constexpr bool StartsWithAt(std::string_view s, size_t at, std::string_view prefix) noexcept
        {
            return at <= s.size() && s.substr(at).starts_with(prefix);
        }

        // Words that combine into one integer type: "long unsigned int",
        // "unsigned __int64", "short", "signed char", ...
        constexpr bool IsIntegerKeyword(std::string_view w) noexcept
        {
            return w == "signed" || w == "unsigned" || w == "short" || w == "long" || w == "int"
                || w == "char" || w == "__int8" || w == "__int16" || w == "__int32" || w == "__int64";
        }

        // Clang's spelling, which is also MSVC's except for __int64.
        constexpr std::string_view CanonicalInteger(bool isUnsigned, bool isSigned, bool isChar,
                                                    bool isShort, int longs) noexcept
        {
            if (isChar)
                return isUnsigned ? "unsigned char" : (isSigned ? "signed char" : "char");
            if (isShort)
                return isUnsigned ? "unsigned short" : "short";
            if (longs == 1)
                return isUnsigned ? "unsigned long" : "long";
            if (longs >= 2)
                return isUnsigned ? "unsigned long long" : "long long";
            return isUnsigned ? "unsigned int" : "int";
        }

        inline constexpr std::string_view kAnonymousNamespace = "(anonymous namespace)";

        // Bounded output buffer. Counts every character it is given, writes the
        // ones that fit; the caller compares Size() against the capacity.
        class Writer
        {
        public:
            constexpr Writer(char* out, size_t capacity) noexcept : m_out(out), m_capacity(capacity) {}

            constexpr size_t Size() const noexcept { return m_size; }
            constexpr char At(size_t i) const noexcept { return i < m_capacity && i < m_size ? m_out[i] : '\0'; }
            constexpr char Last() const noexcept { return m_size ? At(m_size - 1) : '\0'; }

            constexpr void Put(char c) noexcept
            {
                if (m_size < m_capacity)
                    m_out[m_size] = c;
                ++m_size;
            }

            // Appends w, preceded by one space only when it would otherwise fuse
            // with the identifier already at the end.
            constexpr void Word(std::string_view w) noexcept
            {
                if (!w.empty() && IsIdent(Last()) && IsIdent(w.front()))
                    Put(' ');
                for (char c : w)
                    Put(c);
            }

            constexpr bool EndsWith(std::string_view s) const noexcept
            {
                if (s.size() > m_size)
                    return false;
                for (size_t k = 0; k < s.size(); ++k)
                {
                    if (At(m_size - s.size() + k) != s[k])
                        return false;
                }
                return true;
            }

            // The identifier [begin, end) written so far, or "" if out of range.
            constexpr std::string_view View(size_t begin, size_t end) const noexcept
            {
                if (end > m_size || end > m_capacity || begin > end)
                    return {};
                return std::string_view(m_out + begin, end - begin);
            }

            // Start of the identifier that ends at `end`.
            constexpr size_t WordStart(size_t end) const noexcept
            {
                while (end > 0 && IsIdent(At(end - 1)))
                    --end;
                return end;
            }

            // Inserts "w " at pos, shifting the tail right.
            constexpr void InsertWordAt(size_t pos, std::string_view w) noexcept
            {
                const size_t shift = w.size() + 1;
                for (size_t k = 0; k < shift; ++k)
                    Put('\0');
                if (m_size > m_capacity)
                    return;     // overflowed: only the length matters now
                for (size_t i = m_size; i-- > pos + shift;)
                    m_out[i] = m_out[i - shift];
                for (size_t k = 0; k < w.size(); ++k)
                    m_out[pos + k] = w[k];
                m_out[pos + w.size()] = ' ';
            }

        private:
            char* m_out;
            size_t m_capacity;
            size_t m_size = 0;
        };

        constexpr void PutNumber(Writer& w, unsigned long long value) noexcept
        {
            char digits[20] = {};
            size_t count = 0;
            do
            {
                digits[count++] = static_cast<char>('0' + value % 10);
                value /= 10;
            } while (value != 0);
            char text[20] = {};
            for (size_t k = 0; k < count; ++k)
                text[k] = digits[count - 1 - k];
            w.Word(std::string_view(text, count));
        }

        constexpr int HexValue(char c) noexcept
        {
            if (IsDigit(c)) return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        // Parses the character literal at s[at] == '\''. On success stores its
        // value and the index one past the closing quote.
        constexpr bool ParseCharLiteral(std::string_view s, size_t at, size_t& end, unsigned long long& value) noexcept
        {
            size_t j = at + 1;
            if (j >= s.size())
                return false;
            if (s[j] == '\\')
            {
                ++j;
                if (j >= s.size())
                    return false;
                const char e = s[j];
                if (e == 'x')
                {
                    ++j;
                    value = 0;
                    const size_t first = j;
                    while (j < s.size() && HexValue(s[j]) >= 0)
                        value = value * 16 + static_cast<unsigned long long>(HexValue(s[j++]));
                    if (j == first)
                        return false;
                }
                else if (e >= '0' && e <= '7')
                {
                    value = 0;
                    for (int k = 0; k < 3 && j < s.size() && s[j] >= '0' && s[j] <= '7'; ++k)
                        value = value * 8 + static_cast<unsigned long long>(s[j++] - '0');
                }
                else
                {
                    switch (e)
                    {
                        case 'n':  value = 10; break;
                        case 't':  value = 9;  break;
                        case 'r':  value = 13; break;
                        case 'a':  value = 7;  break;
                        case 'b':  value = 8;  break;
                        case 'f':  value = 12; break;
                        case 'v':  value = 11; break;
                        case '\\': value = 92; break;
                        case '\'': value = 39; break;
                        case '"':  value = 34; break;
                        case '?':  value = 63; break;
                        default:   return false;
                    }
                    ++j;
                }
            }
            else
            {
                value = static_cast<unsigned char>(s[j]);
                ++j;
            }
            if (j >= s.size() || s[j] != '\'')
                return false;
            end = j + 1;
            return true;
        }

        // Where the type specifier that ends the output starts: walks back over
        // a (possibly qualified, possibly templated) name or a multi-word
        // integer type. Used to move an east-const qualifier in front of it.
        constexpr size_t TypeSpecifierStart(const Writer& w) noexcept
        {
            size_t pos = w.Size();
            while (pos > 0)
            {
                const char ch = w.At(pos - 1);
                if (ch == '>')
                {
                    int depth = 0;
                    while (pos > 0)
                    {
                        const char d = w.At(--pos);
                        if (d == '>')
                            ++depth;
                        else if (d == '<' && --depth == 0)
                            break;
                    }
                    continue;
                }
                if (ch == ')' && pos >= kAnonymousNamespace.size()
                    && w.View(pos - kAnonymousNamespace.size(), pos) == kAnonymousNamespace)
                {
                    pos -= kAnonymousNamespace.size();
                    continue;
                }
                if (IsIdent(ch) || ch == ':')
                {
                    --pos;
                    continue;
                }
                if (ch == ' ')
                {
                    // Inside "unsigned long long": keep walking.
                    const size_t before = w.WordStart(pos - 1);
                    size_t after = pos;
                    while (IsIdent(w.At(after)))
                        ++after;
                    if (IsIntegerKeyword(w.View(before, pos - 1)) && IsIntegerKeyword(w.View(pos, after)))
                    {
                        pos = before;
                        continue;
                    }
                }
                break;
            }
            return pos;
        }

        // MSVC writes cv-qualifiers after the type ("int const *"), GCC and
        // Clang before it ("const int*"); a qualifier after '*', '&' or ')'
        // qualifies the pointer or member function and stays where it is.
        constexpr void PlaceQualifier(Writer& w, std::string_view qualifier) noexcept
        {
            const char last = w.Last();
            bool eastConst = (last == '>');
            if (IsIdent(last))
            {
                const std::string_view lastWord = w.View(w.WordStart(w.Size()), w.Size());
                eastConst = lastWord != "const" && lastWord != "volatile";
            }
            if (eastConst)
                w.InsertWordAt(TypeSpecifierStart(w), qualifier);
            else
                w.Word(qualifier);
        }
    }

    // Writes the canonical spelling of the raw compiler type name `raw` to
    // out[0, capacity) and returns its length. A result longer than capacity
    // is truncated in `out`, but the full length is still returned.
    constexpr size_t CanonicalizeTypeName(std::string_view raw, char* out, size_t capacity) noexcept
    {
        using namespace TypeNameCanon;
        Writer w(out, capacity);
        const size_t n = raw.size();
        size_t i = 0;
        while (i < n)
        {
            const char c = raw[i];
            if (IsSpace(c))
            {
                ++i;
                continue;
            }

            // MSVC "`anonymous namespace'" ("`anonymous-namespace'" when it is the
            // outermost scope of the whole name) and GCC "{anonymous}". Clang's
            // "(anonymous namespace)" is already canonical and flows through.
            if (StartsWithAt(raw, i, "`anonymous namespace'") || StartsWithAt(raw, i, "`anonymous-namespace'"))
            {
                w.Word(kAnonymousNamespace);
                i += 21;
                continue;
            }
            if (StartsWithAt(raw, i, "{anonymous}"))
            {
                w.Word(kAnonymousNamespace);
                i += 11;
                continue;
            }

            if (c == '\'')
            {
                size_t end = 0;
                unsigned long long value = 0;
                if (ParseCharLiteral(raw, i, end, value))
                {
                    PutNumber(w, value);
                    i = end;
                    continue;
                }
                w.Put(c);
                ++i;
                continue;
            }

            if (!IsIdent(c))
            {
                w.Put(c);
                ++i;
                continue;
            }

            size_t j = i;
            while (j < n && IsIdent(raw[j]))
                ++j;
            const std::string_view token = raw.substr(i, j - i);

            if (IsDigit(c))
            {
                // Integer literal: drop any u/U/l/L suffix.
                size_t keep = token.size();
                while (keep > 1)
                {
                    const char s = token[keep - 1];
                    if (s != 'u' && s != 'U' && s != 'l' && s != 'L')
                        break;
                    --keep;
                }
                w.Word(token.substr(0, keep));
                i = j;
                continue;
            }

            if (token == "class" || token == "struct" || token == "union" || token == "enum" || token == "__cdecl")
            {
                i = j;
                continue;
            }

            if ((token == "__cxx11" || token == "__1") && w.EndsWith("std::") && StartsWithAt(raw, j, "::"))
            {
                i = j + 2;
                continue;
            }

            if (token == "true" || token == "false")
            {
                w.Word(token == "true" ? "1" : "0");
                i = j;
                continue;
            }

            if (IsIntegerKeyword(token))
            {
                bool isUnsigned = false, isSigned = false, isChar = false, isShort = false;
                int longs = 0;
                size_t next = i;
                while (true)
                {
                    size_t wordEnd = next;
                    while (wordEnd < n && IsIdent(raw[wordEnd]))
                        ++wordEnd;
                    const std::string_view word = raw.substr(next, wordEnd - next);
                    if (!IsIntegerKeyword(word))
                        break;
                    if (word == "unsigned")      isUnsigned = true;
                    else if (word == "signed")   isSigned = true;
                    else if (word == "char" || word == "__int8") isChar = true;
                    else if (word == "short" || word == "__int16") isShort = true;
                    else if (word == "long")     ++longs;
                    else if (word == "__int64")  longs += 2;
                    j = wordEnd;
                    next = wordEnd;
                    while (next < n && IsSpace(raw[next]))
                        ++next;
                    if (next >= n || !IsIdent(raw[next]))
                        break;
                }
                w.Word(CanonicalInteger(isUnsigned, isSigned, isChar, isShort, longs));
                i = j;
                continue;
            }

            if (token == "const" || token == "volatile")
            {
                PlaceQualifier(w, token);
                i = j;
                continue;
            }

            w.Word(token);
            i = j;
        }
        return w.Size();
    }

    // Fixed-capacity result of CanonicalizeTypeName, usable as a constexpr value.
    template<size_t Capacity>
    struct CanonicalTypeNameBuffer
    {
        std::array<char, Capacity> chars{};
        size_t size = 0;
    };

    // The canonical form never exceeds twice the raw length plus a constant
    // ("{anonymous}" -> "(anonymous namespace)" is the largest expansion).
    constexpr size_t CanonicalTypeNameCapacity(size_t rawSize) noexcept
    {
        return rawSize * 2 + 32;
    }
}
