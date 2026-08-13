#pragma once

#include <libical/ical.h>
#include <memory>
// • `struct icalrecurrencetype` (libical 3.0)                          → plain VALUE type.
//   `icalproperty_get_rrule()` / `icalproperty_new_rrule()` /
//   `icalrecur_iterator_new()` all take/return it by value — no pointer,
//   no free/unref. Just declare it on the stack and zero it with
//   `icalrecurrencetype_clear()` before filling in fields.
namespace ical {

using ComponentPtr = std::unique_ptr<icalcomponent, decltype(&icalcomponent_free)>;
using RecurIterPtr = std::unique_ptr<icalrecur_iterator, decltype(&icalrecur_iterator_free)>;

// icalmemory_free_buffer takes void*, but we want a typed unique_ptr<char>.
// Wrap the call in a deleter functor.
struct MemBufferDeleter {
    void operator()(char* p) const noexcept {
        if (p) icalmemory_free_buffer(p);
    }
};
using StringPtr = std::unique_ptr<char, MemBufferDeleter>;

// Construction helpers — explicit at call site, hides the deleter wiring.
inline ComponentPtr wrapComponent(icalcomponent* p) noexcept {
    return { p, &icalcomponent_free };
}

inline StringPtr wrapString(char* p) noexcept {
    return StringPtr(p);
}

} // namespace ical
