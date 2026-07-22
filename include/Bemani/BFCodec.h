//
// Created by Naville on 2026/7/16.
//

#ifndef BMT_BFCODEC_H
#define BMT_BFCODEC_H
#include <array>
#include <cstdint>
#include <vector>
#include <utility>
namespace bmt
{
    class BFCodec
    {
    public:
        BFCodec() = delete;
        BFCodec(const std::array<uint32_t,18>& P,const std::array<std::array<uint32_t,256>,4> S,const std::vector<uint8_t>& Key);
        std::vector<uint8_t> DecryptCBC(const std::vector<uint8_t>& cipherText,const std::array<uint8_t,8>& IV) const;
        std::vector<uint8_t> EncryptCBC(const std::vector<uint8_t>& plainText,const std::array<uint8_t,8>& IV) const;
    protected:
        std::array<uint32_t,18> P;
        std::array<std::array<uint32_t,256>,4> S;
    private:
        uint32_t F(uint32_t x) const noexcept;
        std::pair<uint32_t,uint32_t> EncryptBlock(uint32_t L, uint32_t R) const noexcept;
        std::pair<uint32_t,uint32_t> DecryptBlock(uint32_t L, uint32_t R) const noexcept;
    };
}

#endif //BMT_BFCODEC_H
