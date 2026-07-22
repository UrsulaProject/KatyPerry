#include "PlistSupport.h"

#include <cstring>
#include <limits>
#include <stdexcept>

namespace bmt::detail
{
    void PlistDeleter::operator()(plist_t value) const noexcept
    {
        if (value)
            plist_free(value);
    }

    PlistPtr ParsePlist(std::span<const uint8_t> data, plist_format_t* format)
    {
        if (data.size() > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("plist is too large");
        std::string normalized;
        const char* input = reinterpret_cast<const char*>(data.data());
        size_t inputSize = data.size();
        if (data.size() >= 5 && std::memcmp(data.data(), "<?xml", 5) == 0)
        {
            normalized.assign(input, input + inputSize);
            size_t position = 0;
            while ((position = normalized.find("<integer>", position)) != std::string::npos)
            {
                const size_t valueStart = position + 9;
                const size_t valueEnd = normalized.find("</integer>", valueStart);
                if (valueEnd == std::string::npos)
                    break;
                size_t firstDigit = valueStart;
                if (firstDigit < valueEnd && normalized[firstDigit] == '-')
                    ++firstDigit;
                size_t nonZero = firstDigit;
                while (nonZero + 1 < valueEnd && normalized[nonZero] == '0')
                    ++nonZero;
                if (nonZero > firstDigit)
                    normalized.erase(firstDigit, nonZero - firstDigit);
                const size_t normalizedEnd = normalized.find("</integer>", firstDigit);
                position = normalizedEnd == std::string::npos
                    ? normalized.size() : normalizedEnd + 10;
            }
            input = normalized.data();
            inputSize = normalized.size();
        }
        plist_t raw = nullptr;
        plist_format_t parsedFormat = PLIST_FORMAT_NONE;
        if (plist_from_memory(input, static_cast<uint32_t>(inputSize),
                              &raw, &parsedFormat) != PLIST_ERR_SUCCESS || !raw)
            throw std::runtime_error("invalid property list");
        if (format)
            *format = parsedFormat;
        return PlistPtr(raw);
    }

    std::vector<uint8_t> SerializePlist(plist_t value, plist_format_t format)
    {
        char* bytes = nullptr;
        uint32_t size = 0;
        const plist_err_t result = format == PLIST_FORMAT_BINARY
            ? plist_to_bin(value, &bytes, &size)
            : plist_to_xml(value, &bytes, &size);
        if (result != PLIST_ERR_SUCCESS || !bytes)
            throw std::runtime_error("property list serialization failed");
        std::vector<uint8_t> output(bytes, bytes + size);
        plist_mem_free(bytes);
        return output;
    }

    std::string PlistString(plist_t dictionary, const char* key)
    {
        plist_t value = plist_dict_get_item(dictionary, key);
        if (!value || plist_get_node_type(value) != PLIST_STRING)
            return {};
        char* string = nullptr;
        plist_get_string_val(value, &string);
        std::string output = string ? string : "";
        if (string)
            plist_mem_free(string);
        return output;
    }
}
