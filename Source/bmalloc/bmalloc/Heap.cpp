/*
 * Copyright (C) 2014-2016 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "Heap.h"
#include "BumpAllocator.h"
#include "Chunk.h"
#include "LargeObject.h"
#include "PerProcess.h"
#include "SmallLine.h"
#include "SmallPage.h"
#include <thread>

namespace bmalloc {

Heap::Heap(std::lock_guard<StaticMutex>&)
    : m_vmPageSizePhysical(vmPageSizePhysical())
    , m_largeObjects(VMState::HasPhysical::True)
    , m_isAllocatingPages(false)
    , m_scavenger(*this, &Heap::concurrentScavenge)
{
    RELEASE_BASSERT(vmPageSizePhysical() >= smallPageSize);
    RELEASE_BASSERT(vmPageSize() >= vmPageSizePhysical());
    RELEASE_BASSERT(xLargeAlignment >= vmPageSize());

    initializeLineMetadata();
}

void Heap::initializeLineMetadata()
{
    // We assume that m_smallLineMetadata is zero-filled.

    size_t smallLineCount = m_vmPageSizePhysical / smallLineSize;
    m_smallLineMetadata.grow(sizeClassCount * smallLineCount);

    for (size_t sizeClass = 0; sizeClass < sizeClassCount; ++sizeClass) {
        size_t size = objectSize(sizeClass);
        LineMetadata* pageMetadata = &m_smallLineMetadata[sizeClass * smallLineCount];

        size_t object = 0;
        size_t line = 0;
        while (object < m_vmPageSizePhysical) {
            line = object / smallLineSize;
            size_t leftover = object % smallLineSize;

            size_t objectCount;
            size_t remainder;
            divideRoundingUp(smallLineSize - leftover, size, objectCount, remainder);

            pageMetadata[line] = { static_cast<unsigned short>(leftover), static_cast<unsigned short>(objectCount) };

            object += objectCount * size;
        }

        // Don't allow the last object in a page to escape the page.
        if (object > m_vmPageSizePhysical) {
            BASSERT(pageMetadata[line].objectCount);
            --pageMetadata[line].objectCount;
        }
    }
}

void Heap::concurrentScavenge()
{
    std::unique_lock<StaticMutex> lock(PerProcess<Heap>::mutex());
    scavenge(lock, scavengeSleepDuration);
}

void Heap::scavenge(std::unique_lock<StaticMutex>& lock, std::chrono::milliseconds sleepDuration)
{
    waitUntilFalse(lock, sleepDuration, m_isAllocatingPages);

    lock.unlock();
    {
        std::lock_guard<StaticMutex> lock(PerProcess<Heap>::mutex());
        scavengeSmallPages(lock);
    }
    lock.lock();

    scavengeLargeObjects(lock, sleepDuration);
    scavengeXLargeObjects(lock, sleepDuration);

    sleep(lock, sleepDuration);
}

void Heap::scavengeSmallPage(std::lock_guard<StaticMutex>& lock)
{
    SmallPage* page = m_smallPages.pop();

    // Revert the slide() value on intermediate SmallPages so they hash to
    // themselves again.
    for (size_t i = 1; i < page->smallPageCount(); ++i)
        page[i].setSlide(0);

    // Revert our small object page back to large object.
    page->setObjectType(ObjectType::Large);

    LargeObject largeObject(page->begin()->begin());
    deallocateLarge(lock, largeObject);
}

void Heap::scavengeSmallPages(std::lock_guard<StaticMutex>& lock)
{
    while (!m_smallPages.isEmpty())
        scavengeSmallPage(lock);
}

void Heap::scavengeLargeObjects(std::unique_lock<StaticMutex>& lock, std::chrono::milliseconds sleepDuration)
{
    while (LargeObject largeObject = m_largeObjects.takeGreedy()) {
        m_vmHeap.deallocateLargeObject(lock, largeObject);
        waitUntilFalse(lock, sleepDuration, m_isAllocatingPages);
    }
}

void Heap::scavengeXLargeObjects(std::unique_lock<StaticMutex>& lock, std::chrono::milliseconds sleepDuration)
{
    while (XLargeRange range = m_xLargeMap.takePhysical()) {
        lock.unlock();
        vmDeallocatePhysicalPagesSloppy(range.begin(), range.size());
        lock.lock();
        
        range.setVMState(VMState::Virtual);
        m_xLargeMap.addVirtual(range);

        waitUntilFalse(lock, sleepDuration, m_isAllocatingPages);
    }

    m_xLargeMap.shrinkToFit();
}

void Heap::allocateSmallBumpRanges(std::lock_guard<StaticMutex>& lock, size_t sizeClass, BumpAllocator& allocator, BumpRangeCache& rangeCache)
{
    BASSERT(!rangeCache.size());
    SmallPage* page = allocateSmallPage(lock, sizeClass);
    SmallLine* lines = page->begin();
    BASSERT(page->hasFreeLines(lock));
    size_t smallLineCount = m_vmPageSizePhysical / smallLineSize;
    LineMetadata* pageMetadata = &m_smallLineMetadata[sizeClass * smallLineCount];

    // Find a free line.
    for (size_t lineNumber = 0; lineNumber < smallLineCount; ++lineNumber) {
        if (lines[lineNumber].refCount(lock))
            continue;

        LineMetadata& lineMetadata = pageMetadata[lineNumber];
        if (!lineMetadata.objectCount)
            continue;

        // In a fragmented page, some free ranges might not fit in the cache.
        if (rangeCache.size() == rangeCache.capacity()) {
            m_smallPagesWithFreeLines[sizeClass].push(page);
            BASSERT(allocator.canAllocate());
            return;
        }

        char* begin = lines[lineNumber].begin() + lineMetadata.startOffset;
        unsigned short objectCount = lineMetadata.objectCount;
        lines[lineNumber].ref(lock, lineMetadata.objectCount);
        page->ref(lock);

        // Merge with subsequent free lines.
        while (++lineNumber < smallLineCount) {
            if (lines[lineNumber].refCount(lock))
                break;

            LineMetadata& lineMetadata = pageMetadata[lineNumber];
            if (!lineMetadata.objectCount)
                continue;

            objectCount += lineMetadata.objectCount;
            lines[lineNumber].ref(lock, lineMetadata.objectCount);
            page->ref(lock);
        }

        if (!allocator.canAllocate())
            allocator.refill({ begin, objectCount });
        else
            rangeCache.push({ begin, objectCount });
    }

    BASSERT(allocator.canAllocate());
    page->setHasFreeLines(lock, false);
}

SmallPage* Heap::allocateSmallPage(std::lock_guard<StaticMutex>& lock, size_t sizeClass)
{
    if (!m_smallPagesWithFreeLines[sizeClass].isEmpty())
        return m_smallPagesWithFreeLines[sizeClass].popFront();
    
    if (!m_smallPages.isEmpty()) {
        SmallPage* page = m_smallPages.pop();
        page->setSizeClass(sizeClass);
        return page;
    }

    size_t unalignedSize = largeMin + m_vmPageSizePhysical - largeAlignment + m_vmPageSizePhysical;
    LargeObject largeObject = allocateLarge(lock, m_vmPageSizePhysical, m_vmPageSizePhysical, unalignedSize);
    
    // Transform our large object into a small object page. We deref here
    // because our small objects will keep their own line refcounts.
    Object object(largeObject.begin());
    object.line()->deref(lock);
    object.page()->setObjectType(ObjectType::Small);

    SmallPage* page = object.page();
    page->setSizeClass(sizeClass);
    page->setSmallPageCount(m_vmPageSizePhysical / smallPageSize);

    // Set a slide() value on intermediate SmallPages so they hash to their
    // vmPageSizePhysical-sized page.
    for (size_t i = 1; i < page->smallPageCount(); ++i)
        page[i].setSlide(i);

    return object.page();
}

void Heap::deallocateSmallLine(std::lock_guard<StaticMutex>& lock, Object object)
{
    BASSERT(!object.line()->refCount(lock));
    SmallPage* page = object.page();
    if (page->objectType() == ObjectType::Large)
        return deallocateLarge(lock, LargeObject(object.begin()));

    page->deref(lock);
    if (!page->hasFreeLines(lock)) {
        page->setHasFreeLines(lock, true);
        m_smallPagesWithFreeLines[page->sizeClass()].push(page);

        BASSERT(page->refCount(lock));
        return;
    }

    if (page->refCount(lock))
        return;

    m_smallPagesWithFreeLines[page->sizeClass()].remove(page);
    m_smallPages.push(page);
    m_scavenger.run();
}

inline LargeObject& Heap::splitAndAllocate(std::lock_guard<StaticMutex>& lock, LargeObject& largeObject, size_t size)
{
    BASSERT(largeObject.isFree());

    LargeObject nextLargeObject;

    if (largeObject.size() - size >= largeMin) {
        std::pair<LargeObject, LargeObject> split = largeObject.split(size);
        largeObject = split.first;
        nextLargeObject = split.second;
    }

    largeObject.setFree(false);
    Object object(largeObject.begin());
    object.line()->ref(lock);
    BASSERT(object.page()->objectType() == ObjectType::Large);

    if (nextLargeObject) {
        BASSERT(!nextLargeObject.nextCanMerge());
        m_largeObjects.insert(nextLargeObject);
    }

    return largeObject;
}

inline LargeObject& Heap::splitAndAllocate(std::lock_guard<StaticMutex>& lock, LargeObject& largeObject, size_t alignment, size_t size)
{
    LargeObject prevLargeObject;
    LargeObject nextLargeObject;

    size_t alignmentMask = alignment - 1;
    if (test(largeObject.begin(), alignmentMask)) {
        size_t prefixSize = roundUpToMultipleOf(alignment, largeObject.begin() + largeMin) - largeObject.begin();
        std::pair<LargeObject, LargeObject> pair = largeObject.split(prefixSize);
        prevLargeObject = pair.first;
        largeObject = pair.second;
    }

    BASSERT(largeObject.isFree());

    if (largeObject.size() - size >= largeMin) {
        std::pair<LargeObject, LargeObject> split = largeObject.split(size);
        largeObject = split.first;
        nextLargeObject = split.second;
    }

    largeObject.setFree(false);
    Object object(largeObject.begin());
    object.line()->ref(lock);
    BASSERT(object.page()->objectType() == ObjectType::Large);

    if (prevLargeObject) {
        LargeObject merged = prevLargeObject.merge();
        m_largeObjects.insert(merged);
    }

    if (nextLargeObject) {
        LargeObject merged = nextLargeObject.merge();
        m_largeObjects.insert(merged);
    }

    return largeObject;
}

void* Heap::allocateLarge(std::lock_guard<StaticMutex>& lock, size_t size)
{
    BASSERT(size <= largeMax);
    BASSERT(size >= largeMin);
    BASSERT(size == roundUpToMultipleOf<largeAlignment>(size));
    
    if (size <= m_vmPageSizePhysical)
        scavengeSmallPages(lock);

    LargeObject largeObject = m_largeObjects.take(size);
    if (!largeObject)
        largeObject = m_vmHeap.allocateLargeObject(lock, size);

    if (largeObject.vmState().hasVirtual()) {
        m_isAllocatingPages = true;
        // We commit before we split in order to avoid split/merge commit/decommit churn.
        vmAllocatePhysicalPagesSloppy(largeObject.begin(), largeObject.size());
        largeObject.setVMState(VMState::Physical);
    }

    largeObject = splitAndAllocate(lock, largeObject, size);

    return largeObject.begin();
}

void* Heap::allocateLarge(std::lock_guard<StaticMutex>& lock, size_t alignment, size_t size, size_t unalignedSize)
{
    BASSERT(size <= largeMax);
    BASSERT(size >= largeMin);
    BASSERT(size == roundUpToMultipleOf<largeAlignment>(size));
    BASSERT(unalignedSize <= largeMax);
    BASSERT(unalignedSize >= largeMin);
    BASSERT(unalignedSize == roundUpToMultipleOf<largeAlignment>(unalignedSize));
    BASSERT(alignment <= chunkSize / 2);
    BASSERT(alignment >= largeAlignment);
    BASSERT(isPowerOfTwo(alignment));

    if (size <= m_vmPageSizePhysical)
        scavengeSmallPages(lock);

    LargeObject largeObject = m_largeObjects.take(alignment, size, unalignedSize);
    if (!largeObject)
        largeObject = m_vmHeap.allocateLargeObject(lock, alignment, size, unalignedSize);

    if (largeObject.vmState().hasVirtual()) {
        m_isAllocatingPages = true;
        // We commit before we split in order to avoid split/merge commit/decommit churn.
        vmAllocatePhysicalPagesSloppy(largeObject.begin(), largeObject.size());
        largeObject.setVMState(VMState::Physical);
    }

    largeObject = splitAndAllocate(lock, largeObject, alignment, size);

    return largeObject.begin();
}

void Heap::shrinkLarge(std::lock_guard<StaticMutex>& lock, LargeObject& largeObject, size_t newSize)
{
    std::pair<LargeObject, LargeObject> split = largeObject.split(newSize);
    deallocateLarge(lock, split.second);
}

void Heap::deallocateLarge(std::lock_guard<StaticMutex>&, const LargeObject& largeObject)
{
    BASSERT(!largeObject.isFree());
    BASSERT(Object(largeObject.begin()).page()->objectType() == ObjectType::Large);
    largeObject.setFree(true);
    
    LargeObject merged = largeObject.merge();
    m_largeObjects.insert(merged);
    m_scavenger.run();
}

void* Heap::allocateXLarge(std::lock_guard<StaticMutex>& lock, size_t alignment, size_t size)
{
    void* result = tryAllocateXLarge(lock, alignment, size);
    RELEASE_BASSERT(result);
    return result;
}

void* Heap::allocateXLarge(std::lock_guard<StaticMutex>& lock, size_t size)
{
    return allocateXLarge(lock, alignment, size);
}

XLargeRange Heap::splitAndAllocate(XLargeRange& range, size_t alignment, size_t size)
{
    XLargeRange prev;
    XLargeRange next;

    size_t alignmentMask = alignment - 1;
    if (test(range.begin(), alignmentMask)) {
        size_t prefixSize = roundUpToMultipleOf(alignment, range.begin()) - range.begin();
        std::pair<XLargeRange, XLargeRange> pair = range.split(prefixSize);
        prev = pair.first;
        range = pair.second;
    }

    if (range.size() - size >= xLargeAlignment) {
        size_t alignedSize = roundUpToMultipleOf<xLargeAlignment>(size);
        std::pair<XLargeRange, XLargeRange> pair = range.split(alignedSize);
        range = pair.first;
        next = pair.second;
    }

    // At this point our range might contain an unused tail fragment. This is
    // common. We can't allocate the tail fragment because it's aligned to less
    // than xLargeAlignment. So, we pair the allocation with its tail fragment
    // in the allocated list. This is an important optimization because it
    // keeps the free list short, speeding up allocation and merging.

    std::pair<XLargeRange, XLargeRange> allocated = range.split(roundUpToMultipleOf(m_vmPageSizePhysical, size));
    if (allocated.first.vmState().hasVirtual()) {
        vmAllocatePhysicalPagesSloppy(allocated.first.begin(), allocated.first.size());
        allocated.first.setVMState(VMState::Physical);
    }

    m_xLargeMap.addAllocated(prev, allocated, next);
    return allocated.first;
}

void* Heap::tryAllocateXLarge(std::lock_guard<StaticMutex>&, size_t alignment, size_t size)
{
    BASSERT(isPowerOfTwo(alignment));
    BASSERT(alignment < xLargeMax);

    m_isAllocatingPages = true;

    size = std::max(m_vmPageSizePhysical, size);
    alignment = roundUpToMultipleOf<xLargeAlignment>(alignment);

    XLargeRange range = m_xLargeMap.takeFree(alignment, size);
    if (!range) {
        // We allocate VM in aligned multiples to increase the chances that
        // the OS will provide contiguous ranges that we can merge.
        size_t alignedSize = roundUpToMultipleOf<xLargeAlignment>(size);

        void* begin = tryVMAllocate(alignment, alignedSize);
        if (!begin)
            return nullptr;
        range = XLargeRange(begin, alignedSize, VMState::Virtual);
    }

    return splitAndAllocate(range, alignment, size).begin();
}

size_t Heap::xLargeSize(std::unique_lock<StaticMutex>&, void* object)
{
    return m_xLargeMap.getAllocated(object).size();
}

void Heap::shrinkXLarge(std::unique_lock<StaticMutex>&, const Range& object, size_t newSize)
{
    BASSERT(object.size() > newSize);

    if (object.size() - newSize < m_vmPageSizePhysical)
        return;
    
    XLargeRange range = m_xLargeMap.takeAllocated(object.begin());
    splitAndAllocate(range, xLargeAlignment, newSize);

    m_scavenger.run();
}

void Heap::deallocateXLarge(std::unique_lock<StaticMutex>&, void* object)
{
    XLargeRange range = m_xLargeMap.takeAllocated(object);
    m_xLargeMap.addFree(range);
    
    m_scavenger.run();
}

void Heap::heapDestructor()
{
    PerProcess<Heap>::get()->m_scavenger.stop();
}

} // namespace bmalloc
