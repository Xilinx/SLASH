/**
 * The MIT License (MIT)
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * @file allocator.hpp
 * @brief Memory allocator with buddy-system block management.
 */

#ifndef VRT_ALLOCATOR_HPP
#define VRT_ALLOCATOR_HPP

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <vrtd/buffer.hpp>

namespace vrtd {
class Device;
}

namespace vrt {

typedef vrtd::BufferAllocType BufferAllocType;
typedef vrtd::BufferAllocDir BufferAllocDir;

enum class MemoryRangeType {
    HBM,
    DDR,
    HBM_VNOC,
};

enum class HBMRegion : uint64_t {
    HBM0 = 0,
    HBM1 = 1,
    HBM2 = 2,
    HBM3 = 3,
    HBM4 = 4,
    HBM5 = 5,
    HBM6 = 6,
    HBM7 = 7,
    HBM8 = 8,
    HBM9 = 9,
    HBM10 = 10,
    HBM11 = 11,
    HBM12 = 12,
    HBM13 = 13,
    HBM14 = 14,
    HBM15 = 15,
    HBM16 = 16,
    HBM17 = 17,
    HBM18 = 18,
    HBM19 = 19,
    HBM20 = 20,
    HBM21 = 21,
    HBM22 = 22,
    HBM23 = 23,
    HBM24 = 24,
    HBM25 = 25,
    HBM26 = 26,
    HBM27 = 27,
    HBM28 = 28,
    HBM29 = 29,
    HBM30 = 30,
    HBM31 = 31,
    HBM32 = 32,
    HBM33 = 33,
    HBM34 = 34,
    HBM35 = 35,
    HBM36 = 36,
    HBM37 = 37,
    HBM38 = 38,
    HBM39 = 39,
    HBM40 = 40,
    HBM41 = 41,
    HBM42 = 42,
    HBM43 = 43,
    HBM44 = 44,
    HBM45 = 45,
    HBM46 = 46,
    HBM47 = 47,
    HBM48 = 48,
    HBM49 = 49,
    HBM50 = 50,
    HBM51 = 51,
    HBM52 = 52,
    HBM53 = 53,
    HBM54 = 54,
    HBM55 = 55,
    HBM56 = 56,
    HBM57 = 57,
    HBM58 = 58,
    HBM59 = 59,
    HBM60 = 60,
    HBM61 = 61,
    HBM62 = 62,
    HBM63 = 63,

    NON_HBM = std::numeric_limits<uint64_t>::max(),
};

/**
 * @brief Describes the memory type and optional HBM port for a Buffer.
 *
 * Obtained from Kernel::portMemoryConfig() or Kernel::argMemoryConfig() and
 * passed directly to the Buffer constructor so callers do not need to specify
 * type and port separately.
 */
struct MemoryConfig {
    MemoryRangeType type;            ///< DDR, HBM, or HBM_VNOC
    std::optional<uint8_t> hbmPort; ///< Set only when type == HBM
};

/**
 * @brief RAII wrapper around a vrtd::Buffer allocation.
 */
class UntypedBuffer {
    vrtd::Buffer* backingBuffer;

    uint64_t size;
    uint64_t offset;
public:
    UntypedBuffer(std::nullptr_t) noexcept;
    UntypedBuffer(vrtd::Buffer* backingBuffer, uint64_t size = std::numeric_limits<uint64_t>::max(), uint64_t offset = 0);
    UntypedBuffer(const UntypedBuffer& parent, uint64_t size, uint64_t offset);
    virtual ~UntypedBuffer();

    BufferAllocType getAllocType() const noexcept;
    BufferAllocDir getAllocDir() const noexcept;
    HBMRegion getHBMRegion() const noexcept;
    uint64_t getSize() const noexcept;
    uint64_t getPhysAddr() const noexcept;
    void* data() const noexcept;

    void syncToDevice(uint64_t offset = 0, uint64_t size = std::numeric_limits<uint64_t>::max());
    void syncToHost(uint64_t offset = 0, uint64_t size = std::numeric_limits<uint64_t>::max());

    bool operator==(std::nullptr_t) const noexcept;
    bool operator!=(std::nullptr_t) const noexcept;
    friend bool operator==(std::nullptr_t, const UntypedBuffer& buffer) noexcept;
    friend bool operator!=(std::nullptr_t, const UntypedBuffer& buffer) noexcept;
};

/**
 * @brief Abstract base for allocated memory blocks.
 */
class Block {
public:
    Block();
    virtual ~Block();

    virtual UntypedBuffer *getUntypedBuffer() const noexcept = 0;
};

/**
 * @brief Direct allocation for buffers larger than 64 MB.
 */
class LargeBlock : public Block {
    std::unique_ptr<vrtd::Buffer> backingBuffer;
    std::unique_ptr<UntypedBuffer> untypedBuffer;
public:
    LargeBlock(vrtd::Device& device, BufferAllocType type, BufferAllocDir dir, uint64_t size, HBMRegion region = HBMRegion::NON_HBM);
    ~LargeBlock() override;

    UntypedBuffer *getUntypedBuffer() const noexcept override;
};

/**
 * @brief Template base for buddy-system superblock allocators.
 *
 * Manages power-of-two blocks from 2^MIN_K to 2^MAX_K bytes, splitting
 * larger blocks on allocation and coalescing buddies on deallocation.
 *
 * @tparam MIN_K  Log2 of the minimum block size.
 * @tparam MAX_K  Log2 of the maximum block size (superblock size).
 */
template <size_t MIN_K, size_t MAX_K>
class BuddySuperblockBase {
protected:
    static_assert(MIN_K <= MAX_K, "MIN_K must be <= MAX_K");
    static constexpr size_t kMin = MIN_K;
    static constexpr size_t kMax = MAX_K;
    static constexpr size_t kNumBuckets = MAX_K - MIN_K + 1;
    static constexpr size_t kToIndex(size_t k) { return k - MIN_K; }
    static size_t sizeToIndex(size_t size, const char* tooSmallError) {
        size_t k = 64 - __builtin_clzll((unsigned long long)(size - 1));
        if (k < MIN_K) {
            throw std::runtime_error(tooSmallError);
        }
        return kToIndex(k);
    }

    std::array<std::vector<UntypedBuffer>, kNumBuckets> freeList;

    void seed(const UntypedBuffer& whole, const char* tooSmallError, const char* tooLargeError) {
        // Seed the free list with the full superblock as a single buddy.
        size_t index = sizeToIndex(whole.getSize(), tooSmallError);
        if (index >= kNumBuckets) {
            throw std::runtime_error(tooLargeError);
        }
        freeList[index].push_back(whole);
    }

    UntypedBuffer allocate(uint64_t size, const char* tooSmallError) {
        // Round size to a bucket index (power-of-two) and search for a free buddy.
        size_t index = sizeToIndex(size, tooSmallError);
        if (index >= kNumBuckets) {
            throw std::bad_alloc();
        }

        for (size_t i = index; i < kNumBuckets; ++i) {
            if (freeList[i].empty()) {
                continue;
            }

            // Take the smallest available buddy and split until we reach the target size.
            UntypedBuffer buffer = freeList[i].back();
            freeList[i].pop_back();

            while (i > index) {
                --i;
                uint64_t halfSize = 1ULL << (MIN_K + i);
                // Return the upper half to the free list, keep the lower half to keep splitting.
                freeList[i].emplace_back(buffer, halfSize, halfSize);
                buffer = UntypedBuffer(buffer, halfSize, 0);
            }
            return buffer;
        }
        
        return nullptr;
    }

    void deallocate(const UntypedBuffer& whole, UntypedBuffer buffer, const char* tooSmallError, const char* ownershipError) {
        // Compute the buddy bucket for this size class.
        size_t index = sizeToIndex(buffer.getSize(), tooSmallError);
        if (index >= kNumBuckets) {
            throw std::runtime_error("Invalid buffer size for deallocation");
        }
        // Validate the slice belongs to this superblock.
        const uint64_t base = whole.getPhysAddr();
        const uint64_t totalSize = whole.getSize();
        uint64_t size = buffer.getSize();
        uint64_t offset = buffer.getPhysAddr() - base;
        if (offset + size > totalSize) {
            throw std::runtime_error(ownershipError);
        }

        // Coalesce with free buddy blocks while possible.
        size_t i = index;
        while (i + 1 < kNumBuckets) {
            const uint64_t buddyOffset = offset ^ size;
            auto& bucket = freeList[i];
            size_t buddyIndex = bucket.size();
            // Linear search for the buddy in this bucket.
            for (size_t j = 0; j < bucket.size(); ++j) {
                if (bucket[j].getPhysAddr() - base == buddyOffset) {
                    buddyIndex = j;
                    break;
                }
            }
            if (buddyIndex == bucket.size()) {
                break;
            }

            // Remove buddy from free list (swap-pop).
            if (buddyIndex + 1 != bucket.size()) {
                bucket[buddyIndex] = bucket.back();
            }
            bucket.pop_back();
            // Merge into next size class.
            offset = std::min(offset, buddyOffset);
            size <<= 1;
            ++i;
        }

        // Insert the final (possibly coalesced) block into its bucket.
        freeList[i].emplace_back(whole, size, offset);
    }

    bool isFree(const UntypedBuffer& whole, const char* tooSmallError) const {
        // Simplified check: rely on the full-size bucket as the indicator.
        size_t index = sizeToIndex(whole.getSize(), tooSmallError);
        assert(index < kNumBuckets);
        return freeList[index].size() == 1;
    }
};

/**
 * @brief Superblock managing 2 MB -- 64 MB sub-allocations via buddy system.
 */
class LargeBlockSuperblock : public LargeBlock, private BuddySuperblockBase<21, 26> {
    using Buddy = BuddySuperblockBase<21, 26>;  // 2MB - 64MB
public:
    static constexpr size_t MAX_SIZE = 1ULL << Buddy::kMax; // 64MB

    LargeBlockSuperblock(vrtd::Device& device, BufferAllocType type, BufferAllocDir dir, uint64_t size, HBMRegion region = HBMRegion::NON_HBM);
    ~LargeBlockSuperblock() override;

    UntypedBuffer allocate(uint64_t size);
    void deallocate(UntypedBuffer buffer);
    bool isFree() const;
};

/**
 * @brief Allocation from a LargeBlockSuperblock (2 MB -- 64 MB).
 */
class MediumBlock : public Block {
    std::unique_ptr<UntypedBuffer> untypedBuffer;
    LargeBlockSuperblock *backingBlockSuperblock;
public:
    MediumBlock(LargeBlockSuperblock *backingSuperblock, UntypedBuffer untypedBuffer);
    virtual ~MediumBlock() override;

    UntypedBuffer *getUntypedBuffer() const noexcept override;
};

/**
 * @brief Superblock managing 4 KB -- 2 MB sub-allocations via buddy system.
 */
class MediumBlockSuperblock : public MediumBlock, private BuddySuperblockBase<12, 21> {
    using Buddy = BuddySuperblockBase<12, 21>;  // 4KB - 2MB
public:
    static constexpr size_t MAX_SIZE = 1ULL << Buddy::kMax; // 2MB

    MediumBlockSuperblock(LargeBlockSuperblock *backingSuperblock, UntypedBuffer untypedBuffer);
    ~MediumBlockSuperblock() override;

    UntypedBuffer allocate(uint64_t size);
    void deallocate(UntypedBuffer buffer);
    bool isFree() const;
};

/**
 * @brief Allocation from a MediumBlockSuperblock (up to 2 MB).
 */
class SmallBlock : public Block {
    std::unique_ptr<UntypedBuffer> untypedBuffer;
    MediumBlockSuperblock *backingBlockSuperblock;
public:
    SmallBlock(MediumBlockSuperblock *backingBlockSuperblock, UntypedBuffer untypedBuffer);
    ~SmallBlock() override;

    UntypedBuffer *getUntypedBuffer() const noexcept override;
};

/**
 * @brief Top-level allocator dispatching to the buddy-system hierarchy.
 */
class Allocator {
    std::vector<std::unique_ptr<LargeBlockSuperblock>> largeBlockSuperblocks;
    std::vector<std::unique_ptr<MediumBlockSuperblock>> mediumBlockSuperblocks;
public:
    Allocator();
    ~Allocator();

    std::unique_ptr<Block> allocate(vrtd::Device& device, BufferAllocType type, BufferAllocDir dir, uint64_t size, HBMRegion region = HBMRegion::NON_HBM);
    void deallocate(std::unique_ptr<Block> block);
};

}  // namespace vrt

#endif  // VRT_ALLOCATOR_HPP
