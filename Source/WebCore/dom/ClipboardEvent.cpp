/*
 * Copyright (C) 2001 Peter Kelly (pmk@post.com)
 * Copyright (C) 2001 Tobias Anton (anton@stud.fbi.fh-darmstadt.de)
 * Copyright (C) 2006 Samuel Weinig (sam.weinig@gmail.com)
 * Copyright (C) 2003, 2005, 2006, 2008 Apple Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "ClipboardEvent.h"

#include "DataTransfer.h"
#include "DataTransferAccessPolicy.h"
#include "EventNames.h"

namespace WebCore {

ClipboardEventInit::ClipboardEventInit()
    : dataTransfer(DataTransfer::createForCopyAndPaste(DataTransferAccessPolicy::Writable))
{
}

ClipboardEvent::ClipboardEvent(const AtomicString& eventType, bool canBubble, bool cancelable, DataTransfer* dataTransfer)
    : Event(eventType, canBubble, cancelable), m_dataTransfer(dataTransfer)
{
}

ClipboardEvent::ClipboardEvent(const AtomicString& eventType, ClipboardEventInit& initializer)
    : Event(eventType, initializer)
    , m_dataTransfer(initializer.dataTransfer)
{
}

ClipboardEvent::~ClipboardEvent()
{
}

EventInterface ClipboardEvent::eventInterface() const
{
    return ClipboardEventInterfaceType;
}

bool ClipboardEvent::isClipboardEvent() const
{
    return true;
}

} // namespace WebCore
