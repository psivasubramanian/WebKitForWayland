/*
 * Copyright (C) 2014 Apple Inc. All rights reserved.
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

#include "BAssert.h"
#include "Chunk.h"
#include "Deallocator.h"
#include "Heap.h"
#include "Inline.h"
#include "Object.h"
#include "PerProcess.h"
#include <algorithm>
#include <cstdlib>
#include <sys/mman.h>

using namespace std;

namespace bmalloc {

Deallocator::Deallocator(Heap* heap)
    : m_isBmallocEnabled(heap->environment().isBmallocEnabled())
{
    if (!m_isBmallocEnabled) {
        // Fill the object log in order to disable the fast path.
        while (m_objectLog.size() != m_objectLog.capacity())
            m_objectLog.push(nullptr);
    }
}

Deallocator::~Deallocator()
{
    scavenge();
}
    
void Deallocator::scavenge()
{
    if (m_isBmallocEnabled)
        processObjectLog();
}

void Deallocator::deallocateXLarge(void* object)
{
    std::unique_lock<StaticMutex> lock(PerProcess<Heap>::mutex());
    PerProcess<Heap>::getFastCase()->deallocateXLarge(lock, object);
}

void Deallocator::processObjectLog(std::lock_guard<StaticMutex>& lock)
{
    Heap* heap = PerProcess<Heap>::getFastCase();
    
    for (Object object : m_objectLog)
        heap->derefSmallLine(lock, object);

    m_objectLog.clear();
}

void Deallocator::processObjectLog()
{
    std::lock_guard<StaticMutex> lock(PerProcess<Heap>::mutex());
    processObjectLog(lock);
}

void Deallocator::deallocateSlowCase(void* object)
{
    BASSERT(!deallocateFastCase(object));
    
    if (!m_isBmallocEnabled) {
        free(object);
        return;
    }

    if (!object)
        return;

    if (isXLarge(object))
        return deallocateXLarge(object);

    BASSERT(m_objectLog.size() == m_objectLog.capacity());
    processObjectLog();
    m_objectLog.push(object);
}

} // namespace bmalloc
