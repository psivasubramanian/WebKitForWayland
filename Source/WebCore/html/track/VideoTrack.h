/*
 * Copyright (C) 2011 Google Inc. All rights reserved.
 * Copyright (C) 2011, 2012, 2013 Apple Inc.  All rights reserved.
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

#ifndef VideoTrack_h
#define VideoTrack_h

#if ENABLE(VIDEO_TRACK)

#include "ExceptionCode.h"
#include "TrackBase.h"
#include "VideoTrackPrivate.h"
#include <wtf/RefCounted.h>
#include <wtf/text/WTFString.h>

namespace WebCore {

class MediaDescription;
class VideoTrack;

class VideoTrackClient {
public:
    virtual ~VideoTrackClient() { }
    virtual void videoTrackSelectedChanged(VideoTrack*) = 0;
};

class VideoTrack final : public TrackBase, public VideoTrackPrivateClient {
public:
    static Ref<VideoTrack> create(VideoTrackClient* client, PassRefPtr<VideoTrackPrivate> trackPrivate)
    {
        return adoptRef(*new VideoTrack(client, trackPrivate));
    }
    virtual ~VideoTrack();

    static const AtomicString& alternativeKeyword();
    static const AtomicString& captionsKeyword();
    static const AtomicString& mainKeyword();
    static const AtomicString& signKeyword();
    static const AtomicString& subtitlesKeyword();
    static const AtomicString& commentaryKeyword();
    const AtomicString& defaultKindKeyword() const override { return emptyAtom; }

    bool selected() const { return m_selected; }
    virtual void setSelected(const bool);

    void clearClient() override { m_client = 0; }
    VideoTrackClient* client() const { return m_client; }

    size_t inbandTrackIndex();

#if ENABLE(MEDIA_SOURCE)
    void setKind(const AtomicString&) override;
    void setLanguage(const AtomicString&) override;
#endif

    const MediaDescription& description() const;

    void setPrivate(PassRefPtr<VideoTrackPrivate>);

protected:
    VideoTrack(VideoTrackClient*, PassRefPtr<VideoTrackPrivate> privateTrack);

private:
    bool isValidKind(const AtomicString&) const override;

    // VideoTrackPrivateClient
    void selectedChanged(VideoTrackPrivate*, bool) override;

    // TrackPrivateBaseClient
    void idChanged(TrackPrivateBase*, const AtomicString&) override;
    void labelChanged(TrackPrivateBase*, const AtomicString&) override;
    void languageChanged(TrackPrivateBase*, const AtomicString&) override;
    void willRemove(TrackPrivateBase*) override;

    bool enabled() const override { return selected(); }

    void updateKindFromPrivate();

    bool m_selected;
    VideoTrackClient* m_client;

    RefPtr<VideoTrackPrivate> m_private;
};

inline VideoTrack* toVideoTrack(TrackBase* track)
{
    ASSERT_WITH_SECURITY_IMPLICATION(track->type() == TrackBase::VideoTrack);
    return static_cast<VideoTrack*>(track);
}

} // namespace WebCore

#endif
#endif
