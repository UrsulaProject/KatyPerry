#ifndef BMT_PLIST_SUPPORT_H
#define BMT_PLIST_SUPPORT_H

#include <plist/plist.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace bmt::detail
{
    struct PlistDeleter
    {
        void operator()(plist_t value) const noexcept;
    };
    using PlistPtr = std::unique_ptr<std::remove_pointer_t<plist_t>, PlistDeleter>;

    PlistPtr ParsePlist(std::span<const uint8_t> data,
                        plist_format_t* format = nullptr);
    std::vector<uint8_t> SerializePlist(plist_t value, plist_format_t format);
    std::string PlistString(plist_t dictionary, const char* key);
}

#endif
