// vec.h
// the data behind a vec handle, so another plugin can read one with a plain #include (no runtime dependency).
//
// good to know
//   - the handle is a pointer Value tagged "vec.handle". its .ptr is a Vec. keep the tag in sync with vec.cpp
//   - a vec is a reference: copies of the handle share the same elements. not synchronized, one thread only

#pragma once

#include <embr/embr.h>

#include <vector>

namespace embrvec {

struct Vec {
    std::vector<embr::Value> items;
};

inline constexpr const char* kTag = "vec.handle";

// the Vec behind v, or null when v is not a vec handle
inline Vec* vecOf(const embr::Value& v) {
    if (!v.isPointer()) return nullptr;
    const auto& p = v.asPointer();
    return p.type == kTag ? static_cast<Vec*>(p.ptr) : nullptr;
}

} // namespace embrvec
