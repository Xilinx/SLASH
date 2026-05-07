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
 * @file allocator.cpp
 * @brief Memory allocator implementation.
 */

#include <vrt/allocator/allocator.hpp>

#include <cstdlib>

#include <vrt/utils/logger.hpp>
#include <vrtd/device.hpp>

namespace vrt {
namespace {

bool matchesAllocation(const UntypedBuffer* buffer, BufferAllocType type, BufferAllocDir dir,
                       HBMRegion region) {
    if (buffer == nullptr) {
        return false;
    }

    if (buffer->getAllocType() != type || buffer->getAllocDir() != dir) {
        return false;
    }

    // Region is meaningful only for explicit HBM allocations.
    if (type == BufferAllocType::Hbm) {
        return buffer->getHBMRegion() == region;
    }

    return true;
}

}  // namespace


UntypedBuffer::UntypedBuffer(std::nullptr_t) noexcept
    : backingBuffer(nullptr),
      size(0),
      offset(0)
{}

UntypedBuffer::UntypedBuffer(vrtd::Buffer* backingBuffer, uint64_t size, uint64_t offset)
    : backingBuffer(backingBuffer),
      // Default to full backing size when size is "max" sentinel.
      size(size == std::numeric_limits<uint64_t>::max() ? backingBuffer->getSize() : size),
      // Offset is relative to the backing buffer base.
      offset(offset)
{}

UntypedBuffer::UntypedBuffer(const UntypedBuffer& parent, uint64_t size, uint64_t offset)
    : backingBuffer(parent.backingBuffer),
      // Size defaults to parent size when size is "max" sentinel.
      size(size == std::numeric_limits<uint64_t>::max() ? parent.size : size),
      // Child offsets are relative to the parent slice.
      offset(parent.offset + offset)
{}

UntypedBuffer::~UntypedBuffer() {}

BufferAllocType UntypedBuffer::getAllocType() const noexcept {
    return backingBuffer->getAllocType();
}

BufferAllocDir UntypedBuffer::getAllocDir() const noexcept {
    return backingBuffer->getAllocDir();
}

HBMRegion UntypedBuffer::getHBMRegion() const noexcept {
    // Only HBM allocations encode a region in the alloc arg.
    if (getAllocType() != BufferAllocType::Hbm) {
        return HBMRegion::NON_HBM;
    }
    return static_cast<HBMRegion>(backingBuffer->getAllocArg());
}

uint64_t UntypedBuffer::getSize() const noexcept {
    return size;
}

uint64_t UntypedBuffer::getPhysAddr() const noexcept {
    // Physical address is backing base + slice offset.
    return backingBuffer->getPhysAddr() + offset;
}

void* UntypedBuffer::data() const noexcept {
    // Host pointer is backing base + slice offset.
    return static_cast<uint8_t*>(backingBuffer->data()) + offset;
}

void UntypedBuffer::syncToDevice(uint64_t offset, uint64_t size) {
    // Clamp sync size and validate bounds against this slice.
    uint64_t syncSize = (size == std::numeric_limits<uint64_t>::max()) ? this->size - offset : size;
    if (offset + syncSize > this->size) {
        throw std::out_of_range("Sync range exceeds buffer size");
    }
    // Forward to backing buffer with adjusted offset.
    backingBuffer->syncToDevice(this->offset + offset, syncSize);
}

void UntypedBuffer::syncToHost(uint64_t offset, uint64_t size) {
    // Clamp sync size and validate bounds against this slice.
    uint64_t syncSize = (size == std::numeric_limits<uint64_t>::max()) ? this->size - offset : size;
    if (offset + syncSize > this->size) {
        throw std::out_of_range("Sync range exceeds buffer size");
    }
    // Forward to backing buffer with adjusted offset.
    backingBuffer->syncFromDevice(this->offset + offset, syncSize);
}

bool UntypedBuffer::operator==(std::nullptr_t) const noexcept {
    return backingBuffer == nullptr;
}

bool UntypedBuffer::operator!=(std::nullptr_t) const noexcept {
    return backingBuffer != nullptr;
}

bool operator==(std::nullptr_t, const UntypedBuffer& buffer) noexcept {
    return buffer.backingBuffer == nullptr;
}

bool operator!=(std::nullptr_t, const UntypedBuffer& buffer) noexcept {
    return buffer.backingBuffer != nullptr;
}

Block::Block() = default;
Block::~Block() = default;

LargeBlock::LargeBlock(vrtd::Device& device, BufferAllocType type, BufferAllocDir dir, uint64_t size, HBMRegion region)
    // Back large blocks with a dedicated device buffer.
    : backingBuffer(std::make_unique<vrtd::Buffer>(
          device.openBuffer(type, size, static_cast<uint64_t>(region), dir))),
      untypedBuffer(std::make_unique<UntypedBuffer>(backingBuffer.get(), size)) {}

LargeBlock::~LargeBlock() = default;

UntypedBuffer *LargeBlock::getUntypedBuffer() const noexcept {
    return untypedBuffer.get();
}

MediumBlock::MediumBlock(LargeBlockSuperblock *backingSuperblock, UntypedBuffer untypedBuffer)
    // Medium blocks are carved out of a large-block superblock.
    : backingBlockSuperblock(backingSuperblock),
      untypedBuffer(std::make_unique<UntypedBuffer>(untypedBuffer)) {}

MediumBlock::~MediumBlock() {
    // Return the slice to the backing superblock.
    backingBlockSuperblock->deallocate(*untypedBuffer);
}

UntypedBuffer *MediumBlock::getUntypedBuffer() const noexcept {
    return untypedBuffer.get();
}

SmallBlock::SmallBlock(MediumBlockSuperblock *backingBlockSuperblock, UntypedBuffer untypedBuffer)
    // Small blocks are carved out of a medium-block superblock.
    : backingBlockSuperblock(backingBlockSuperblock),
      untypedBuffer(std::make_unique<UntypedBuffer>(untypedBuffer)) {}

SmallBlock::~SmallBlock() {
    // Return the slice to the backing superblock.
    backingBlockSuperblock->deallocate(*untypedBuffer);
}

UntypedBuffer *SmallBlock::getUntypedBuffer() const noexcept {
    return untypedBuffer.get();
}

LargeBlockSuperblock::LargeBlockSuperblock(vrtd::Device& device, BufferAllocType type, BufferAllocDir dir, uint64_t size, HBMRegion region)
    : LargeBlock(device, type, dir, size, region) {
    Buddy::seed(*getUntypedBuffer(),
                "Size too small for LargeBlockSuperblock",
                "LargeBlockSuperblock size exceeds maximum bucket size");
}

LargeBlockSuperblock::~LargeBlockSuperblock() {
    if (!isFree()) {
        utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__,
            "LargeBlockSuperblock destroyed while not all memory was deallocated");
        std::abort();
    }
}

UntypedBuffer LargeBlockSuperblock::allocate(uint64_t size) {
    return Buddy::allocate(size, "Size too small for LargeBlockSuperblock");
}

void LargeBlockSuperblock::deallocate(UntypedBuffer buffer) {
    Buddy::deallocate(*getUntypedBuffer(), buffer,
                      "Size too small for LargeBlockSuperblock",
                      "Buffer does not belong to this LargeBlockSuperblock");
}

bool LargeBlockSuperblock::isFree() const {
    return Buddy::isFree(*getUntypedBuffer(), "Size too small for LargeBlockSuperblock");
}

MediumBlockSuperblock::MediumBlockSuperblock(LargeBlockSuperblock *backingSuperblock, UntypedBuffer untypedBuffer)
    : MediumBlock(backingSuperblock, untypedBuffer) {
    Buddy::seed(*getUntypedBuffer(),
                "Size too small for MediumBlockSuperblock",
                "MediumBlockSuperblock size exceeds maximum bucket size");
}

MediumBlockSuperblock::~MediumBlockSuperblock() {
    if (!isFree()) {
        utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__,
            "MediumBlockSuperblock destroyed while not all memory was deallocated");
        std::abort();
    }
}

UntypedBuffer MediumBlockSuperblock::allocate(uint64_t size) {
    return Buddy::allocate(size, "Size too small for MediumBlockSuperblock");
}

void MediumBlockSuperblock::deallocate(UntypedBuffer buffer) {
    Buddy::deallocate(*getUntypedBuffer(), buffer,
                      "Size too small for MediumBlockSuperblock",
                      "Buffer does not belong to this MediumBlockSuperblock");
}

bool MediumBlockSuperblock::isFree() const {
    return Buddy::isFree(*getUntypedBuffer(), "Size too small for MediumBlockSuperblock");
}

Allocator::Allocator() = default;
Allocator::~Allocator() = default;

std::unique_ptr<Block> Allocator::allocate(vrtd::Device& device, BufferAllocType type, BufferAllocDir dir, uint64_t size, HBMRegion region) {
    if (size > LargeBlockSuperblock::MAX_SIZE) {
        return std::make_unique<LargeBlock>(device, type, dir, size, region);
    }

    if (size > MediumBlockSuperblock::MAX_SIZE) {
        for (auto& superblock : largeBlockSuperblocks) {
            if (!matchesAllocation(superblock->getUntypedBuffer(), type, dir, region)) {
                continue;
            }
            try {
                UntypedBuffer buffer = superblock->allocate(size);
                return std::make_unique<MediumBlock>(superblock.get(), buffer);
            } catch (const std::bad_alloc&) {
                continue;
            }
        }

        largeBlockSuperblocks.emplace_back(
            std::make_unique<LargeBlockSuperblock>(device, type, dir, LargeBlockSuperblock::MAX_SIZE, region));
        UntypedBuffer buffer = largeBlockSuperblocks.back()->allocate(size);
        return std::make_unique<MediumBlock>(largeBlockSuperblocks.back().get(), buffer);
    }

    for (auto& superblock : mediumBlockSuperblocks) {
        if (!matchesAllocation(superblock->getUntypedBuffer(), type, dir, region)) {
            continue;
        }
        try {
            UntypedBuffer buffer = superblock->allocate(size);
            return std::make_unique<SmallBlock>(superblock.get(), buffer);
        } catch (const std::bad_alloc&) {
            continue;
        }
    }

    for (auto& superblock : largeBlockSuperblocks) {
        if (!matchesAllocation(superblock->getUntypedBuffer(), type, dir, region)) {
            continue;
        }
        try {
            UntypedBuffer backing = superblock->allocate(MediumBlockSuperblock::MAX_SIZE);
            mediumBlockSuperblocks.emplace_back(
                std::make_unique<MediumBlockSuperblock>(superblock.get(), backing));
            UntypedBuffer buffer = mediumBlockSuperblocks.back()->allocate(size);
            return std::make_unique<SmallBlock>(mediumBlockSuperblocks.back().get(), buffer);
        } catch (const std::bad_alloc&) {
            continue;
        }
    }

    largeBlockSuperblocks.emplace_back(
        std::make_unique<LargeBlockSuperblock>(device, type, dir, LargeBlockSuperblock::MAX_SIZE, region));
    UntypedBuffer backing = largeBlockSuperblocks.back()->allocate(MediumBlockSuperblock::MAX_SIZE);
    mediumBlockSuperblocks.emplace_back(
        std::make_unique<MediumBlockSuperblock>(largeBlockSuperblocks.back().get(), backing));
    UntypedBuffer buffer = mediumBlockSuperblocks.back()->allocate(size);
    return std::make_unique<SmallBlock>(mediumBlockSuperblocks.back().get(), buffer);
}

void Allocator::deallocate(std::unique_ptr<Block> block) {
    block.reset();

    mediumBlockSuperblocks.erase(
        std::remove_if(mediumBlockSuperblocks.begin(), mediumBlockSuperblocks.end(),
                       [](const std::unique_ptr<MediumBlockSuperblock>& superblock) {
                           return superblock->isFree();
                       }),
        mediumBlockSuperblocks.end());

    largeBlockSuperblocks.erase(
        std::remove_if(largeBlockSuperblocks.begin(), largeBlockSuperblocks.end(),
                       [](const std::unique_ptr<LargeBlockSuperblock>& superblock) {
                           return superblock->isFree();
                       }),
        largeBlockSuperblocks.end());
}

}  // namespace vrt
