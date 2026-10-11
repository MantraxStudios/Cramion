#ifndef CRAMION_FX_CORE_CHARS_H
#define CRAMION_FX_CORE_CHARS_H

// Numeros con decimales desde texto, como std::from_chars. La libc++ del
// Android NDK (antes de LLVM 20) no trae from_chars para float/double: alli
// se usa strtod sobre una copia del trozo (el texto no tiene por que acabar
// en '\0').

#include <charconv>
#include <cstdlib>
#include <string>
#include <system_error>

namespace cramion::core {

template <class T>
std::from_chars_result parseFloat(const char* first, const char* last, T& value) {
#if defined(__cpp_lib_to_chars)
    return std::from_chars(first, last, value);
#else
    const std::string text(first, last);
    char* end = nullptr;
    const double d = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) return {first, std::errc::invalid_argument};
    value = static_cast<T>(d);
    return {first + (end - text.c_str()), std::errc{}};
#endif
}

}  // namespace cramion::core

#endif  // CRAMION_FX_CORE_CHARS_H
