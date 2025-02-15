/* Discretix Session management
 *
 * Copyright (C) 2015 Igalia S.L
 * Copyright (C) 2015 Metrological
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
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */

#include "config.h"
#include "DiscretixSession.h"

#if USE(DXDRM)
#include "MediaKeyError.h"
#include "MediaPlayerPrivateGStreamer.h"

#include <dxdrm/DxDrmDebugApi.h>
#include <runtime/JSCInlines.h>
#include <runtime/TypedArrayInlines.h>
#include <wtf/PassRefPtr.h>
#include <wtf/text/CString.h>

GST_DEBUG_CATEGORY_EXTERN(webkit_media_playready_decrypt_debug_category);
#define GST_CAT_DEFAULT webkit_media_playready_decrypt_debug_category

#define MAX_CHALLENGE_LEN 64000

namespace WebCore {

void reportError(const EDxDrmStatus status)
{
    switch (status) {
    case DX_ERROR_CONTENT_NOT_RECOGNIZED:
        GST_ERROR("The specified file is not protected by one of the supported DRM schemes");
        break;
    case DX_ERROR_NOT_INITIALIZED:
        GST_ERROR("The DRM Client has not been initialized");
        break;
    case DX_ERROR_BAD_ARGUMENTS:
        GST_ERROR("Bad arguments");
        break;
    default:
        GST_ERROR("Unknown error: %d", status);
        break;
    }
}

DiscretixSession::DiscretixSession()
    : m_key()
    , m_DxDrmStream(nullptr)
    , m_state(PHASE_INITIAL)
    , m_ready(false)
    , m_keyRequested(false)
{
    DxStatus status = DxLoadConfigFile("/etc/dxdrm/dxdrm.config");
    if (status != DX_SUCCESS) {
        GST_ERROR("Discretix configuration file not found");
        m_status = DX_ERROR_BAD_ARGUMENTS;
    } else {
        m_status = DxDrmClient_Init();
        if (m_status != DX_SUCCESS)
            GST_ERROR("failed to initialize the DxDrmClient (status: %d)", m_status);

        // Set Secure Clock
        /*   m_status = DxDrmStream_AdjustClock(m_DxDrmStream, DX_AUTO_NO_UI);
             if (m_status != DX_SUCCESS) 
             {
             GST_ERROR("failed setting secure clock (%d)", m_status);
             }
        */
    }
    GST_DEBUG("Discretix initialized");
}

DiscretixSession::~DiscretixSession()
{
    if (m_DxDrmStream != nullptr) {
        DxDrmStream_Close(&m_DxDrmStream);
        m_DxDrmStream = nullptr;
    }
    DxDrmClient_Terminate();
}

//
// Expected synchronisation from caller. This method is not thread-safe!
//
RefPtr<Uint8Array> DiscretixSession::dxdrmGenerateKeyRequest(Uint8Array* initData, String& destinationURL, unsigned short& errorCode, uint32_t& systemCode)
{
    RefPtr<Uint8Array> result;

    EDxDrmStatus status = DxDrmClient_OpenDrmStreamFromData(&m_DxDrmStream, initData->data(), initData->byteLength());

    GST_DEBUG("generating key request");
    m_keyRequested = true;
    if (status != DX_SUCCESS) {
        GST_ERROR("failed to create DxDrmClient from initData (status: %d)", status);
        reportError(status);
        errorCode = MediaKeyError::MEDIA_KEYERR_CLIENT;
        result = nullptr;
    } else {
        uint32_t challengeLength = MAX_CHALLENGE_LEN;
        unsigned char* challenge = static_cast<unsigned char*>(g_malloc0(challengeLength));

        // Get challenge
        status = DxDrmStream_GetLicenseChallenge(m_DxDrmStream, challenge, &challengeLength);
        if (status != DX_SUCCESS) {
            GST_ERROR("failed to generate challenge request (status: %d)", status);
            errorCode = MediaKeyError::MEDIA_KEYERR_CLIENT;
            result = nullptr;
        } else {
            // Get License URL
            destinationURL = static_cast<const char *>(DxDrmStream_GetTextAttribute(m_DxDrmStream, DX_ATTR_SILENT_URL, DX_ACTIVE_CONTENT));
            GST_DEBUG("destination URL : %s", destinationURL.utf8().data());

            GST_MEMDUMP("generated license request :", challenge, challengeLength);

            result = Uint8Array::create(challenge, challengeLength);
            errorCode = 0;
        }

        g_free(challenge);
    }

    systemCode = status;

    return result;
}

//
// Expected synchronisation from caller. This method is not thread-safe!
//
bool DiscretixSession::dxdrmProcessKey(Uint8Array* key, RefPtr<Uint8Array>& nextMessage, unsigned short& errorCode, uint32_t& systemCode)
{
    GST_MEMDUMP("response received :", key->data(), key->byteLength());

    bool isAckRequired;
    HDxResponseResult responseResult = nullptr;
    EDxDrmStatus status = DX_ERROR_CONTENT_NOT_RECOGNIZED;

    errorCode = 0;
    if (m_state == PHASE_INITIAL) {
        // Server replied to our license request
        status = DxDrmStream_ProcessLicenseResponse(m_DxDrmStream, key->data(), key->byteLength(), &responseResult, &isAckRequired);

        if (status == DX_SUCCESS) {
            // Create a deep copy of the key.
            m_key = key->buffer();
            m_state = (isAckRequired ? PHASE_ACKNOWLEDGE : PHASE_PROVISIONED);
            GST_DEBUG("Acknowledgement required: %s", isAckRequired ? "yes" : "no");
        }

    } else if (m_state == PHASE_ACKNOWLEDGE) {

        // Server replied to our license response acknowledge
        status = DxDrmClient_ProcessServerResponse(key->data(), key->byteLength(), DX_RESPONSE_LICENSE_ACK, &responseResult, &isAckRequired);

        if (status == DX_SUCCESS) {
            // Create a deep copy of the key.
            m_key = key->buffer();
            m_state = (isAckRequired ? PHASE_ACKNOWLEDGE : PHASE_PROVISIONED);

            if (m_state == PHASE_ACKNOWLEDGE)
                GST_WARNING("Acknowledging an Ack. Strange situation.");
        }
    } else
        GST_WARNING("Unexpected call. We are already provisioned");

    if (status != DX_SUCCESS) {
        GST_ERROR("failed processing license response (status: %d)", status);
        errorCode = MediaKeyError::MEDIA_KEYERR_CLIENT;
    } else if (m_state == PHASE_PROVISIONED) {
        status = DxDrmStream_SetIntent(m_DxDrmStream, DX_INTENT_AUTO_PLAY, DX_AUTO_NO_UI);
        if (status != DX_SUCCESS)
            GST_ERROR("opening stream failed because there are no rights (license) to play the content (status: %d)", status);
        else {
            GST_INFO("playback rights found");

            /* starting consumption of the file - notifying the drm that the file is being used */
            status = DxDrmFile_HandleConsumptionEvent(m_DxDrmStream, DX_EVENT_START);
            if (status != DX_SUCCESS)
                GST_ERROR("Content consumption failed");
            else {
                GST_INFO("Stream was opened and is ready for playback");
                m_ready = true;
            }
        }

    } else if (m_state == PHASE_ACKNOWLEDGE) {
        uint32_t challengeLength = MAX_CHALLENGE_LEN;
        unsigned char* challenge = static_cast<unsigned char*>(g_malloc0(challengeLength));

        status = DxDrmClient_GetLicenseAcq_GenerateAck(&responseResult, challenge, &challengeLength);
        if (status != DX_SUCCESS)
            GST_ERROR("failed generating license ack challenge (status: %d, response result %p)", status, responseResult);

        GST_MEMDUMP("generated license ack request :", challenge, challengeLength);

        nextMessage = Uint8Array::create(challenge, challengeLength);
        g_free(challenge);
    }

    systemCode = status;

    return (status == DX_SUCCESS);
}

int DiscretixSession::processPayload(const void* iv, uint32_t ivSize, void* payloadData, uint32_t payloadDataSize)
{
    EDxDrmStatus status = DxDrmStream_ProcessPayload(m_DxDrmStream,
        0, // trackId
        iv, ivSize,
        0, // blockOffset
        0, // byteOffset
        payloadData, payloadDataSize,
        nullptr, // decryptedPayloadSize
        nullptr, // last15bytes
        0, // last15bytesSize
        nullptr // paddingScheme
    );
    return (status == DX_DRM_SUCCESS ? 0 : status);
}
}

#endif
