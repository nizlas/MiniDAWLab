#pragma once

// ============================================================================
// SPIKE-01 DIAGNOSTIC SCAFFOLDING — NOT PRODUCT CODE.
//
// Self-contained SHA-256 (FIPS 180-4) used only by the SPIKE-01 authoritative
// plugin-state capture probe (roadmap slice P0/P1A, canonical steering
// docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md §9.2, PID-001).
//
// Purpose: plugin state blobs may contain private paths, account or licensing
// material. The spike therefore never logs, stores, or prints raw state
// bytes — only {size, SHA-256 hex, timing}. This header is dependency-free
// (no JUCE) so the deterministic selftests can verify it against published
// FIPS test vectors.
//
// This is NOT the production fingerprint hash (PID-001 field-set/serialization
// remains a P1C concern); it exists solely so the spike can compare captures
// for byte equality without retaining the bytes.
//
// Removal: delete src/diagnostics/Spike01*.* and the flag-gated hook in the
// app; nothing in the product path depends on this file.
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace spike01
{

class Sha256
{
public:
    Sha256() { reset(); }

    void reset()
    {
        state_[0] = 0x6a09e667u; state_[1] = 0xbb67ae85u;
        state_[2] = 0x3c6ef372u; state_[3] = 0xa54ff53au;
        state_[4] = 0x510e527fu; state_[5] = 0x9b05688cu;
        state_[6] = 0x1f83d9abu; state_[7] = 0x5be0cd19u;
        totalBytes_ = 0;
        bufferLen_ = 0;
    }

    void update(const void* data, std::size_t len)
    {
        const auto* p = static_cast<const std::uint8_t*>(data);
        totalBytes_ += static_cast<std::uint64_t>(len);

        while (len > 0)
        {
            const std::size_t space = kBlockSize - bufferLen_;
            const std::size_t take = (len < space) ? len : space;
            std::memcpy(buffer_ + bufferLen_, p, take);
            bufferLen_ += take;
            p += take;
            len -= take;
            if (bufferLen_ == kBlockSize)
            {
                processBlock(buffer_);
                bufferLen_ = 0;
            }
        }
    }

    /// Finalizes and returns lowercase hex digest. The object must be reset()
    /// before reuse.
    std::string finishHex()
    {
        const std::uint64_t bitLen = totalBytes_ * 8u;

        // Padding: 0x80 then zeros until 56 mod 64, then 64-bit big-endian length.
        const std::uint8_t pad80 = 0x80u;
        update(&pad80, 1);
        // NOTE: update() above already counted the pad byte into totalBytes_,
        // but bitLen was latched first, so the encoded length is correct.
        const std::uint8_t zero = 0x00u;
        while (bufferLen_ != 56)
            update(&zero, 1);

        std::uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i)
            lenBytes[i] = static_cast<std::uint8_t>((bitLen >> (56 - 8 * i)) & 0xffu);
        update(lenBytes, 8);
        // After the final 8 length bytes the buffer is exactly full and has
        // been processed; bufferLen_ is 0 again.

        static const char* hexDigits = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (int i = 0; i < 8; ++i)
        {
            const std::uint32_t w = state_[i];
            for (int shift = 28; shift >= 0; shift -= 4)
                out.push_back(hexDigits[(w >> shift) & 0xfu]);
        }
        return out;
    }

    static std::string hashHex(const void* data, std::size_t len)
    {
        Sha256 h;
        h.update(data, len);
        return h.finishHex();
    }

private:
    static constexpr std::size_t kBlockSize = 64;

    static std::uint32_t rotr(std::uint32_t x, int n)
    {
        return (x >> n) | (x << (32 - n));
    }

    void processBlock(const std::uint8_t* block)
    {
        static const std::uint32_t K[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
            0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
            0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
            0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
            0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
            0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
        };

        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
        {
            w[i] = (static_cast<std::uint32_t>(block[i * 4 + 0]) << 24)
                 | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16)
                 | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8)
                 | (static_cast<std::uint32_t>(block[i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i)
        {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

        for (int i = 0; i < 64; ++i)
        {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + S1 + ch + K[i] + w[i];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = S0 + maj;

            h = g; g = f; f = e;
            e = d + temp1;
            d = c; c = b; b = a;
            a = temp1 + temp2;
        }

        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::uint32_t state_[8] {};
    std::uint64_t totalBytes_ = 0;
    std::uint8_t buffer_[kBlockSize] {};
    std::size_t bufferLen_ = 0;
};

} // namespace spike01
