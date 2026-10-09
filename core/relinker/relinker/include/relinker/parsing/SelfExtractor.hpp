#ifndef RELINKER_PARSING_SELFEXTRACTOR_HPP
#define RELINKER_PARSING_SELFEXTRACTOR_HPP

#include <cstdint>
#include <vector>

namespace Relinker {

class SelfExtractor {
public:
    std::vector<std::uint8_t> Extract(std::vector<std::uint8_t> bytes) const;
};

}

#endif
