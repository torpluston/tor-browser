/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef XP_WIN
#  include "objbase.h"
// Some Windows header defines this, so undef it as it conflicts with our
// function of the same name.
#  undef GetCurrentTime
#endif

#include "mozilla/dom/HTMLMediaElement.h"
#include "AudioChannelService.h"
#include "AudioDeviceInfo.h"
#include "AudioStreamTrack.h"
#include "AutoplayPolicy.h"
#include "ChannelMediaDecoder.h"
#include "DOMMediaStream.h"
#include "DecoderDoctorDiagnostics.h"
#include "DecoderDoctorLogger.h"
#include "DecoderTraits.h"
#include "FrameStatistics.h"
#include "GMPCrashHelper.h"
#ifdef MOZ_ANDROID_HLS_SUPPORT
#  include "HLSDecoder.h"
#endif
#include "HTMLMediaElement.h"
#include "ImageContainer.h"
#include "Layers.h"
#include "MP4Decoder.h"
#include "MediaContainerType.h"
#include "MediaError.h"
#include "MediaManager.h"
#include "MediaMetadataManager.h"
#include "MediaResource.h"
#include "MediaShutdownManager.h"
#include "MediaSourceDecoder.h"
#include "MediaStreamError.h"
#include "MediaStreamGraph.h"
#include "MediaStreamListener.h"
#include "MediaTrackList.h"
#include "SVGObserverUtils.h"
#include "TimeRanges.h"
#include "VideoFrameContainer.h"
#include "VideoStreamTrack.h"
#include "base/basictypes.h"
#include "jsapi.h"
#include "mozilla/ArrayUtils.h"
#include "mozilla/AsyncEventDispatcher.h"
#include "mozilla/EMEUtils.h"
#include "mozilla/EventDispatcher.h"
#include "mozilla/EventStateManager.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/NotNull.h"
#include "mozilla/Preferences.h"
#include "mozilla/PresShell.h"
#include "mozilla/Sprintf.h"
#include "mozilla/StaticPrefs.h"
#include "mozilla/Telemetry.h"
#include "mozilla/dom/AudioTrack.h"
#include "mozilla/dom/AudioTrackList.h"
#include "mozilla/dom/BlobURLProtocolHandler.h"
#include "mozilla/dom/ElementInlines.h"
#include "mozilla/dom/HTMLAudioElement.h"
#include "mozilla/dom/HTMLInputElement.h"
#include "mozilla/dom/HTMLMediaElementBinding.h"
#include "mozilla/dom/HTMLSourceElement.h"
#include "mozilla/dom/HTMLVideoElement.h"
#include "mozilla/dom/MediaEncryptedEvent.h"
#include "mozilla/dom/MediaErrorBinding.h"
#include "mozilla/dom/MediaSource.h"
#include "mozilla/dom/PlayPromise.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/TextTrack.h"
#include "mozilla/dom/VideoPlaybackQuality.h"
#include "mozilla/dom/VideoTrack.h"
#include "mozilla/dom/VideoTrackList.h"
#include "mozilla/dom/WakeLock.h"
#include "mozilla/dom/power/PowerManagerService.h"
#include "mozilla/net/UrlClassifierFeatureFactory.h"
#include "nsAttrValueInlines.h"
#include "nsContentPolicyUtils.h"
#include "nsContentUtils.h"
#include "nsCycleCollectionParticipant.h"
#include "nsDisplayList.h"
#include "nsDocShell.h"
#include "nsError.h"
#include "nsGenericHTMLElement.h"
#include "nsGkAtoms.h"
#include "nsIAsyncVerifyRedirectCallback.h"
#include "nsIAutoplay.h"
#include "nsICachingChannel.h"
#include "nsICategoryManager.h"
#include "nsIClassOfService.h"
#include "nsIContentPolicy.h"
#include "nsIContentSecurityPolicy.h"
#include "nsIDocShell.h"
#include "mozilla/dom/Document.h"
#include "nsIFrame.h"
#include "nsIObserverService.h"
#include "nsIPermissionManager.h"
#include "nsIRequest.h"
#include "nsIScriptError.h"
#include "nsIScriptSecurityManager.h"
#include "nsISupportsPrimitives.h"
#include "nsIThreadInternal.h"
#include "nsITimer.h"
#include "nsIXPConnect.h"
#include "nsJSUtils.h"
#include "nsLayoutUtils.h"
#include "nsMediaFragmentURIParser.h"
#include "nsMimeTypes.h"
#include "nsNetUtil.h"
#include "nsNodeInfoManager.h"
#include "nsPresContext.h"
#include "nsQueryObject.h"
#include "nsRange.h"
#include "nsSize.h"
#include "nsThreadUtils.h"
#include "nsURIHashKey.h"
#include "nsVideoFrame.h"
#include "ReferrerInfo.h"
#include "xpcpublic.h"
#include <algorithm>
#include <cmath>
#include <limits>

mozilla::LazyLogModule gMediaElementLog("nsMediaElement");
static mozilla::LazyLogModule gMediaElementEventsLog("nsMediaElementEvents");

extern mozilla::LazyLogModule gAutoplayPermissionLog;
#define AUTOPLAY_LOG(msg, ...) \
  MOZ_LOG(gAutoplayPermissionLog, LogLevel::Debug, (msg, ##__VA_ARGS__))

#define LOG(type, msg) MOZ_LOG(gMediaElementLog, type, msg)
#define LOG_EVENT(type, msg) MOZ_LOG(gMediaElementEventsLog, type, msg)

using namespace mozilla::layers;
using mozilla::net::nsMediaFragmentURIParser;
using namespace mozilla::dom::HTMLMediaElement_Binding;

namespace mozilla {
namespace dom {

// Number of milliseconds between progress events as defined by spec
static const uint32_t PROGRESS_MS = 350;

// Number of milliseconds of no data before a stall event is fired as defined by
// spec
static const uint32_t STALL_MS = 3000;

// Used by AudioChannel for suppresssing the volume to this ratio.
#define FADED_VOLUME_RATIO 0.25

// These constants are arbitrary
// Minimum playbackRate for a media
static const double MIN_PLAYBACKRATE = 1.0 / 16;
// Maximum playbackRate for a media
static const double MAX_PLAYBACKRATE = 16.0;
// These are the limits beyonds which SoundTouch does not perform too well and
// when speech is hard to understand anyway. Threshold above which audio is
// muted
static const double THRESHOLD_HIGH_PLAYBACKRATE_AUDIO = 4.0;
// Threshold under which audio is muted
static const double THRESHOLD_LOW_PLAYBACKRATE_AUDIO = 0.25;

static double ClampPlaybackRate(double aPlaybackRate) {
  MOZ_ASSERT(aPlaybackRate >= 0.0);

  if (aPlaybackRate == 0.0) {
    return aPlaybackRate;
  }
  if (aPlaybackRate < MIN_PLAYBACKRATE) {
    return MIN_PLAYBACKRATE;
  }
  if (aPlaybackRate > MAX_PLAYBACKRATE) {
    return MAX_PLAYBACKRATE;
  }
  return aPlaybackRate;
}

// Media error values.  These need to match the ones in MediaError.webidl.
static const unsigned short MEDIA_ERR_ABORTED = 1;
static const unsigned short MEDIA_ERR_NETWORK = 2;
static const unsigned short MEDIA_ERR_DECODE = 3;
static const unsigned short MEDIA_ERR_SRC_NOT_SUPPORTED = 4;

static void ResolvePromisesWithUndefined(
    const nsTArray<RefPtr<PlayPromise>>& aPromises) {
  for (auto& promise : aPromises) {
    promise->MaybeResolveWithUndefined();
  }
}

static void RejectPromises(const nsTArray<RefPtr<PlayPromise>>& aPromises,
                           nsresult aError) {
  for (auto& promise : aPromises) {
    promise->MaybeReject(aError);
  }
}

// Under certain conditions there may be no-one holding references to
// a media element from script, DOM parent, etc, but the element may still
// fire meaningful events in the future so we can't destroy it yet:
// 1) If the element is delaying the load event (or would be, if it were
// in a document), then events up to loadeddata or error could be fired,
// so we need to stay alive.
// 2) If the element is not paused and playback has not ended, then
// we will (or might) play, sending timeupdate and ended events and possibly
// audio output, so we need to stay alive.
// 3) if the element is seeking then we will fire seeking events and possibly
// start playing afterward, so we need to stay alive.
// 4) If autoplay could start playback in this element (if we got enough data),
// then we need to stay alive.
// 5) if the element is currently loading, not suspended, and its source is
// not a MediaSource, then script might be waiting for progress events or a
// 'stalled' or 'suspend' event, so we need to stay alive.
// If we're already suspended then (all other conditions being met),
// it's OK to just disappear without firing any more events,
// since we have the freedom to remain suspended indefinitely. Note
// that we could use this 'suspended' loophole to garbage-collect a suspended
// element in case 4 even if it had 'autoplay' set, but we choose not to.
// If someone throws away all references to a loading 'autoplay' element
// sound should still eventually play.
// 6) If the source is a MediaSource, most loading events will not fire unless
// appendBuffer() is called on a SourceBuffer, in which case something is
// already referencing the SourceBuffer, which keeps the associated media
// element alive. Further, a MediaSource will never time out the resource
// fetch, and so should not keep the media element alive if it is
// unreferenced. A pending 'stalled' event keeps the media element alive.
//
// Media elements owned by inactive documents (i.e. documents not contained in
// any document viewer) should never hold a self-reference because none of the
// above conditions are allowed: the element will stop loading and playing
// and never resume loading or playing unless its owner document changes to
// an active document (which can only happen if there is an external reference
// to the element).
// Media elements with no owner doc should be able to hold a self-reference.
// Something native must have created the element and may expect it to
// stay alive to play.

// It's very important that any change in state which could change the value of
// needSelfReference in AddRemoveSelfReference be followed by a call to
// AddRemoveSelfReference before this element could die!
// It's especially important if needSelfReference would change to 'true',
// since if we neglect to add a self-reference, this element might be
// garbage collected while there are still event listeners that should
// receive events. If we neglect to remove the self-reference then the element
// just lives longer than it needs to.

class nsMediaEvent : public Runnable {
 public:
  explicit nsMediaEvent(const char* aName, HTMLMediaElement* aElement)
      : Runnable(aName),
        mElement(aElement),
        mLoadID(mElement->GetCurrentLoadID()) {}
  ~nsMediaEvent() {}

  NS_IMETHOD Run() override = 0;

 protected:
  bool IsCancelled() { return mElement->GetCurrentLoadID() != mLoadID; }

  RefPtr<HTMLMediaElement> mElement;
  uint32_t mLoadID;
};

class HTMLMediaElement::nsAsyncEventRunner : public nsMediaEvent {
 private:
  nsString mName;

 public:
  nsAsyncEventRunner(const nsAString& aName, HTMLMediaElement* aElement)
      : nsMediaEvent("HTMLMediaElement::nsAsyncEventRunner", aElement),
        mName(aName) {}

  NS_IMETHOD Run() override {
    // Silently cancel if our load has been cancelled.
    if (IsCancelled()) return NS_OK;

    return mElement->DispatchEvent(mName);
  }
};

/*
 * If no error is passed while constructing an instance, the instance will
 * resolve the passed promises with undefined; otherwise, the instance will
 * reject the passed promises with the passed error.
 *
 * The constructor appends the constructed instance into the passed media
 * element's mPendingPlayPromisesRunners member and once the the runner is run
 * (whether fulfilled or canceled), it removes itself from
 * mPendingPlayPromisesRunners.
 */
class HTMLMediaElement::nsResolveOrRejectPendingPlayPromisesRunner
    : public nsMediaEvent {
  nsTArray<RefPtr<PlayPromise>> mPromises;
  nsresult mError;

 public:
  nsResolveOrRejectPendingPlayPromisesRunner(
      HTMLMediaElement* aElement, nsTArray<RefPtr<PlayPromise>>&& aPromises,
      nsresult aError = NS_OK)
      : nsMediaEvent(
            "HTMLMediaElement::nsResolveOrRejectPendingPlayPromisesRunner",
            aElement),
        mPromises(std::move(aPromises)),
        mError(aError) {
    mElement->mPendingPlayPromisesRunners.AppendElement(this);
  }

  void ResolveOrReject() {
    if (NS_SUCCEEDED(mError)) {
      ResolvePromisesWithUndefined(mPromises);
    } else {
      RejectPromises(mPromises, mError);
    }
  }

  NS_IMETHOD Run() override {
    if (!IsCancelled()) {
      ResolveOrReject();
    }

    mElement->mPendingPlayPromisesRunners.RemoveElement(this);
    return NS_OK;
  }
};

class HTMLMediaElement::nsNotifyAboutPlayingRunner
    : public nsResolveOrRejectPendingPlayPromisesRunner {
 public:
  nsNotifyAboutPlayingRunner(
      HTMLMediaElement* aElement,
      nsTArray<RefPtr<PlayPromise>>&& aPendingPlayPromises)
      : nsResolveOrRejectPendingPlayPromisesRunner(
            aElement, std::move(aPendingPlayPromises)) {}

  NS_IMETHOD Run() override {
    if (IsCancelled()) {
      mElement->mPendingPlayPromisesRunners.RemoveElement(this);
      return NS_OK;
    }

    mElement->DispatchEvent(NS_LITERAL_STRING("playing"));
    return nsResolveOrRejectPendingPlayPromisesRunner::Run();
  }
};

class nsSourceErrorEventRunner : public nsMediaEvent {
 private:
  nsCOMPtr<nsIContent> mSource;

 public:
  nsSourceErrorEventRunner(HTMLMediaElement* aElement, nsIContent* aSource)
      : nsMediaEvent("dom::nsSourceErrorEventRunner", aElement),
        mSource(aSource) {}

  NS_IMETHOD Run() override {
    // Silently cancel if our load has been cancelled.
    if (IsCancelled()) return NS_OK;
    LOG_EVENT(LogLevel::Debug,
              ("%p Dispatching simple event source error", mElement.get()));
    return nsContentUtils::DispatchTrustedEvent(
        mElement->OwnerDoc(), mSource, NS_LITERAL_STRING("error"),
        CanBubble::eNo, Cancelable::eNo);
  }
};

/**
 * This listener observes the first video frame to arrive with a non-empty size,
 * and calls HTMLMediaElement::UpdateInitialMediaSize() with that size.
 */
class HTMLMediaElement::VideoFrameListener
    : public DirectMediaStreamTrackListener {
 public:
  explicit VideoFrameListener(HTMLMediaElement* aElement)
      : mElement(aElement),
        mMainThreadEventTarget(aElement->MainThreadEventTarget()),
        mInitialSizeFound(false) {
    MOZ_ASSERT(NS_IsMainThread());
    MOZ_ASSERT(mElement);
    MOZ_ASSERT(mMainThreadEventTarget);
  }

  void Forget() {
    MOZ_ASSERT(NS_IsMainThread());
    mElement = nullptr;
  }

  void ReceivedSize(gfx::IntSize aSize) {
    MOZ_ASSERT(NS_IsMainThread());

    if (!mElement) {
      return;
    }

    mElement->UpdateInitialMediaSize(aSize);
  }

  void NotifyRealtimeTrackData(MediaStreamGraph* aGraph,
                               StreamTime aTrackOffset,
                               const MediaSegment& aMedia) override {
    if (mInitialSizeFound) {
      return;
    }

    if (aMedia.GetType() != MediaSegment::VIDEO) {
      MOZ_ASSERT(false, "Should only lock on to a video track");
      return;
    }

    const VideoSegment& video = static_cast<const VideoSegment&>(aMedia);
    for (VideoSegment::ConstChunkIterator c(video); !c.IsEnded(); c.Next()) {
      if (c->mFrame.GetIntrinsicSize() != gfx::IntSize(0, 0)) {
        mInitialSizeFound = true;
        // This is fine to dispatch straight to main thread (instead of via
        // ...AfterStreamUpdate()) since it reflects state of the element,
        // not the stream. Events reflecting stream or track state should be
        // dispatched so their order is preserved.
        mMainThreadEventTarget->Dispatch(NewRunnableMethod<gfx::IntSize>(
            "dom::HTMLMediaElement::VideoFrameListener::ReceivedSize", this,
            &VideoFrameListener::ReceivedSize, c->mFrame.GetIntrinsicSize()));
        return;
      }
    }
  }

 private:
  // These fields may only be accessed on the main thread
  WeakPtr<HTMLMediaElement> mElement;
  // We hold mElement->MainThreadEventTarget() here because the mElement could
  // be reset in Forget().
  nsCOMPtr<nsISerialEventTarget> mMainThreadEventTarget;

  // These fields may only be accessed on the MSG's appending thread.
  // (this is a direct listener so we get called by whoever is producing
  // this track's data)
  bool mInitialSizeFound;
};

class HTMLMediaElement::StreamCaptureTrackSource
    : public MediaStreamTrackSource,
      public MediaStreamTrackSource::Sink {
 public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(StreamCaptureTrackSource,
                                           MediaStreamTrackSource)

  StreamCaptureTrackSource(HTMLMediaElement* aElement,
                           MediaStreamTrackSource* aCapturedTrackSource,
                           DOMMediaStream* aOwningStream,
                           TrackID aDestinationTrackID)
      : MediaStreamTrackSource(aCapturedTrackSource->GetPrincipal(),
                               nsString()),
        mElement(aElement),
        mCapturedTrackSource(aCapturedTrackSource),
        mOwningStream(aOwningStream),
        mDestinationTrackID(aDestinationTrackID) {
    MOZ_ASSERT(mElement);
    MOZ_ASSERT(mCapturedTrackSource);
    MOZ_ASSERT(mOwningStream);
    MOZ_ASSERT(IsTrackIDExplicit(mDestinationTrackID));

    mCapturedTrackSource->RegisterSink(this);
  }

  void Destroy() override {
    if (mCapturedTrackSource) {
      mCapturedTrackSource->UnregisterSink(this);
      mCapturedTrackSource = nullptr;
    }
  }

  MediaSourceEnum GetMediaSource() const override {
    return MediaSourceEnum::Other;
  }

  CORSMode GetCORSMode() const override {
    if (!mCapturedTrackSource) {
      // This could happen during shutdown.
      return CORS_NONE;
    }

    return mCapturedTrackSource->GetCORSMode();
  }

  void Stop() override {
    if (mElement && mElement->mSrcStream) {
      // Only notify if we're still playing the source stream. GC might have
      // cleared it before the track sources.
      mElement->NotifyOutputTrackStopped(mOwningStream, mDestinationTrackID);
    }
    mElement = nullptr;
    mOwningStream = nullptr;

    Destroy();
  }

  /**
   * Do not keep the track source alive. The source lifetime is controlled by
   * its associated tracks.
   */
  bool KeepsSourceAlive() const override { return false; }

  /**
   * Do not keep the track source on. It is controlled by its associated tracks.
   */
  bool Enabled() const override { return false; }

  void Disable() override {}

  void Enable() override {}

  void PrincipalChanged() override {
    if (!mCapturedTrackSource) {
      // This could happen during shutdown.
      return;
    }

    mPrincipal = mCapturedTrackSource->GetPrincipal();
    MediaStreamTrackSource::PrincipalChanged();
  }

  void MutedChanged(bool aNewState) override {
    if (!mCapturedTrackSource) {
      // This could happen during shutdown.
      return;
    }

    MediaStreamTrackSource::MutedChanged(aNewState);
  }

 private:
  virtual ~StreamCaptureTrackSource() = default;

  RefPtr<HTMLMediaElement> mElement;
  RefPtr<MediaStreamTrackSource> mCapturedTrackSource;
  RefPtr<DOMMediaStream> mOwningStream;
  TrackID mDestinationTrackID;
};

NS_IMPL_ADDREF_INHERITED(HTMLMediaElement::StreamCaptureTrackSource,
                         MediaStreamTrackSource)
NS_IMPL_RELEASE_INHERITED(HTMLMediaElement::StreamCaptureTrackSource,
                          MediaStreamTrackSource)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(
    HTMLMediaElement::StreamCaptureTrackSource)
NS_INTERFACE_MAP_END_INHERITING(MediaStreamTrackSource)
NS_IMPL_CYCLE_COLLECTION_INHERITED(HTMLMediaElement::StreamCaptureTrackSource,
                                   MediaStreamTrackSource, mElement,
                                   mCapturedTrackSource, mOwningStream)

/**
 * There is a reference cycle involving this class: MediaLoadListener
 * holds a reference to the HTMLMediaElement, which holds a reference
 * to an nsIChannel, which holds a reference to this listener.
 * We break the reference cycle in OnStartRequest by clearing mElement.
 */
class HTMLMediaElement::MediaLoadListener final
    : public nsIStreamListener,
      public nsIChannelEventSink,
      public nsIInterfaceRequestor,
      public nsIObserver,
      public nsIThreadRetargetableStreamListener {
  ~MediaLoadListener() {}

  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIREQUESTOBSERVER
  NS_DECL_NSISTREAMLISTENER
  NS_DECL_NSICHANNELEVENTSINK
  NS_DECL_NSIOBSERVER
  NS_DECL_NSIINTERFACEREQUESTOR
  NS_DECL_NSITHREADRETARGETABLESTREAMLISTENER

 public:
  explicit MediaLoadListener(HTMLMediaElement* aElement)
      : mElement(aElement), mLoadID(aElement->GetCurrentLoadID()) {
    MOZ_ASSERT(mElement, "Must pass an element to call back");
  }

 private:
  RefPtr<HTMLMediaElement> mElement;
  nsCOMPtr<nsIStreamListener> mNextListener;
  const uint32_t mLoadID;
};

NS_IMPL_ISUPPORTS(HTMLMediaElement::MediaLoadListener, nsIRequestObserver,
                  nsIStreamListener, nsIChannelEventSink, nsIInterfaceRequestor,
                  nsIObserver, nsIThreadRetargetableStreamListener)

NS_IMETHODIMP
HTMLMediaElement::MediaLoadListener::Observe(nsISupports* aSubject,
                                             const char* aTopic,
                                             const char16_t* aData) {
  nsContentUtils::UnregisterShutdownObserver(this);

  // Clear mElement to break cycle so we don't leak on shutdown
  mElement = nullptr;
  return NS_OK;
}

NS_IMETHODIMP
HTMLMediaElement::MediaLoadListener::OnStartRequest(nsIRequest* aRequest) {
  nsContentUtils::UnregisterShutdownObserver(this);

  if (!mElement) {
    // We've been notified by the shutdown observer, and are shutting down.
    return NS_BINDING_ABORTED;
  }

  // Media element playback is not currently supported when recording or
  // replaying. See bug 1304146.
  if (recordreplay::IsRecordingOrReplaying()) {
    mElement->ReportLoadError("Media elements not available when recording",
                              nullptr, 0);
    return NS_ERROR_NOT_AVAILABLE;
  }

  // The element is only needed until we've had a chance to call
  // InitializeDecoderForChannel. So make sure mElement is cleared here.
  RefPtr<HTMLMediaElement> element;
  element.swap(mElement);

  AbstractThread::AutoEnter context(element->AbstractMainThread());

  if (mLoadID != element->GetCurrentLoadID()) {
    // The channel has been cancelled before we had a chance to create
    // a decoder. Abort, don't dispatch an "error" event, as the new load
    // may not be in an error state.
    return NS_BINDING_ABORTED;
  }

  // Don't continue to load if the request failed or has been canceled.
  nsresult status;
  nsresult rv = aRequest->GetStatus(&status);
  NS_ENSURE_SUCCESS(rv, rv);
  if (NS_FAILED(status)) {
    if (element) {
      // Handle media not loading error because source was a tracking URL (or
      // fingerprinting, cryptomining, etc).
      // We make a note of this media node by including it in a dedicated
      // array of blocked tracking nodes under its parent document.
      if (net::UrlClassifierFeatureFactory::IsClassifierBlockingErrorCode(
              status)) {
        Document* ownerDoc = element->OwnerDoc();
        if (ownerDoc) {
          ownerDoc->AddBlockedNodeByClassifier(element);
        }
      }
      element->NotifyLoadError(
          nsPrintfCString("%u: %s", uint32_t(status), "Request failed"));
    }
    return status;
  }

  nsCOMPtr<nsIHttpChannel> hc = do_QueryInterface(aRequest);
  bool succeeded;
  if (hc && NS_SUCCEEDED(hc->GetRequestSucceeded(&succeeded)) && !succeeded) {
    uint32_t responseStatus = 0;
    Unused << hc->GetResponseStatus(&responseStatus);
    nsAutoCString statusText;
    Unused << hc->GetResponseStatusText(statusText);
    element->NotifyLoadError(
        nsPrintfCString("%u: %s", responseStatus, statusText.get()));

    nsAutoString code;
    code.AppendInt(responseStatus);
    nsAutoString src;
    element->GetCurrentSrc(src);
    const char16_t* params[] = {code.get(), src.get()};
    element->ReportLoadError("MediaLoadHttpError", params, ArrayLength(params));
    return NS_BINDING_ABORTED;
  }

  nsCOMPtr<nsIChannel> channel = do_QueryInterface(aRequest);
  if (channel &&
      NS_SUCCEEDED(rv = element->InitializeDecoderForChannel(
                       channel, getter_AddRefs(mNextListener))) &&
      mNextListener) {
    rv = mNextListener->OnStartRequest(aRequest);
  } else {
    // If InitializeDecoderForChannel() returned an error, fire a network error.
    if (NS_FAILED(rv) && !mNextListener) {
      // Load failed, attempt to load the next candidate resource. If there
      // are none, this will trigger a MEDIA_ERR_SRC_NOT_SUPPORTED error.
      element->NotifyLoadError(NS_LITERAL_CSTRING("Failed to init decoder"));
    }
    // If InitializeDecoderForChannel did not return a listener (but may
    // have otherwise succeeded), we abort the connection since we aren't
    // interested in keeping the channel alive ourselves.
    rv = NS_BINDING_ABORTED;
  }

  return rv;
}

NS_IMETHODIMP
HTMLMediaElement::MediaLoadListener::OnStopRequest(nsIRequest* aRequest,
                                                   nsresult aStatus) {
  if (mNextListener) {
    return mNextListener->OnStopRequest(aRequest, aStatus);
  }
  return NS_OK;
}

NS_IMETHODIMP
HTMLMediaElement::MediaLoadListener::OnDataAvailable(nsIRequest* aRequest,
                                                     nsIInputStream* aStream,
                                                     uint64_t aOffset,
                                                     uint32_t aCount) {
  if (!mNextListener) {
    NS_ERROR(
        "Must have a chained listener; OnStartRequest should have "
        "canceled this request");
    return NS_BINDING_ABORTED;
  }
  return mNextListener->OnDataAvailable(aRequest, aStream, aOffset, aCount);
}

NS_IMETHODIMP
HTMLMediaElement::MediaLoadListener::AsyncOnChannelRedirect(
    nsIChannel* aOldChannel, nsIChannel* aNewChannel, uint32_t aFlags,
    nsIAsyncVerifyRedirectCallback* cb) {
  // TODO is this really correct?? See bug #579329.
  if (mElement) {
    mElement->OnChannelRedirect(aOldChannel, aNewChannel, aFlags);
  }
  nsCOMPtr<nsIChannelEventSink> sink = do_QueryInterface(mNextListener);
  if (sink) {
    return sink->AsyncOnChannelRedirect(aOldChannel, aNewChannel, aFlags, cb);
  }
  cb->OnRedirectVerifyCallback(NS_OK);
  return NS_OK;
}

NS_IMETHODIMP
HTMLMediaElement::MediaLoadListener::CheckListenerChain() {
  MOZ_ASSERT(mNextListener);
  nsCOMPtr<nsIThreadRetargetableStreamListener> retargetable =
      do_QueryInterface(mNextListener);
  if (retargetable) {
    return retargetable->CheckListenerChain();
  }
  return NS_ERROR_NO_INTERFACE;
}

NS_IMETHODIMP
HTMLMediaElement::MediaLoadListener::GetInterface(const nsIID& aIID,
                                                  void** aResult) {
  return QueryInterface(aIID, aResult);
}

void HTMLMediaElement::ReportLoadError(const char* aMsg,
                                       const char16_t** aParams,
                                       uint32_t aParamCount) {
  ReportToConsole(nsIScriptError::warningFlag, aMsg, aParams, aParamCount);
}

void HTMLMediaElement::ReportToConsole(uint32_t aErrorFlags, const char* aMsg,
                                       const char16_t** aParams,
                                       uint32_t aParamCount) const {
  nsContentUtils::ReportToConsole(aErrorFlags, NS_LITERAL_CSTRING("Media"),
                                  OwnerDoc(), nsContentUtils::eDOM_PROPERTIES,
                                  aMsg, aParams, aParamCount);
}

class HTMLMediaElement::AudioChannelAgentCallback final
    : public nsIAudioChannelAgentCallback {
 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_CLASS(AudioChannelAgentCallback)

  explicit AudioChannelAgentCallback(HTMLMediaElement* aOwner)
      : mOwner(aOwner),
        mAudioChannelVolume(1.0),
        mPlayingThroughTheAudioChannel(false),
        mAudioCapturedByWindow(false),
        mSuspended(nsISuspendedTypes::NONE_SUSPENDED),
        mIsOwnerAudible(IsOwnerAudible()),
        mIsShutDown(false) {
    MOZ_ASSERT(mOwner);
    MaybeCreateAudioChannelAgent();
  }

  void UpdateAudioChannelPlayingState(bool aForcePlaying = false) {
    MOZ_ASSERT(!mIsShutDown);
    bool playingThroughTheAudioChannel =
        aForcePlaying || IsPlayingThroughTheAudioChannel();

    if (playingThroughTheAudioChannel != mPlayingThroughTheAudioChannel) {
      if (!MaybeCreateAudioChannelAgent()) {
        return;
      }

      mPlayingThroughTheAudioChannel = playingThroughTheAudioChannel;
      NotifyAudioChannelAgent(mPlayingThroughTheAudioChannel);
    }
  }

  bool ShouldResetSuspend() const {
    // The disposable-pause should be clear after media starts playing.
    if (!mOwner->Paused() &&
        mSuspended == nsISuspendedTypes::SUSPENDED_PAUSE_DISPOSABLE) {
      return true;
    }

    // If the blocked media is paused, we don't need to resume it. We reset the
    // mSuspended in order to unregister the agent.
    if (mOwner->Paused() && mSuspended == nsISuspendedTypes::SUSPENDED_BLOCK) {
      return true;
    }

    return false;
  }

  void NotifyPlayStateChanged() {
    MOZ_ASSERT(!mIsShutDown);
    if (ShouldResetSuspend()) {
      SetSuspended(nsISuspendedTypes::NONE_SUSPENDED);
      NotifyAudioPlaybackChanged(
          AudioChannelService::AudibleChangedReasons::ePauseStateChanged);
    }
    UpdateAudioChannelPlayingState();
  }

  NS_IMETHODIMP WindowVolumeChanged(float aVolume, bool aMuted) override {
    MOZ_ASSERT(mAudioChannelAgent);

    MOZ_LOG(
        AudioChannelService::GetAudioChannelLog(), LogLevel::Debug,
        ("HTMLMediaElement::AudioChannelAgentCallback, WindowVolumeChanged, "
         "this = %p, aVolume = %f, aMuted = %s\n",
         this, aVolume, aMuted ? "true" : "false"));

    if (mAudioChannelVolume != aVolume) {
      mAudioChannelVolume = aVolume;
      mOwner->SetVolumeInternal();
    }

    const uint32_t muted = mOwner->mMuted;
    if (aMuted && !mOwner->ComputedMuted()) {
      mOwner->SetMutedInternal(muted | MUTED_BY_AUDIO_CHANNEL);
    } else if (!aMuted && mOwner->ComputedMuted()) {
      mOwner->SetMutedInternal(muted & ~MUTED_BY_AUDIO_CHANNEL);
    }

    return NS_OK;
  }

  NS_IMETHODIMP WindowSuspendChanged(SuspendTypes aSuspend) override {
    MOZ_ASSERT(mAudioChannelAgent);

    MOZ_LOG(
        AudioChannelService::GetAudioChannelLog(), LogLevel::Debug,
        ("HTMLMediaElement::AudioChannelAgentCallback, WindowSuspendChanged, "
         "this = %p, aSuspend = %s\n",
         this, SuspendTypeToStr(aSuspend)));

    switch (aSuspend) {
      case nsISuspendedTypes::NONE_SUSPENDED:
        Resume();
        break;
      case nsISuspendedTypes::SUSPENDED_PAUSE:
      case nsISuspendedTypes::SUSPENDED_PAUSE_DISPOSABLE:
      case nsISuspendedTypes::SUSPENDED_BLOCK:
        Suspend(aSuspend);
        break;
      case nsISuspendedTypes::SUSPENDED_STOP_DISPOSABLE:
        Stop();
        break;
      default:
        MOZ_LOG(AudioChannelService::GetAudioChannelLog(), LogLevel::Debug,
                ("HTMLMediaElement::AudioChannelAgentCallback, "
                 "WindowSuspendChanged, "
                 "this = %p, Error : unknown suspended type!\n",
                 this));
    }
    return NS_OK;
  }

  NS_IMETHODIMP WindowAudioCaptureChanged(bool aCapture) override {
    MOZ_ASSERT(mAudioChannelAgent);

    if (mAudioCapturedByWindow != aCapture) {
      mAudioCapturedByWindow = aCapture;
      AudioCaptureStreamChangeIfNeeded();
    }
    return NS_OK;
  }

  void AudioCaptureStreamChangeIfNeeded() {
    MOZ_ASSERT(!mIsShutDown);
    if (!IsPlayingStarted()) {
      return;
    }

    if (!mOwner->HasAudio()) {
      return;
    }

    mOwner->AudioCaptureStreamChange(mAudioCapturedByWindow);
  }

  void NotifyAudioPlaybackChanged(AudibleChangedReasons aReason) {
    MOZ_ASSERT(!mIsShutDown);
    if (!IsPlayingStarted()) {
      return;
    }

    AudibleState newAudibleState = IsOwnerAudible();
    if (mIsOwnerAudible == newAudibleState) {
      return;
    }

    mIsOwnerAudible = newAudibleState;
    mAudioChannelAgent->NotifyStartedAudible(mIsOwnerAudible, aReason);
  }

  bool IsPlaybackBlocked() {
    MOZ_ASSERT(!mIsShutDown);
    // If the tab hasn't been activated yet, the media element in that tab can't
    // be playback now until the tab goes to foreground first time or user
    // clicks the unblocking tab icon.
    if (!IsTabActivated()) {
      // Even we haven't start playing yet, we still need to notify the audio
      // channe system because we need to receive the resume notification later.
      UpdateAudioChannelPlayingState(true /* force to start */);
      return true;
    }

    return false;
  }

  void Shutdown() {
    MOZ_ASSERT(!mIsShutDown);
    if (mAudioChannelAgent) {
      mAudioChannelAgent->NotifyStoppedPlaying();
      mAudioChannelAgent = nullptr;
    }
    mIsShutDown = true;
  }

  float GetEffectiveVolume() const {
    MOZ_ASSERT(!mIsShutDown);
    return mOwner->Volume() * mAudioChannelVolume;
  }

  SuspendTypes GetSuspendType() const {
    MOZ_ASSERT(!mIsShutDown);
    return mSuspended;
  }

 private:
  ~AudioChannelAgentCallback() { MOZ_ASSERT(mIsShutDown); };

  bool MaybeCreateAudioChannelAgent() {
    if (mAudioChannelAgent) {
      return true;
    }

    mAudioChannelAgent = new AudioChannelAgent();
    nsresult rv =
        mAudioChannelAgent->Init(mOwner->OwnerDoc()->GetInnerWindow(), this);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      mAudioChannelAgent = nullptr;
      MOZ_LOG(
          AudioChannelService::GetAudioChannelLog(), LogLevel::Debug,
          ("HTMLMediaElement::AudioChannelAgentCallback, Fail to initialize "
           "the audio channel agent, this = %p\n",
           this));
      return false;
    }

    return true;
  }

  void NotifyAudioChannelAgent(bool aPlaying) {
    MOZ_ASSERT(mAudioChannelAgent);

    if (aPlaying) {
      AudioPlaybackConfig config;
      nsresult rv =
          mAudioChannelAgent->NotifyStartedPlaying(&config, IsOwnerAudible());
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return;
      }

      WindowVolumeChanged(config.mVolume, config.mMuted);
      WindowSuspendChanged(config.mSuspend);
    } else {
      mAudioChannelAgent->NotifyStoppedPlaying();
    }
  }

  void SetSuspended(SuspendTypes aSuspend) {
    if (mSuspended == aSuspend) {
      return;
    }

    MaybeNotifyMediaResumed(aSuspend);
    mSuspended = aSuspend;
    MOZ_LOG(AudioChannelService::GetAudioChannelLog(), LogLevel::Debug,
            ("HTMLMediaElement::AudioChannelAgentCallback, "
             "SetAudioChannelSuspended, "
             "this = %p, aSuspend = %s\n",
             this, SuspendTypeToStr(aSuspend)));
  }

  void Resume() {
    if (!IsSuspended()) {
      MOZ_LOG(AudioChannelService::GetAudioChannelLog(), LogLevel::Debug,
              ("HTMLMediaElement::AudioChannelAgentCallback, "
               "ResumeFromAudioChannel, "
               "this = %p, don't need to be resumed!\n",
               this));
      return;
    }

    SetSuspended(nsISuspendedTypes::NONE_SUSPENDED);
    IgnoredErrorResult rv;
    RefPtr<Promise> toBeIgnored = mOwner->Play(rv);
    MOZ_ASSERT_IF(
        toBeIgnored && toBeIgnored->State() == Promise::PromiseState::Rejected,
        rv.Failed());
    if (rv.Failed()) {
      NS_WARNING("Not able to resume from AudioChannel.");
    }

    NotifyAudioPlaybackChanged(
        AudioChannelService::AudibleChangedReasons::ePauseStateChanged);
  }

  void Suspend(SuspendTypes aSuspend) {
    if (IsSuspended()) {
      return;
    }

    SetSuspended(aSuspend);
    if (aSuspend == nsISuspendedTypes::SUSPENDED_PAUSE ||
        aSuspend == nsISuspendedTypes::SUSPENDED_PAUSE_DISPOSABLE) {
      IgnoredErrorResult rv;
      mOwner->Pause(rv);
      if (NS_WARN_IF(rv.Failed())) {
        return;
      }
    }
    NotifyAudioPlaybackChanged(
        AudioChannelService::AudibleChangedReasons::ePauseStateChanged);
  }

  void Stop() {
    SetSuspended(nsISuspendedTypes::NONE_SUSPENDED);
    mOwner->Pause();
  }

  bool IsPlayingStarted() {
    if (MaybeCreateAudioChannelAgent()) {
      return mAudioChannelAgent->IsPlayingStarted();
    }
    return false;
  }

  void MaybeNotifyMediaResumed(SuspendTypes aSuspend) {
    // In fennec, we should send the notification when media is resumed from the
    // pause-disposable which was called by media control.
    if (mSuspended != nsISuspendedTypes::SUSPENDED_PAUSE_DISPOSABLE &&
        aSuspend != nsISuspendedTypes::NONE_SUSPENDED) {
      return;
    }

    if (!IsPlayingStarted()) {
      return;
    }

    uint64_t windowID = mAudioChannelAgent->WindowID();
    mOwner->MainThreadEventTarget()->Dispatch(NS_NewRunnableFunction(
        "dom::HTMLMediaElement::AudioChannelAgentCallback::"
        "MaybeNotifyMediaResumed",
        [windowID]() -> void {
          nsCOMPtr<nsIObserverService> observerService =
              services::GetObserverService();
          if (NS_WARN_IF(!observerService)) {
            return;
          }

          nsCOMPtr<nsISupportsPRUint64> wrapper =
              do_CreateInstance(NS_SUPPORTS_PRUINT64_CONTRACTID);
          if (NS_WARN_IF(!wrapper)) {
            return;
          }

          wrapper->SetData(windowID);
          observerService->NotifyObservers(wrapper, "media-playback-resumed",
                                           u"active");
        }));
  }

  bool IsTabActivated() {
    if (MaybeCreateAudioChannelAgent()) {
      return !mAudioChannelAgent->ShouldBlockMedia();
    }
    return false;
  }

  bool IsSuspended() const {
    return (mSuspended == nsISuspendedTypes::SUSPENDED_PAUSE ||
            mSuspended == nsISuspendedTypes::SUSPENDED_PAUSE_DISPOSABLE ||
            mSuspended == nsISuspendedTypes::SUSPENDED_BLOCK);
  }

  AudibleState IsOwnerAudible() const {
    // Muted or the volume should not be ~0
    if (mOwner->mMuted || (std::fabs(mOwner->Volume()) <= 1e-7)) {
      return mOwner->HasAudio()
                 ? AudioChannelService::AudibleState::eMaybeAudible
                 : AudioChannelService::AudibleState::eNotAudible;
    }

    // No audio track.
    if (!mOwner->HasAudio()) {
      return AudioChannelService::AudibleState::eNotAudible;
    }

    // Might be audible but not yet.
    if (mOwner->HasAudio() && !mOwner->mIsAudioTrackAudible) {
      return AudioChannelService::AudibleState::eMaybeAudible;
    }

    // Suspended or paused media doesn't produce any sound.
    if (mSuspended != nsISuspendedTypes::NONE_SUSPENDED || mOwner->mPaused) {
      return AudioChannelService::AudibleState::eNotAudible;
    }

    return AudioChannelService::AudibleState::eAudible;
  }

  bool IsPlayingThroughTheAudioChannel() const {
    // If we have an error, we are not playing.
    if (mOwner->GetError()) {
      return false;
    }

    // We should consider any bfcached page or inactive document as non-playing.
    if (!mOwner->IsActive()) {
      return false;
    }

    // It might be resumed from remote, we should keep the audio channel agent.
    if (IsSuspended()) {
      return true;
    }

    // Are we paused
    if (mOwner->mPaused) {
      return false;
    }

    // No audio track
    if (!mOwner->HasAudio()) {
      return false;
    }

    // A loop always is playing
    if (mOwner->HasAttr(kNameSpaceID_None, nsGkAtoms::loop)) {
      return true;
    }

    // If we are actually playing...
    if (mOwner->IsCurrentlyPlaying()) {
      return true;
    }

    // If we are playing an external stream.
    if (mOwner->mSrcAttrStream) {
      return true;
    }

    return false;
  }

  RefPtr<AudioChannelAgent> mAudioChannelAgent;
  HTMLMediaElement* mOwner;

  // The audio channel volume
  float mAudioChannelVolume;
  // Is this media element playing?
  bool mPlayingThroughTheAudioChannel;
  // True if the sound is being captured by the window.
  bool mAudioCapturedByWindow;
  // We have different kinds of suspended cases,
  // - SUSPENDED_PAUSE
  // It's used when we temporary lost platform audio focus. MediaElement can
  // only be resumed when we gain the audio focus again.
  // - SUSPENDED_PAUSE_DISPOSABLE
  // It's used when user press the pause button on the remote media-control.
  // MediaElement can be resumed by remote media-control or via play().
  // - SUSPENDED_BLOCK
  // It's used to reduce the power consumption, we won't play the auto-play
  // audio/video in the page we have never visited before. MediaElement would
  // be resumed when the page is active. See bug647429 for more details.
  // - SUSPENDED_STOP_DISPOSABLE
  // When we permanently lost platform audio focus, we should stop playing
  // and stop the audio channel agent. MediaElement can only be restarted by
  // play().
  SuspendTypes mSuspended;
  // Indicate whether media element is audible for users.
  AudibleState mIsOwnerAudible;
  bool mIsShutDown;
};

NS_IMPL_CYCLE_COLLECTION_CLASS(HTMLMediaElement::AudioChannelAgentCallback)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(
    HTMLMediaElement::AudioChannelAgentCallback)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mAudioChannelAgent)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(
    HTMLMediaElement::AudioChannelAgentCallback)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mAudioChannelAgent)
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(
    HTMLMediaElement::AudioChannelAgentCallback)
  NS_INTERFACE_MAP_ENTRY(nsIAudioChannelAgentCallback)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(HTMLMediaElement::AudioChannelAgentCallback)
NS_IMPL_CYCLE_COLLECTING_RELEASE(HTMLMediaElement::AudioChannelAgentCallback)

class HTMLMediaElement::ChannelLoader final {
 public:
  NS_INLINE_DECL_REFCOUNTING(ChannelLoader);

  void LoadInternal(HTMLMediaElement* aElement) {
    if (mCancelled) {
      return;
    }

    // determine what security checks need to be performed in AsyncOpen().
    nsSecurityFlags securityFlags =
        aElement->ShouldCheckAllowOrigin()
            ? nsILoadInfo::SEC_REQUIRE_CORS_DATA_INHERITS
            : nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_DATA_INHERITS;

    if (aElement->GetCORSMode() == CORS_USE_CREDENTIALS) {
      securityFlags |= nsILoadInfo::SEC_COOKIES_INCLUDE;
    }

    MOZ_ASSERT(
        aElement->IsAnyOfHTMLElements(nsGkAtoms::audio, nsGkAtoms::video));
    nsContentPolicyType contentPolicyType =
        aElement->IsHTMLElement(nsGkAtoms::audio)
            ? nsIContentPolicy::TYPE_INTERNAL_AUDIO
            : nsIContentPolicy::TYPE_INTERNAL_VIDEO;

    // If aElement has 'triggeringprincipal' attribute, we will use the value as
    // triggeringPrincipal for the channel, otherwise it will default to use
    // aElement->NodePrincipal().
    // This function returns true when aElement has 'triggeringprincipal', so if
    // setAttrs is true we will override the origin attributes on the channel
    // later.
    nsCOMPtr<nsIPrincipal> triggeringPrincipal;
    bool setAttrs = nsContentUtils::QueryTriggeringPrincipal(
        aElement, aElement->mLoadingSrcTriggeringPrincipal,
        getter_AddRefs(triggeringPrincipal));

    nsCOMPtr<nsILoadGroup> loadGroup = aElement->GetDocumentLoadGroup();
    nsCOMPtr<nsIChannel> channel;
    nsresult rv = NS_NewChannelWithTriggeringPrincipal(
        getter_AddRefs(channel), aElement->mLoadingSrc,
        static_cast<Element*>(aElement), triggeringPrincipal, securityFlags,
        contentPolicyType,
        nullptr,  // aPerformanceStorage
        loadGroup,
        nullptr,  // aCallbacks
        nsICachingChannel::LOAD_BYPASS_LOCAL_CACHE_IF_BUSY |
            nsIChannel::LOAD_MEDIA_SNIFFER_OVERRIDES_CONTENT_TYPE |
            nsIChannel::LOAD_CALL_CONTENT_SNIFFERS);

    if (NS_FAILED(rv)) {
      // Notify load error so the element will try next resource candidate.
      aElement->NotifyLoadError(NS_LITERAL_CSTRING("Fail to create channel"));
      return;
    }

    if (setAttrs) {
      nsCOMPtr<nsILoadInfo> loadInfo = channel->LoadInfo();
      // The function simply returns NS_OK, so we ignore the return value.
      Unused << loadInfo->SetOriginAttributes(
          triggeringPrincipal->OriginAttributesRef());
    }

    nsCOMPtr<nsIClassOfService> cos(do_QueryInterface(channel));
    if (cos) {
      if (aElement->mUseUrgentStartForChannel) {
        cos->AddClassFlags(nsIClassOfService::UrgentStart);

        // Reset the flag to avoid loading again without initiated by user
        // interaction.
        aElement->mUseUrgentStartForChannel = false;
      }

      // Unconditionally disable throttling since we want the media to fluently
      // play even when we switch the tab to background.
      cos->AddClassFlags(nsIClassOfService::DontThrottle);
    }

    // The listener holds a strong reference to us.  This creates a
    // reference cycle, once we've set mChannel, which is manually broken
    // in the listener's OnStartRequest method after it is finished with
    // the element. The cycle will also be broken if we get a shutdown
    // notification before OnStartRequest fires.  Necko guarantees that
    // OnStartRequest will eventually fire if we don't shut down first.
    RefPtr<MediaLoadListener> loadListener = new MediaLoadListener(aElement);

    channel->SetNotificationCallbacks(loadListener);

    nsCOMPtr<nsIHttpChannel> hc = do_QueryInterface(channel);
    if (hc) {
      // Use a byte range request from the start of the resource.
      // This enables us to detect if the stream supports byte range
      // requests, and therefore seeking, early.
      rv = hc->SetRequestHeader(NS_LITERAL_CSTRING("Range"),
                                NS_LITERAL_CSTRING("bytes=0-"), false);
      MOZ_ASSERT(NS_SUCCEEDED(rv));
      aElement->SetRequestHeaders(hc);
    }

    rv = channel->AsyncOpen(loadListener);
    if (NS_FAILED(rv)) {
      // Notify load error so the element will try next resource candidate.
      aElement->NotifyLoadError(NS_LITERAL_CSTRING("Failed to open channel"));
      return;
    }

    // Else the channel must be open and starting to download. If it encounters
    // a non-catastrophic failure, it will set a new task to continue loading
    // another candidate.  It's safe to set it as mChannel now.
    mChannel = channel;

    // loadListener will be unregistered either on shutdown or when
    // OnStartRequest for the channel we just opened fires.
    nsContentUtils::RegisterShutdownObserver(loadListener);
  }

  nsresult Load(HTMLMediaElement* aElement) {
    MOZ_ASSERT(aElement);
    // Per bug 1235183 comment 8, we can't spin the event loop from stable
    // state. Defer NS_NewChannel() to a new regular runnable.
    return aElement->MainThreadEventTarget()->Dispatch(
        NewRunnableMethod<HTMLMediaElement*>("ChannelLoader::LoadInternal",
                                             this, &ChannelLoader::LoadInternal,
                                             aElement));
  }

  void Cancel() {
    mCancelled = true;
    if (mChannel) {
      mChannel->Cancel(NS_BINDING_ABORTED);
      mChannel = nullptr;
    }
  }

  void Done() {
    MOZ_ASSERT(mChannel);
    // Decoder successfully created, the decoder now owns the MediaResource
    // which owns the channel.
    mChannel = nullptr;
  }

  nsresult Redirect(nsIChannel* aChannel, nsIChannel* aNewChannel,
                    uint32_t aFlags) {
    NS_ASSERTION(aChannel == mChannel, "Channels should match!");
    mChannel = aNewChannel;

    // Handle forwarding of Range header so that the intial detection
    // of seeking support (via result code 206) works across redirects.
    nsCOMPtr<nsIHttpChannel> http = do_QueryInterface(aChannel);
    NS_ENSURE_STATE(http);

    NS_NAMED_LITERAL_CSTRING(rangeHdr, "Range");

    nsAutoCString rangeVal;
    if (NS_SUCCEEDED(http->GetRequestHeader(rangeHdr, rangeVal))) {
      NS_ENSURE_STATE(!rangeVal.IsEmpty());

      http = do_QueryInterface(aNewChannel);
      NS_ENSURE_STATE(http);

      nsresult rv = http->SetRequestHeader(rangeHdr, rangeVal, false);
      NS_ENSURE_SUCCESS(rv, rv);
    }

    return NS_OK;
  }

 private:
  ~ChannelLoader() { MOZ_ASSERT(!mChannel); }
  // Holds a reference to the first channel we open to the media resource.
  // Once the decoder is created, control over the channel passes to the
  // decoder, and we null out this reference. We must store this in case
  // we need to cancel the channel before control of it passes to the decoder.
  nsCOMPtr<nsIChannel> mChannel;

  bool mCancelled = false;
};

class HTMLMediaElement::ErrorSink {
 public:
  explicit ErrorSink(HTMLMediaElement* aOwner) : mOwner(aOwner) {
    MOZ_ASSERT(mOwner);
  }

  void SetError(uint16_t aErrorCode, const nsACString& aErrorDetails) {
    // Since we have multiple paths calling into DecodeError, e.g.
    // MediaKeys::Terminated and EMEH264Decoder::Error. We should take the 1st
    // one only in order not to fire multiple 'error' events.
    if (mError) {
      return;
    }

    if (!IsValidErrorCode(aErrorCode)) {
      NS_ASSERTION(false, "Undefined MediaError codes!");
      return;
    }

    mError = new MediaError(mOwner, aErrorCode, aErrorDetails);
    mOwner->DispatchAsyncEvent(NS_LITERAL_STRING("error"));
    if (mOwner->ReadyState() == HAVE_NOTHING &&
        aErrorCode == MEDIA_ERR_ABORTED) {
      // https://html.spec.whatwg.org/multipage/embedded-content.html#media-data-processing-steps-list
      // "If the media data fetching process is aborted by the user"
      mOwner->DispatchAsyncEvent(NS_LITERAL_STRING("abort"));
      mOwner->ChangeNetworkState(NETWORK_EMPTY);
      mOwner->DispatchAsyncEvent(NS_LITERAL_STRING("emptied"));
      if (mOwner->mDecoder) {
        mOwner->ShutdownDecoder();
      }
    } else if (aErrorCode == MEDIA_ERR_SRC_NOT_SUPPORTED) {
      mOwner->ChangeNetworkState(NETWORK_NO_SOURCE);
    } else {
      mOwner->ChangeNetworkState(NETWORK_IDLE);
    }
  }

  void ResetError() { mError = nullptr; }

  RefPtr<MediaError> mError;

 private:
  bool IsValidErrorCode(const uint16_t& aErrorCode) const {
    return (aErrorCode == MEDIA_ERR_DECODE || aErrorCode == MEDIA_ERR_NETWORK ||
            aErrorCode == MEDIA_ERR_ABORTED ||
            aErrorCode == MEDIA_ERR_SRC_NOT_SUPPORTED);
  }

  // Media elememt's life cycle would be longer than error sink, so we use the
  // raw pointer and this class would only be referenced by media element.
  HTMLMediaElement* mOwner;
};

NS_IMPL_CYCLE_COLLECTION_CLASS(HTMLMediaElement)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(HTMLMediaElement,
                                                  nsGenericHTMLElement)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mMediaSource)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mSrcMediaSource)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mSrcStream)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mSrcAttrStream)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mSourcePointer)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mLoadBlockedDoc)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mSourceLoadCandidate)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mAudioChannelWrapper)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mErrorSink->mError)
  for (uint32_t i = 0; i < tmp->mOutputStreams.Length(); ++i) {
    NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mOutputStreams[i].mStream)
  }
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mPlayed);
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mTextTrackManager)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mAudioTrackList)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mVideoTrackList)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mMediaKeys)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mIncomingMediaKeys)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mSelectedVideoStreamTrack)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mPendingPlayPromises)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mSeekDOMPromise)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mSetMediaKeysDOMPromise)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(HTMLMediaElement,
                                                nsGenericHTMLElement)
  tmp->RemoveMutationObserver(tmp);
  if (tmp->mSrcStream) {
    // Need to EndMediaStreamPlayback to clear mSrcStream and make sure
    // everything gets unhooked correctly.
    tmp->EndSrcMediaStreamPlayback();
  }
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mSrcAttrStream)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mMediaSource)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mSrcMediaSource)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mSourcePointer)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mLoadBlockedDoc)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mSourceLoadCandidate)
  if (tmp->mAudioChannelWrapper) {
    tmp->mAudioChannelWrapper->Shutdown();
  }
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mAudioChannelWrapper)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mErrorSink->mError)
  for (OutputMediaStream& s : tmp->mOutputStreams) {
    s.mStream->SetFinishedOnInactive(true);
  }
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mOutputStreams)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mPlayed)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mTextTrackManager)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mAudioTrackList)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mVideoTrackList)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mMediaKeys)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mIncomingMediaKeys)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mSelectedVideoStreamTrack)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mPendingPlayPromises)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mSeekDOMPromise)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mSetMediaKeysDOMPromise)
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_ISUPPORTS_CYCLE_COLLECTION_INHERITED_0(HTMLMediaElement,
                                               nsGenericHTMLElement)

void HTMLMediaElement::ContentRemoved(nsIContent* aChild,
                                      nsIContent* aPreviousSibling) {
  if (aChild == mSourcePointer) {
    mSourcePointer = aPreviousSibling;
  }
}

already_AddRefed<MediaSource> HTMLMediaElement::GetMozMediaSourceObject()
    const {
  RefPtr<MediaSource> source = mMediaSource;
  return source.forget();
}

void HTMLMediaElement::GetMozDebugReaderData(nsAString& aString) {
  if (mDecoder && !mSrcStream) {
    nsAutoCString result;
    mDecoder->GetMozDebugReaderData(result);
    CopyUTF8toUTF16(result, aString);
  }
}

already_AddRefed<Promise> HTMLMediaElement::MozRequestDebugInfo(
    ErrorResult& aRv) {
  RefPtr<Promise> promise = CreateDOMPromise(aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  nsAutoString result;
  GetMozDebugReaderData(result);

  if (mVideoFrameContainer) {
    result.AppendPrintf(
        "Compositor dropped frame(including when element's invisible): %u\n",
        mVideoFrameContainer->GetDroppedImageCount());
  }

  if (mMediaKeys) {
    nsString EMEInfo;
    GetEMEInfo(EMEInfo);
    result.AppendLiteral("EME Info: ");
    result.Append(EMEInfo);
    result.AppendLiteral("\n");
  }

  if (mDecoder) {
    mDecoder->RequestDebugInfo()->Then(
        mAbstractMainThread, __func__,
        [promise, result](const nsACString& aString) {
          promise->MaybeResolve(result + NS_ConvertUTF8toUTF16(aString));
        },
        [promise, result]() { promise->MaybeResolve(result); });
  } else {
    promise->MaybeResolve(result);
  }

  return promise.forget();
}

/* static */
void HTMLMediaElement::MozEnableDebugLog(const GlobalObject&) {
  DecoderDoctorLogger::EnableLogging();
}

already_AddRefed<Promise> HTMLMediaElement::MozRequestDebugLog(
    ErrorResult& aRv) {
  RefPtr<Promise> promise = CreateDOMPromise(aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  DecoderDoctorLogger::RetrieveMessages(this)->Then(
      mAbstractMainThread, __func__,
      [promise](const nsACString& aString) {
        promise->MaybeResolve(NS_ConvertUTF8toUTF16(aString));
      },
      [promise](nsresult rv) { promise->MaybeReject(rv); });

  return promise.forget();
}

already_AddRefed<Promise> HTMLMediaElement::MozDumpDebugInfo() {
  ErrorResult rv;
  RefPtr<Promise> promise = CreateDOMPromise(rv);
  if (NS_WARN_IF(rv.Failed())) {
    return nullptr;
  }
  if (mDecoder) {
    mDecoder->DumpDebugInfo()->Then(mAbstractMainThread, __func__,
                                    promise.get(),
                                    &Promise::MaybeResolveWithUndefined);
  } else {
    promise->MaybeResolveWithUndefined();
  }
  return promise.forget();
}

void HTMLMediaElement::SetVisible(bool aVisible) {
  mForcedHidden = !aVisible;
  if (mDecoder) {
    mDecoder->SetForcedHidden(!aVisible);
  }
}

bool HTMLMediaElement::IsVideoDecodingSuspended() const {
  return mDecoder && mDecoder->IsVideoDecodingSuspended();
}

bool HTMLMediaElement::IsVisible() const {
  return mVisibilityState == Visibility::ApproximatelyVisible;
}

already_AddRefed<layers::Image> HTMLMediaElement::GetCurrentImage() {
  MarkAsTainted();

  // TODO: In bug 1345404, handle case when video decoder is already suspended.
  ImageContainer* container = GetImageContainer();
  if (!container) {
    return nullptr;
  }

  AutoLockImage lockImage(container);
  RefPtr<layers::Image> image = lockImage.GetImage(TimeStamp::Now());
  return image.forget();
}

bool HTMLMediaElement::HasSuspendTaint() const {
  MOZ_ASSERT(!mDecoder || (mDecoder->HasSuspendTaint() == mHasSuspendTaint));
  return mHasSuspendTaint;
}

already_AddRefed<DOMMediaStream> HTMLMediaElement::GetSrcObject() const {
  NS_ASSERTION(!mSrcAttrStream || mSrcAttrStream->GetPlaybackStream(),
               "MediaStream should have been set up properly");
  RefPtr<DOMMediaStream> stream = mSrcAttrStream;
  return stream.forget();
}

void HTMLMediaElement::SetSrcObject(DOMMediaStream& aValue) {
  SetSrcObject(&aValue);
}

void HTMLMediaElement::SetSrcObject(DOMMediaStream* aValue) {
  mSrcAttrStream = aValue;
  UpdateAudioChannelPlayingState();
  DoLoad();
}

bool HTMLMediaElement::Ended() {
  return (mDecoder && mDecoder->IsEnded()) ||
         (mSrcStream && mSrcStreamPlaybackEnded);
}

void HTMLMediaElement::GetCurrentSrc(nsAString& aCurrentSrc) {
  nsAutoCString src;
  GetCurrentSpec(src);
  CopyUTF8toUTF16(src, aCurrentSrc);
}

nsresult HTMLMediaElement::OnChannelRedirect(nsIChannel* aChannel,
                                             nsIChannel* aNewChannel,
                                             uint32_t aFlags) {
  MOZ_ASSERT(mChannelLoader);
  return mChannelLoader->Redirect(aChannel, aNewChannel, aFlags);
}

void HTMLMediaElement::ShutdownDecoder() {
  RemoveMediaElementFromURITable();
  NS_ASSERTION(mDecoder, "Must have decoder to shut down");

  mWaitingForKeyListener.DisconnectIfExists();
  if (mMediaSource) {
    mMediaSource->CompletePendingTransactions();
  }
  if (!mOutputStreams.IsEmpty()) {
    mNextAvailableMediaDecoderOutputTrackID =
        mDecoder->GetNextOutputStreamTrackID();
  }
  DiscardFinishWhenEndedOutputStreams();
  mDecoder->Shutdown();
  DDUNLINKCHILD(mDecoder.get());
  mDecoder = nullptr;
  ReportAudioTrackSilenceProportionTelemetry();
}

void HTMLMediaElement::ReportPlayedTimeAfterBlockedTelemetry() {
  if (!mHasPlayEverBeenBlocked) {
    return;
  }
  mHasPlayEverBeenBlocked = false;

  const double playTimeThreshold = 7.0;
  const double playTimeAfterBlocked = mCurrentLoadPlayTime.Total();
  if (playTimeAfterBlocked <= 0.0) {
    return;
  }

  const bool isDurationLessThanTimeThresholdAndMediaPlayedToTheEnd =
      Duration() < playTimeThreshold && Ended();
  LOG(LogLevel::Debug, ("%p PLAYED_TIME_AFTER_AUTOPLAY_BLOCKED=%f, isVideo=%d",
                        this, playTimeAfterBlocked, IsVideo()));
  if (IsVideo() && playTimeAfterBlocked >= playTimeThreshold) {
    AccumulateCategorical(
        mozilla::Telemetry::LABELS_MEDIA_PLAYED_TIME_AFTER_AUTOPLAY_BLOCKED::
            VPlayedMoreThan7s);
  } else if (IsVideo() && playTimeAfterBlocked < playTimeThreshold) {
    if (isDurationLessThanTimeThresholdAndMediaPlayedToTheEnd) {
      AccumulateCategorical(
          mozilla::Telemetry::LABELS_MEDIA_PLAYED_TIME_AFTER_AUTOPLAY_BLOCKED::
              VPlayedToTheEnd);
    } else {
      AccumulateCategorical(
          mozilla::Telemetry::LABELS_MEDIA_PLAYED_TIME_AFTER_AUTOPLAY_BLOCKED::
              VPlayedLessThan7s);
    }
  } else if (!IsVideo() && playTimeAfterBlocked >= playTimeThreshold) {
    AccumulateCategorical(
        mozilla::Telemetry::LABELS_MEDIA_PLAYED_TIME_AFTER_AUTOPLAY_BLOCKED::
            APlayedMoreThan7s);
  } else if (!IsVideo() && playTimeAfterBlocked < playTimeThreshold) {
    if (isDurationLessThanTimeThresholdAndMediaPlayedToTheEnd) {
      AccumulateCategorical(
          mozilla::Telemetry::LABELS_MEDIA_PLAYED_TIME_AFTER_AUTOPLAY_BLOCKED::
              APlayedToTheEnd);
    } else {
      AccumulateCategorical(
          mozilla::Telemetry::LABELS_MEDIA_PLAYED_TIME_AFTER_AUTOPLAY_BLOCKED::
              APlayedLessThan7s);
    }
  }
}

void HTMLMediaElement::AbortExistingLoads() {
  // Abort any already-running instance of the resource selection algorithm.
  mLoadWaitStatus = NOT_WAITING;

  // Set a new load ID. This will cause events which were enqueued
  // with a different load ID to silently be cancelled.
  mCurrentLoadID++;

  // Immediately reject or resolve the already-dispatched
  // nsResolveOrRejectPendingPlayPromisesRunners. These runners won't be
  // executed again later since the mCurrentLoadID had been changed.
  for (auto& runner : mPendingPlayPromisesRunners) {
    runner->ResolveOrReject();
  }
  mPendingPlayPromisesRunners.Clear();

  if (mChannelLoader) {
    mChannelLoader->Cancel();
    mChannelLoader = nullptr;
  }

  bool fireTimeUpdate = false;

  // We need to remove VideoFrameListener before VideoTracks get emptied.
  if (mVideoFrameListener) {
    mSelectedVideoStreamTrack->RemoveDirectListener(mVideoFrameListener);
    mVideoFrameListener->Forget();
    mVideoFrameListener = nullptr;
  }

  // When aborting the existing loads, empty the objects in audio track list and
  // video track list, no events (in particular, no removetrack events) are
  // fired as part of this. Ending MediaStream sends track ended notifications,
  // so we empty the track lists prior.
  if (AudioTracks()) {
    AudioTracks()->EmptyTracks();
  }
  if (VideoTracks()) {
    VideoTracks()->EmptyTracks();
  }

  if (mDecoder) {
    fireTimeUpdate = mDecoder->GetCurrentTime() != 0.0;
    ShutdownDecoder();
  }
  if (mSrcStream) {
    EndSrcMediaStreamPlayback();
  }

  DiscardFinishWhenEndedOutputStreams();

  RemoveMediaElementFromURITable();
  mLoadingSrc = nullptr;
  mLoadingSrcTriggeringPrincipal = nullptr;
  DDLOG(DDLogCategory::Property, "loading_src", "");
  DDUNLINKCHILD(mMediaSource.get());
  mMediaSource = nullptr;

  if (mNetworkState == NETWORK_LOADING || mNetworkState == NETWORK_IDLE) {
    DispatchAsyncEvent(NS_LITERAL_STRING("abort"));
  }

  mErrorSink->ResetError();
  mCurrentPlayRangeStart = -1.0;
  mLoadedDataFired = false;
  mAutoplaying = true;
  mIsLoadingFromSourceChildren = false;
  mSuspendedAfterFirstFrame = false;
  mAllowSuspendAfterFirstFrame = true;
  mHaveQueuedSelectResource = false;
  mSuspendedForPreloadNone = false;
  mDownloadSuspendedByCache = false;
  mMediaInfo = MediaInfo();
  mIsEncrypted = false;
  mPendingEncryptedInitData.Reset();
  mWaitingForKey = NOT_WAITING_FOR_KEY;
  mSourcePointer = nullptr;
  mBlockedAsWithoutMetadata = false;

  mTags = nullptr;
  mAudioTrackSilenceStartedTime = 0.0;

  if (mNetworkState != NETWORK_EMPTY) {
    NS_ASSERTION(!mDecoder && !mSrcStream,
                 "How did someone setup a new stream/decoder already?");
    // ChangeNetworkState() will call UpdateAudioChannelPlayingState()
    // indirectly which depends on mPaused. So we need to update mPaused first.
    if (!mPaused) {
      mPaused = true;
      RejectPromises(TakePendingPlayPromises(), NS_ERROR_DOM_MEDIA_ABORT_ERR);
    }
    ChangeNetworkState(NETWORK_EMPTY);
    ChangeReadyState(HAVE_NOTHING);

    // TODO: Apply the rules for text track cue rendering Bug 865407
    if (mTextTrackManager) {
      mTextTrackManager->GetTextTracks()->SetCuesInactive();
    }

    if (fireTimeUpdate) {
      // Since we destroyed the decoder above, the current playback position
      // will now be reported as 0. The playback position was non-zero when
      // we destroyed the decoder, so fire a timeupdate event so that the
      // change will be reflected in the controls.
      FireTimeUpdate(false);
    }
    DispatchAsyncEvent(NS_LITERAL_STRING("emptied"));
    UpdateAudioChannelPlayingState();
  }

  // We may have changed mPaused, mAutoplaying, and other
  // things which can affect AddRemoveSelfReference
  AddRemoveSelfReference();

  mIsRunningSelectResource = false;

  mEventDeliveryPaused = false;
  mPendingEvents.Clear();
  mCurrentLoadPlayTime.Reset();

  AssertReadyStateIsNothing();
}

void HTMLMediaElement::NoSupportedMediaSourceError(
    const nsACString& aErrorDetails) {
  if (mDecoder) {
    ShutdownDecoder();
  }
  mErrorSink->SetError(MEDIA_ERR_SRC_NOT_SUPPORTED, aErrorDetails);
  ChangeDelayLoadStatus(false);
  UpdateAudioChannelPlayingState();
  RejectPromises(TakePendingPlayPromises(),
                 NS_ERROR_DOM_MEDIA_NOT_SUPPORTED_ERR);
}

typedef void (HTMLMediaElement::*SyncSectionFn)();

// Runs a "synchronous section", a function that must run once the event loop
// has reached a "stable state". See:
// http://www.whatwg.org/specs/web-apps/current-work/multipage/webappapis.html#synchronous-section
class nsSyncSection : public nsMediaEvent {
 private:
  nsCOMPtr<nsIRunnable> mRunnable;

 public:
  nsSyncSection(HTMLMediaElement* aElement, nsIRunnable* aRunnable)
      : nsMediaEvent("dom::nsSyncSection", aElement), mRunnable(aRunnable) {}

  NS_IMETHOD Run() override {
    // Silently cancel if our load has been cancelled.
    if (IsCancelled()) return NS_OK;
    mRunnable->Run();
    return NS_OK;
  }
};

void HTMLMediaElement::RunInStableState(nsIRunnable* aRunnable) {
  if (mShuttingDown) {
    return;
  }

  nsCOMPtr<nsIRunnable> event = new nsSyncSection(this, aRunnable);
  nsContentUtils::RunInStableState(event.forget());
}

void HTMLMediaElement::QueueLoadFromSourceTask() {
  if (!mIsLoadingFromSourceChildren || mShuttingDown) {
    return;
  }

  if (mDecoder) {
    // Reset readyState to HAVE_NOTHING since we're going to load a new decoder.
    ShutdownDecoder();
    ChangeReadyState(HAVE_NOTHING);
  }

  AssertReadyStateIsNothing();

  ChangeDelayLoadStatus(true);
  ChangeNetworkState(NETWORK_LOADING);
  RefPtr<Runnable> r =
      NewRunnableMethod("HTMLMediaElement::LoadFromSourceChildren", this,
                        &HTMLMediaElement::LoadFromSourceChildren);
  RunInStableState(r);
}

void HTMLMediaElement::QueueSelectResourceTask() {
  // Don't allow multiple async select resource calls to be queued.
  if (mHaveQueuedSelectResource) return;
  mHaveQueuedSelectResource = true;
  ChangeNetworkState(NETWORK_NO_SOURCE);
  RefPtr<Runnable> r =
      NewRunnableMethod("HTMLMediaElement::SelectResourceWrapper", this,
                        &HTMLMediaElement::SelectResourceWrapper);
  RunInStableState(r);
}

static bool HasSourceChildren(nsIContent* aElement) {
  for (nsIContent* child = aElement->GetFirstChild(); child;
       child = child->GetNextSibling()) {
    if (child->IsHTMLElement(nsGkAtoms::source)) {
      return true;
    }
  }
  return false;
}

static nsCString DocumentOrigin(Document* aDoc) {
  if (!aDoc) {
    return NS_LITERAL_CSTRING("null");
  }
  nsCOMPtr<nsIPrincipal> principal = aDoc->NodePrincipal();
  if (!principal) {
    return NS_LITERAL_CSTRING("null");
  }
  nsCString origin;
  if (NS_FAILED(principal->GetOrigin(origin))) {
    return NS_LITERAL_CSTRING("null");
  }
  return origin;
}

void HTMLMediaElement::Load() {
  LOG(LogLevel::Debug,
      ("%p Load() hasSrcAttrStream=%d hasSrcAttr=%d hasSourceChildren=%d "
       "handlingInput=%d hasAutoplayAttr=%d IsAllowedToPlay=%d "
       "ownerDoc=%p (%s) ownerDocUserActivated=%d "
       "muted=%d volume=%f",
       this, !!mSrcAttrStream, HasAttr(kNameSpaceID_None, nsGkAtoms::src),
       HasSourceChildren(this), EventStateManager::IsHandlingUserInput(),
       HasAttr(kNameSpaceID_None, nsGkAtoms::autoplay),
       AutoplayPolicy::IsAllowedToPlay(*this), OwnerDoc(),
       DocumentOrigin(OwnerDoc()).get(),
       OwnerDoc() ? OwnerDoc()->HasBeenUserGestureActivated() : 0, mMuted,
       mVolume));

  if (mIsRunningLoadMethod) {
    return;
  }

  mIsDoingExplicitLoad = true;
  DoLoad();
}

void HTMLMediaElement::DoLoad() {
  // Check if media is allowed for the docshell.
  nsCOMPtr<nsIDocShell> docShell = OwnerDoc()->GetDocShell();
  if (docShell && !docShell->GetAllowMedia()) {
    LOG(LogLevel::Debug, ("%p Media not allowed", this));
    return;
  }

  if (mIsRunningLoadMethod) {
    return;
  }

  if (EventStateManager::IsHandlingUserInput()) {
    // Detect if user has interacted with element so that play will not be
    // blocked when initiated by a script. This enables sites to capture user
    // intent to play by calling load() in the click handler of a "catalog
    // view" of a gallery of videos.
    mIsBlessed = true;
    // Mark the channel as urgent-start when autopaly so that it will play the
    // media from src after loading enough resource.
    if (HasAttr(kNameSpaceID_None, nsGkAtoms::autoplay)) {
      mUseUrgentStartForChannel = true;
    }
  }

  SetPlayedOrSeeked(false);
  mIsRunningLoadMethod = true;
  AbortExistingLoads();
  SetPlaybackRate(mDefaultPlaybackRate, IgnoreErrors());
  QueueSelectResourceTask();
  ResetState();
  mIsRunningLoadMethod = false;
}

void HTMLMediaElement::ResetState() {
  // There might be a pending MediaDecoder::PlaybackPositionChanged() which
  // will overwrite |mMediaInfo.mVideo.mDisplay| in UpdateMediaSize() to give
  // staled videoWidth and videoHeight. We have to call ForgetElement() here
  // such that the staled callbacks won't reach us.
  if (mVideoFrameContainer) {
    mVideoFrameContainer->ForgetElement();
    mVideoFrameContainer = nullptr;
  }
}

void HTMLMediaElement::SelectResourceWrapper() {
  SelectResource();
  MaybeBeginCloningVisually();
  mIsRunningSelectResource = false;
  mHaveQueuedSelectResource = false;
  mIsDoingExplicitLoad = false;
}

void HTMLMediaElement::SelectResource() {
  if (!mSrcAttrStream && !HasAttr(kNameSpaceID_None, nsGkAtoms::src) &&
      !HasSourceChildren(this)) {
    // The media element has neither a src attribute nor any source
    // element children, abort the load.
    ChangeNetworkState(NETWORK_EMPTY);
    ChangeDelayLoadStatus(false);
    return;
  }

  ChangeDelayLoadStatus(true);

  ChangeNetworkState(NETWORK_LOADING);
  DispatchAsyncEvent(NS_LITERAL_STRING("loadstart"));

  // Delay setting mIsRunningSeletResource until after UpdatePreloadAction
  // so that we don't lose our state change by bailing out of the preload
  // state update
  UpdatePreloadAction();
  mIsRunningSelectResource = true;

  // If we have a 'src' attribute, use that exclusively.
  nsAutoString src;
  if (mSrcAttrStream) {
    SetupSrcMediaStreamPlayback(mSrcAttrStream);
  } else if (GetAttr(kNameSpaceID_None, nsGkAtoms::src, src)) {
    nsCOMPtr<nsIURI> uri;
    MediaResult rv = NewURIFromString(src, getter_AddRefs(uri));
    if (NS_SUCCEEDED(rv)) {
      LOG(LogLevel::Debug, ("%p Trying load from src=%s", this,
                            NS_ConvertUTF16toUTF8(src).get()));
      NS_ASSERTION(
          !mIsLoadingFromSourceChildren,
          "Should think we're not loading from source children by default");

      RemoveMediaElementFromURITable();
      mLoadingSrc = uri;
      mLoadingSrcTriggeringPrincipal = mSrcAttrTriggeringPrincipal;
      DDLOG(DDLogCategory::Property, "loading_src",
            nsCString(NS_ConvertUTF16toUTF8(src)));
      mMediaSource = mSrcMediaSource;
      DDLINKCHILD("mediasource", mMediaSource.get());
      UpdatePreloadAction();
      if (mPreloadAction == HTMLMediaElement::PRELOAD_NONE && !mMediaSource) {
        // preload:none media, suspend the load here before we make any
        // network requests.
        SuspendLoad();
        return;
      }

      rv = LoadResource();
      if (NS_SUCCEEDED(rv)) {
        return;
      }
    } else {
      const char16_t* params[] = {src.get()};
      ReportLoadError("MediaLoadInvalidURI", params, ArrayLength(params));
      rv = MediaResult(rv.Code(), "MediaLoadInvalidURI");
    }
    // The media element has neither a src attribute nor a source element child:
    // set the networkState to NETWORK_EMPTY, and abort these steps; the
    // synchronous section ends.
    mMainThreadEventTarget->Dispatch(NewRunnableMethod<nsCString>(
        "HTMLMediaElement::NoSupportedMediaSourceError", this,
        &HTMLMediaElement::NoSupportedMediaSourceError, rv.Description()));
  } else {
    // Otherwise, the source elements will be used.
    mIsLoadingFromSourceChildren = true;
    LoadFromSourceChildren();
  }
}

void HTMLMediaElement::NotifyLoadError(const nsACString& aErrorDetails) {
  if (!mIsLoadingFromSourceChildren) {
    LOG(LogLevel::Debug, ("NotifyLoadError(), no supported media error"));
    NoSupportedMediaSourceError(aErrorDetails);
  } else if (mSourceLoadCandidate) {
    DispatchAsyncSourceError(mSourceLoadCandidate);
    QueueLoadFromSourceTask();
  } else {
    NS_WARNING("Should know the source we were loading from!");
  }
}

void HTMLMediaElement::NotifyMediaTrackEnabled(MediaTrack* aTrack) {
  MOZ_ASSERT(aTrack);
  if (!aTrack) {
    return;
  }
#ifdef DEBUG
  nsString id;
  aTrack->GetId(id);

  LOG(LogLevel::Debug, ("MediaElement %p %sTrack with id %s enabled", this,
                        aTrack->AsAudioTrack() ? "Audio" : "Video",
                        NS_ConvertUTF16toUTF8(id).get()));
#endif

  MOZ_ASSERT((aTrack->AsAudioTrack() && aTrack->AsAudioTrack()->Enabled()) ||
             (aTrack->AsVideoTrack() && aTrack->AsVideoTrack()->Selected()));

  if (aTrack->AsAudioTrack()) {
    SetMutedInternal(mMuted & ~MUTED_BY_AUDIO_TRACK);
  } else if (aTrack->AsVideoTrack()) {
    if (!IsVideo()) {
      MOZ_ASSERT(false);
      return;
    }
    mDisableVideo = false;
  } else {
    MOZ_ASSERT(false, "Unknown track type");
  }

  if (mSrcStream) {
    if (aTrack->AsVideoTrack()) {
      MOZ_ASSERT(!mSelectedVideoStreamTrack);
      MOZ_ASSERT(!mVideoFrameListener);

      mSelectedVideoStreamTrack = aTrack->AsVideoTrack()->GetVideoStreamTrack();
      VideoFrameContainer* container = GetVideoFrameContainer();
      if (mSrcStreamIsPlaying && container) {
        mSelectedVideoStreamTrack->AddVideoOutput(container);
        MaybeBeginCloningVisually();
      }
      HTMLVideoElement* self = static_cast<HTMLVideoElement*>(this);
      if (self->VideoWidth() <= 1 && self->VideoHeight() <= 1) {
        // MediaInfo uses dummy values of 1 for width and height to
        // mark video as valid. We need a new stream size listener
        // if size is 0x0 or 1x1.
        mVideoFrameListener = new VideoFrameListener(this);
        mSelectedVideoStreamTrack->AddDirectListener(mVideoFrameListener);
      }
    }

    if (mReadyState == HAVE_NOTHING) {
      // No MediaStreamTracks are captured until we have metadata.
      return;
    }
    for (OutputMediaStream& ms : mOutputStreams) {
      if (aTrack->AsVideoTrack() && ms.mCapturingAudioOnly) {
        // If the output stream is for audio only we ignore video tracks.
        continue;
      }
      AddCaptureMediaTrackToOutputStream(aTrack, ms);
    }
  }
}

void HTMLMediaElement::NotifyMediaTrackDisabled(MediaTrack* aTrack) {
  MOZ_ASSERT(aTrack);
  if (!aTrack) {
    return;
  }
#ifdef DEBUG
  nsString id;
  aTrack->GetId(id);

  LOG(LogLevel::Debug, ("MediaElement %p %sTrack with id %s disabled", this,
                        aTrack->AsAudioTrack() ? "Audio" : "Video",
                        NS_ConvertUTF16toUTF8(id).get()));
#endif

  MOZ_ASSERT((!aTrack->AsAudioTrack() || !aTrack->AsAudioTrack()->Enabled()) &&
             (!aTrack->AsVideoTrack() || !aTrack->AsVideoTrack()->Selected()));

  if (aTrack->AsAudioTrack()) {
    // If we don't have any alive track , we don't need to mute MediaElement.
    MOZ_DIAGNOSTIC_ASSERT(AudioTracks(), "Element can't have been unlinked");
    if (AudioTracks()->Length() > 0) {
      bool shouldMute = true;
      for (uint32_t i = 0; i < AudioTracks()->Length(); ++i) {
        if ((*AudioTracks())[i]->Enabled()) {
          shouldMute = false;
          break;
        }
      }

      if (shouldMute) {
        SetMutedInternal(mMuted | MUTED_BY_AUDIO_TRACK);
      }
    }
  } else if (aTrack->AsVideoTrack()) {
    if (mSrcStream) {
      MOZ_ASSERT(mSelectedVideoStreamTrack);
      if (mSelectedVideoStreamTrack && mVideoFrameListener) {
        mSelectedVideoStreamTrack->RemoveDirectListener(mVideoFrameListener);
        mVideoFrameListener->Forget();
        mVideoFrameListener = nullptr;
      }
      VideoFrameContainer* container = GetVideoFrameContainer();
      if (mSrcStreamIsPlaying && container) {
        mSelectedVideoStreamTrack->RemoveVideoOutput(container);
      }
      mSelectedVideoStreamTrack = nullptr;
    }
  }

  if (mReadyState == HAVE_NOTHING) {
    // No MediaStreamTracks are captured until we have metadata, and code
    // below doesn't do anything for captured decoders.
    return;
  }

  for (OutputMediaStream& ms : mOutputStreams) {
    if (ms.mCapturingDecoder) {
      MOZ_ASSERT(!ms.mCapturingMediaStream);
      continue;
    }
    MOZ_ASSERT(ms.mCapturingMediaStream);
    for (int32_t i = ms.mTrackPorts.Length() - 1; i >= 0; --i) {
      if (ms.mTrackPorts[i].first() == aTrack->GetId()) {
        // The source of this track just ended. Force-notify that it ended.
        // If we bounce it to the MediaStreamGraph it might not be picked up,
        // for instance if the MediaInputPort was destroyed in the same
        // iteration as it was added.
        MediaStreamTrack* outputTrack = ms.mStream->FindOwnedDOMTrack(
            ms.mTrackPorts[i].second()->GetDestination(),
            ms.mTrackPorts[i].second()->GetDestinationTrackId());
        MOZ_ASSERT(outputTrack);
        if (outputTrack) {
          mMainThreadEventTarget->Dispatch(
              NewRunnableMethod("MediaStreamTrack::OverrideEnded", outputTrack,
                                &MediaStreamTrack::OverrideEnded));
        }

        ms.mTrackPorts[i].second()->Destroy();
        ms.mTrackPorts.RemoveElementAt(i);
        break;
      }
    }
#ifdef DEBUG
    for (auto pair : ms.mTrackPorts) {
      MOZ_ASSERT(pair.first() != aTrack->GetId(),
                 "The same MediaTrack was forwarded to the output stream more "
                 "than once. This shouldn't happen.");
    }
#endif
  }
}

void HTMLMediaElement::DealWithFailedElement(nsIContent* aSourceElement) {
  if (mShuttingDown) {
    return;
  }

  DispatchAsyncSourceError(aSourceElement);
  mMainThreadEventTarget->Dispatch(
      NewRunnableMethod("HTMLMediaElement::QueueLoadFromSourceTask", this,
                        &HTMLMediaElement::QueueLoadFromSourceTask));
}

void HTMLMediaElement::NotifyOutputTrackStopped(DOMMediaStream* aOwningStream,
                                                TrackID aDestinationTrackID) {
  for (OutputMediaStream& ms : mOutputStreams) {
    if (!ms.mCapturingMediaStream) {
      continue;
    }

    if (ms.mStream != aOwningStream) {
      continue;
    }

    for (int32_t i = ms.mTrackPorts.Length() - 1; i >= 0; --i) {
      MediaInputPort* port = ms.mTrackPorts[i].second();
      if (port->GetDestinationTrackId() != aDestinationTrackID) {
        continue;
      }

      port->Destroy();
      ms.mTrackPorts.RemoveElementAt(i);
      return;
    }
  }

  // An output track ended but its port is already gone.
  // It was probably cleared by the removal of the source MediaTrack.
}

void HTMLMediaElement::LoadFromSourceChildren() {
  NS_ASSERTION(mDelayingLoadEvent,
               "Should delay load event (if in document) during load");
  NS_ASSERTION(mIsLoadingFromSourceChildren,
               "Must remember we're loading from source children");

  AddMutationObserverUnlessExists(this);

  while (true) {
    Element* child = GetNextSource();
    if (!child) {
      // Exhausted candidates, wait for more candidates to be appended to
      // the media element.
      mLoadWaitStatus = WAITING_FOR_SOURCE;
      ChangeNetworkState(NETWORK_NO_SOURCE);
      ChangeDelayLoadStatus(false);
      ReportLoadError("MediaLoadExhaustedCandidates");
      return;
    }

    // Must have src attribute.
    nsAutoString src;
    if (!child->GetAttr(kNameSpaceID_None, nsGkAtoms::src, src)) {
      ReportLoadError("MediaLoadSourceMissingSrc");
      DealWithFailedElement(child);
      return;
    }

    // If we have a type attribute, it must be a supported type.
    nsAutoString type;
    if (child->GetAttr(kNameSpaceID_None, nsGkAtoms::type, type)) {
      DecoderDoctorDiagnostics diagnostics;
      CanPlayStatus canPlay = GetCanPlay(type, &diagnostics);
      diagnostics.StoreFormatDiagnostics(OwnerDoc(), type,
                                         canPlay != CANPLAY_NO, __func__);
      if (canPlay == CANPLAY_NO) {
        const char16_t* params[] = {type.get(), src.get()};
        ReportLoadError("MediaLoadUnsupportedTypeAttribute", params,
                        ArrayLength(params));
        DealWithFailedElement(child);
        return;
      }
    }
    HTMLSourceElement* childSrc = HTMLSourceElement::FromNode(child);
    LOG(LogLevel::Debug,
        ("%p Trying load from <source>=%s type=%s", this,
         NS_ConvertUTF16toUTF8(src).get(), NS_ConvertUTF16toUTF8(type).get()));

    nsCOMPtr<nsIURI> uri;
    NewURIFromString(src, getter_AddRefs(uri));
    if (!uri) {
      const char16_t* params[] = {src.get()};
      ReportLoadError("MediaLoadInvalidURI", params, ArrayLength(params));
      DealWithFailedElement(child);
      return;
    }

    RemoveMediaElementFromURITable();
    mLoadingSrc = uri;
    mLoadingSrcTriggeringPrincipal = childSrc->GetSrcTriggeringPrincipal();
    DDLOG(DDLogCategory::Property, "loading_src",
          nsCString(NS_ConvertUTF16toUTF8(src)));
    mMediaSource = childSrc->GetSrcMediaSource();
    DDLINKCHILD("mediasource", mMediaSource.get());
    NS_ASSERTION(mNetworkState == NETWORK_LOADING,
                 "Network state should be loading");

    if (mPreloadAction == HTMLMediaElement::PRELOAD_NONE && !mMediaSource) {
      // preload:none media, suspend the load here before we make any
      // network requests.
      SuspendLoad();
      return;
    }

    if (NS_SUCCEEDED(LoadResource())) {
      return;
    }

    // If we fail to load, loop back and try loading the next resource.
    DispatchAsyncSourceError(child);
  }
  MOZ_ASSERT_UNREACHABLE("Execution should not reach here!");
}

void HTMLMediaElement::SuspendLoad() {
  mSuspendedForPreloadNone = true;
  ChangeNetworkState(NETWORK_IDLE);
  ChangeDelayLoadStatus(false);
}

void HTMLMediaElement::ResumeLoad(PreloadAction aAction) {
  NS_ASSERTION(mSuspendedForPreloadNone,
               "Must be halted for preload:none to resume from preload:none "
               "suspended load.");
  mSuspendedForPreloadNone = false;
  mPreloadAction = aAction;
  ChangeDelayLoadStatus(true);
  ChangeNetworkState(NETWORK_LOADING);
  if (!mIsLoadingFromSourceChildren) {
    // We were loading from the element's src attribute.
    MediaResult rv = LoadResource();
    if (NS_FAILED(rv)) {
      NoSupportedMediaSourceError(rv.Description());
    }
  } else {
    // We were loading from a child <source> element. Try to resume the
    // load of that child, and if that fails, try the next child.
    if (NS_FAILED(LoadResource())) {
      LoadFromSourceChildren();
    }
  }
}

bool HTMLMediaElement::AllowedToPlay() const {
  return AutoplayPolicy::IsAllowedToPlay(*this);
}

uint32_t HTMLMediaElement::GetPreloadDefault() const {
  if (mMediaSource) {
    return HTMLMediaElement::PRELOAD_ATTR_METADATA;
  }
  if (OnCellularConnection()) {
    return Preferences::GetInt("media.preload.default.cellular",
                               HTMLMediaElement::PRELOAD_ATTR_NONE);
  }
  return Preferences::GetInt("media.preload.default",
                             HTMLMediaElement::PRELOAD_ATTR_METADATA);
}

uint32_t HTMLMediaElement::GetPreloadDefaultAuto() const {
  if (OnCellularConnection()) {
    return Preferences::GetInt("media.preload.auto.cellular",
                               HTMLMediaElement::PRELOAD_ATTR_METADATA);
  }
  return Preferences::GetInt("media.preload.auto",
                             HTMLMediaElement::PRELOAD_ENOUGH);
}

void HTMLMediaElement::UpdatePreloadAction() {
  PreloadAction nextAction = PRELOAD_UNDEFINED;
  // If autoplay is set, or we're playing, we should always preload data,
  // as we'll need it to play.
  if ((AutoplayPolicy::IsAllowedToPlay(*this) &&
       HasAttr(kNameSpaceID_None, nsGkAtoms::autoplay)) ||
      !mPaused) {
    nextAction = HTMLMediaElement::PRELOAD_ENOUGH;
  } else {
    // Find the appropriate preload action by looking at the attribute.
    const nsAttrValue* val =
        mAttrs.GetAttr(nsGkAtoms::preload, kNameSpaceID_None);
    // MSE doesn't work if preload is none, so it ignores the pref when src is
    // from MSE.
    uint32_t preloadDefault = GetPreloadDefault();
    uint32_t preloadAuto = GetPreloadDefaultAuto();
    if (!val) {
      // Attribute is not set. Use the preload action specified by the
      // media.preload.default pref, or just preload metadata if not present.
      nextAction = static_cast<PreloadAction>(preloadDefault);
    } else if (val->Type() == nsAttrValue::eEnum) {
      PreloadAttrValue attr =
          static_cast<PreloadAttrValue>(val->GetEnumValue());
      if (attr == HTMLMediaElement::PRELOAD_ATTR_EMPTY ||
          attr == HTMLMediaElement::PRELOAD_ATTR_AUTO) {
        nextAction = static_cast<PreloadAction>(preloadAuto);
      } else if (attr == HTMLMediaElement::PRELOAD_ATTR_METADATA) {
        nextAction = HTMLMediaElement::PRELOAD_METADATA;
      } else if (attr == HTMLMediaElement::PRELOAD_ATTR_NONE) {
        nextAction = HTMLMediaElement::PRELOAD_NONE;
      }
    } else {
      // Use the suggested "missing value default" of "metadata", or the value
      // specified by the media.preload.default, if present.
      nextAction = static_cast<PreloadAction>(preloadDefault);
    }
  }

  if (nextAction == HTMLMediaElement::PRELOAD_NONE && mIsDoingExplicitLoad) {
    nextAction = HTMLMediaElement::PRELOAD_METADATA;
  }

  mPreloadAction = nextAction;

  if (nextAction == HTMLMediaElement::PRELOAD_ENOUGH) {
    if (mSuspendedForPreloadNone) {
      // Our load was previouly suspended due to the media having preload
      // value "none". The preload value has changed to preload:auto, so
      // resume the load.
      ResumeLoad(PRELOAD_ENOUGH);
    } else {
      // Preload as much of the video as we can, i.e. don't suspend after
      // the first frame.
      StopSuspendingAfterFirstFrame();
    }

  } else if (nextAction == HTMLMediaElement::PRELOAD_METADATA) {
    // Ensure that the video can be suspended after first frame.
    mAllowSuspendAfterFirstFrame = true;
    if (mSuspendedForPreloadNone) {
      // Our load was previouly suspended due to the media having preload
      // value "none". The preload value has changed to preload:metadata, so
      // resume the load. We'll pause the load again after we've read the
      // metadata.
      ResumeLoad(PRELOAD_METADATA);
    }
  }
}

MediaResult HTMLMediaElement::LoadResource() {
  AbstractThread::AutoEnter context(AbstractMainThread());

  NS_ASSERTION(mDelayingLoadEvent,
               "Should delay load event (if in document) during load");

  if (mChannelLoader) {
    mChannelLoader->Cancel();
    mChannelLoader = nullptr;
  }

  // Set the media element's CORS mode only when loading a resource
  mCORSMode = AttrValueToCORSMode(GetParsedAttr(nsGkAtoms::crossorigin));

  HTMLMediaElement* other = LookupMediaElementURITable(mLoadingSrc);
  if (other && other->mDecoder) {
    // Clone it.
    // TODO: remove the cast by storing ChannelMediaDecoder in the URI table.
    nsresult rv = InitializeDecoderAsClone(
        static_cast<ChannelMediaDecoder*>(other->mDecoder.get()));
    if (NS_SUCCEEDED(rv)) return rv;
  }

  if (mMediaSource) {
    MediaDecoderInit decoderInit(
        this, mMuted ? 0.0 : mVolume, mPreservesPitch,
        ClampPlaybackRate(mPlaybackRate),
        mPreloadAction == HTMLMediaElement::PRELOAD_METADATA, mHasSuspendTaint,
        HasAttr(kNameSpaceID_None, nsGkAtoms::loop),
        MediaContainerType(MEDIAMIMETYPE("application/x.mediasource")));

    RefPtr<MediaSourceDecoder> decoder = new MediaSourceDecoder(decoderInit);
    if (!mMediaSource->Attach(decoder)) {
      // TODO: Handle failure: run "If the media data cannot be fetched at
      // all, due to network errors, causing the user agent to give up
      // trying to fetch the resource" section of resource fetch algorithm.
      decoder->Shutdown();
      return MediaResult(NS_ERROR_FAILURE, "Failed to attach MediaSource");
    }
    ChangeDelayLoadStatus(false);
    nsresult rv = decoder->Load(mMediaSource->GetPrincipal());
    if (NS_FAILED(rv)) {
      decoder->Shutdown();
      LOG(LogLevel::Debug,
          ("%p Failed to load for decoder %p", this, decoder.get()));
      return MediaResult(rv, "Fail to load decoder");
    }
    rv = FinishDecoderSetup(decoder);
    return MediaResult(rv, "Failed to set up decoder");
  }

  AssertReadyStateIsNothing();

  RefPtr<ChannelLoader> loader = new ChannelLoader;
  nsresult rv = loader->Load(this);
  if (NS_SUCCEEDED(rv)) {
    mChannelLoader = loader.forget();
  }
  return MediaResult(rv, "Failed to load channel");
}

nsresult HTMLMediaElement::LoadWithChannel(nsIChannel* aChannel,
                                           nsIStreamListener** aListener) {
  NS_ENSURE_ARG_POINTER(aChannel);
  NS_ENSURE_ARG_POINTER(aListener);

  *aListener = nullptr;

  // Make sure we don't reenter during synchronous abort events.
  if (mIsRunningLoadMethod) return NS_OK;
  mIsRunningLoadMethod = true;
  AbortExistingLoads();
  mIsRunningLoadMethod = false;

  mLoadingSrcTriggeringPrincipal = nullptr;
  nsresult rv = aChannel->GetOriginalURI(getter_AddRefs(mLoadingSrc));
  NS_ENSURE_SUCCESS(rv, rv);

  ChangeDelayLoadStatus(true);
  rv = InitializeDecoderForChannel(aChannel, aListener);
  if (NS_FAILED(rv)) {
    ChangeDelayLoadStatus(false);
    return rv;
  }

  SetPlaybackRate(mDefaultPlaybackRate, IgnoreErrors());
  DispatchAsyncEvent(NS_LITERAL_STRING("loadstart"));

  return NS_OK;
}

bool HTMLMediaElement::Seeking() const {
  return mDecoder && mDecoder->IsSeeking();
}

double HTMLMediaElement::CurrentTime() const {
  if (MediaStream* stream = GetSrcMediaStream()) {
    MediaStreamGraph* graph = stream->Graph();
    GraphTime currentGraphTime =
        mSrcStreamPausedGraphTime.valueOr(graph->CurrentTime());
    StreamTime currentStreamTime = currentGraphTime - mSrcStreamGraphTimeOffset;
    return stream->StreamTimeToSeconds(currentStreamTime);
  }

  if (mDefaultPlaybackStartPosition == 0.0 && mDecoder) {
    return mDecoder->GetCurrentTime();
  }

  return mDefaultPlaybackStartPosition;
}

void HTMLMediaElement::FastSeek(double aTime, ErrorResult& aRv) {
  LOG(LogLevel::Debug, ("%p FastSeek(%f) called by JS", this, aTime));
  LOG(LogLevel::Debug, ("Reporting telemetry VIDEO_FASTSEEK_USED"));
  Telemetry::Accumulate(Telemetry::VIDEO_FASTSEEK_USED, 1);
  RefPtr<Promise> tobeDropped =
      Seek(aTime, SeekTarget::PrevSyncPoint, IgnoreErrors());
}

already_AddRefed<Promise> HTMLMediaElement::SeekToNextFrame(ErrorResult& aRv) {
  /* This will cause JIT code to be kept around longer, to help performance
   * when using SeekToNextFrame to iterate through every frame of a video.
   */
  nsPIDOMWindowInner* win = OwnerDoc()->GetInnerWindow();

  if (win) {
    if (JSObject* obj = win->AsGlobal()->GetGlobalJSObject()) {
      js::NotifyAnimationActivity(obj);
    }
  }

  return Seek(CurrentTime(), SeekTarget::NextFrame, aRv);
}

void HTMLMediaElement::SetCurrentTime(double aCurrentTime, ErrorResult& aRv) {
  LOG(LogLevel::Debug,
      ("%p SetCurrentTime(%f) called by JS", this, aCurrentTime));
  RefPtr<Promise> tobeDropped =
      Seek(aCurrentTime, SeekTarget::Accurate, IgnoreErrors());
}

/**
 * Check if aValue is inside a range of aRanges, and if so returns true
 * and puts the range index in aIntervalIndex. If aValue is not
 * inside a range, returns false, and aIntervalIndex
 * is set to the index of the range which starts immediately after aValue
 * (and can be aRanges.Length() if aValue is after the last range).
 */
static bool IsInRanges(TimeRanges& aRanges, double aValue,
                       uint32_t& aIntervalIndex) {
  uint32_t length = aRanges.Length();

  for (uint32_t i = 0; i < length; i++) {
    double start = aRanges.Start(i);
    if (start > aValue) {
      aIntervalIndex = i;
      return false;
    }
    double end = aRanges.End(i);
    if (aValue <= end) {
      aIntervalIndex = i;
      return true;
    }
  }
  aIntervalIndex = length;
  return false;
}

already_AddRefed<Promise> HTMLMediaElement::Seek(double aTime,
                                                 SeekTarget::Type aSeekType,
                                                 ErrorResult& aRv) {
  // Note: Seek is called both by synchronous code that expects errors thrown in
  // aRv, as well as asynchronous code that expects a promise. Make sure all
  // synchronous errors are returned using aRv, not promise rejections.

  // aTime should be non-NaN.
  MOZ_ASSERT(!mozilla::IsNaN(aTime));

  // Seeking step1, Set the media element's show poster flag to false.
  // https://html.spec.whatwg.org/multipage/media.html#dom-media-seek
  mShowPoster = false;

  RefPtr<Promise> promise = CreateDOMPromise(aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  // Detect if user has interacted with element by seeking so that
  // play will not be blocked when initiated by a script.
  if (EventStateManager::IsHandlingUserInput()) {
    mIsBlessed = true;
  }

  StopSuspendingAfterFirstFrame();

  if (mSrcStream) {
    // do nothing since media streams have an empty Seekable range.
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }

  if (mPlayed && mCurrentPlayRangeStart != -1.0) {
    double rangeEndTime = CurrentTime();
    LOG(LogLevel::Debug, ("%p Adding \'played\' a range : [%f, %f]", this,
                          mCurrentPlayRangeStart, rangeEndTime));
    // Multiple seek without playing, or seek while playing.
    if (mCurrentPlayRangeStart != rangeEndTime) {
      mPlayed->Add(mCurrentPlayRangeStart, rangeEndTime);
    }
    // Reset the current played range start time. We'll re-set it once
    // the seek completes.
    mCurrentPlayRangeStart = -1.0;
  }

  if (mReadyState == HAVE_NOTHING) {
    mDefaultPlaybackStartPosition = aTime;
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }

  if (!mDecoder) {
    // mDecoder must always be set in order to reach this point.
    NS_ASSERTION(mDecoder, "SetCurrentTime failed: no decoder");
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }

  // Clamp the seek target to inside the seekable ranges.
  media::TimeIntervals seekableIntervals = mDecoder->GetSeekable();
  if (seekableIntervals.IsInvalid()) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }
  RefPtr<TimeRanges> seekable =
      new TimeRanges(ToSupports(OwnerDoc()), seekableIntervals);
  uint32_t length = seekable->Length();
  if (length == 0) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }

  // If the position we want to seek to is not in a seekable range, we seek
  // to the closest position in the seekable ranges instead. If two positions
  // are equally close, we seek to the closest position from the currentTime.
  // See seeking spec, point 7 :
  // http://www.whatwg.org/specs/web-apps/current-work/multipage/the-video-element.html#seeking
  uint32_t range = 0;
  bool isInRange = IsInRanges(*seekable, aTime, range);
  if (!isInRange) {
    if (range == 0) {
      // aTime is before the first range in |seekable|, the closest point we can
      // seek to is the start of the first range.
      aTime = seekable->Start(0);
    } else if (range == length) {
      // Seek target is after the end last range in seekable data.
      // Clamp the seek target to the end of the last seekable range.
      aTime = seekable->End(length - 1);
    } else {
      double leftBound = seekable->End(range - 1);
      double rightBound = seekable->Start(range);
      double distanceLeft = Abs(leftBound - aTime);
      double distanceRight = Abs(rightBound - aTime);
      if (distanceLeft == distanceRight) {
        double currentTime = CurrentTime();
        distanceLeft = Abs(leftBound - currentTime);
        distanceRight = Abs(rightBound - currentTime);
      }
      aTime = (distanceLeft < distanceRight) ? leftBound : rightBound;
    }
  }

  // TODO: The spec requires us to update the current time to reflect the
  //       actual seek target before beginning the synchronous section, but
  //       that requires changing all MediaDecoderReaders to support telling
  //       us the fastSeek target, and it's currently not possible to get
  //       this information as we don't yet control the demuxer for all
  //       MediaDecoderReaders.

  mPlayingBeforeSeek = IsPotentiallyPlaying();

  // If the audio track is silent before seeking, we should end current silence
  // range and start a new range after seeking. Since seek() could be called
  // multiple times before seekEnd() executed, we should only calculate silence
  // range when first time seek() called. Calculating on other seek() calls
  // would cause a wrong result. In order to get correct time, this checking
  // should be called before decoder->seek().
  if (IsAudioTrackCurrentlySilent() &&
      !mHasAccumulatedSilenceRangeBeforeSeekEnd) {
    AccumulateAudioTrackSilence();
    mHasAccumulatedSilenceRangeBeforeSeekEnd = true;
  }

  // The media backend is responsible for dispatching the timeupdate
  // event if it changes the playback position as a result of the seek.
  LOG(LogLevel::Debug, ("%p SetCurrentTime(%f) starting seek", this, aTime));
  mDecoder->Seek(aTime, aSeekType);

  // We changed whether we're seeking so we need to AddRemoveSelfReference.
  AddRemoveSelfReference();

  // Keep the DOM promise.
  mSeekDOMPromise = promise;

  return promise.forget();
}

double HTMLMediaElement::Duration() const {
  if (mSrcStream) {
    return std::numeric_limits<double>::infinity();
  }

  if (mDecoder) {
    return mDecoder->GetDuration();
  }

  return std::numeric_limits<double>::quiet_NaN();
}

already_AddRefed<TimeRanges> HTMLMediaElement::Seekable() const {
  media::TimeIntervals seekable =
      mDecoder ? mDecoder->GetSeekable() : media::TimeIntervals();
  RefPtr<TimeRanges> ranges = new TimeRanges(ToSupports(OwnerDoc()), seekable);
  return ranges.forget();
}

already_AddRefed<TimeRanges> HTMLMediaElement::Played() {
  RefPtr<TimeRanges> ranges = new TimeRanges(ToSupports(OwnerDoc()));

  uint32_t timeRangeCount = 0;
  if (mPlayed) {
    timeRangeCount = mPlayed->Length();
  }
  for (uint32_t i = 0; i < timeRangeCount; i++) {
    double begin = mPlayed->Start(i);
    double end = mPlayed->End(i);
    ranges->Add(begin, end);
  }

  if (mCurrentPlayRangeStart != -1.0) {
    double now = CurrentTime();
    if (mCurrentPlayRangeStart != now) {
      ranges->Add(mCurrentPlayRangeStart, now);
    }
  }

  ranges->Normalize();
  return ranges.forget();
}

void HTMLMediaElement::Pause(ErrorResult& aRv) {
  LOG(LogLevel::Debug, ("%p Pause() called by JS", this));
  if (mNetworkState == NETWORK_EMPTY) {
    LOG(LogLevel::Debug, ("Loading due to Pause()"));
    DoLoad();
  } else if (mDecoder) {
    mDecoder->Pause();
  }

  bool oldPaused = mPaused;
  mPaused = true;
  mAutoplaying = false;
  // We changed mPaused and mAutoplaying which can affect AddRemoveSelfReference
  AddRemoveSelfReference();
  UpdateSrcMediaStreamPlaying();
  if (mAudioChannelWrapper) {
    mAudioChannelWrapper->NotifyPlayStateChanged();
  }

  if (!oldPaused) {
    FireTimeUpdate(false);
    DispatchAsyncEvent(NS_LITERAL_STRING("pause"));
    AsyncRejectPendingPlayPromises(NS_ERROR_DOM_MEDIA_ABORT_ERR);
  }
}

void HTMLMediaElement::SetVolume(double aVolume, ErrorResult& aRv) {
  LOG(LogLevel::Debug, ("%p SetVolume(%f) called by JS", this, aVolume));

  if (aVolume < 0.0 || aVolume > 1.0) {
    aRv.Throw(NS_ERROR_DOM_INDEX_SIZE_ERR);
    return;
  }

  if (aVolume == mVolume) return;

  mVolume = aVolume;

  // Here we want just to update the volume.
  SetVolumeInternal();

  DispatchAsyncEvent(NS_LITERAL_STRING("volumechange"));

  // We allow inaudible autoplay. But changing our volume may make this
  // media audible. So pause if we are no longer supposed to be autoplaying.
  PauseIfShouldNotBePlaying();
}

void HTMLMediaElement::MozGetMetadata(JSContext* cx,
                                      JS::MutableHandle<JSObject*> aRetval,
                                      ErrorResult& aRv) {
  if (mReadyState < HAVE_METADATA) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return;
  }

  JS::Rooted<JSObject*> tags(cx, JS_NewPlainObject(cx));
  if (!tags) {
    aRv.Throw(NS_ERROR_FAILURE);
    return;
  }
  if (mTags) {
    for (auto iter = mTags->ConstIter(); !iter.Done(); iter.Next()) {
      nsString wideValue;
      CopyUTF8toUTF16(iter.UserData(), wideValue);
      JS::Rooted<JSString*> string(cx,
                                   JS_NewUCStringCopyZ(cx, wideValue.Data()));
      if (!string || !JS_DefineProperty(cx, tags, iter.Key().Data(), string,
                                        JSPROP_ENUMERATE)) {
        NS_WARNING("couldn't create metadata object!");
        aRv.Throw(NS_ERROR_FAILURE);
        return;
      }
    }
  }

  aRetval.set(tags);
}

void HTMLMediaElement::SetMutedInternal(uint32_t aMuted) {
  uint32_t oldMuted = mMuted;
  mMuted = aMuted;

  if (!!aMuted == !!oldMuted) {
    return;
  }

  SetVolumeInternal();
}

void HTMLMediaElement::PauseIfShouldNotBePlaying() {
  if (GetPaused()) {
    return;
  }
  if (!AutoplayPolicy::IsAllowedToPlay(*this)) {
    AUTOPLAY_LOG("pause because not allowed to play, element=%p", this);
    ErrorResult rv;
    Pause(rv);
    OwnerDoc()->SetDocTreeHadPlayRevoked();
  }
}

void HTMLMediaElement::SetVolumeInternal() {
  float effectiveVolume = ComputedVolume();

  if (mDecoder) {
    mDecoder->SetVolume(effectiveVolume);
  } else if (MediaStream* stream = GetSrcMediaStream()) {
    if (mSrcStreamIsPlaying) {
      stream->SetAudioOutputVolume(this, effectiveVolume);
    }
  }

  NotifyAudioPlaybackChanged(
      AudioChannelService::AudibleChangedReasons::eVolumeChanged);
}

void HTMLMediaElement::SetMuted(bool aMuted) {
  LOG(LogLevel::Debug, ("%p SetMuted(%d) called by JS", this, aMuted));
  if (aMuted == Muted()) {
    return;
  }

  if (aMuted) {
    SetMutedInternal(mMuted | MUTED_BY_CONTENT);
  } else {
    SetMutedInternal(mMuted & ~MUTED_BY_CONTENT);
  }

  DispatchAsyncEvent(NS_LITERAL_STRING("volumechange"));

  // We allow inaudible autoplay. But changing our mute status may make this
  // media audible. So pause if we are no longer supposed to be autoplaying.
  PauseIfShouldNotBePlaying();
}

void HTMLMediaElement::SetCapturedOutputStreamsEnabled(bool aEnabled) {
  for (OutputMediaStream& ms : mOutputStreams) {
    if (ms.mCapturingDecoder) {
      MOZ_ASSERT(!ms.mCapturingMediaStream);
      continue;
    }
    for (auto pair : ms.mTrackPorts) {
      MediaStream* outputSource = ms.mStream->GetInputStream();
      if (!outputSource) {
        NS_ERROR("No output source stream");
        return;
      }

      TrackID id = pair.second()->GetDestinationTrackId();
      outputSource->SetTrackEnabled(
          id, aEnabled ? DisabledTrackMode::ENABLED
                       : DisabledTrackMode::SILENCE_FREEZE);

      LOG(LogLevel::Debug,
          ("%s track %d for captured MediaStream %p",
           aEnabled ? "Enabled" : "Disabled", id, ms.mStream.get()));
    }
  }
}

void HTMLMediaElement::AddCaptureMediaTrackToOutputStream(
    MediaTrack* aTrack, OutputMediaStream& aOutputStream, bool aAsyncAddtrack) {
  if (aOutputStream.mCapturingDecoder) {
    MOZ_ASSERT(!aOutputStream.mCapturingMediaStream);
    return;
  }
  aOutputStream.mCapturingMediaStream = true;

  if (aOutputStream.mStream == mSrcStream) {
    // Cycle detected. This can happen since tracks are added async.
    // We avoid forwarding it to the output here or we'd get into an infloop.
    return;
  }

  MediaStream* outputSource = aOutputStream.mStream->GetInputStream();
  if (!outputSource) {
    NS_ERROR("No output source stream");
    return;
  }

  ProcessedMediaStream* processedOutputSource =
      outputSource->AsProcessedStream();
  if (!processedOutputSource) {
    NS_ERROR("Input stream not a ProcessedMediaStream");
    return;
  }

  if (!aTrack) {
    MOZ_ASSERT(false, "Bad MediaTrack");
    return;
  }

  MediaStreamTrack* inputTrack = mSrcStream->GetTrackById(aTrack->GetId());
  MOZ_ASSERT(inputTrack);
  if (!inputTrack) {
    NS_ERROR("Input track not found in source stream");
    return;
  }

#if DEBUG
  for (auto pair : aOutputStream.mTrackPorts) {
    MOZ_ASSERT(pair.first() != aTrack->GetId(),
               "Captured track already captured to output stream");
  }
#endif

  TrackID destinationTrackID = aOutputStream.mNextAvailableTrackID++;
  RefPtr<MediaStreamTrackSource> source =
      new StreamCaptureTrackSource(this, &inputTrack->GetSource(),
                                   aOutputStream.mStream, destinationTrackID);

  MediaSegment::Type type = inputTrack->AsAudioStreamTrack()
                                ? MediaSegment::AUDIO
                                : MediaSegment::VIDEO;

  RefPtr<MediaStreamTrack> track =
      aOutputStream.mStream->CreateDOMTrack(destinationTrackID, type, source);

  if (aAsyncAddtrack) {
    mMainThreadEventTarget->Dispatch(
        NewRunnableMethod<StoreRefPtrPassByPtr<MediaStreamTrack>>(
            "DOMMediaStream::AddTrackInternal", aOutputStream.mStream,
            &DOMMediaStream::AddTrackInternal, track));
  } else {
    aOutputStream.mStream->AddTrackInternal(track);
  }

  // Track is muted initially, so we don't leak data if it's added while paused
  // and an MSG iteration passes before the mute comes into effect.
  processedOutputSource->SetTrackEnabled(destinationTrackID,
                                         DisabledTrackMode::SILENCE_FREEZE);
  RefPtr<MediaInputPort> port = inputTrack->ForwardTrackContentsTo(
      processedOutputSource, destinationTrackID);

  Pair<nsString, RefPtr<MediaInputPort>> p(aTrack->GetId(), port);
  aOutputStream.mTrackPorts.AppendElement(std::move(p));

  if (mSrcStreamIsPlaying) {
    processedOutputSource->SetTrackEnabled(destinationTrackID,
                                           DisabledTrackMode::ENABLED);
  }

  LOG(LogLevel::Debug,
      ("Created %s track %p with id %d from track %p through MediaInputPort %p",
       inputTrack->AsAudioStreamTrack() ? "audio" : "video", track.get(),
       destinationTrackID, inputTrack, port.get()));
}

void HTMLMediaElement::DiscardFinishWhenEndedOutputStreams() {
  // Discard all output streams that have finished now.
  for (int32_t i = mOutputStreams.Length() - 1; i >= 0; --i) {
    if (!mOutputStreams[i].mFinishWhenEnded) {
      continue;
    }
    LOG(LogLevel::Debug,
        ("Playback ended. Letting output stream %p go inactive",
         mOutputStreams[i].mStream.get()));
    mOutputStreams[i].mStream->SetFinishedOnInactive(true);
    if (mOutputStreams[i].mCapturingDecoder) {
      mDecoder->RemoveOutputStream(mOutputStreams[i].mStream);
    }
    mOutputStreams.RemoveElementAt(i);
  }
}

bool HTMLMediaElement::CanBeCaptured(StreamCaptureType aCaptureType) {
  // Don't bother capturing when the document has gone away
  nsPIDOMWindowInner* window = OwnerDoc()->GetInnerWindow();
  if (!window) {
    return false;
  }

  // Prevent capturing restricted video
  if (aCaptureType == StreamCaptureType::CAPTURE_ALL_TRACKS &&
      ContainsRestrictedContent()) {
    return false;
  }
  return true;
}

already_AddRefed<DOMMediaStream> HTMLMediaElement::CaptureStreamInternal(
    StreamCaptureBehavior aFinishBehavior, StreamCaptureType aStreamCaptureType,
    MediaStreamGraph* aGraph) {
  MOZ_RELEASE_ASSERT(aGraph);
  MOZ_ASSERT(CanBeCaptured(aStreamCaptureType));

  MarkAsContentSource(CallerAPI::CAPTURE_STREAM);
  MarkAsTainted();

  // We don't support routing to a different graph.
  if (!mOutputStreams.IsEmpty() &&
      aGraph != mOutputStreams[0].mStream->GetInputStream()->Graph()) {
    return nullptr;
  }

  OutputMediaStream* out = mOutputStreams.AppendElement();
  nsPIDOMWindowInner* window = OwnerDoc()->GetInnerWindow();
  out->mStream = DOMMediaStream::CreateTrackUnionStreamAsInput(window, aGraph);
  out->mStream->SetFinishedOnInactive(false);
  out->mFinishWhenEnded =
      aFinishBehavior == StreamCaptureBehavior::FINISH_WHEN_ENDED;
  out->mCapturingAudioOnly =
      aStreamCaptureType == StreamCaptureType::CAPTURE_AUDIO;

  if (aStreamCaptureType == StreamCaptureType::CAPTURE_AUDIO) {
    if (mSrcStream) {
      // We don't support applying volume and mute to the captured stream, when
      // capturing a MediaStream.
      ReportToConsole(nsIScriptError::errorFlag,
                      "MediaElementAudioCaptureOfMediaStreamError");
      return nullptr;
    }

    // mAudioCaptured tells the user that the audio played by this media element
    // is being routed to the captureStreams *instead* of being played to
    // speakers.
    mAudioCaptured = true;
  }

  if (mDecoder) {
    out->mCapturingDecoder = true;
    mDecoder->AddOutputStream(out->mStream);
  } else if (mSrcStream) {
    out->mCapturingMediaStream = true;
  }

  if (mReadyState == HAVE_NOTHING) {
    // Do not expose the tracks until we have metadata.
    RefPtr<DOMMediaStream> result = out->mStream;
    return result.forget();
  }

  if (mSrcStream) {
    MOZ_DIAGNOSTIC_ASSERT(AudioTracks(), "Element can't have been unlinked");
    for (size_t i = 0; i < AudioTracks()->Length(); ++i) {
      AudioTrack* t = (*AudioTracks())[i];
      if (t->Enabled()) {
        AddCaptureMediaTrackToOutputStream(t, *out, false);
      }
    }
    if (IsVideo() && !out->mCapturingAudioOnly) {
      MOZ_DIAGNOSTIC_ASSERT(VideoTracks(), "Element can't have been unlinked");
      // Only add video tracks if we're a video element and the output stream
      // wants video.
      for (size_t i = 0; i < VideoTracks()->Length(); ++i) {
        VideoTrack* t = (*VideoTracks())[i];
        if (t->Selected()) {
          AddCaptureMediaTrackToOutputStream(t, *out, false);
        }
      }
    }
  }
  RefPtr<DOMMediaStream> result = out->mStream;
  return result.forget();
}

already_AddRefed<DOMMediaStream> HTMLMediaElement::CaptureAudio(
    ErrorResult& aRv, MediaStreamGraph* aGraph) {
  MOZ_RELEASE_ASSERT(aGraph);

  if (!CanBeCaptured(StreamCaptureType::CAPTURE_AUDIO)) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  RefPtr<DOMMediaStream> stream =
      CaptureStreamInternal(StreamCaptureBehavior::CONTINUE_WHEN_ENDED,
                            StreamCaptureType::CAPTURE_AUDIO, aGraph);
  if (!stream) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  return stream.forget();
}

RefPtr<GenericNonExclusivePromise> HTMLMediaElement::GetAllowedToPlayPromise() {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!mOutputStreams.IsEmpty(),
             "This method should only be called during stream capturing!");
  if (AutoplayPolicy::IsAllowedToPlay(*this)) {
    AUTOPLAY_LOG("MediaElement %p has allowed to play, resolve promise", this);
    return GenericNonExclusivePromise::CreateAndResolve(true, __func__);
  }
  AUTOPLAY_LOG("create allow-to-play promise for MediaElement %p", this);
  return mAllowedToPlayPromise.Ensure(__func__);
}

already_AddRefed<DOMMediaStream> HTMLMediaElement::MozCaptureStream(
    ErrorResult& aRv) {
  MediaStreamGraph::GraphDriverType graphDriverType =
      HasAudio() ? MediaStreamGraph::AUDIO_THREAD_DRIVER
                 : MediaStreamGraph::SYSTEM_THREAD_DRIVER;

  nsPIDOMWindowInner* window = OwnerDoc()->GetInnerWindow();
  if (!window) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  if (!CanBeCaptured(StreamCaptureType::CAPTURE_ALL_TRACKS)) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  MediaStreamGraph* graph = MediaStreamGraph::GetInstance(
      graphDriverType, window, MediaStreamGraph::REQUEST_DEFAULT_SAMPLE_RATE);

  RefPtr<DOMMediaStream> stream =
      CaptureStreamInternal(StreamCaptureBehavior::CONTINUE_WHEN_ENDED,
                            StreamCaptureType::CAPTURE_ALL_TRACKS, graph);
  if (!stream) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  return stream.forget();
}

already_AddRefed<DOMMediaStream> HTMLMediaElement::MozCaptureStreamUntilEnded(
    ErrorResult& aRv) {
  MediaStreamGraph::GraphDriverType graphDriverType =
      HasAudio() ? MediaStreamGraph::AUDIO_THREAD_DRIVER
                 : MediaStreamGraph::SYSTEM_THREAD_DRIVER;

  nsPIDOMWindowInner* window = OwnerDoc()->GetInnerWindow();
  if (!window) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  if (!CanBeCaptured(StreamCaptureType::CAPTURE_ALL_TRACKS)) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  MediaStreamGraph* graph = MediaStreamGraph::GetInstance(
      graphDriverType, window, MediaStreamGraph::REQUEST_DEFAULT_SAMPLE_RATE);

  RefPtr<DOMMediaStream> stream =
      CaptureStreamInternal(StreamCaptureBehavior::FINISH_WHEN_ENDED,
                            StreamCaptureType::CAPTURE_ALL_TRACKS, graph);
  if (!stream) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  return stream.forget();
}

class MediaElementSetForURI : public nsURIHashKey {
 public:
  explicit MediaElementSetForURI(const nsIURI* aKey) : nsURIHashKey(aKey) {}
  MediaElementSetForURI(MediaElementSetForURI&& aOther)
      : nsURIHashKey(std::move(aOther)),
        mElements(std::move(aOther.mElements)) {}
  nsTArray<HTMLMediaElement*> mElements;
};

typedef nsTHashtable<MediaElementSetForURI> MediaElementURITable;
// Elements in this table must have non-null mDecoder and mLoadingSrc, and those
// can't change while the element is in the table. The table is keyed by
// the element's mLoadingSrc. Each entry has a list of all elements with the
// same mLoadingSrc.
static MediaElementURITable* gElementTable;

#ifdef DEBUG
static bool URISafeEquals(nsIURI* a1, nsIURI* a2) {
  if (!a1 || !a2) {
    // Consider two empty URIs *not* equal!
    return false;
  }
  bool equal = false;
  nsresult rv = a1->Equals(a2, &equal);
  return NS_SUCCEEDED(rv) && equal;
}
// Returns the number of times aElement appears in the media element table
// for aURI. If this returns other than 0 or 1, there's a bug somewhere!
static unsigned MediaElementTableCount(HTMLMediaElement* aElement,
                                       nsIURI* aURI) {
  if (!gElementTable || !aElement) {
    return 0;
  }
  uint32_t uriCount = 0;
  uint32_t otherCount = 0;
  for (auto it = gElementTable->ConstIter(); !it.Done(); it.Next()) {
    MediaElementSetForURI* entry = it.Get();
    uint32_t count = 0;
    for (const auto& elem : entry->mElements) {
      if (elem == aElement) {
        count++;
      }
    }
    if (URISafeEquals(aURI, entry->GetKey())) {
      uriCount = count;
    } else {
      otherCount += count;
    }
  }
  NS_ASSERTION(otherCount == 0, "Should not have entries for unknown URIs");
  return uriCount;
}
#endif

void HTMLMediaElement::AddMediaElementToURITable() {
  NS_ASSERTION(mDecoder, "Call this only with decoder Load called");
  NS_ASSERTION(
      MediaElementTableCount(this, mLoadingSrc) == 0,
      "Should not have entry for element in element table before addition");
  if (!gElementTable) {
    gElementTable = new MediaElementURITable();
  }
  MediaElementSetForURI* entry = gElementTable->PutEntry(mLoadingSrc);
  entry->mElements.AppendElement(this);
  NS_ASSERTION(
      MediaElementTableCount(this, mLoadingSrc) == 1,
      "Should have a single entry for element in element table after addition");
}

void HTMLMediaElement::RemoveMediaElementFromURITable() {
  if (!mDecoder || !mLoadingSrc || !gElementTable) {
    return;
  }
  MediaElementSetForURI* entry = gElementTable->GetEntry(mLoadingSrc);
  if (!entry) {
    return;
  }
  entry->mElements.RemoveElement(this);
  if (entry->mElements.IsEmpty()) {
    gElementTable->RemoveEntry(entry);
    if (gElementTable->Count() == 0) {
      delete gElementTable;
      gElementTable = nullptr;
    }
  }
  NS_ASSERTION(MediaElementTableCount(this, mLoadingSrc) == 0,
               "After remove, should no longer have an entry in element table");
}

HTMLMediaElement* HTMLMediaElement::LookupMediaElementURITable(nsIURI* aURI) {
  if (!gElementTable) {
    return nullptr;
  }
  MediaElementSetForURI* entry = gElementTable->GetEntry(aURI);
  if (!entry) {
    return nullptr;
  }
  for (uint32_t i = 0; i < entry->mElements.Length(); ++i) {
    HTMLMediaElement* elem = entry->mElements[i];
    bool equal;
    // Look for elements that have the same principal and CORS mode.
    // Ditto for anything else that could cause us to send different headers.
    if (NS_SUCCEEDED(elem->NodePrincipal()->Equals(NodePrincipal(), &equal)) &&
        equal && elem->mCORSMode == mCORSMode) {
      // See SetupDecoder() below. We only add a element to the table when
      // mDecoder is a ChannelMediaDecoder.
      auto decoder = static_cast<ChannelMediaDecoder*>(elem->mDecoder.get());
      NS_ASSERTION(decoder, "Decoder gone");
      if (decoder->CanClone()) {
        return elem;
      }
    }
  }
  return nullptr;
}

class HTMLMediaElement::ShutdownObserver : public nsIObserver {
  enum class Phase : int8_t { Init, Subscribed, Unsubscribed };

 public:
  NS_DECL_ISUPPORTS

  NS_IMETHOD Observe(nsISupports*, const char* aTopic,
                     const char16_t*) override {
    if (mPhase != Phase::Subscribed) {
      // Bail out if we are not subscribed for this might be called even after
      // |nsContentUtils::UnregisterShutdownObserver(this)|.
      return NS_OK;
    }
    MOZ_DIAGNOSTIC_ASSERT(mWeak);
    if (strcmp(aTopic, NS_XPCOM_SHUTDOWN_OBSERVER_ID) == 0) {
      mWeak->NotifyShutdownEvent();
    }
    return NS_OK;
  }
  void Subscribe(HTMLMediaElement* aPtr) {
    MOZ_DIAGNOSTIC_ASSERT(mPhase == Phase::Init);
    MOZ_DIAGNOSTIC_ASSERT(!mWeak);
    mWeak = aPtr;
    nsContentUtils::RegisterShutdownObserver(this);
    mPhase = Phase::Subscribed;
  }
  void Unsubscribe() {
    MOZ_DIAGNOSTIC_ASSERT(mPhase == Phase::Subscribed);
    MOZ_DIAGNOSTIC_ASSERT(mWeak);
    MOZ_DIAGNOSTIC_ASSERT(!mAddRefed,
                          "ReleaseMediaElement should have been called first");
    mWeak = nullptr;
    nsContentUtils::UnregisterShutdownObserver(this);
    mPhase = Phase::Unsubscribed;
  }
  void AddRefMediaElement() {
    MOZ_DIAGNOSTIC_ASSERT(mWeak);
    MOZ_DIAGNOSTIC_ASSERT(!mAddRefed, "Should only ever AddRef once");
    mWeak->AddRef();
    mAddRefed = true;
  }
  void ReleaseMediaElement() {
    MOZ_DIAGNOSTIC_ASSERT(mWeak);
    MOZ_DIAGNOSTIC_ASSERT(mAddRefed, "Should only release after AddRef");
    mWeak->Release();
    mAddRefed = false;
  }

 private:
  virtual ~ShutdownObserver() {
    MOZ_DIAGNOSTIC_ASSERT(mPhase == Phase::Unsubscribed);
    MOZ_DIAGNOSTIC_ASSERT(!mWeak);
    MOZ_DIAGNOSTIC_ASSERT(!mAddRefed,
                          "ReleaseMediaElement should have been called first");
  }
  // Guaranteed to be valid by HTMLMediaElement.
  HTMLMediaElement* mWeak = nullptr;
  Phase mPhase = Phase::Init;
  bool mAddRefed = false;
};

NS_IMPL_ISUPPORTS(HTMLMediaElement::ShutdownObserver, nsIObserver)

HTMLMediaElement::HTMLMediaElement(
    already_AddRefed<mozilla::dom::NodeInfo>&& aNodeInfo)
    : nsGenericHTMLElement(std::move(aNodeInfo)),
      mWatchManager(this,
                    OwnerDoc()->AbstractMainThreadFor(TaskCategory::Other)),
      mMainThreadEventTarget(OwnerDoc()->EventTargetFor(TaskCategory::Other)),
      mAbstractMainThread(
          OwnerDoc()->AbstractMainThreadFor(TaskCategory::Other)),
      mShutdownObserver(new ShutdownObserver),
      mPlayed(new TimeRanges(ToSupports(OwnerDoc()))),
      mPaused(true, "HTMLMediaElement::mPaused"),
      mErrorSink(new ErrorSink(this)),
      mAudioChannelWrapper(new AudioChannelAgentCallback(this)),
      mSink(MakePair(nsString(), RefPtr<AudioDeviceInfo>())),
      mShowPoster(IsVideo()) {
  MOZ_ASSERT(mMainThreadEventTarget);
  MOZ_ASSERT(mAbstractMainThread);
  // Please don't add anything to this constructor or the initialization
  // list that can cause AddRef to be called. This prevents subclasses
  // from overriding AddRef in a way that works with our refcount
  // logging mechanisms. Put these things inside of the ::Init method
  // instead.
}

void HTMLMediaElement::Init() {
  MOZ_ASSERT(mRefCnt == 0 && !mRefCnt.IsPurple(),
             "HTMLMediaElement::Init called when AddRef has been called "
             "at least once already, probably in the constructor. Please "
             "see the documentation in the HTMLMediaElement constructor.");
  MOZ_ASSERT(!mRefCnt.IsPurple());

  mAudioTrackList = new AudioTrackList(OwnerDoc()->GetParentObject(), this);
  mVideoTrackList = new VideoTrackList(OwnerDoc()->GetParentObject(), this);

  DecoderDoctorLogger::LogConstruction(this);

  mWatchManager.Watch(mPaused, &HTMLMediaElement::UpdateWakeLock);

  ErrorResult rv;

  double defaultVolume = Preferences::GetFloat("media.default_volume", 1.0);
  SetVolume(defaultVolume, rv);

  RegisterActivityObserver();
  NotifyOwnerDocumentActivityChanged();

  // We initialize the MediaShutdownManager as the HTMLMediaElement is always
  // constructed on the main thread, and not during stable state.
  // (MediaShutdownManager make use of nsIAsyncShutdownClient which is written
  // in JS)
  MediaShutdownManager::InitStatics();

  mShutdownObserver->Subscribe(this);
  mInitialized = true;
}

HTMLMediaElement::~HTMLMediaElement() {
  MOZ_ASSERT(mInitialized,
             "HTMLMediaElement must be initialized before it is destroyed.");
  NS_ASSERTION(
      !mHasSelfReference,
      "How can we be destroyed if we're still holding a self reference?");

  mShutdownObserver->Unsubscribe();

  if (mVideoFrameContainer) {
    mVideoFrameContainer->ForgetElement();
  }
  UnregisterActivityObserver();

  mSetCDMRequest.DisconnectIfExists();
  mAllowedToPlayPromise.RejectIfExists(NS_ERROR_FAILURE, __func__);

  if (mDecoder) {
    ShutdownDecoder();
  }
  if (mProgressTimer) {
    StopProgress();
  }
  if (mVideoDecodeSuspendTimer) {
    mVideoDecodeSuspendTimer->Cancel();
    mVideoDecodeSuspendTimer = nullptr;
  }
  if (mSrcStream) {
    EndSrcMediaStreamPlayback();
  }

  if (mCaptureStreamPort) {
    mCaptureStreamPort->Destroy();
    mCaptureStreamPort = nullptr;
  }

  NS_ASSERTION(MediaElementTableCount(this, mLoadingSrc) == 0,
               "Destroyed media element should no longer be in element table");

  if (mChannelLoader) {
    mChannelLoader->Cancel();
  }

  if (mAudioChannelWrapper) {
    mAudioChannelWrapper->Shutdown();
    mAudioChannelWrapper = nullptr;
  }

  WakeLockRelease();
  ReportPlayedTimeAfterBlockedTelemetry();

  DecoderDoctorLogger::LogDestruction(this);
}

void HTMLMediaElement::StopSuspendingAfterFirstFrame() {
  mAllowSuspendAfterFirstFrame = false;
  if (!mSuspendedAfterFirstFrame) return;
  mSuspendedAfterFirstFrame = false;
  if (mDecoder) {
    mDecoder->Resume();
  }
}

void HTMLMediaElement::SetPlayedOrSeeked(bool aValue) {
  if (aValue == mHasPlayedOrSeeked) {
    return;
  }

  mHasPlayedOrSeeked = aValue;

  // Force a reflow so that the poster frame hides or shows immediately.
  nsIFrame* frame = GetPrimaryFrame();
  if (!frame) {
    return;
  }
  frame->PresShell()->FrameNeedsReflow(frame, IntrinsicDirty::TreeChange,
                                       NS_FRAME_IS_DIRTY);
}

void HTMLMediaElement::NotifyXPCOMShutdown() { ShutdownDecoder(); }

bool HTMLMediaElement::AudioChannelAgentDelayingPlayback() {
  return mAudioChannelWrapper && mAudioChannelWrapper->IsPlaybackBlocked();
}

void HTMLMediaElement::UpdateHadAudibleAutoplayState() {
  // If we're audible, and autoplaying...
  if ((Volume() > 0.0 && !Muted()) &&
      (!OwnerDoc()->HasBeenUserGestureActivated() || Autoplay())) {
    OwnerDoc()->SetDocTreeHadAudibleMedia();
    if (AutoplayPolicy::WouldBeAllowedToPlayIfAutoplayDisabled(*this)) {
      ScalarAdd(Telemetry::ScalarID::MEDIA_AUTOPLAY_WOULD_BE_ALLOWED_COUNT, 1);
      if (mReadyState >= HAVE_METADATA && !HasAudio()) {
        ScalarAdd(
            Telemetry::ScalarID::MEDIA_ALLOWED_AUTOPLAY_NO_AUDIO_TRACK_COUNT,
            1);
      }
    } else {
      if (mReadyState < HAVE_METADATA) {
        mBlockedAsWithoutMetadata = true;
        ScalarAdd(Telemetry::ScalarID::MEDIA_BLOCKED_NO_METADATA, 1);
      }
      ScalarAdd(Telemetry::ScalarID::MEDIA_AUTOPLAY_WOULD_NOT_BE_ALLOWED_COUNT,
                1);
    }
  }
}

already_AddRefed<Promise> HTMLMediaElement::Play(ErrorResult& aRv) {
  LOG(LogLevel::Debug,
      ("%p Play() called by JS readyState=%d", this, mReadyState));

  // 4.8.12.8
  // When the play() method on a media element is invoked, the user agent must
  // run the following steps.

  RefPtr<PlayPromise> promise = CreatePlayPromise(aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  // 4.8.12.8 - Step 1:
  // If the media element is not allowed to play, return a promise rejected
  // with a "NotAllowedError" DOMException and abort these steps.
  // NOTE: we may require requesting permission from the user, so we do the
  // "not allowed" check below.

  // 4.8.12.8 - Step 2:
  // If the media element's error attribute is not null and its code
  // attribute has the value MEDIA_ERR_SRC_NOT_SUPPORTED, return a promise
  // rejected with a "NotSupportedError" DOMException and abort these steps.
  if (GetError() && GetError()->Code() == MEDIA_ERR_SRC_NOT_SUPPORTED) {
    LOG(LogLevel::Debug,
        ("%p Play() promise rejected because source not supported.", this));
    promise->MaybeReject(NS_ERROR_DOM_MEDIA_NOT_SUPPORTED_ERR);
    return promise.forget();
  }

  // 4.8.12.8 - Step 3:
  // Let promise be a new promise and append promise to the list of pending
  // play promises.
  // Note: Promise appended to list of pending promises as needed below.

  if (AudioChannelAgentDelayingPlayback()) {
    // The audio channel agent may delay starting playback of a media resource
    // until the tab the media element is in has been in the foreground.
    // Save a reference to the promise, and return it. The AudioChannelAgent
    // will call Play() again if the tab is brought to the foreground, or the
    // audio tab indicator is clicked, which will resolve the promise if we end
    // up playing.
    LOG(LogLevel::Debug, ("%p Play() call delayed by AudioChannelAgent", this));
    MaybeDoLoad();
    mPendingPlayPromises.AppendElement(promise);
    return promise.forget();
  }

  if (AudioChannelAgentBlockedPlay()) {
    LOG(LogLevel::Debug, ("%p play blocked by AudioChannelAgent.", this));
    promise->MaybeReject(NS_ERROR_DOM_MEDIA_NOT_ALLOWED_ERR);
    return promise.forget();
  }

  UpdateHadAudibleAutoplayState();

  const bool handlingUserInput = EventStateManager::IsHandlingUserInput();
  mPendingPlayPromises.AppendElement(promise);

  if (AutoplayPolicy::IsAllowedToPlay(*this)) {
    AUTOPLAY_LOG("allow MediaElement %p to play", this);
    mAllowedToPlayPromise.ResolveIfExists(true, __func__);
    PlayInternal(handlingUserInput);
    UpdateCustomPolicyAfterPlayed();
  } else {
    AUTOPLAY_LOG("reject MediaElement %p to play", this);
    AsyncRejectPendingPlayPromises(NS_ERROR_DOM_MEDIA_NOT_ALLOWED_ERR);
  }
  return promise.forget();
}

void HTMLMediaElement::DispatchEventsWhenPlayWasNotAllowed() {
  if (StaticPrefs::MediaBlockEventEnabled()) {
    DispatchAsyncEvent(NS_LITERAL_STRING("blocked"));
  }
#if defined(MOZ_WIDGET_ANDROID)
  RefPtr<AsyncEventDispatcher> asyncDispatcher = new AsyncEventDispatcher(
      this, NS_LITERAL_STRING("MozAutoplayMediaBlocked"), CanBubble::eYes,
      ChromeOnlyDispatch::eYes);
  asyncDispatcher->PostDOMEvent();
#endif
  OwnerDoc()->MaybeNotifyAutoplayBlocked();
  ReportToConsole(nsIScriptError::warningFlag, "BlockAutoplayError");
  mHasPlayEverBeenBlocked = true;
  mHasEverBeenBlockedForAutoplay = true;
}

void HTMLMediaElement::PlayInternal(bool aHandlingUserInput) {
  if (mPreloadAction == HTMLMediaElement::PRELOAD_NONE) {
    // The media load algorithm will be initiated by a user interaction.
    // We want to boost the channel priority for better responsiveness.
    // Note this must be done before UpdatePreloadAction() which will
    // update |mPreloadAction|.
    mUseUrgentStartForChannel = true;
  }

  StopSuspendingAfterFirstFrame();
  SetPlayedOrSeeked(true);

  // 4.8.12.8 - Step 4:
  // If the media element's networkState attribute has the value NETWORK_EMPTY,
  // invoke the media element's resource selection algorithm.
  MaybeDoLoad();
  if (mSuspendedForPreloadNone) {
    ResumeLoad(PRELOAD_ENOUGH);
  }

  // 4.8.12.8 - Step 5:
  // If the playback has ended and the direction of playback is forwards,
  // seek to the earliest possible position of the media resource.

  // Even if we just did Load() or ResumeLoad(), we could already have a decoder
  // here if we managed to clone an existing decoder.
  if (mDecoder) {
    if (mDecoder->IsEnded()) {
      SetCurrentTime(0);
    }
    if (!mPausedForInactiveDocumentOrChannel) {
      mDecoder->Play();
    }
  }

  if (mCurrentPlayRangeStart == -1.0) {
    mCurrentPlayRangeStart = CurrentTime();
  }

  const bool oldPaused = mPaused;
  mPaused = false;
  mAutoplaying = false;

  // We changed mPaused and mAutoplaying which can affect AddRemoveSelfReference
  // and our preload status.
  AddRemoveSelfReference();
  UpdatePreloadAction();
  UpdateSrcMediaStreamPlaying();

  // Once play() has been called in a user generated event handler,
  // it is allowed to autoplay. Note: we can reach here when not in
  // a user generated event handler if our readyState has not yet
  // reached HAVE_METADATA.
  mIsBlessed |= aHandlingUserInput;

  // TODO: If the playback has ended, then the user agent must set
  // seek to the effective start.

  // 4.8.12.8 - Step 6:
  // If the media element's paused attribute is true, run the following steps:
  if (oldPaused) {
    // 6.1. Change the value of paused to false. (Already done.)
    // This step is uplifted because the "block-media-playback" feature needs
    // the mPaused to be false before UpdateAudioChannelPlayingState() being
    // called.

    // 6.2. If the show poster flag is true, set the element's show poster flag
    //      to false and run the time marches on steps.
    if (mShowPoster) {
      mShowPoster = false;
      if (mTextTrackManager) {
        mTextTrackManager->TimeMarchesOn();
      }
    }

    // 6.3. Queue a task to fire a simple event named play at the element.
    DispatchAsyncEvent(NS_LITERAL_STRING("play"));

    // 6.4. If the media element's readyState attribute has the value
    //      HAVE_NOTHING, HAVE_METADATA, or HAVE_CURRENT_DATA, queue a task to
    //      fire a simple event named waiting at the element.
    //      Otherwise, the media element's readyState attribute has the value
    //      HAVE_FUTURE_DATA or HAVE_ENOUGH_DATA: notify about playing for the
    //      element.
    switch (mReadyState) {
      case HAVE_NOTHING:
        DispatchAsyncEvent(NS_LITERAL_STRING("waiting"));
        break;
      case HAVE_METADATA:
      case HAVE_CURRENT_DATA:
        FireTimeUpdate(false);
        DispatchAsyncEvent(NS_LITERAL_STRING("waiting"));
        break;
      case HAVE_FUTURE_DATA:
      case HAVE_ENOUGH_DATA:
        FireTimeUpdate(false);
        NotifyAboutPlaying();
        break;
    }
  } else if (mReadyState >= HAVE_FUTURE_DATA) {
    // 7. Otherwise, if the media element's readyState attribute has the value
    //    HAVE_FUTURE_DATA or HAVE_ENOUGH_DATA, take pending play promises and
    //    queue a task to resolve pending play promises with the result.
    AsyncResolvePendingPlayPromises();
  }

  // 8. Set the media element's autoplaying flag to false. (Already done.)

  // 9. Return promise.
  // (Done in caller.)
}

void HTMLMediaElement::MaybeDoLoad() {
  if (mNetworkState == NETWORK_EMPTY) {
    DoLoad();
  }
}

void HTMLMediaElement::UpdateWakeLock() {
  MOZ_ASSERT(NS_IsMainThread());
  // Ensure we have a wake lock if we're playing audibly. This ensures the
  // device doesn't sleep while playing.
  bool playing = !mPaused;
  bool isAudible = Volume() > 0.0 && !mMuted && mIsAudioTrackAudible;
  // WakeLock when playing audible media.
  if (playing && isAudible) {
    CreateAudioWakeLockIfNeeded();
  } else {
    ReleaseAudioWakeLockIfExists();
  }
}

void HTMLMediaElement::CreateAudioWakeLockIfNeeded() {
  if (!mWakeLock) {
    RefPtr<power::PowerManagerService> pmService =
        power::PowerManagerService::GetInstance();
    NS_ENSURE_TRUE_VOID(pmService);

    ErrorResult rv;
    mWakeLock = pmService->NewWakeLock(NS_LITERAL_STRING("audio-playing"),
                                       OwnerDoc()->GetInnerWindow(), rv);
  }
}

void HTMLMediaElement::ReleaseAudioWakeLockIfExists() {
  if (mWakeLock) {
    ErrorResult rv;
    mWakeLock->Unlock(rv);
    rv.SuppressException();
    mWakeLock = nullptr;
  }
}

void HTMLMediaElement::WakeLockRelease() { ReleaseAudioWakeLockIfExists(); }

HTMLMediaElement::OutputMediaStream::OutputMediaStream()
    : mNextAvailableTrackID(1),
      mFinishWhenEnded(false),
      mCapturingAudioOnly(false),
      mCapturingDecoder(false),
      mCapturingMediaStream(false) {}

HTMLMediaElement::OutputMediaStream::~OutputMediaStream() {
  for (auto pair : mTrackPorts) {
    pair.second()->Destroy();
  }
}

void HTMLMediaElement::GetEventTargetParent(EventChainPreVisitor& aVisitor) {
  if (!this->Controls() || !aVisitor.mEvent->mFlags.mIsTrusted) {
    nsGenericHTMLElement::GetEventTargetParent(aVisitor);
    return;
  }

  HTMLInputElement* el = nullptr;
  nsCOMPtr<nsINode> node;

  // We will need to trap pointer, touch, and mouse events within the media
  // element, allowing media control exclusive consumption on these events,
  // and preventing the content from handling them.
  switch (aVisitor.mEvent->mMessage) {
    case ePointerDown:
    case ePointerUp:
    case eTouchEnd:
    // Always prevent touchmove captured in video element from being handled by
    // content, since we always do that for touchstart.
    case eTouchMove:
    case eTouchStart:
    case eMouseClick:
    case eMouseDoubleClick:
    case eMouseDown:
    case eMouseUp:
      aVisitor.mCanHandle = false;
      return;

    // The *move events however are only comsumed when the range input is being
    // dragged.
    case ePointerMove:
    case eMouseMove:
      node = do_QueryInterface(aVisitor.mEvent->mOriginalTarget);
      if (node->IsInNativeAnonymousSubtree() || node->IsInUAWidget()) {
        if (node->IsHTMLElement(nsGkAtoms::input)) {
          // The node is a <input type="range">
          el = static_cast<HTMLInputElement*>(node.get());
        } else if (node->GetParentNode() &&
                   node->GetParentNode()->IsHTMLElement(nsGkAtoms::input)) {
          // The node is a child of <input type="range">
          el = static_cast<HTMLInputElement*>(node->GetParentNode());
        }
      }
      if (el && el->IsDraggingRange()) {
        aVisitor.mCanHandle = false;
        return;
      }
      nsGenericHTMLElement::GetEventTargetParent(aVisitor);
      return;

    default:
      nsGenericHTMLElement::GetEventTargetParent(aVisitor);
      return;
  }
}

bool HTMLMediaElement::ParseAttribute(int32_t aNamespaceID, nsAtom* aAttribute,
                                      const nsAString& aValue,
                                      nsIPrincipal* aMaybeScriptedPrincipal,
                                      nsAttrValue& aResult) {
  // Mappings from 'preload' attribute strings to an enumeration.
  static const nsAttrValue::EnumTable kPreloadTable[] = {
      {"", HTMLMediaElement::PRELOAD_ATTR_EMPTY},
      {"none", HTMLMediaElement::PRELOAD_ATTR_NONE},
      {"metadata", HTMLMediaElement::PRELOAD_ATTR_METADATA},
      {"auto", HTMLMediaElement::PRELOAD_ATTR_AUTO},
      {nullptr, 0}};

  if (aNamespaceID == kNameSpaceID_None) {
    if (ParseImageAttribute(aAttribute, aValue, aResult)) {
      return true;
    }
    if (aAttribute == nsGkAtoms::crossorigin) {
      ParseCORSValue(aValue, aResult);
      return true;
    }
    if (aAttribute == nsGkAtoms::preload) {
      return aResult.ParseEnumValue(aValue, kPreloadTable, false);
    }
  }

  return nsGenericHTMLElement::ParseAttribute(aNamespaceID, aAttribute, aValue,
                                              aMaybeScriptedPrincipal, aResult);
}

void HTMLMediaElement::DoneCreatingElement() {
  if (HasAttr(kNameSpaceID_None, nsGkAtoms::muted)) {
    mMuted |= MUTED_BY_CONTENT;
  }
}

bool HTMLMediaElement::IsHTMLFocusable(bool aWithMouse, bool* aIsFocusable,
                                       int32_t* aTabIndex) {
  if (nsGenericHTMLElement::IsHTMLFocusable(aWithMouse, aIsFocusable,
                                            aTabIndex)) {
    return true;
  }

  *aIsFocusable = true;
  return false;
}

int32_t HTMLMediaElement::TabIndexDefault() { return 0; }

nsresult HTMLMediaElement::AfterSetAttr(int32_t aNameSpaceID, nsAtom* aName,
                                        const nsAttrValue* aValue,
                                        const nsAttrValue* aOldValue,
                                        nsIPrincipal* aMaybeScriptedPrincipal,
                                        bool aNotify) {
  if (aNameSpaceID == kNameSpaceID_None) {
    if (aName == nsGkAtoms::src) {
      mSrcMediaSource = nullptr;
      mSrcAttrTriggeringPrincipal = nsContentUtils::GetAttrTriggeringPrincipal(
          this, aValue ? aValue->GetStringValue() : EmptyString(),
          aMaybeScriptedPrincipal);
      if (aValue) {
        nsString srcStr = aValue->GetStringValue();
        nsCOMPtr<nsIURI> uri;
        NewURIFromString(srcStr, getter_AddRefs(uri));
        if (uri && IsMediaSourceURI(uri)) {
          nsresult rv = NS_GetSourceForMediaSourceURI(
              uri, getter_AddRefs(mSrcMediaSource));
          if (NS_FAILED(rv)) {
            nsAutoString spec;
            GetCurrentSrc(spec);
            const char16_t* params[] = {spec.get()};
            ReportLoadError("MediaLoadInvalidURI", params, ArrayLength(params));
          }
        }
      }
    } else if (aName == nsGkAtoms::autoplay) {
      if (aNotify) {
        if (aValue) {
          StopSuspendingAfterFirstFrame();
          CheckAutoplayDataReady();
        }
        // This attribute can affect AddRemoveSelfReference
        AddRemoveSelfReference();
        UpdatePreloadAction();
      }
    } else if (aName == nsGkAtoms::preload) {
      UpdatePreloadAction();
    } else if (aName == nsGkAtoms::loop) {
      if (mDecoder) {
        mDecoder->SetLooping(!!aValue);
      }
    } else if (aName == nsGkAtoms::controls && IsInComposedDoc()) {
      NotifyUAWidgetSetupOrChange();
    }
  }

  // Since AfterMaybeChangeAttr may call DoLoad, make sure that it is called
  // *after* any possible changes to mSrcMediaSource.
  if (aValue) {
    AfterMaybeChangeAttr(aNameSpaceID, aName, aNotify);
  }

  return nsGenericHTMLElement::AfterSetAttr(
      aNameSpaceID, aName, aValue, aOldValue, aMaybeScriptedPrincipal, aNotify);
}

nsresult HTMLMediaElement::OnAttrSetButNotChanged(
    int32_t aNamespaceID, nsAtom* aName, const nsAttrValueOrString& aValue,
    bool aNotify) {
  AfterMaybeChangeAttr(aNamespaceID, aName, aNotify);

  return nsGenericHTMLElement::OnAttrSetButNotChanged(aNamespaceID, aName,
                                                      aValue, aNotify);
}

void HTMLMediaElement::AfterMaybeChangeAttr(int32_t aNamespaceID, nsAtom* aName,
                                            bool aNotify) {
  if (aNamespaceID == kNameSpaceID_None) {
    if (aName == nsGkAtoms::src) {
      DoLoad();
    }
  }
}

nsresult HTMLMediaElement::BindToTree(Document* aDocument, nsIContent* aParent,
                                      nsIContent* aBindingParent) {
  nsresult rv =
      nsGenericHTMLElement::BindToTree(aDocument, aParent, aBindingParent);

  if (IsInComposedDoc()) {
    // Construct Shadow Root so web content can be hidden in the DOM.
    AttachAndSetUAShadowRoot();
    NotifyUAWidgetSetupOrChange();
  }

  mUnboundFromTree = false;

  if (aDocument) {
    // The preload action depends on the value of the autoplay attribute.
    // It's value may have changed, so update it.
    UpdatePreloadAction();
  }

  NotifyDecoderActivityChanges();

  return rv;
}

/* static */
void HTMLMediaElement::VideoDecodeSuspendTimerCallback(nsITimer* aTimer,
                                                       void* aClosure) {
  MOZ_ASSERT(NS_IsMainThread());
  auto element = static_cast<HTMLMediaElement*>(aClosure);
  element->mVideoDecodeSuspendTime.Start();
  element->mVideoDecodeSuspendTimer = nullptr;
}

void HTMLMediaElement::HiddenVideoStart() {
  MOZ_ASSERT(NS_IsMainThread());
  mHiddenPlayTime.Start();
  if (mVideoDecodeSuspendTimer) {
    // Already started, just keep it running.
    return;
  }
  NS_NewTimerWithFuncCallback(
      getter_AddRefs(mVideoDecodeSuspendTimer), VideoDecodeSuspendTimerCallback,
      this, StaticPrefs::MediaSuspendBkgndVideoDelayMs(),
      nsITimer::TYPE_ONE_SHOT,
      "HTMLMediaElement::VideoDecodeSuspendTimerCallback",
      mMainThreadEventTarget);
}

void HTMLMediaElement::HiddenVideoStop() {
  MOZ_ASSERT(NS_IsMainThread());
  mHiddenPlayTime.Pause();
  mVideoDecodeSuspendTime.Pause();
  if (!mVideoDecodeSuspendTimer) {
    return;
  }
  mVideoDecodeSuspendTimer->Cancel();
  mVideoDecodeSuspendTimer = nullptr;
}

void HTMLMediaElement::ReportTelemetry() {
  // Report telemetry for videos when a page is unloaded. We
  // want to know data on what state the video is at when
  // the user has exited.
  enum UnloadedState {
    ENDED = 0,
    PAUSED = 1,
    STALLED = 2,
    SEEKING = 3,
    OTHER = 4
  };

  UnloadedState state = OTHER;
  if (Seeking()) {
    state = SEEKING;
  } else if (Ended()) {
    state = ENDED;
  } else if (Paused()) {
    state = PAUSED;
  } else {
    // For buffering we check if the current playback position is at the end
    // of a buffered range, within a margin of error. We also consider to be
    // buffering if the last frame status was buffering and the ready state is
    // HAVE_CURRENT_DATA to account for times where we are in a buffering state
    // regardless of what actual data we have buffered.
    bool stalled = false;
    RefPtr<TimeRanges> ranges = Buffered();
    const double errorMargin = 0.05;
    double t = CurrentTime();
    TimeRanges::index_type index = ranges->Find(t, errorMargin);
    stalled =
        index != TimeRanges::NoIndex && (ranges->End(index) - t) < errorMargin;
    stalled |= mDecoder &&
               NextFrameStatus() ==
                   MediaDecoderOwner::NEXT_FRAME_UNAVAILABLE_BUFFERING &&
               mReadyState == HAVE_CURRENT_DATA;
    if (stalled) {
      state = STALLED;
    }
  }

  Telemetry::Accumulate(Telemetry::VIDEO_UNLOAD_STATE, state);
  LOG(LogLevel::Debug, ("%p VIDEO_UNLOAD_STATE = %d", this, state));

  FrameStatisticsData data;

  if (HTMLVideoElement* vid = HTMLVideoElement::FromNodeOrNull(this)) {
    FrameStatistics* stats = vid->GetFrameStatistics();
    if (stats) {
      data = stats->GetFrameStatisticsData();
      uint64_t parsedFrames = stats->GetParsedFrames();
      if (parsedFrames) {
        uint64_t droppedFrames = stats->GetDroppedFrames();
        MOZ_ASSERT(droppedFrames <= parsedFrames);
        // Dropped frames <= total frames, so 'percentage' cannot be higher than
        // 100 and therefore can fit in a uint32_t (that Telemetry takes).
        uint32_t percentage = 100 * droppedFrames / parsedFrames;
        LOG(LogLevel::Debug,
            ("Reporting telemetry DROPPED_FRAMES_IN_VIDEO_PLAYBACK"));
        Telemetry::Accumulate(Telemetry::VIDEO_DROPPED_FRAMES_PROPORTION,
                              percentage);
      }
    }
  }

  if (mMediaInfo.HasVideo() && mMediaInfo.mVideo.mImage.height > 0) {
    // We have a valid video.
    double playTime = mPlayTime.Total();
    double hiddenPlayTime = mHiddenPlayTime.Total();
    double videoDecodeSuspendTime = mVideoDecodeSuspendTime.Total();

    Telemetry::Accumulate(Telemetry::VIDEO_PLAY_TIME_MS,
                          SECONDS_TO_MS(playTime));
    LOG(LogLevel::Debug, ("%p VIDEO_PLAY_TIME_MS = %f", this, playTime));

    Telemetry::Accumulate(Telemetry::VIDEO_HIDDEN_PLAY_TIME_MS,
                          SECONDS_TO_MS(hiddenPlayTime));
    LOG(LogLevel::Debug,
        ("%p VIDEO_HIDDEN_PLAY_TIME_MS = %f", this, hiddenPlayTime));

    if (playTime > 0.0) {
      // We have actually played something -> Report some valid-video telemetry.

      // Keyed by audio+video or video alone, and by a resolution range.
      nsCString key(mMediaInfo.HasAudio() ? "AV," : "V,");
      static const struct {
        int32_t mH;
        const char* mRes;
      } sResolutions[] = {{240, "0<h<=240"},     {480, "240<h<=480"},
                          {576, "480<h<=576"},   {720, "576<h<=720"},
                          {1080, "720<h<=1080"}, {2160, "1080<h<=2160"}};
      const char* resolution = "h>2160";
      int32_t height = mMediaInfo.mVideo.mImage.height;
      for (const auto& res : sResolutions) {
        if (height <= res.mH) {
          resolution = res.mRes;
          break;
        }
      }
      key.AppendASCII(resolution);

      uint32_t hiddenPercentage =
          uint32_t(hiddenPlayTime / playTime * 100.0 + 0.5);
      Telemetry::Accumulate(Telemetry::VIDEO_HIDDEN_PLAY_TIME_PERCENTAGE, key,
                            hiddenPercentage);
      // Also accumulate all percentages in an "All" key.
      Telemetry::Accumulate(Telemetry::VIDEO_HIDDEN_PLAY_TIME_PERCENTAGE,
                            NS_LITERAL_CSTRING("All"), hiddenPercentage);
      LOG(LogLevel::Debug,
          ("%p VIDEO_HIDDEN_PLAY_TIME_PERCENTAGE = %u, keys: '%s' and 'All'",
           this, hiddenPercentage, key.get()));

      uint32_t videoDecodeSuspendPercentage =
          uint32_t(videoDecodeSuspendTime / playTime * 100.0 + 0.5);
      Telemetry::Accumulate(Telemetry::VIDEO_INFERRED_DECODE_SUSPEND_PERCENTAGE,
                            key, videoDecodeSuspendPercentage);
      Telemetry::Accumulate(Telemetry::VIDEO_INFERRED_DECODE_SUSPEND_PERCENTAGE,
                            NS_LITERAL_CSTRING("All"),
                            videoDecodeSuspendPercentage);
      LOG(LogLevel::Debug,
          ("%p VIDEO_INFERRED_DECODE_SUSPEND_PERCENTAGE = %u, keys: '%s' and "
           "'All'",
           this, videoDecodeSuspendPercentage, key.get()));

      if (data.mInterKeyframeCount != 0) {
        uint32_t average_ms = uint32_t(std::min<uint64_t>(
            double(data.mInterKeyframeSum_us) /
                    double(data.mInterKeyframeCount) / 1000.0 +
                0.5,
            UINT32_MAX));
        Telemetry::Accumulate(Telemetry::VIDEO_INTER_KEYFRAME_AVERAGE_MS, key,
                              average_ms);
        Telemetry::Accumulate(Telemetry::VIDEO_INTER_KEYFRAME_AVERAGE_MS,
                              NS_LITERAL_CSTRING("All"), average_ms);
        LOG(LogLevel::Debug,
            ("%p VIDEO_INTER_KEYFRAME_AVERAGE_MS = %u, keys: '%s' and 'All'",
             this, average_ms, key.get()));

        uint32_t max_ms = uint32_t(std::min<uint64_t>(
            (data.mInterKeyFrameMax_us + 500) / 1000, UINT32_MAX));
        Telemetry::Accumulate(Telemetry::VIDEO_INTER_KEYFRAME_MAX_MS, key,
                              max_ms);
        Telemetry::Accumulate(Telemetry::VIDEO_INTER_KEYFRAME_MAX_MS,
                              NS_LITERAL_CSTRING("All"), max_ms);
        LOG(LogLevel::Debug,
            ("%p VIDEO_INTER_KEYFRAME_MAX_MS = %u, keys: '%s' and 'All'", this,
             max_ms, key.get()));
      } else {
        // Here, we have played *some* of the video, but didn't get more than 1
        // keyframe. Report '0' if we have played for longer than the video-
        // decode-suspend delay (showing recovery would be difficult).
        uint32_t suspendDelay_ms = StaticPrefs::MediaSuspendBkgndVideoDelayMs();
        if (uint32_t(playTime * 1000.0) > suspendDelay_ms) {
          Telemetry::Accumulate(Telemetry::VIDEO_INTER_KEYFRAME_MAX_MS, key, 0);
          Telemetry::Accumulate(Telemetry::VIDEO_INTER_KEYFRAME_MAX_MS,
                                NS_LITERAL_CSTRING("All"), 0);
          LOG(LogLevel::Debug,
              ("%p VIDEO_INTER_KEYFRAME_MAX_MS = 0 (only 1 keyframe), keys: "
               "'%s' and 'All'",
               this, key.get()));
        }
      }
    }
  }
}

void HTMLMediaElement::UnbindFromTree(bool aDeep, bool aNullParent) {
  mUnboundFromTree = true;
  mVisibilityState = Visibility::Untracked;

  if (IsInComposedDoc()) {
    NotifyUAWidgetTeardown();
  }

  nsGenericHTMLElement::UnbindFromTree(aDeep, aNullParent);

  MOZ_ASSERT(IsHidden());
  NotifyDecoderActivityChanges();

  RefPtr<HTMLMediaElement> self(this);
  nsCOMPtr<nsIRunnable> task =
      NS_NewRunnableFunction("dom::HTMLMediaElement::UnbindFromTree", [self]() {
        if (self->mUnboundFromTree) {
          self->Pause();
        }
      });
  RunInStableState(task);
}

/* static */
CanPlayStatus HTMLMediaElement::GetCanPlay(
    const nsAString& aType, DecoderDoctorDiagnostics* aDiagnostics) {
  Maybe<MediaContainerType> containerType = MakeMediaContainerType(aType);
  if (!containerType) {
    return CANPLAY_NO;
  }
  CanPlayStatus status =
      DecoderTraits::CanHandleContainerType(*containerType, aDiagnostics);
  if (status == CANPLAY_YES &&
      (*containerType).ExtendedType().Codecs().IsEmpty()) {
    // Per spec: 'Generally, a user agent should never return "probably" for a
    // type that allows the `codecs` parameter if that parameter is not
    // present.' As all our currently-supported types allow for `codecs`, we can
    // do this check here.
    // TODO: Instead, missing `codecs` should be checked in each decoder's
    // `IsSupportedType` call from `CanHandleCodecsType()`.
    // See bug 1399023.
    return CANPLAY_MAYBE;
  }
  return status;
}

void HTMLMediaElement::CanPlayType(const nsAString& aType, nsAString& aResult) {
  DecoderDoctorDiagnostics diagnostics;
  CanPlayStatus canPlay = GetCanPlay(aType, &diagnostics);
  diagnostics.StoreFormatDiagnostics(OwnerDoc(), aType, canPlay != CANPLAY_NO,
                                     __func__);
  switch (canPlay) {
    case CANPLAY_NO:
      aResult.Truncate();
      break;
    case CANPLAY_YES:
      aResult.AssignLiteral("probably");
      break;
    case CANPLAY_MAYBE:
      aResult.AssignLiteral("maybe");
      break;
    default:
      MOZ_ASSERT_UNREACHABLE("Unexpected case.");
      break;
  }

  LOG(LogLevel::Debug,
      ("%p CanPlayType(%s) = \"%s\"", this, NS_ConvertUTF16toUTF8(aType).get(),
       NS_ConvertUTF16toUTF8(aResult).get()));
}

void HTMLMediaElement::AssertReadyStateIsNothing() {
#ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED
  if (mReadyState != HAVE_NOTHING) {
    char buf[1024];
    SprintfLiteral(buf,
                   "readyState=%d networkState=%d mLoadWaitStatus=%d "
                   "mSourceLoadCandidate=%d "
                   "mIsLoadingFromSourceChildren=%d mPreloadAction=%d "
                   "mSuspendedForPreloadNone=%d error=%d",
                   int(mReadyState), int(mNetworkState), int(mLoadWaitStatus),
                   !!mSourceLoadCandidate, mIsLoadingFromSourceChildren,
                   int(mPreloadAction), mSuspendedForPreloadNone,
                   GetError() ? GetError()->Code() : 0);
    MOZ_CRASH_UNSAFE_PRINTF("ReadyState should be HAVE_NOTHING! %s", buf);
  }
#endif
}

nsresult HTMLMediaElement::InitializeDecoderAsClone(
    ChannelMediaDecoder* aOriginal) {
  NS_ASSERTION(mLoadingSrc, "mLoadingSrc must already be set");
  NS_ASSERTION(mDecoder == nullptr, "Shouldn't have a decoder");
  AssertReadyStateIsNothing();

  MediaDecoderInit decoderInit(
      this, mMuted ? 0.0 : mVolume, mPreservesPitch,
      ClampPlaybackRate(mPlaybackRate),
      mPreloadAction == HTMLMediaElement::PRELOAD_METADATA, mHasSuspendTaint,
      HasAttr(kNameSpaceID_None, nsGkAtoms::loop), aOriginal->ContainerType());

  RefPtr<ChannelMediaDecoder> decoder = aOriginal->Clone(decoderInit);
  if (!decoder) return NS_ERROR_FAILURE;

  LOG(LogLevel::Debug,
      ("%p Cloned decoder %p from %p", this, decoder.get(), aOriginal));

  return FinishDecoderSetup(decoder);
}

template <typename DecoderType, typename... LoadArgs>
nsresult HTMLMediaElement::SetupDecoder(DecoderType* aDecoder,
                                        LoadArgs&&... aArgs) {
  LOG(LogLevel::Debug, ("%p Created decoder %p for type %s", this, aDecoder,
                        aDecoder->ContainerType().OriginalString().Data()));

  nsresult rv = aDecoder->Load(std::forward<LoadArgs>(aArgs)...);
  if (NS_FAILED(rv)) {
    aDecoder->Shutdown();
    LOG(LogLevel::Debug, ("%p Failed to load for decoder %p", this, aDecoder));
    return rv;
  }

  rv = FinishDecoderSetup(aDecoder);
  // Only ChannelMediaDecoder supports resource cloning.
  if (IsSame<DecoderType, ChannelMediaDecoder>::value && NS_SUCCEEDED(rv)) {
    AddMediaElementToURITable();
    NS_ASSERTION(
        MediaElementTableCount(this, mLoadingSrc) == 1,
        "Media element should have single table entry if decode initialized");
  }

  return rv;
}

nsresult HTMLMediaElement::InitializeDecoderForChannel(
    nsIChannel* aChannel, nsIStreamListener** aListener) {
  NS_ASSERTION(mLoadingSrc, "mLoadingSrc must already be set");
  AssertReadyStateIsNothing();

  DecoderDoctorDiagnostics diagnostics;

  nsAutoCString mimeType;
  aChannel->GetContentType(mimeType);
  NS_ASSERTION(!mimeType.IsEmpty(), "We should have the Content-Type.");
  NS_ConvertUTF8toUTF16 mimeUTF16(mimeType);

  RefPtr<HTMLMediaElement> self = this;
  auto reportCanPlay = [&, self](bool aCanPlay) {
    diagnostics.StoreFormatDiagnostics(self->OwnerDoc(), mimeUTF16, aCanPlay,
                                       __func__);
    if (!aCanPlay) {
      nsAutoString src;
      self->GetCurrentSrc(src);
      const char16_t* params[] = {mimeUTF16.get(), src.get()};
      self->ReportLoadError("MediaLoadUnsupportedMimeType", params,
                            ArrayLength(params));
    }
  };

  auto onExit = MakeScopeExit([self] {
    if (self->mChannelLoader) {
      self->mChannelLoader->Done();
      self->mChannelLoader = nullptr;
    }
  });

  Maybe<MediaContainerType> containerType = MakeMediaContainerType(mimeType);
  if (!containerType) {
    reportCanPlay(false);
    return NS_ERROR_FAILURE;
  }

  MediaDecoderInit decoderInit(
      this, mMuted ? 0.0 : mVolume, mPreservesPitch,
      ClampPlaybackRate(mPlaybackRate),
      mPreloadAction == HTMLMediaElement::PRELOAD_METADATA, mHasSuspendTaint,
      HasAttr(kNameSpaceID_None, nsGkAtoms::loop), *containerType);

#ifdef MOZ_ANDROID_HLS_SUPPORT
  if (HLSDecoder::IsSupportedType(*containerType)) {
    RefPtr<HLSDecoder> decoder = HLSDecoder::Create(decoderInit);
    if (!decoder) {
      reportCanPlay(false);
      return NS_ERROR_OUT_OF_MEMORY;
    }
    reportCanPlay(true);
    return SetupDecoder(decoder.get(), aChannel);
  }
#endif

  RefPtr<ChannelMediaDecoder> decoder =
      ChannelMediaDecoder::Create(decoderInit, &diagnostics);
  if (!decoder) {
    reportCanPlay(false);
    return NS_ERROR_FAILURE;
  }

  reportCanPlay(true);
  bool isPrivateBrowsing = NodePrincipal()->GetPrivateBrowsingId() > 0;
  return SetupDecoder(decoder.get(), aChannel, isPrivateBrowsing, aListener);
}

nsresult HTMLMediaElement::FinishDecoderSetup(MediaDecoder* aDecoder) {
  ChangeNetworkState(NETWORK_LOADING);

  // Set mDecoder now so if methods like GetCurrentSrc get called between
  // here and Load(), they work.
  SetDecoder(aDecoder);

  // Notify the decoder of the initial activity status.
  NotifyDecoderActivityChanges();

  // Update decoder principal before we start decoding, since it
  // can affect how we feed data to MediaStreams
  NotifyDecoderPrincipalChanged();

  // Set sink device if we have one. Otherwise the default is used.
  if (mSink.second()) {
    mDecoder
        ->SetSink(mSink.second())
#ifdef DEBUG
        ->Then(mAbstractMainThread, __func__,
               [](const GenericPromise::ResolveOrRejectValue& aValue) {
                 MOZ_ASSERT(aValue.IsResolve() && !aValue.ResolveValue());
               });
#else
        ;
#endif
  }

  // Set CORSMode now before any streams are added. It won't change over time.
  mDecoder->SetOutputStreamCORSMode(mCORSMode);

  if (!mOutputStreams.IsEmpty()) {
    mDecoder->SetNextOutputStreamTrackID(
        mNextAvailableMediaDecoderOutputTrackID);
  }

  for (OutputMediaStream& ms : mOutputStreams) {
    if (ms.mCapturingMediaStream) {
      MOZ_ASSERT(!ms.mCapturingDecoder);
      continue;
    }

    ms.mCapturingDecoder = true;
    aDecoder->AddOutputStream(ms.mStream);
  }

  if (mMediaKeys) {
    if (mMediaKeys->GetCDMProxy()) {
      mDecoder->SetCDMProxy(mMediaKeys->GetCDMProxy());
    } else {
      // CDM must have crashed.
      ShutdownDecoder();
      return NS_ERROR_FAILURE;
    }
  }

  if (mChannelLoader) {
    mChannelLoader->Done();
    mChannelLoader = nullptr;
  }

  // We may want to suspend the new stream now.
  // This will also do an AddRemoveSelfReference.
  NotifyOwnerDocumentActivityChanged();

  if (mPausedForInactiveDocumentOrChannel) {
    mDecoder->Suspend();
  }

  if (!mPaused) {
    SetPlayedOrSeeked(true);
    if (!mPausedForInactiveDocumentOrChannel) {
      mDecoder->Play();
    }
  }

  MaybeBeginCloningVisually();

  return NS_OK;
}

class HTMLMediaElement::MediaStreamTrackListener
    : public DOMMediaStream::TrackListener {
 public:
  explicit MediaStreamTrackListener(HTMLMediaElement* aElement)
      : mElement(aElement) {}

  void NotifyTrackAdded(const RefPtr<MediaStreamTrack>& aTrack) override {
    if (!mElement) {
      return;
    }
    mElement->NotifyMediaStreamTrackAdded(aTrack);
  }

  void NotifyTrackRemoved(const RefPtr<MediaStreamTrack>& aTrack) override {
    if (!mElement) {
      return;
    }
    mElement->NotifyMediaStreamTrackRemoved(aTrack);
  }

  void NotifyActive() override {
    if (!mElement) {
      return;
    }
    LOG(LogLevel::Debug, ("%p, mSrcStream %p became active", mElement.get(),
                          mElement->mSrcStream.get()));
    mElement->CheckAutoplayDataReady();
  }

  void NotifyInactive() override {
    if (!mElement) {
      return;
    }
    if (mElement->IsPlaybackEnded()) {
      return;
    }
    LOG(LogLevel::Debug, ("%p, mSrcStream %p became inactive", mElement.get(),
                          mElement->mSrcStream.get()));
    MOZ_ASSERT(!mElement->mSrcStream->Active());
    mElement->PlaybackEnded();
    mElement->UpdateReadyStateInternal();
  }

 protected:
  const WeakPtr<HTMLMediaElement> mElement;
};

void HTMLMediaElement::UpdateSrcMediaStreamPlaying(uint32_t aFlags) {
  if (!mSrcStream) {
    return;
  }
  // We might be in cycle collection with mSrcStream->GetPlaybackStream()
  // already returning null due to unlinking.

  MediaStream* stream = GetSrcMediaStream();
  MediaStreamGraph* graph = stream ? stream->Graph() : nullptr;
  bool shouldPlay = !(aFlags & REMOVING_SRC_STREAM) && !mPaused &&
                    !mPausedForInactiveDocumentOrChannel && stream;
  if (shouldPlay == mSrcStreamIsPlaying) {
    return;
  }
  mSrcStreamIsPlaying = shouldPlay;

  LOG(LogLevel::Debug,
      ("MediaElement %p %s playback of DOMMediaStream %p", this,
       shouldPlay ? "Setting up" : "Removing", mSrcStream.get()));

  if (shouldPlay) {
    mSrcStreamPlaybackEnded = false;
    mSrcStreamGraphTimeOffset +=
        graph->CurrentTime() - mSrcStreamPausedGraphTime.ref();
    mSrcStreamPausedGraphTime = Nothing();

    mWatchManager.Watch(graph->CurrentTime(),
                        &HTMLMediaElement::UpdateSrcStreamTime);

    stream->AddAudioOutput(this);
    SetVolumeInternal();
    if (mSink.second()) {
      NS_WARNING(
          "setSinkId() when playing a MediaStream is not supported yet and "
          "will be ignored");
    }

    VideoFrameContainer* container = GetVideoFrameContainer();
    if (mSelectedVideoStreamTrack && container) {
      mSelectedVideoStreamTrack->AddVideoOutput(container);
      MaybeBeginCloningVisually();
    }

    SetCapturedOutputStreamsEnabled(true);  // Unmute
    // If the input is a media stream, we don't check its data and always regard
    // it as audible when it's playing.
    SetAudibleState(true);
  } else {
    if (stream) {
      MOZ_DIAGNOSTIC_ASSERT(mSrcStreamPausedGraphTime.isNothing());
      mSrcStreamPausedGraphTime = Some(graph->CurrentTime().Ref());

      mWatchManager.Unwatch(graph->CurrentTime(),
                            &HTMLMediaElement::UpdateSrcStreamTime);

      stream->RemoveAudioOutput(this);
      VideoFrameContainer* container = GetVideoFrameContainer();
      if (mSelectedVideoStreamTrack && container) {
        mSelectedVideoStreamTrack->RemoveVideoOutput(container);
      }

      SetCapturedOutputStreamsEnabled(false);  // Mute
    }
    // If stream is null, then DOMMediaStream::Destroy must have been
    // called and that will remove all listeners/outputs.
  }
}

void HTMLMediaElement::UpdateSrcStreamTime() {
  MOZ_ASSERT(NS_IsMainThread());

  if (mSrcStreamPlaybackEnded) {
    // We do a separate FireTimeUpdate() when this is set.
    return;
  }

  FireTimeUpdate(true);
}

void HTMLMediaElement::SetupSrcMediaStreamPlayback(DOMMediaStream* aStream) {
  NS_ASSERTION(!mSrcStream && !mVideoFrameListener,
               "Should have been ended already");

  mSrcStream = aStream;

  nsPIDOMWindowInner* window = OwnerDoc()->GetInnerWindow();
  if (!window) {
    return;
  }

  mSrcStreamPausedGraphTime = Some(0);
  if (MediaStream* stream = GetSrcMediaStream()) {
    if (MediaStreamGraph* graph = stream->Graph()) {
      // The current graph time will represent 0 for this media element.
      mSrcStreamGraphTimeOffset = graph->CurrentTime();
      mSrcStreamPausedGraphTime = Some(mSrcStreamGraphTimeOffset);
    }
  }

  UpdateSrcMediaStreamPlaying();

  // If we pause this media element, track changes in the underlying stream
  // will continue to fire events at this element and alter its track list.
  // That's simpler than delaying the events, but probably confusing...
  nsTArray<RefPtr<MediaStreamTrack>> tracks;
  mSrcStream->GetTracks(tracks);
  for (const RefPtr<MediaStreamTrack>& track : tracks) {
    NotifyMediaStreamTrackAdded(track);
  }

  mMediaStreamTrackListener = MakeUnique<MediaStreamTrackListener>(this);
  mSrcStream->RegisterTrackListener(mMediaStreamTrackListener.get());

  mSrcStream->AddPrincipalChangeObserver(this);
  mSrcStreamVideoPrincipal = mSrcStream->GetVideoPrincipal();

  ChangeNetworkState(NETWORK_IDLE);
  ChangeDelayLoadStatus(false);
  CheckAutoplayDataReady();

  // FirstFrameLoaded() will be called when the stream has tracks.
}

void HTMLMediaElement::EndSrcMediaStreamPlayback() {
  MOZ_ASSERT(mSrcStream);

  UpdateSrcMediaStreamPlaying(REMOVING_SRC_STREAM);

  if (mVideoFrameListener) {
    MOZ_ASSERT(mSelectedVideoStreamTrack);
    if (mSelectedVideoStreamTrack) {
      mSelectedVideoStreamTrack->RemoveDirectListener(mVideoFrameListener);
    }
    mVideoFrameListener->Forget();
  }
  mSelectedVideoStreamTrack = nullptr;
  mVideoFrameListener = nullptr;

  mSrcStream->UnregisterTrackListener(mMediaStreamTrackListener.get());
  mMediaStreamTrackListener = nullptr;
  mSrcStreamTracksAvailable = false;

  mSrcStream->RemovePrincipalChangeObserver(this);
  mSrcStreamVideoPrincipal = nullptr;

  for (OutputMediaStream& ms : mOutputStreams) {
    for (auto pair : ms.mTrackPorts) {
      pair.second()->Destroy();
    }
    ms.mTrackPorts.Clear();
  }

  mSrcStream = nullptr;
}

static already_AddRefed<AudioTrack> CreateAudioTrack(
    AudioStreamTrack* aStreamTrack, nsIGlobalObject* aOwnerGlobal) {
  nsAutoString id;
  nsAutoString label;
  aStreamTrack->GetId(id);
  aStreamTrack->GetLabel(label, CallerType::System);

  return MediaTrackList::CreateAudioTrack(
      aOwnerGlobal, id, NS_LITERAL_STRING("main"), label, EmptyString(), true);
}

static already_AddRefed<VideoTrack> CreateVideoTrack(
    VideoStreamTrack* aStreamTrack, nsIGlobalObject* aOwnerGlobal) {
  nsAutoString id;
  nsAutoString label;
  aStreamTrack->GetId(id);
  aStreamTrack->GetLabel(label, CallerType::System);

  return MediaTrackList::CreateVideoTrack(aOwnerGlobal, id,
                                          NS_LITERAL_STRING("main"), label,
                                          EmptyString(), aStreamTrack);
}

void HTMLMediaElement::NotifyMediaStreamTrackAdded(
    const RefPtr<MediaStreamTrack>& aTrack) {
  MOZ_ASSERT(aTrack);

  if (aTrack->Ended()) {
    return;
  }

#ifdef DEBUG
  nsString id;
  aTrack->GetId(id);

  LOG(LogLevel::Debug, ("%p, Adding %sTrack with id %s", this,
                        aTrack->AsAudioStreamTrack() ? "Audio" : "Video",
                        NS_ConvertUTF16toUTF8(id).get()));
#endif

  if (AudioStreamTrack* t = aTrack->AsAudioStreamTrack()) {
    MOZ_DIAGNOSTIC_ASSERT(AudioTracks(), "Element can't have been unlinked");
    RefPtr<AudioTrack> audioTrack =
        CreateAudioTrack(t, AudioTracks()->GetOwnerGlobal());
    AudioTracks()->AddTrack(audioTrack);
  } else if (VideoStreamTrack* t = aTrack->AsVideoStreamTrack()) {
    // TODO: Fix this per the spec on bug 1273443.
    if (!IsVideo()) {
      return;
    }
    MOZ_DIAGNOSTIC_ASSERT(VideoTracks(), "Element can't have been unlinked");
    RefPtr<VideoTrack> videoTrack =
        CreateVideoTrack(t, VideoTracks()->GetOwnerGlobal());
    VideoTracks()->AddTrack(videoTrack);
    // New MediaStreamTrack added, set the new added video track as selected
    // video track when there is no selected track.
    if (VideoTracks()->SelectedIndex() == -1) {
      MOZ_ASSERT(!mSelectedVideoStreamTrack);
      videoTrack->SetEnabledInternal(true, MediaTrack::FIRE_NO_EVENTS);
    }
  }

  UpdateReadyStateInternal();

  if (!mSrcStreamTracksAvailable) {
    mAbstractMainThread->Dispatch(NS_NewRunnableFunction(
        "HTMLMediaElement::NotifyMediaStreamTrackAdded->FirstFrameLoaded",
        [this, self = RefPtr<HTMLMediaElement>(this), stream = mSrcStream]() {
          if (!mSrcStream || mSrcStream != stream) {
            return;
          }

          LOG(LogLevel::Debug,
              ("MediaElement %p MediaStream tracks available", this));

          mSrcStreamTracksAvailable = true;

          FirstFrameLoaded();
          UpdateReadyStateInternal();
        }));
  }
}

void HTMLMediaElement::NotifyMediaStreamTrackRemoved(
    const RefPtr<MediaStreamTrack>& aTrack) {
  MOZ_ASSERT(aTrack);

  nsAutoString id;
  aTrack->GetId(id);

  LOG(LogLevel::Debug, ("%p, Removing %sTrack with id %s", this,
                        aTrack->AsAudioStreamTrack() ? "Audio" : "Video",
                        NS_ConvertUTF16toUTF8(id).get()));

  MOZ_DIAGNOSTIC_ASSERT(AudioTracks() && VideoTracks(),
                        "Element can't have been unlinked");
  if (MediaTrack* t = AudioTracks()->GetTrackById(id)) {
    AudioTracks()->RemoveTrack(t);
  } else if (MediaTrack* t = VideoTracks()->GetTrackById(id)) {
    VideoTracks()->RemoveTrack(t);
  } else {
    NS_ASSERTION(aTrack->AsVideoStreamTrack() && !IsVideo(),
                 "MediaStreamTrack ended but did not exist in track lists. "
                 "This is only allowed if a video element ends and we are an "
                 "audio element.");
    return;
  }
}

void HTMLMediaElement::ProcessMediaFragmentURI() {
  nsMediaFragmentURIParser parser(mLoadingSrc);

  if (mDecoder && parser.HasEndTime()) {
    mFragmentEnd = parser.GetEndTime();
  }

  if (parser.HasStartTime()) {
    SetCurrentTime(parser.GetStartTime());
    mFragmentStart = parser.GetStartTime();
  }
}

void HTMLMediaElement::MetadataLoaded(const MediaInfo* aInfo,
                                      UniquePtr<const MetadataTags> aTags) {
  MOZ_ASSERT(NS_IsMainThread());

  SetMediaInfo(*aInfo);

  mIsEncrypted =
      aInfo->IsEncrypted() || mPendingEncryptedInitData.IsEncrypted();
  mTags = std::move(aTags);
  mLoadedDataFired = false;
  ChangeReadyState(HAVE_METADATA);

  DispatchAsyncEvent(NS_LITERAL_STRING("durationchange"));
  if (IsVideo() && HasVideo()) {
    DispatchAsyncEvent(NS_LITERAL_STRING("resize"));
  }
  NS_ASSERTION(!HasVideo() || (mMediaInfo.mVideo.mDisplay.width > 0 &&
                               mMediaInfo.mVideo.mDisplay.height > 0),
               "Video resolution must be known on 'loadedmetadata'");
  DispatchAsyncEvent(NS_LITERAL_STRING("loadedmetadata"));

  if (mBlockedAsWithoutMetadata && !HasAudio()) {
    mBlockedAsWithoutMetadata = false;
    ScalarAdd(
        Telemetry::ScalarID::MEDIA_BLOCKED_NO_METADATA_ENDUP_NO_AUDIO_TRACK, 1);
  }

  if (mDecoder && mDecoder->IsTransportSeekable() &&
      mDecoder->IsMediaSeekable()) {
    ProcessMediaFragmentURI();
    mDecoder->SetFragmentEndTime(mFragmentEnd);
  }
  if (mIsEncrypted) {
    // We only support playback of encrypted content via MSE by default.
    if (!mMediaSource && Preferences::GetBool("media.eme.mse-only", true)) {
      DecodeError(
          MediaResult(NS_ERROR_DOM_MEDIA_FATAL_ERR,
                      "Encrypted content not supported outside of MSE"));
      return;
    }

    // Dispatch a distinct 'encrypted' event for each initData we have.
    for (const auto& initData : mPendingEncryptedInitData.mInitDatas) {
      DispatchEncrypted(initData.mInitData, initData.mType);
    }
    mPendingEncryptedInitData.Reset();
  }

  if (IsVideo() && aInfo->HasVideo()) {
    // We are a video element playing video so update the screen wakelock
    NotifyOwnerDocumentActivityChanged();
  }

  if (mDefaultPlaybackStartPosition != 0.0) {
    SetCurrentTime(mDefaultPlaybackStartPosition);
    mDefaultPlaybackStartPosition = 0.0;
  }

  UpdateReadyStateInternal();

  if (!mSrcStream) {
    return;
  }

  for (OutputMediaStream& ms : mOutputStreams) {
    if (AudioTracks()) {
      for (size_t i = 0; i < AudioTracks()->Length(); ++i) {
        AudioTrack* t = (*AudioTracks())[i];
        if (t->Enabled()) {
          AddCaptureMediaTrackToOutputStream(t, ms);
        }
      }
    }
    if (VideoTracks() && IsVideo() && !ms.mCapturingAudioOnly) {
      // Only add video tracks if we're a video element and the output stream
      // wants video.
      for (size_t i = 0; i < VideoTracks()->Length(); ++i) {
        VideoTrack* t = (*VideoTracks())[i];
        if (t->Selected()) {
          AddCaptureMediaTrackToOutputStream(t, ms);
        }
      }
    }
  }
}

void HTMLMediaElement::FirstFrameLoaded() {
  LOG(LogLevel::Debug,
      ("%p, FirstFrameLoaded() mFirstFrameLoaded=%d mWaitingForKey=%d", this,
       mFirstFrameLoaded, mWaitingForKey));

  NS_ASSERTION(!mSuspendedAfterFirstFrame, "Should not have already suspended");

  if (!mFirstFrameLoaded) {
    mFirstFrameLoaded = true;
    UpdateReadyStateInternal();
  }

  ChangeDelayLoadStatus(false);

  if (mDecoder && mAllowSuspendAfterFirstFrame && mPaused &&
      !HasAttr(kNameSpaceID_None, nsGkAtoms::autoplay) &&
      mPreloadAction == HTMLMediaElement::PRELOAD_METADATA) {
    mSuspendedAfterFirstFrame = true;
    mDecoder->Suspend();
  }
}

void HTMLMediaElement::NetworkError(const MediaResult& aError) {
  if (mReadyState == HAVE_NOTHING) {
    NoSupportedMediaSourceError(aError.Description());
  } else {
    Error(MEDIA_ERR_NETWORK);
  }
}

void HTMLMediaElement::DecodeError(const MediaResult& aError) {
  nsAutoString src;
  GetCurrentSrc(src);
  const char16_t* params[] = {src.get()};
  ReportLoadError("MediaLoadDecodeError", params, ArrayLength(params));

  DecoderDoctorDiagnostics diagnostics;
  diagnostics.StoreDecodeError(OwnerDoc(), aError, src, __func__);

  if (AudioTracks()) {
    AudioTracks()->EmptyTracks();
  }
  if (VideoTracks()) {
    VideoTracks()->EmptyTracks();
  }
  if (mIsLoadingFromSourceChildren) {
    mErrorSink->ResetError();
    if (mSourceLoadCandidate) {
      DispatchAsyncSourceError(mSourceLoadCandidate);
      QueueLoadFromSourceTask();
    } else {
      NS_WARNING("Should know the source we were loading from!");
    }
  } else if (mReadyState == HAVE_NOTHING) {
    NoSupportedMediaSourceError(aError.Description());
  } else {
    Error(MEDIA_ERR_DECODE, aError.Description());
  }
}

void HTMLMediaElement::DecodeWarning(const MediaResult& aError) {
  nsAutoString src;
  GetCurrentSrc(src);
  DecoderDoctorDiagnostics diagnostics;
  diagnostics.StoreDecodeWarning(OwnerDoc(), aError, src, __func__);
}

bool HTMLMediaElement::HasError() const { return GetError(); }

void HTMLMediaElement::LoadAborted() { Error(MEDIA_ERR_ABORTED); }

void HTMLMediaElement::Error(uint16_t aErrorCode,
                             const nsACString& aErrorDetails) {
  mErrorSink->SetError(aErrorCode, aErrorDetails);
  ChangeDelayLoadStatus(false);
  UpdateAudioChannelPlayingState();
}

void HTMLMediaElement::PlaybackEnded() {
  // We changed state which can affect AddRemoveSelfReference
  AddRemoveSelfReference();

  NS_ASSERTION(!mDecoder || mDecoder->IsEnded(),
               "Decoder fired ended, but not in ended state");

  DiscardFinishWhenEndedOutputStreams();

  if (mSrcStream) {
    LOG(LogLevel::Debug,
        ("%p, got duration by reaching the end of the resource", this));
    mSrcStreamPlaybackEnded = true;
    DispatchAsyncEvent(NS_LITERAL_STRING("durationchange"));
  }

  if (HasAttr(kNameSpaceID_None, nsGkAtoms::loop)) {
    SetCurrentTime(0);
    return;
  }

  FireTimeUpdate(false);

  if (!mPaused) {
    Pause();
  }

  if (mSrcStream) {
    // A MediaStream that goes from inactive to active shall be eligible for
    // autoplay again according to the mediacapture-main spec.
    mAutoplaying = true;
  }

  DispatchAsyncEvent(NS_LITERAL_STRING("ended"));
}

void HTMLMediaElement::SeekStarted() {
  DispatchAsyncEvent(NS_LITERAL_STRING("seeking"));
}

void HTMLMediaElement::SeekCompleted() {
  mPlayingBeforeSeek = false;
  SetPlayedOrSeeked(true);
  if (mTextTrackManager) {
    mTextTrackManager->DidSeek();
  }
  FireTimeUpdate(false);
  DispatchAsyncEvent(NS_LITERAL_STRING("seeked"));
  // We changed whether we're seeking so we need to AddRemoveSelfReference
  AddRemoveSelfReference();
  if (mCurrentPlayRangeStart == -1.0) {
    mCurrentPlayRangeStart = CurrentTime();
  }

  // After seeking completed, if the audio track is silent, start another new
  // silence range.
  mHasAccumulatedSilenceRangeBeforeSeekEnd = false;
  if (IsAudioTrackCurrentlySilent()) {
    UpdateAudioTrackSilenceRange(mIsAudioTrackAudible);
  }
}

void HTMLMediaElement::NotifySuspendedByCache(bool aSuspendedByCache) {
  mDownloadSuspendedByCache = aSuspendedByCache;
  UpdateReadyStateInternal();
}

void HTMLMediaElement::DownloadSuspended() {
  if (mNetworkState == NETWORK_LOADING) {
    DispatchAsyncEvent(NS_LITERAL_STRING("progress"));
  }
  ChangeNetworkState(NETWORK_IDLE);
}

void HTMLMediaElement::DownloadResumed() {
  ChangeNetworkState(NETWORK_LOADING);
}

void HTMLMediaElement::CheckProgress(bool aHaveNewProgress) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mNetworkState == NETWORK_LOADING);

  TimeStamp now = TimeStamp::NowLoRes();

  if (aHaveNewProgress) {
    mDataTime = now;
  }

  // If this is the first progress, or PROGRESS_MS has passed since the last
  // progress event fired and more data has arrived since then, fire a
  // progress event.
  NS_ASSERTION(
      (mProgressTime.IsNull() && !aHaveNewProgress) || !mDataTime.IsNull(),
      "null TimeStamp mDataTime should not be used in comparison");
  if (mProgressTime.IsNull()
          ? aHaveNewProgress
          : (now - mProgressTime >=
                 TimeDuration::FromMilliseconds(PROGRESS_MS) &&
             mDataTime > mProgressTime)) {
    DispatchAsyncEvent(NS_LITERAL_STRING("progress"));
    // Resolution() ensures that future data will have now > mProgressTime,
    // and so will trigger another event.  mDataTime is not reset because it
    // is still required to detect stalled; it is similarly offset by
    // resolution to indicate the new data has not yet arrived.
    mProgressTime = now - TimeDuration::Resolution();
    if (mDataTime > mProgressTime) {
      mDataTime = mProgressTime;
    }
    if (!mProgressTimer) {
      NS_ASSERTION(aHaveNewProgress,
                   "timer dispatched when there was no timer");
      // Were stalled.  Restart timer.
      StartProgressTimer();
      if (!mLoadedDataFired) {
        ChangeDelayLoadStatus(true);
      }
    }
    // Download statistics may have been updated, force a recheck of the
    // readyState.
    UpdateReadyStateInternal();
  }

  if (now - mDataTime >= TimeDuration::FromMilliseconds(STALL_MS)) {
    if (!mMediaSource) {
      DispatchAsyncEvent(NS_LITERAL_STRING("stalled"));
    } else {
      ChangeDelayLoadStatus(false);
    }

    NS_ASSERTION(mProgressTimer, "detected stalled without timer");
    // Stop timer events, which prevents repeated stalled events until there
    // is more progress.
    StopProgress();
  }

  AddRemoveSelfReference();
}

/* static */
void HTMLMediaElement::ProgressTimerCallback(nsITimer* aTimer, void* aClosure) {
  auto decoder = static_cast<HTMLMediaElement*>(aClosure);
  decoder->CheckProgress(false);
}

void HTMLMediaElement::StartProgressTimer() {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mNetworkState == NETWORK_LOADING);
  NS_ASSERTION(!mProgressTimer, "Already started progress timer.");

  NS_NewTimerWithFuncCallback(
      getter_AddRefs(mProgressTimer), ProgressTimerCallback, this, PROGRESS_MS,
      nsITimer::TYPE_REPEATING_SLACK, "HTMLMediaElement::ProgressTimerCallback",
      mMainThreadEventTarget);
}

void HTMLMediaElement::StartProgress() {
  // Record the time now for detecting stalled.
  mDataTime = TimeStamp::NowLoRes();
  // Reset mProgressTime so that mDataTime is not indicating bytes received
  // after the last progress event.
  mProgressTime = TimeStamp();
  StartProgressTimer();
}

void HTMLMediaElement::StopProgress() {
  MOZ_ASSERT(NS_IsMainThread());
  if (!mProgressTimer) {
    return;
  }

  mProgressTimer->Cancel();
  mProgressTimer = nullptr;
}

void HTMLMediaElement::DownloadProgressed() {
  if (mNetworkState != NETWORK_LOADING) {
    return;
  }
  CheckProgress(true);
}

bool HTMLMediaElement::ShouldCheckAllowOrigin() {
  return mCORSMode != CORS_NONE;
}

bool HTMLMediaElement::IsCORSSameOrigin() {
  bool subsumes;
  RefPtr<nsIPrincipal> principal = GetCurrentPrincipal();
  return (NS_SUCCEEDED(NodePrincipal()->Subsumes(principal, &subsumes)) &&
          subsumes) ||
         ShouldCheckAllowOrigin();
}

void HTMLMediaElement::UpdateReadyStateInternal() {
  if (!mDecoder && !mSrcStream) {
    // Not initialized - bail out.
    LOG(LogLevel::Debug, ("MediaElement %p UpdateReadyStateInternal() "
                          "Not initialized",
                          this));
    return;
  }

  if (mDecoder && mReadyState < HAVE_METADATA) {
    // aNextFrame might have a next frame because the decoder can advance
    // on its own thread before MetadataLoaded gets a chance to run.
    // The arrival of more data can't change us out of this readyState.
    LOG(LogLevel::Debug, ("MediaElement %p UpdateReadyStateInternal() "
                          "Decoder ready state < HAVE_METADATA",
                          this));
    return;
  }

  if (mSrcStream && mReadyState < HAVE_METADATA) {
    if (!mSrcStreamTracksAvailable) {
      LOG(LogLevel::Debug, ("MediaElement %p UpdateReadyStateInternal() "
                            "MediaStreamTracks not available yet",
                            this));
      return;
    }

    bool hasAudioTracks = AudioTracks() && !AudioTracks()->IsEmpty();
    bool hasVideoTracks = VideoTracks() && !VideoTracks()->IsEmpty();
    if (!hasAudioTracks && !hasVideoTracks) {
      LOG(LogLevel::Debug, ("MediaElement %p UpdateReadyStateInternal() "
                            "Stream with no tracks",
                            this));
      // Give it one last chance to remove the self reference if needed.
      AddRemoveSelfReference();
      return;
    }

    if (IsVideo() && hasVideoTracks && !HasVideo()) {
      LOG(LogLevel::Debug, ("MediaElement %p UpdateReadyStateInternal() "
                            "Stream waiting for video",
                            this));
      return;
    }

    LOG(LogLevel::Debug,
        ("MediaElement %p UpdateReadyStateInternal() Stream has "
         "metadata; audioTracks=%d, videoTracks=%d, "
         "hasVideoFrame=%d",
         this, AudioTracks()->Length(), VideoTracks()->Length(), HasVideo()));

    // We are playing a stream that has video and a video frame is now set.
    // This means we have all metadata needed to change ready state.
    MediaInfo mediaInfo = mMediaInfo;
    if (hasAudioTracks) {
      mediaInfo.EnableAudio();
    }
    if (hasVideoTracks) {
      mediaInfo.EnableVideo();
    }
    MetadataLoaded(&mediaInfo, nullptr);
  }

  if (mMediaSource) {
    // readyState has changed, assuming it's following the pending mediasource
    // operations. Notify the Mediasource that the operations have completed.
    mMediaSource->CompletePendingTransactions();
  }

  enum NextFrameStatus nextFrameStatus = NextFrameStatus();
  if (mWaitingForKey == NOT_WAITING_FOR_KEY) {
    if (nextFrameStatus == NEXT_FRAME_UNAVAILABLE && mDecoder &&
        !mDecoder->IsEnded()) {
      nextFrameStatus = mDecoder->NextFrameBufferedStatus();
    }
  } else if (mWaitingForKey == WAITING_FOR_KEY) {
    if (nextFrameStatus == NEXT_FRAME_UNAVAILABLE ||
        nextFrameStatus == NEXT_FRAME_UNAVAILABLE_BUFFERING) {
      // http://w3c.github.io/encrypted-media/#wait-for-key
      // Continuing 7.3.4 Queue a "waitingforkey" Event
      // 4. Queue a task to fire a simple event named waitingforkey
      // at the media element.
      // 5. Set the readyState of media element to HAVE_METADATA.
      // NOTE: We'll change to HAVE_CURRENT_DATA or HAVE_METADATA
      // depending on whether we've loaded the first frame or not
      // below.
      // 6. Suspend playback.
      // Note: Playback will already be stalled, as the next frame is
      // unavailable.
      mWaitingForKey = WAITING_FOR_KEY_DISPATCHED;
      DispatchAsyncEvent(NS_LITERAL_STRING("waitingforkey"));
    }
  } else {
    MOZ_ASSERT(mWaitingForKey == WAITING_FOR_KEY_DISPATCHED);
    if (nextFrameStatus == NEXT_FRAME_AVAILABLE) {
      // We have new frames after dispatching "waitingforkey".
      // This means we've got the key and can reset mWaitingForKey now.
      mWaitingForKey = NOT_WAITING_FOR_KEY;
    }
  }

  if (nextFrameStatus == MediaDecoderOwner::NEXT_FRAME_UNAVAILABLE_SEEKING) {
    LOG(LogLevel::Debug,
        ("MediaElement %p UpdateReadyStateInternal() "
         "NEXT_FRAME_UNAVAILABLE_SEEKING; Forcing HAVE_METADATA",
         this));
    ChangeReadyState(HAVE_METADATA);
    return;
  }

  if (IsVideo() && VideoTracks() && !VideoTracks()->IsEmpty() &&
      !IsPlaybackEnded() && GetImageContainer() &&
      !GetImageContainer()->HasCurrentImage()) {
    // Don't advance if we are playing video, but don't have a video frame.
    // Also, if video became available after advancing to HAVE_CURRENT_DATA
    // while we are still playing, we need to revert to HAVE_METADATA until
    // a video frame is available.
    LOG(LogLevel::Debug,
        ("MediaElement %p UpdateReadyStateInternal() "
         "Playing video but no video frame; Forcing HAVE_METADATA",
         this));
    ChangeReadyState(HAVE_METADATA);
    return;
  }

  if (!mFirstFrameLoaded) {
    // We haven't yet loaded the first frame, making us unable to determine
    // if we have enough valid data at the present stage.
    return;
  }

  if (nextFrameStatus == NEXT_FRAME_UNAVAILABLE_BUFFERING) {
    // Force HAVE_CURRENT_DATA when buffering.
    ChangeReadyState(HAVE_CURRENT_DATA);
    return;
  }

  // TextTracks must be loaded for the HAVE_ENOUGH_DATA and
  // HAVE_FUTURE_DATA.
  // So force HAVE_CURRENT_DATA if text tracks not loaded.
  if (mTextTrackManager && !mTextTrackManager->IsLoaded()) {
    ChangeReadyState(HAVE_CURRENT_DATA);
    return;
  }

  if (mDownloadSuspendedByCache && mDecoder && !mDecoder->IsEnded()) {
    // The decoder has signaled that the download has been suspended by the
    // media cache. So move readyState into HAVE_ENOUGH_DATA, in case there's
    // script waiting for a "canplaythrough" event; without this forced
    // transition, we will never fire the "canplaythrough" event if the
    // media cache is too small, and scripts are bound to fail. Don't force
    // this transition if the decoder is in ended state; the readyState
    // should remain at HAVE_CURRENT_DATA in this case.
    // Note that this state transition includes the case where we finished
    // downloaded the whole data stream.
    LOG(LogLevel::Debug, ("MediaElement %p UpdateReadyStateInternal() "
                          "Decoder download suspended by cache",
                          this));
    ChangeReadyState(HAVE_ENOUGH_DATA);
    return;
  }

  if (nextFrameStatus != MediaDecoderOwner::NEXT_FRAME_AVAILABLE) {
    LOG(LogLevel::Debug, ("MediaElement %p UpdateReadyStateInternal() "
                          "Next frame not available",
                          this));
    ChangeReadyState(HAVE_CURRENT_DATA);
    return;
  }

  if (mSrcStream) {
    LOG(LogLevel::Debug, ("MediaElement %p UpdateReadyStateInternal() "
                          "Stream HAVE_ENOUGH_DATA",
                          this));
    ChangeReadyState(HAVE_ENOUGH_DATA);
    return;
  }

  // Now see if we should set HAVE_ENOUGH_DATA.
  // If it's something we don't know the size of, then we can't
  // make a real estimate, so we go straight to HAVE_ENOUGH_DATA once
  // we've downloaded enough data that our download rate is considered
  // reliable. We have to move to HAVE_ENOUGH_DATA at some point or
  // autoplay elements for live streams will never play. Otherwise we
  // move to HAVE_ENOUGH_DATA if we can play through the entire media
  // without stopping to buffer.
  if (mDecoder->CanPlayThrough()) {
    LOG(LogLevel::Debug, ("MediaElement %p UpdateReadyStateInternal() "
                          "Decoder can play through",
                          this));
    ChangeReadyState(HAVE_ENOUGH_DATA);
    return;
  }
  LOG(LogLevel::Debug, ("MediaElement %p UpdateReadyStateInternal() "
                        "Default; Decoder has future data",
                        this));
  ChangeReadyState(HAVE_FUTURE_DATA);
}

static const char* const gReadyStateToString[] = {
    "HAVE_NOTHING", "HAVE_METADATA", "HAVE_CURRENT_DATA", "HAVE_FUTURE_DATA",
    "HAVE_ENOUGH_DATA"};

void HTMLMediaElement::ChangeReadyState(nsMediaReadyState aState) {
  if (mReadyState == aState) {
    return;
  }

  nsMediaReadyState oldState = mReadyState;
  mReadyState = aState;
  LOG(LogLevel::Debug,
      ("%p Ready state changed to %s", this, gReadyStateToString[aState]));

  DDLOG(DDLogCategory::Property, "ready_state", gReadyStateToString[aState]);

  // https://html.spec.whatwg.org/multipage/media.html#text-track-cue-active-flag
  // The user agent must synchronously unset cues' active flag whenever the
  // media element's readyState is changed back to HAVE_NOTHING.
  if (mReadyState == HAVE_NOTHING && mTextTrackManager) {
    mTextTrackManager->NotifyReset();
  }

  if (mNetworkState == NETWORK_EMPTY) {
    return;
  }

  UpdateAudioChannelPlayingState();

  // Handle raising of "waiting" event during seek (see 4.8.10.9)
  // or
  // 4.8.12.7 Ready states:
  // "If the previous ready state was HAVE_FUTURE_DATA or more, and the new
  // ready state is HAVE_CURRENT_DATA or less
  // If the media element was potentially playing before its readyState
  // attribute changed to a value lower than HAVE_FUTURE_DATA, and the element
  // has not ended playback, and playback has not stopped due to errors,
  // paused for user interaction, or paused for in-band content, the user agent
  // must queue a task to fire a simple event named timeupdate at the element,
  // and queue a task to fire a simple event named waiting at the element."
  if (mPlayingBeforeSeek && mReadyState < HAVE_FUTURE_DATA) {
    DispatchAsyncEvent(NS_LITERAL_STRING("waiting"));
  } else if (oldState >= HAVE_FUTURE_DATA && mReadyState < HAVE_FUTURE_DATA &&
             !Paused() && !Ended() && !mErrorSink->mError) {
    FireTimeUpdate(false);
    DispatchAsyncEvent(NS_LITERAL_STRING("waiting"));
  }

  if (oldState < HAVE_CURRENT_DATA && mReadyState >= HAVE_CURRENT_DATA &&
      !mLoadedDataFired) {
    DispatchAsyncEvent(NS_LITERAL_STRING("loadeddata"));
    mLoadedDataFired = true;
  }

  if (oldState < HAVE_FUTURE_DATA && mReadyState >= HAVE_FUTURE_DATA) {
    DispatchAsyncEvent(NS_LITERAL_STRING("canplay"));
    if (!mPaused) {
      if (mDecoder && !mPausedForInactiveDocumentOrChannel) {
        MOZ_ASSERT(AutoplayPolicy::IsAllowedToPlay(*this));
        mDecoder->Play();
      }
      NotifyAboutPlaying();
    }
  }

  CheckAutoplayDataReady();

  if (oldState < HAVE_ENOUGH_DATA && mReadyState >= HAVE_ENOUGH_DATA) {
    DispatchAsyncEvent(NS_LITERAL_STRING("canplaythrough"));
  }
}

static const char* const gNetworkStateToString[] = {"EMPTY", "IDLE", "LOADING",
                                                    "NO_SOURCE"};

void HTMLMediaElement::ChangeNetworkState(nsMediaNetworkState aState) {
  if (mNetworkState == aState) {
    return;
  }

  nsMediaNetworkState oldState = mNetworkState;
  mNetworkState = aState;
  LOG(LogLevel::Debug,
      ("%p Network state changed to %s", this, gNetworkStateToString[aState]));
  DDLOG(DDLogCategory::Property, "network_state",
        gNetworkStateToString[aState]);

  if (oldState == NETWORK_LOADING) {
    // Stop progress notification when exiting NETWORK_LOADING.
    StopProgress();
  }

  if (mNetworkState == NETWORK_LOADING) {
    // Start progress notification when entering NETWORK_LOADING.
    StartProgress();
  } else if (mNetworkState == NETWORK_IDLE && !mErrorSink->mError) {
    // Fire 'suspend' event when entering NETWORK_IDLE and no error presented.
    DispatchAsyncEvent(NS_LITERAL_STRING("suspend"));
  }

  // According to the resource selection (step2, step9-18), dedicated media
  // source failure step (step4) and aborting existing load (step4), set show
  // poster flag to true. https://html.spec.whatwg.org/multipage/media.html
  if (mNetworkState == NETWORK_NO_SOURCE || mNetworkState == NETWORK_EMPTY) {
    mShowPoster = true;
  }

  // Changing mNetworkState affects AddRemoveSelfReference().
  AddRemoveSelfReference();
}

bool HTMLMediaElement::CanActivateAutoplay() {
  // For stream inputs, we activate autoplay on HAVE_NOTHING because
  // this element itself might be blocking the stream from making progress by
  // being paused. We only check that it has data by checking its active state.
  // We also activate autoplay when playing a media source since the data
  // download is controlled by the script and there is no way to evaluate
  // MediaDecoder::CanPlayThrough().

  if (!HasAttr(kNameSpaceID_None, nsGkAtoms::autoplay)) {
    return false;
  }

  if (!mAutoplaying) {
    return false;
  }

  if (IsEditable()) {
    return false;
  }

  if (!mPaused) {
    return false;
  }

  if (mPausedForInactiveDocumentOrChannel) {
    return false;
  }

  // Static document is used for print preview and printing, should not be
  // autoplay
  if (OwnerDoc()->IsStaticDocument()) {
    return false;
  }

  if (mAudioChannelWrapper) {
    // Note: SUSPENDED_PAUSE and SUSPENDED_BLOCK will be merged into one single
    // state.
    if (mAudioChannelWrapper->GetSuspendType() ==
            nsISuspendedTypes::SUSPENDED_PAUSE ||
        mAudioChannelWrapper->GetSuspendType() ==
            nsISuspendedTypes::SUSPENDED_BLOCK ||
        mAudioChannelWrapper->IsPlaybackBlocked()) {
      return false;
    }
  }

  bool hasData = (mDecoder && mReadyState >= HAVE_ENOUGH_DATA) ||
                 (mSrcStream && mSrcStream->Active());

  return hasData;
}

void HTMLMediaElement::CheckAutoplayDataReady() {
  if (!CanActivateAutoplay()) {
    return;
  }

  UpdateHadAudibleAutoplayState();
  if (!AutoplayPolicy::IsAllowedToPlay(*this)) {
    DispatchEventsWhenPlayWasNotAllowed();
    return;
  }

  mAllowedToPlayPromise.ResolveIfExists(true, __func__);
  mPaused = false;
  // We changed mPaused which can affect AddRemoveSelfReference
  AddRemoveSelfReference();
  UpdateSrcMediaStreamPlaying();
  UpdateAudioChannelPlayingState();

  if (mDecoder) {
    SetPlayedOrSeeked(true);
    if (mCurrentPlayRangeStart == -1.0) {
      mCurrentPlayRangeStart = CurrentTime();
    }
    MOZ_ASSERT(!mPausedForInactiveDocumentOrChannel);
    mDecoder->Play();
  } else if (mSrcStream) {
    SetPlayedOrSeeked(true);
  }

  // https://html.spec.whatwg.org/multipage/media.html#ready-states:show-poster-flag
  if (mShowPoster) {
    mShowPoster = false;
    if (mTextTrackManager) {
      mTextTrackManager->TimeMarchesOn();
    }
  }

  // For blocked media, the event would be pending until it is resumed.
  DispatchAsyncEvent(NS_LITERAL_STRING("play"));

  DispatchAsyncEvent(NS_LITERAL_STRING("playing"));
}

bool HTMLMediaElement::IsActive() const {
  Document* ownerDoc = OwnerDoc();
  return ownerDoc && ownerDoc->IsActive() && ownerDoc->IsVisible();
}

bool HTMLMediaElement::IsHidden() const {
  Document* ownerDoc;
  return mUnboundFromTree || !(ownerDoc = OwnerDoc()) || ownerDoc->Hidden();
}

VideoFrameContainer* HTMLMediaElement::GetVideoFrameContainer() {
  if (mShuttingDown) {
    return nullptr;
  }

  if (mVideoFrameContainer) return mVideoFrameContainer;

  // Only video frames need an image container.
  if (!IsVideo()) {
    return nullptr;
  }

  mVideoFrameContainer = new VideoFrameContainer(
      this, LayerManager::CreateImageContainer(ImageContainer::ASYNCHRONOUS));

  return mVideoFrameContainer;
}

void HTMLMediaElement::PrincipalChanged(DOMMediaStream* aStream) {
  LOG(LogLevel::Info, ("HTMLMediaElement %p Stream principal changed.", this));
  nsContentUtils::CombineResourcePrincipals(&mSrcStreamVideoPrincipal,
                                            aStream->GetVideoPrincipal());

  LOG(LogLevel::Debug,
      ("HTMLMediaElement %p Stream video principal changed to "
       "%p. Waiting for it to reach VideoFrameContainer before "
       "setting.",
       this, aStream->GetVideoPrincipal()));
  if (mVideoFrameContainer) {
    UpdateSrcStreamVideoPrincipal(
        mVideoFrameContainer->GetLastPrincipalHandle());
  }
}

void HTMLMediaElement::UpdateSrcStreamVideoPrincipal(
    const PrincipalHandle& aPrincipalHandle) {
  nsTArray<RefPtr<VideoStreamTrack>> videoTracks;
  mSrcStream->GetVideoTracks(videoTracks);

  PrincipalHandle handle(aPrincipalHandle);
  bool matchesTrackPrincipal = false;
  for (const RefPtr<VideoStreamTrack>& track : videoTracks) {
    if (PrincipalHandleMatches(handle, track->GetPrincipal()) &&
        !track->Ended()) {
      // When the PrincipalHandle for the VideoFrameContainer changes to that of
      // a track in mSrcStream we know that a removed track was displayed but
      // is no longer so.
      matchesTrackPrincipal = true;
      LOG(LogLevel::Debug, ("HTMLMediaElement %p VideoFrameContainer's "
                            "PrincipalHandle matches track %p. That's all we "
                            "need.",
                            this, track.get()));
      break;
    }
  }

  if (matchesTrackPrincipal) {
    mSrcStreamVideoPrincipal = mSrcStream->GetVideoPrincipal();
  }
}

void HTMLMediaElement::PrincipalHandleChangedForVideoFrameContainer(
    VideoFrameContainer* aContainer,
    const PrincipalHandle& aNewPrincipalHandle) {
  MOZ_ASSERT(NS_IsMainThread());

  if (!mSrcStream) {
    return;
  }

  LOG(LogLevel::Debug, ("HTMLMediaElement %p PrincipalHandle changed in "
                        "VideoFrameContainer.",
                        this));

  UpdateSrcStreamVideoPrincipal(aNewPrincipalHandle);
}

nsresult HTMLMediaElement::DispatchEvent(const nsAString& aName) {
  LOG_EVENT(LogLevel::Debug, ("%p Dispatching event %s", this,
                              NS_ConvertUTF16toUTF8(aName).get()));

  // Save events that occur while in the bfcache. These will be dispatched
  // if the page comes out of the bfcache.
  if (mEventDeliveryPaused) {
    mPendingEvents.AppendElement(aName);
    return NS_OK;
  }

  return nsContentUtils::DispatchTrustedEvent(
      OwnerDoc(), static_cast<nsIContent*>(this), aName, CanBubble::eNo,
      Cancelable::eNo);
}

void HTMLMediaElement::DispatchAsyncEvent(const nsAString& aName) {
  LOG_EVENT(LogLevel::Debug,
            ("%p Queuing event %s", this, NS_ConvertUTF16toUTF8(aName).get()));
  DDLOG(DDLogCategory::Event, "HTMLMediaElement",
        nsCString(NS_ConvertUTF16toUTF8(aName)));

  // Save events that occur while in the bfcache. These will be dispatched
  // if the page comes out of the bfcache.
  if (mEventDeliveryPaused) {
    mPendingEvents.AppendElement(aName);
    return;
  }

  nsCOMPtr<nsIRunnable> event;

  if (aName.EqualsLiteral("playing")) {
    event = new nsNotifyAboutPlayingRunner(this, TakePendingPlayPromises());
  } else {
    event = new nsAsyncEventRunner(aName, this);
  }

  mMainThreadEventTarget->Dispatch(event.forget());

  if ((aName.EqualsLiteral("play") || aName.EqualsLiteral("playing"))) {
    mPlayTime.Start();
    mCurrentLoadPlayTime.Start();
    if (IsHidden()) {
      HiddenVideoStart();
    }
  } else if (aName.EqualsLiteral("waiting")) {
    mPlayTime.Pause();
    mCurrentLoadPlayTime.Pause();
    HiddenVideoStop();
  } else if (aName.EqualsLiteral("pause")) {
    mPlayTime.Pause();
    mCurrentLoadPlayTime.Pause();
    HiddenVideoStop();
  }

  // It would happen when (1) media aborts current load (2) media pauses (3)
  // media end (4) media unbind from tree (because we would pause it)
  if (aName.EqualsLiteral("pause")) {
    ReportPlayedTimeAfterBlockedTelemetry();
  }
}

nsresult HTMLMediaElement::DispatchPendingMediaEvents() {
  NS_ASSERTION(!mEventDeliveryPaused,
               "Must not be in bfcache when dispatching pending media events");

  uint32_t count = mPendingEvents.Length();
  for (uint32_t i = 0; i < count; ++i) {
    DispatchAsyncEvent(mPendingEvents[i]);
  }
  mPendingEvents.Clear();

  return NS_OK;
}

bool HTMLMediaElement::IsPotentiallyPlaying() const {
  // TODO:
  //   playback has not stopped due to errors,
  //   and the element has not paused for user interaction
  return !mPaused &&
         (mReadyState == HAVE_ENOUGH_DATA || mReadyState == HAVE_FUTURE_DATA) &&
         !IsPlaybackEnded();
}

bool HTMLMediaElement::IsPlaybackEnded() const {
  // TODO:
  //   the current playback position is equal to the effective end of the media
  //   resource. See bug 449157.
  if (mDecoder) {
    return mReadyState >= HAVE_METADATA && mDecoder->IsEnded();
  } else if (mSrcStream) {
    return mReadyState >= HAVE_METADATA && mSrcStreamPlaybackEnded;
  } else {
    return false;
  }
}

already_AddRefed<nsIPrincipal> HTMLMediaElement::GetCurrentPrincipal() {
  if (mDecoder) {
    return mDecoder->GetCurrentPrincipal();
  }
  if (mSrcStream) {
    nsCOMPtr<nsIPrincipal> principal = mSrcStream->GetPrincipal();
    return principal.forget();
  }
  return nullptr;
}

already_AddRefed<nsIPrincipal> HTMLMediaElement::GetCurrentVideoPrincipal() {
  if (mDecoder) {
    return mDecoder->GetCurrentPrincipal();
  }
  if (mSrcStream) {
    nsCOMPtr<nsIPrincipal> principal = mSrcStreamVideoPrincipal;
    return principal.forget();
  }
  return nullptr;
}

void HTMLMediaElement::NotifyDecoderPrincipalChanged() {
  RefPtr<nsIPrincipal> principal = GetCurrentPrincipal();

  mDecoder->UpdateSameOriginStatus(!principal || IsCORSSameOrigin());
}

void HTMLMediaElement::Invalidate(bool aImageSizeChanged,
                                  Maybe<nsIntSize>& aNewIntrinsicSize,
                                  bool aForceInvalidate) {
  nsIFrame* frame = GetPrimaryFrame();
  if (aNewIntrinsicSize) {
    UpdateMediaSize(aNewIntrinsicSize.value());
    if (frame) {
      nsPresContext* presContext = frame->PresContext();
      PresShell* presShell = presContext->PresShell();
      presShell->FrameNeedsReflow(frame, IntrinsicDirty::StyleChange,
                                  NS_FRAME_IS_DIRTY);
    }
  }

  RefPtr<ImageContainer> imageContainer = GetImageContainer();
  bool asyncInvalidate =
      imageContainer && imageContainer->IsAsync() && !aForceInvalidate;
  if (frame) {
    if (aImageSizeChanged) {
      frame->InvalidateFrame();
    } else {
      frame->InvalidateLayer(DisplayItemType::TYPE_VIDEO, nullptr, nullptr,
                             asyncInvalidate ? nsIFrame::UPDATE_IS_ASYNC : 0);
    }
  }

  SVGObserverUtils::InvalidateDirectRenderingObservers(this);
}

void HTMLMediaElement::UpdateMediaSize(const nsIntSize& aSize) {
  if (IsVideo() && mReadyState != HAVE_NOTHING &&
      mMediaInfo.mVideo.mDisplay != aSize) {
    DispatchAsyncEvent(NS_LITERAL_STRING("resize"));
  }

  mMediaInfo.mVideo.mDisplay = aSize;
  UpdateReadyStateInternal();
}

void HTMLMediaElement::UpdateInitialMediaSize(const nsIntSize& aSize) {
  if (!mMediaInfo.HasVideo()) {
    UpdateMediaSize(aSize);
  }

  if (!mVideoFrameListener) {
    return;
  }

  if (!mSelectedVideoStreamTrack) {
    MOZ_ASSERT(false);
    return;
  }

  mSelectedVideoStreamTrack->RemoveDirectListener(mVideoFrameListener);
  mVideoFrameListener->Forget();
  mVideoFrameListener = nullptr;
}

void HTMLMediaElement::SuspendOrResumeElement(bool aPauseElement,
                                              bool aSuspendEvents) {
  LOG(LogLevel::Debug,
      ("%p SuspendOrResumeElement(pause=%d, suspendEvents=%d) hidden=%d", this,
       aPauseElement, aSuspendEvents, OwnerDoc()->Hidden()));

  if (aPauseElement != mPausedForInactiveDocumentOrChannel) {
    mPausedForInactiveDocumentOrChannel = aPauseElement;
    UpdateSrcMediaStreamPlaying();
    UpdateAudioChannelPlayingState();
    if (aPauseElement) {
      mCurrentLoadPlayTime.Pause();
      ReportTelemetry();

      // For EME content, we may force destruction of the CDM client (and CDM
      // instance if this is the last client for that CDM instance) and
      // the CDM's decoder. This ensures the CDM gets reliable and prompt
      // shutdown notifications, as it may have book-keeping it needs
      // to do on shutdown.
      if (mMediaKeys) {
        nsAutoString keySystem;
        mMediaKeys->GetKeySystem(keySystem);
      }
      if (mDecoder) {
        mDecoder->Pause();
        mDecoder->Suspend();
      }
      mEventDeliveryPaused = aSuspendEvents;
    } else {
      if (!mPaused) {
        mCurrentLoadPlayTime.Start();
      }
      if (mDecoder) {
        mDecoder->Resume();
        if (!mPaused && !mDecoder->IsEnded()) {
          mDecoder->Play();
        }
      }
      if (mEventDeliveryPaused) {
        mEventDeliveryPaused = false;
        DispatchPendingMediaEvents();
      }
      // If the media element has been blocked and isn't still allowed to play
      // when it comes back from the bfcache, we would notify front end to show
      // the blocking icon in order to inform user that the site is still being
      // blocked.
      if (mHasEverBeenBlockedForAutoplay &&
          !AutoplayPolicy::IsAllowedToPlay(*this)) {
        OwnerDoc()->MaybeNotifyAutoplayBlocked();
      }
    }
  }
}

bool HTMLMediaElement::IsBeingDestroyed() {
  Document* ownerDoc = OwnerDoc();
  nsIDocShell* docShell = ownerDoc ? ownerDoc->GetDocShell() : nullptr;
  bool isBeingDestroyed = false;
  if (docShell) {
    docShell->IsBeingDestroyed(&isBeingDestroyed);
  }
  return isBeingDestroyed;
}

void HTMLMediaElement::NotifyOwnerDocumentActivityChanged() {
  bool visible = !IsHidden();
  if (visible) {
    // Visible -> Just pause hidden play time (no-op if already paused).
    HiddenVideoStop();
  } else if (mPlayTime.IsStarted()) {
    // Not visible, play time is running -> Start hidden play time if needed.
    HiddenVideoStart();
  }

  if (mDecoder && !IsBeingDestroyed()) {
    NotifyDecoderActivityChanges();
  }

  bool pauseElement = ShouldElementBePaused();
  SuspendOrResumeElement(pauseElement, !IsActive());

  // If the owning document has become inactive we should shutdown the CDM.
  if (!OwnerDoc()->IsCurrentActiveDocument() && mMediaKeys) {
    // We don't shutdown MediaKeys here because it also listens for document
    // activity and will take care of shutting down itself.
    DDUNLINKCHILD(mMediaKeys.get());
    mMediaKeys = nullptr;
    if (mDecoder) {
      ShutdownDecoder();
    }
  }

  AddRemoveSelfReference();
}

void HTMLMediaElement::AddRemoveSelfReference() {
  // XXX we could release earlier here in many situations if we examined
  // which event listeners are attached. Right now we assume there is a
  // potential listener for every event. We would also have to keep the
  // element alive if it was playing and producing audio output --- right now
  // that's covered by the !mPaused check.
  Document* ownerDoc = OwnerDoc();

  // See the comment at the top of this file for the explanation of this
  // boolean expression.
  bool needSelfReference =
      !mShuttingDown && ownerDoc->IsActive() &&
      (mDelayingLoadEvent || (!mPaused && !Ended()) ||
       (mDecoder && mDecoder->IsSeeking()) || CanActivateAutoplay() ||
       (mMediaSource ? mProgressTimer : mNetworkState == NETWORK_LOADING));

  if (needSelfReference != mHasSelfReference) {
    mHasSelfReference = needSelfReference;
    RefPtr<HTMLMediaElement> self = this;
    if (needSelfReference) {
      // The shutdown observer will hold a strong reference to us. This
      // will do to keep us alive. We need to know about shutdown so that
      // we can release our self-reference.
      mMainThreadEventTarget->Dispatch(NS_NewRunnableFunction(
          "dom::HTMLMediaElement::AddSelfReference",
          [self]() { self->mShutdownObserver->AddRefMediaElement(); }));
    } else {
      // Dispatch Release asynchronously so that we don't destroy this object
      // inside a call stack of method calls on this object
      mMainThreadEventTarget->Dispatch(NS_NewRunnableFunction(
          "dom::HTMLMediaElement::AddSelfReference",
          [self]() { self->mShutdownObserver->ReleaseMediaElement(); }));
    }
  }
}

void HTMLMediaElement::NotifyShutdownEvent() {
  mShuttingDown = true;
  ResetState();
  AddRemoveSelfReference();
}

void HTMLMediaElement::DispatchAsyncSourceError(nsIContent* aSourceElement) {
  LOG_EVENT(LogLevel::Debug, ("%p Queuing simple source error event", this));

  nsCOMPtr<nsIRunnable> event =
      new nsSourceErrorEventRunner(this, aSourceElement);
  mMainThreadEventTarget->Dispatch(event.forget());
}

void HTMLMediaElement::NotifyAddedSource() {
  // If a source element is inserted as a child of a media element
  // that has no src attribute and whose networkState has the value
  // NETWORK_EMPTY, the user agent must invoke the media element's
  // resource selection algorithm.
  if (!HasAttr(kNameSpaceID_None, nsGkAtoms::src) &&
      mNetworkState == NETWORK_EMPTY) {
    AssertReadyStateIsNothing();
    QueueSelectResourceTask();
  }

  // A load was paused in the resource selection algorithm, waiting for
  // a new source child to be added, resume the resource selection algorithm.
  if (mLoadWaitStatus == WAITING_FOR_SOURCE) {
    // Rest the flag so we don't queue multiple LoadFromSourceTask() when
    // multiple <source> are attached in an event loop.
    mLoadWaitStatus = NOT_WAITING;
    QueueLoadFromSourceTask();
  }
}

Element* HTMLMediaElement::GetNextSource() {
  mSourceLoadCandidate = nullptr;

  while (true) {
    if (mSourcePointer == nsINode::GetLastChild()) {
      return nullptr;  // no more children
    }

    if (!mSourcePointer) {
      mSourcePointer = nsINode::GetFirstChild();
    } else {
      mSourcePointer = mSourcePointer->GetNextSibling();
    }
    nsIContent* child = mSourcePointer;

    // If child is a <source> element, it is the next candidate.
    if (child && child->IsHTMLElement(nsGkAtoms::source)) {
      mSourceLoadCandidate = child;
      return child->AsElement();
    }
  }
  MOZ_ASSERT_UNREACHABLE("Execution should not reach here!");
  return nullptr;
}

void HTMLMediaElement::ChangeDelayLoadStatus(bool aDelay) {
  if (mDelayingLoadEvent == aDelay) return;

  mDelayingLoadEvent = aDelay;

  LOG(LogLevel::Debug, ("%p ChangeDelayLoadStatus(%d) doc=0x%p", this, aDelay,
                        mLoadBlockedDoc.get()));
  if (mDecoder) {
    mDecoder->SetLoadInBackground(!aDelay);
  }
  if (aDelay) {
    mLoadBlockedDoc = OwnerDoc();
    mLoadBlockedDoc->BlockOnload();
  } else {
    // mLoadBlockedDoc might be null due to GC unlinking
    if (mLoadBlockedDoc) {
      mLoadBlockedDoc->UnblockOnload(false);
      mLoadBlockedDoc = nullptr;
    }
  }

  // We changed mDelayingLoadEvent which can affect AddRemoveSelfReference
  AddRemoveSelfReference();
}

already_AddRefed<nsILoadGroup> HTMLMediaElement::GetDocumentLoadGroup() {
  if (!OwnerDoc()->IsActive()) {
    NS_WARNING("Load group requested for media element in inactive document.");
  }
  return OwnerDoc()->GetDocumentLoadGroup();
}

nsresult HTMLMediaElement::CopyInnerTo(Element* aDest) {
  nsresult rv = nsGenericHTMLElement::CopyInnerTo(aDest);
  NS_ENSURE_SUCCESS(rv, rv);
  if (aDest->OwnerDoc()->IsStaticDocument()) {
    HTMLMediaElement* dest = static_cast<HTMLMediaElement*>(aDest);
    dest->SetMediaInfo(mMediaInfo);
  }
  return rv;
}

already_AddRefed<TimeRanges> HTMLMediaElement::Buffered() const {
  media::TimeIntervals buffered =
      mDecoder ? mDecoder->GetBuffered() : media::TimeIntervals();
  RefPtr<TimeRanges> ranges = new TimeRanges(ToSupports(OwnerDoc()), buffered);
  return ranges.forget();
}

void HTMLMediaElement::SetRequestHeaders(nsIHttpChannel* aChannel) {
  // Send Accept header for video and audio types only (Bug 489071)
  SetAcceptHeader(aChannel);

  // Apache doesn't send Content-Length when gzip transfer encoding is used,
  // which prevents us from estimating the video length (if explicit
  // Content-Duration and a length spec in the container are not present either)
  // and from seeking. So, disable the standard "Accept-Encoding: gzip,deflate"
  // that we usually send. See bug 614760.
  DebugOnly<nsresult> rv = aChannel->SetRequestHeader(
      NS_LITERAL_CSTRING("Accept-Encoding"), EmptyCString(), false);
  MOZ_ASSERT(NS_SUCCEEDED(rv));

  // Set the Referer header
  nsCOMPtr<nsIReferrerInfo> referrerInfo = new ReferrerInfo(
      OwnerDoc()->GetDocumentURI(), OwnerDoc()->GetReferrerPolicy());
  rv = aChannel->SetReferrerInfoWithoutClone(referrerInfo);
  MOZ_ASSERT(NS_SUCCEEDED(rv));
}

void HTMLMediaElement::FireTimeUpdate(bool aPeriodic) {
  NS_ASSERTION(NS_IsMainThread(), "Should be on main thread.");

  TimeStamp now = TimeStamp::Now();
  double time = CurrentTime();

  // Fire a timeupdate event if this is not a periodic update (i.e. it's a
  // timeupdate event mandated by the spec), or if it's a periodic update
  // and TIMEUPDATE_MS has passed since the last timeupdate event fired and
  // the time has changed.
  if (!aPeriodic || (mLastCurrentTime != time &&
                     (mTimeUpdateTime.IsNull() ||
                      now - mTimeUpdateTime >=
                          TimeDuration::FromMilliseconds(TIMEUPDATE_MS)))) {
    DispatchAsyncEvent(NS_LITERAL_STRING("timeupdate"));
    mTimeUpdateTime = now;
    mLastCurrentTime = time;
  }
  if (mFragmentEnd >= 0.0 && time >= mFragmentEnd) {
    Pause();
    mFragmentEnd = -1.0;
    mFragmentStart = -1.0;
    mDecoder->SetFragmentEndTime(mFragmentEnd);
  }

  // Update the cues displaying on the video.
  // Here mTextTrackManager can be null if the cycle collector has unlinked
  // us before our parent. In that case UnbindFromTree will call us
  // when our parent is unlinked.
  if (mTextTrackManager) {
    mTextTrackManager->TimeMarchesOn();
  }
}

MediaStream* HTMLMediaElement::GetSrcMediaStream() const {
  if (!mSrcStream) {
    return nullptr;
  }
  return mSrcStream->GetPlaybackStream();
}

MediaError* HTMLMediaElement::GetError() const { return mErrorSink->mError; }

void HTMLMediaElement::GetCurrentSpec(nsCString& aString) {
  if (mLoadingSrc) {
    mLoadingSrc->GetSpec(aString);
  } else {
    aString.Truncate();
  }
}

double HTMLMediaElement::MozFragmentEnd() {
  double duration = Duration();

  // If there is no end fragment, or the fragment end is greater than the
  // duration, return the duration.
  return (mFragmentEnd < 0.0 || mFragmentEnd > duration) ? duration
                                                         : mFragmentEnd;
}

void HTMLMediaElement::SetDefaultPlaybackRate(double aDefaultPlaybackRate,
                                              ErrorResult& aRv) {
  if (aDefaultPlaybackRate < 0) {
    aRv.Throw(NS_ERROR_NOT_IMPLEMENTED);
    return;
  }

  mDefaultPlaybackRate = ClampPlaybackRate(aDefaultPlaybackRate);
  DispatchAsyncEvent(NS_LITERAL_STRING("ratechange"));
}

void HTMLMediaElement::SetPlaybackRate(double aPlaybackRate, ErrorResult& aRv) {
  // Changing the playback rate of a media that has more than two channels is
  // not supported.
  if (aPlaybackRate < 0) {
    aRv.Throw(NS_ERROR_DOM_NOT_SUPPORTED_ERR);
    return;
  }

  if (mPlaybackRate == aPlaybackRate) {
    return;
  }

  mPlaybackRate = aPlaybackRate;

  if (mPlaybackRate != 0.0 &&
      (mPlaybackRate > THRESHOLD_HIGH_PLAYBACKRATE_AUDIO ||
       mPlaybackRate < THRESHOLD_LOW_PLAYBACKRATE_AUDIO)) {
    SetMutedInternal(mMuted | MUTED_BY_INVALID_PLAYBACK_RATE);
  } else {
    SetMutedInternal(mMuted & ~MUTED_BY_INVALID_PLAYBACK_RATE);
  }

  if (mDecoder) {
    mDecoder->SetPlaybackRate(ClampPlaybackRate(mPlaybackRate));
  }
  DispatchAsyncEvent(NS_LITERAL_STRING("ratechange"));
}

void HTMLMediaElement::SetMozPreservesPitch(bool aPreservesPitch) {
  mPreservesPitch = aPreservesPitch;
  if (mDecoder) {
    mDecoder->SetPreservesPitch(mPreservesPitch);
  }
}

ImageContainer* HTMLMediaElement::GetImageContainer() {
  VideoFrameContainer* container = GetVideoFrameContainer();
  return container ? container->GetImageContainer() : nullptr;
}

void HTMLMediaElement::UpdateAudioChannelPlayingState(bool aForcePlaying) {
  if (mAudioChannelWrapper) {
    mAudioChannelWrapper->UpdateAudioChannelPlayingState(aForcePlaying);
  }
}

bool HTMLMediaElement::AudioChannelAgentBlockedPlay() {
  if (!mAudioChannelWrapper) {
    // If the mAudioChannelWrapper doesn't exist that means the CC happened.
    LOG(LogLevel::Debug,
        ("%p AudioChannelAgentBlockedPlay() returning true due to null "
         "AudioChannelAgent.",
         this));
    return true;
  }

  // Note: SUSPENDED_PAUSE and SUSPENDED_BLOCK will be merged into one single
  // state.
  const auto suspendType = mAudioChannelWrapper->GetSuspendType();
  return suspendType == nsISuspendedTypes::SUSPENDED_PAUSE ||
         suspendType == nsISuspendedTypes::SUSPENDED_BLOCK;
}

static const char* VisibilityString(Visibility aVisibility) {
  switch (aVisibility) {
    case Visibility::Untracked: {
      return "Untracked";
    }
    case Visibility::ApproximatelyNonVisible: {
      return "ApproximatelyNonVisible";
    }
    case Visibility::ApproximatelyVisible: {
      return "ApproximatelyVisible";
    }
  }

  return "NAN";
}

void HTMLMediaElement::OnVisibilityChange(Visibility aNewVisibility) {
  LOG(LogLevel::Debug,
      ("OnVisibilityChange(): %s\n", VisibilityString(aNewVisibility)));

  mVisibilityState = aNewVisibility;
  if (StaticPrefs::MediaTestVideoSuspend()) {
    DispatchAsyncEvent(NS_LITERAL_STRING("visibilitychanged"));
  }

  if (!mDecoder) {
    return;
  }

  switch (aNewVisibility) {
    case Visibility::Untracked: {
      MOZ_ASSERT_UNREACHABLE("Shouldn't notify for untracked visibility");
      return;
    }
    case Visibility::ApproximatelyNonVisible: {
      if (mPlayTime.IsStarted()) {
        // Not visible, play time is running -> Start hidden play time if
        // needed.
        HiddenVideoStart();
      }
      break;
    }
    case Visibility::ApproximatelyVisible: {
      // Visible -> Just pause hidden play time (no-op if already paused).
      HiddenVideoStop();
      break;
    }
  }

  NotifyDecoderActivityChanges();
}

MediaKeys* HTMLMediaElement::GetMediaKeys() const { return mMediaKeys; }

bool HTMLMediaElement::ContainsRestrictedContent() {
  return GetMediaKeys() != nullptr;
}

void HTMLMediaElement::SetCDMProxyFailure(const MediaResult& aResult) {
  LOG(LogLevel::Debug, ("%s", __func__));
  MOZ_ASSERT(mSetMediaKeysDOMPromise);

  ResetSetMediaKeysTempVariables();

  mSetMediaKeysDOMPromise->MaybeReject(aResult.Code(), aResult.Message());
}

void HTMLMediaElement::RemoveMediaKeys() {
  LOG(LogLevel::Debug, ("%s", __func__));
  // 5.2.3 Stop using the CDM instance represented by the mediaKeys attribute
  // to decrypt media data and remove the association with the media element.
  if (mMediaKeys) {
    mMediaKeys->Unbind();
  }
  mMediaKeys = nullptr;
}

bool HTMLMediaElement::TryRemoveMediaKeysAssociation() {
  MOZ_ASSERT(mMediaKeys);
  LOG(LogLevel::Debug, ("%s", __func__));
  // 5.2.1 If the user agent or CDM do not support removing the association,
  // let this object's attaching media keys value be false and reject promise
  // with a new DOMException whose name is NotSupportedError.
  // 5.2.2 If the association cannot currently be removed, let this object's
  // attaching media keys value be false and reject promise with a new
  // DOMException whose name is InvalidStateError.
  if (mDecoder) {
    RefPtr<HTMLMediaElement> self = this;
    mDecoder->SetCDMProxy(nullptr)
        ->Then(
            mAbstractMainThread, __func__,
            [self]() {
              self->mSetCDMRequest.Complete();

              self->RemoveMediaKeys();
              if (self->AttachNewMediaKeys()) {
                // No incoming MediaKeys object or MediaDecoder is not
                // created yet.
                self->MakeAssociationWithCDMResolved();
              }
            },
            [self](const MediaResult& aResult) {
              self->mSetCDMRequest.Complete();
              // 5.2.4 If the preceding step failed, let this object's
              // attaching media keys value be false and reject promise with
              // a new DOMException whose name is the appropriate error name.
              self->SetCDMProxyFailure(aResult);
            })
        ->Track(mSetCDMRequest);
    return false;
  }

  RemoveMediaKeys();
  return true;
}

bool HTMLMediaElement::DetachExistingMediaKeys() {
  LOG(LogLevel::Debug, ("%s", __func__));
  MOZ_ASSERT(mSetMediaKeysDOMPromise);
  // 5.1 If mediaKeys is not null, CDM instance represented by mediaKeys is
  // already in use by another media element, and the user agent is unable
  // to use it with this element, let this object's attaching media keys
  // value be false and reject promise with a new DOMException whose name
  // is QuotaExceededError.
  if (mIncomingMediaKeys && mIncomingMediaKeys->IsBoundToMediaElement()) {
    SetCDMProxyFailure(MediaResult(
        NS_ERROR_DOM_QUOTA_EXCEEDED_ERR,
        "MediaKeys object is already bound to another HTMLMediaElement"));
    return false;
  }

  // 5.2 If the mediaKeys attribute is not null, run the following steps:
  if (mMediaKeys) {
    return TryRemoveMediaKeysAssociation();
  }
  return true;
}

void HTMLMediaElement::MakeAssociationWithCDMResolved() {
  LOG(LogLevel::Debug, ("%s", __func__));
  MOZ_ASSERT(mSetMediaKeysDOMPromise);

  // 5.4 Set the mediaKeys attribute to mediaKeys.
  mMediaKeys = mIncomingMediaKeys;
  // 5.5 Let this object's attaching media keys value be false.
  ResetSetMediaKeysTempVariables();
  // 5.6 Resolve promise.
  mSetMediaKeysDOMPromise->MaybeResolveWithUndefined();
  mSetMediaKeysDOMPromise = nullptr;
}

bool HTMLMediaElement::TryMakeAssociationWithCDM(CDMProxy* aProxy) {
  LOG(LogLevel::Debug, ("%s", __func__));
  MOZ_ASSERT(aProxy);

  // 5.3.3 Queue a task to run the "Attempt to Resume Playback If Necessary"
  // algorithm on the media element.
  // Note: Setting the CDMProxy on the MediaDecoder will unblock playback.
  if (mDecoder) {
    // CDMProxy is set asynchronously in MediaFormatReader, once it's done,
    // HTMLMediaElement should resolve or reject the DOM promise.
    RefPtr<HTMLMediaElement> self = this;
    mDecoder->SetCDMProxy(aProxy)
        ->Then(
            mAbstractMainThread, __func__,
            [self]() {
              self->mSetCDMRequest.Complete();
              self->MakeAssociationWithCDMResolved();
            },
            [self](const MediaResult& aResult) {
              self->mSetCDMRequest.Complete();
              self->SetCDMProxyFailure(aResult);
            })
        ->Track(mSetCDMRequest);
    return false;
  }
  return true;
}

bool HTMLMediaElement::AttachNewMediaKeys() {
  LOG(LogLevel::Debug,
      ("%s incoming MediaKeys(%p)", __func__, mIncomingMediaKeys.get()));
  MOZ_ASSERT(mSetMediaKeysDOMPromise);

  // 5.3. If mediaKeys is not null, run the following steps:
  if (mIncomingMediaKeys) {
    auto cdmProxy = mIncomingMediaKeys->GetCDMProxy();
    if (!cdmProxy) {
      SetCDMProxyFailure(MediaResult(
          NS_ERROR_DOM_INVALID_STATE_ERR,
          "CDM crashed before binding MediaKeys object to HTMLMediaElement"));
      return false;
    }

    // 5.3.1 Associate the CDM instance represented by mediaKeys with the
    // media element for decrypting media data.
    if (NS_FAILED(mIncomingMediaKeys->Bind(this))) {
      // 5.3.2 If the preceding step failed, run the following steps:

      // 5.3.2.1 Set the mediaKeys attribute to null.
      mMediaKeys = nullptr;
      // 5.3.2.2 Let this object's attaching media keys value be false.
      // 5.3.2.3 Reject promise with a new DOMException whose name is
      // the appropriate error name.
      SetCDMProxyFailure(
          MediaResult(NS_ERROR_DOM_INVALID_STATE_ERR,
                      "Failed to bind MediaKeys object to HTMLMediaElement"));
      return false;
    }
    return TryMakeAssociationWithCDM(cdmProxy);
  }
  return true;
}

void HTMLMediaElement::ResetSetMediaKeysTempVariables() {
  mAttachingMediaKey = false;
  mIncomingMediaKeys = nullptr;
}

already_AddRefed<Promise> HTMLMediaElement::SetMediaKeys(
    mozilla::dom::MediaKeys* aMediaKeys, ErrorResult& aRv) {
  LOG(LogLevel::Debug, ("%p SetMediaKeys(%p) mMediaKeys=%p mDecoder=%p", this,
                        aMediaKeys, mMediaKeys.get(), mDecoder.get()));

  if (MozAudioCaptured()) {
    aRv.Throw(NS_ERROR_DOM_NOT_SUPPORTED_ERR);
    return nullptr;
  }

  nsPIDOMWindowInner* win = OwnerDoc()->GetInnerWindow();
  if (!win) {
    aRv.Throw(NS_ERROR_UNEXPECTED);
    return nullptr;
  }
  RefPtr<DetailedPromise> promise = DetailedPromise::Create(
      win->AsGlobal(), aRv,
      NS_LITERAL_CSTRING("HTMLMediaElement.setMediaKeys"));
  if (aRv.Failed()) {
    return nullptr;
  }

  // 1. If mediaKeys and the mediaKeys attribute are the same object,
  // return a resolved promise.
  if (mMediaKeys == aMediaKeys) {
    promise->MaybeResolveWithUndefined();
    return promise.forget();
  }

  // 2. If this object's attaching media keys value is true, return a
  // promise rejected with a new DOMException whose name is InvalidStateError.
  if (mAttachingMediaKey) {
    promise->MaybeReject(
        NS_ERROR_DOM_INVALID_STATE_ERR,
        NS_LITERAL_CSTRING("A MediaKeys object is in attaching operation."));
    return promise.forget();
  }

  // 3. Let this object's attaching media keys value be true.
  mAttachingMediaKey = true;
  mIncomingMediaKeys = aMediaKeys;

  // 4. Let promise be a new promise.
  mSetMediaKeysDOMPromise = promise;

  // 5. Run the following steps in parallel:

  // 5.1 & 5.2 & 5.3
  if (!DetachExistingMediaKeys() || !AttachNewMediaKeys()) {
    return promise.forget();
  }

  // 5.4, 5.5, 5.6
  MakeAssociationWithCDMResolved();

  // 6. Return promise.
  return promise.forget();
}

EventHandlerNonNull* HTMLMediaElement::GetOnencrypted() {
  return EventTarget::GetEventHandler(nsGkAtoms::onencrypted);
}

void HTMLMediaElement::SetOnencrypted(EventHandlerNonNull* aCallback) {
  EventTarget::SetEventHandler(nsGkAtoms::onencrypted, aCallback);
}

EventHandlerNonNull* HTMLMediaElement::GetOnwaitingforkey() {
  return EventTarget::GetEventHandler(nsGkAtoms::onwaitingforkey);
}

void HTMLMediaElement::SetOnwaitingforkey(EventHandlerNonNull* aCallback) {
  EventTarget::SetEventHandler(nsGkAtoms::onwaitingforkey, aCallback);
}

void HTMLMediaElement::DispatchEncrypted(const nsTArray<uint8_t>& aInitData,
                                         const nsAString& aInitDataType) {
  LOG(LogLevel::Debug, ("%p DispatchEncrypted initDataType='%s'", this,
                        NS_ConvertUTF16toUTF8(aInitDataType).get()));

  if (mReadyState == HAVE_NOTHING) {
    // Ready state not HAVE_METADATA (yet), don't dispatch encrypted now.
    // Queueing for later dispatch in MetadataLoaded.
    mPendingEncryptedInitData.AddInitData(aInitDataType, aInitData);
    return;
  }

  RefPtr<MediaEncryptedEvent> event;
  if (IsCORSSameOrigin()) {
    event = MediaEncryptedEvent::Constructor(this, aInitDataType, aInitData);
  } else {
    event = MediaEncryptedEvent::Constructor(this);
  }

  RefPtr<AsyncEventDispatcher> asyncDispatcher =
      new AsyncEventDispatcher(this, event);
  asyncDispatcher->PostDOMEvent();
}

bool HTMLMediaElement::IsEventAttributeNameInternal(nsAtom* aName) {
  return aName == nsGkAtoms::onencrypted ||
         nsGenericHTMLElement::IsEventAttributeNameInternal(aName);
}

already_AddRefed<nsIPrincipal> HTMLMediaElement::GetTopLevelPrincipal() {
  RefPtr<nsIPrincipal> principal;
  nsCOMPtr<nsPIDOMWindowInner> window = OwnerDoc()->GetInnerWindow();
  if (!window) {
    return nullptr;
  }
  // XXXkhuey better hope we always have an outer ...
  nsCOMPtr<nsPIDOMWindowOuter> top = window->GetOuterWindow()->GetTop();
  if (!top) {
    return nullptr;
  }
  Document* doc = top->GetExtantDoc();
  if (!doc) {
    return nullptr;
  }
  principal = doc->NodePrincipal();
  return principal.forget();
}

void HTMLMediaElement::NotifyWaitingForKey() {
  LOG(LogLevel::Debug, ("%p, NotifyWaitingForKey()", this));

  // http://w3c.github.io/encrypted-media/#wait-for-key
  // 7.3.4 Queue a "waitingforkey" Event
  // 1. Let the media element be the specified HTMLMediaElement object.
  // 2. If the media element's waiting for key value is true, abort these steps.
  if (mWaitingForKey == NOT_WAITING_FOR_KEY) {
    // 3. Set the media element's waiting for key value to true.
    // Note: algorithm continues in UpdateReadyStateInternal() when all decoded
    // data enqueued in the MDSM is consumed.
    mWaitingForKey = WAITING_FOR_KEY;
    UpdateReadyStateInternal();
  }
}

AudioTrackList* HTMLMediaElement::AudioTracks() { return mAudioTrackList; }

VideoTrackList* HTMLMediaElement::VideoTracks() { return mVideoTrackList; }

TextTrackList* HTMLMediaElement::GetTextTracks() {
  return GetOrCreateTextTrackManager()->GetTextTracks();
}

already_AddRefed<TextTrack> HTMLMediaElement::AddTextTrack(
    TextTrackKind aKind, const nsAString& aLabel, const nsAString& aLanguage) {
  return GetOrCreateTextTrackManager()->AddTextTrack(
      aKind, aLabel, aLanguage, TextTrackMode::Hidden,
      TextTrackReadyState::Loaded, TextTrackSource::AddTextTrack);
}

void HTMLMediaElement::PopulatePendingTextTrackList() {
  if (mTextTrackManager) {
    mTextTrackManager->PopulatePendingList();
  }
}

TextTrackManager* HTMLMediaElement::GetOrCreateTextTrackManager() {
  if (!mTextTrackManager) {
    mTextTrackManager = new TextTrackManager(this);
    mTextTrackManager->AddListeners();
  }
  return mTextTrackManager;
}

MediaDecoderOwner::NextFrameStatus HTMLMediaElement::NextFrameStatus() {
  if (mDecoder) {
    return mDecoder->NextFrameStatus();
  }
  if (mSrcStream) {
    if (mSrcStreamTracksAvailable && !mSrcStreamPlaybackEnded) {
      return NEXT_FRAME_AVAILABLE;
    }
    return NEXT_FRAME_UNAVAILABLE;
  }
  return NEXT_FRAME_UNINITIALIZED;
}

void HTMLMediaElement::SetDecoder(MediaDecoder* aDecoder) {
  MOZ_ASSERT(aDecoder);  // Use ShutdownDecoder() to clear.
  if (mDecoder) {
    ShutdownDecoder();
  }
  mDecoder = aDecoder;
  DDLINKCHILD("decoder", mDecoder.get());
  if (mDecoder && mForcedHidden) {
    mDecoder->SetForcedHidden(mForcedHidden);
  }
}

float HTMLMediaElement::ComputedVolume() const {
  return mMuted
             ? 0.0f
             : mAudioChannelWrapper ? mAudioChannelWrapper->GetEffectiveVolume()
                                    : mVolume;
}

bool HTMLMediaElement::ComputedMuted() const {
  return (mMuted & MUTED_BY_AUDIO_CHANNEL);
}

nsSuspendedTypes HTMLMediaElement::ComputedSuspended() const {
  return mAudioChannelWrapper ? mAudioChannelWrapper->GetSuspendType()
                              : nsISuspendedTypes::NONE_SUSPENDED;
}

bool HTMLMediaElement::IsCurrentlyPlaying() const {
  // We have playable data, but we still need to check whether data is "real"
  // current data.
  return mReadyState >= HAVE_CURRENT_DATA && !IsPlaybackEnded();
}

void HTMLMediaElement::SetAudibleState(bool aAudible) {
  if (mIsAudioTrackAudible != aAudible) {
    UpdateAudioTrackSilenceRange(aAudible);
    mIsAudioTrackAudible = aAudible;
    NotifyAudioPlaybackChanged(
        AudioChannelService::AudibleChangedReasons::eDataAudibleChanged);
  }
}

bool HTMLMediaElement::IsAudioTrackCurrentlySilent() const {
  return HasAudio() && !mIsAudioTrackAudible;
}

void HTMLMediaElement::UpdateAudioTrackSilenceRange(bool aAudible) {
  if (!HasAudio()) {
    return;
  }

  if (!aAudible) {
    mAudioTrackSilenceStartedTime = CurrentTime();
    return;
  }

  AccumulateAudioTrackSilence();
}

void HTMLMediaElement::AccumulateAudioTrackSilence() {
  MOZ_ASSERT(HasAudio());
  const double current = CurrentTime();
  if (current < mAudioTrackSilenceStartedTime) {
    return;
  }
  const auto start =
      media::TimeUnit::FromSeconds(mAudioTrackSilenceStartedTime);
  const auto end = media::TimeUnit::FromSeconds(current);
  mSilenceTimeRanges += media::TimeInterval(start, end);
}

void HTMLMediaElement::ReportAudioTrackSilenceProportionTelemetry() {
  if (!HasAudio()) {
    return;
  }

  // Add last silence range to our ranges set.
  if (!mIsAudioTrackAudible) {
    AccumulateAudioTrackSilence();
  }

  RefPtr<TimeRanges> ranges = Played();
  const uint32_t lengthPlayedRange = ranges->Length();
  const uint32_t lengthSilenceRange = mSilenceTimeRanges.Length();
  if (!lengthPlayedRange || !lengthSilenceRange) {
    return;
  }

  double playedTime = 0.0, silenceTime = 0.0;
  for (uint32_t idx = 0; idx < lengthPlayedRange; idx++) {
    playedTime += ranges->End(idx) - ranges->Start(idx);
  }

  for (uint32_t idx = 0; idx < lengthSilenceRange; idx++) {
    silenceTime += mSilenceTimeRanges.End(idx).ToSeconds() -
                   mSilenceTimeRanges.Start(idx).ToSeconds();
  }

  double silenceProportion = (silenceTime / playedTime) * 100;
  // silenceProportion should be in the range [0, 100]
  silenceProportion = std::min(100.0, std::max(silenceProportion, 0.0));
  Telemetry::Accumulate(Telemetry::AUDIO_TRACK_SILENCE_PROPORTION,
                        silenceProportion);
}

void HTMLMediaElement::NotifyAudioPlaybackChanged(
    AudibleChangedReasons aReason) {
  if (mAudioChannelWrapper) {
    mAudioChannelWrapper->NotifyAudioPlaybackChanged(aReason);
  }
  // only request wake lock for audible media.
  UpdateWakeLock();
}

bool HTMLMediaElement::ShouldElementBePaused() {
  // Bfcached page or inactive document.
  if (!IsActive()) {
    return true;
  }

  return false;
}

void HTMLMediaElement::SetMediaInfo(const MediaInfo& aInfo) {
  const bool oldHasAudio = mMediaInfo.HasAudio();
  mMediaInfo = aInfo;
  if (aInfo.HasAudio() != oldHasAudio) {
    UpdateAudioChannelPlayingState();
    NotifyAudioPlaybackChanged(
        AudioChannelService::AudibleChangedReasons::eDataAudibleChanged);
  }
  if (mAudioChannelWrapper) {
    mAudioChannelWrapper->AudioCaptureStreamChangeIfNeeded();
  }
  UpdateWakeLock();
}

void HTMLMediaElement::AudioCaptureStreamChange(bool aCapture) {
  // No need to capture a silence media element.
  if (!HasAudio()) {
    return;
  }

  if (aCapture && !mCaptureStreamPort) {
    nsCOMPtr<nsPIDOMWindowInner> window = OwnerDoc()->GetInnerWindow();
    if (!OwnerDoc()->GetInnerWindow()) {
      return;
    }

    uint64_t id = window->WindowID();
    MediaStreamGraph* msg = MediaStreamGraph::GetInstance(
        MediaStreamGraph::AUDIO_THREAD_DRIVER, window,
        MediaStreamGraph::REQUEST_DEFAULT_SAMPLE_RATE);

    if (GetSrcMediaStream()) {
      mCaptureStreamPort = msg->ConnectToCaptureStream(id, GetSrcMediaStream());
    } else {
      RefPtr<DOMMediaStream> stream =
          CaptureStreamInternal(StreamCaptureBehavior::CONTINUE_WHEN_ENDED,
                                StreamCaptureType::CAPTURE_AUDIO, msg);
      mCaptureStreamPort =
          msg->ConnectToCaptureStream(id, stream->GetPlaybackStream());
    }
  } else if (!aCapture && mCaptureStreamPort) {
    if (mDecoder) {
      ProcessedMediaStream* ps =
          mCaptureStreamPort->GetSource()->AsProcessedStream();
      MOZ_ASSERT(ps);

      for (uint32_t i = 0; i < mOutputStreams.Length(); i++) {
        if (mOutputStreams[i].mStream->GetPlaybackStream() == ps) {
          mDecoder->RemoveOutputStream(mOutputStreams[i].mStream);
          mOutputStreams.RemoveElementAt(i);
          break;
        }
      }
    }
    mCaptureStreamPort->Destroy();
    mCaptureStreamPort = nullptr;
  }
}

void HTMLMediaElement::NotifyCueDisplayStatesChanged() {
  if (!mTextTrackManager) {
    return;
  }

  mTextTrackManager->DispatchUpdateCueDisplay();
}

void HTMLMediaElement::MarkAsContentSource(CallerAPI aAPI) {
  const bool isVisible = mVisibilityState == Visibility::ApproximatelyVisible;

  if (isVisible) {
    // 0 = ALL_VISIBLE
    Telemetry::Accumulate(Telemetry::VIDEO_AS_CONTENT_SOURCE, 0);
  } else {
    // 1 = ALL_INVISIBLE
    Telemetry::Accumulate(Telemetry::VIDEO_AS_CONTENT_SOURCE, 1);

    if (IsInComposedDoc()) {
      // 0 = ALL_IN_TREE
      Telemetry::Accumulate(Telemetry::VIDEO_AS_CONTENT_SOURCE_IN_TREE_OR_NOT,
                            0);
    } else {
      // 1 = ALL_NOT_IN_TREE
      Telemetry::Accumulate(Telemetry::VIDEO_AS_CONTENT_SOURCE_IN_TREE_OR_NOT,
                            1);
    }
  }

  switch (aAPI) {
    case CallerAPI::DRAW_IMAGE: {
      if (isVisible) {
        // 2 = drawImage_VISIBLE
        Telemetry::Accumulate(Telemetry::VIDEO_AS_CONTENT_SOURCE, 2);
      } else {
        // 3 = drawImage_INVISIBLE
        Telemetry::Accumulate(Telemetry::VIDEO_AS_CONTENT_SOURCE, 3);

        if (IsInComposedDoc()) {
          // 2 = drawImage_IN_TREE
          Telemetry::Accumulate(
              Telemetry::VIDEO_AS_CONTENT_SOURCE_IN_TREE_OR_NOT, 2);
        } else {
          // 3 = drawImage_NOT_IN_TREE
          Telemetry::Accumulate(
              Telemetry::VIDEO_AS_CONTENT_SOURCE_IN_TREE_OR_NOT, 3);
        }
      }
      break;
    }
    case CallerAPI::CREATE_PATTERN: {
      if (isVisible) {
        // 4 = createPattern_VISIBLE
        Telemetry::Accumulate(Telemetry::VIDEO_AS_CONTENT_SOURCE, 4);
      } else {
        // 5 = createPattern_INVISIBLE
        Telemetry::Accumulate(Telemetry::VIDEO_AS_CONTENT_SOURCE, 5);

        if (IsInComposedDoc()) {
          // 4 = createPattern_IN_TREE
          Telemetry::Accumulate(
              Telemetry::VIDEO_AS_CONTENT_SOURCE_IN_TREE_OR_NOT, 4);
        } else {
          // 5 = createPattern_NOT_IN_TREE
          Telemetry::Accumulate(
              Telemetry::VIDEO_AS_CONTENT_SOURCE_IN_TREE_OR_NOT, 5);
        }
      }
      break;
    }
    case CallerAPI::CREATE_IMAGEBITMAP: {
      if (isVisible) {
        // 6 = createImageBitmap_VISIBLE
        Telemetry::Accumulate(Telemetry::VIDEO_AS_CONTENT_SOURCE, 6);
      } else {
        // 7 = createImageBitmap_INVISIBLE
        Telemetry::Accumulate(Telemetry::VIDEO_AS_CONTENT_SOURCE, 7);

        if (IsInComposedDoc()) {
          // 6 = createImageBitmap_IN_TREE
          Telemetry::Accumulate(
              Telemetry::VIDEO_AS_CONTENT_SOURCE_IN_TREE_OR_NOT, 6);
        } else {
          // 7 = createImageBitmap_NOT_IN_TREE
          Telemetry::Accumulate(
              Telemetry::VIDEO_AS_CONTENT_SOURCE_IN_TREE_OR_NOT, 7);
        }
      }
      break;
    }
    case CallerAPI::CAPTURE_STREAM: {
      if (isVisible) {
        // 8 = captureStream_VISIBLE
        Telemetry::Accumulate(Telemetry::VIDEO_AS_CONTENT_SOURCE, 8);
      } else {
        // 9 = captureStream_INVISIBLE
        Telemetry::Accumulate(Telemetry::VIDEO_AS_CONTENT_SOURCE, 9);

        if (IsInComposedDoc()) {
          // 8 = captureStream_IN_TREE
          Telemetry::Accumulate(
              Telemetry::VIDEO_AS_CONTENT_SOURCE_IN_TREE_OR_NOT, 8);
        } else {
          // 9 = captureStream_NOT_IN_TREE
          Telemetry::Accumulate(
              Telemetry::VIDEO_AS_CONTENT_SOURCE_IN_TREE_OR_NOT, 9);
        }
      }
      break;
    }
  }

  LOG(LogLevel::Debug,
      ("%p Log VIDEO_AS_CONTENT_SOURCE: visibility = %u, API: '%d' and 'All'",
       this, isVisible, static_cast<int>(aAPI)));

  if (!isVisible) {
    LOG(LogLevel::Debug,
        ("%p Log VIDEO_AS_CONTENT_SOURCE_IN_TREE_OR_NOT: inTree = %u, API: "
         "'%d' and 'All'",
         this, IsInComposedDoc(), static_cast<int>(aAPI)));
  }
}

void HTMLMediaElement::UpdateCustomPolicyAfterPlayed() {
  if (mAudioChannelWrapper) {
    mAudioChannelWrapper->NotifyPlayStateChanged();
  }
}

AbstractThread* HTMLMediaElement::AbstractMainThread() const {
  MOZ_ASSERT(mAbstractMainThread);

  return mAbstractMainThread;
}

nsTArray<RefPtr<PlayPromise>> HTMLMediaElement::TakePendingPlayPromises() {
  return std::move(mPendingPlayPromises);
}

void HTMLMediaElement::NotifyAboutPlaying() {
  // Stick to the DispatchAsyncEvent() call path for now because we want to
  // trigger some telemetry-related codes in the DispatchAsyncEvent() method.
  DispatchAsyncEvent(NS_LITERAL_STRING("playing"));
}

already_AddRefed<PlayPromise> HTMLMediaElement::CreatePlayPromise(
    ErrorResult& aRv) const {
  nsPIDOMWindowInner* win = OwnerDoc()->GetInnerWindow();

  if (!win) {
    aRv.Throw(NS_ERROR_UNEXPECTED);
    return nullptr;
  }

  RefPtr<PlayPromise> promise = PlayPromise::Create(win->AsGlobal(), aRv);
  LOG(LogLevel::Debug, ("%p created PlayPromise %p", this, promise.get()));

  return promise.forget();
}

already_AddRefed<Promise> HTMLMediaElement::CreateDOMPromise(
    ErrorResult& aRv) const {
  nsPIDOMWindowInner* win = OwnerDoc()->GetInnerWindow();

  if (!win) {
    aRv.Throw(NS_ERROR_UNEXPECTED);
    return nullptr;
  }

  return Promise::Create(win->AsGlobal(), aRv);
}

void HTMLMediaElement::AsyncResolvePendingPlayPromises() {
  if (mShuttingDown) {
    return;
  }

  nsCOMPtr<nsIRunnable> event = new nsResolveOrRejectPendingPlayPromisesRunner(
      this, TakePendingPlayPromises());

  mMainThreadEventTarget->Dispatch(event.forget());
}

void HTMLMediaElement::AsyncRejectPendingPlayPromises(nsresult aError) {
  if (!mPaused) {
    mPaused = true;
    DispatchAsyncEvent(NS_LITERAL_STRING("pause"));
  }

  if (mShuttingDown) {
    return;
  }

  if (aError == NS_ERROR_DOM_MEDIA_NOT_ALLOWED_ERR) {
    DispatchEventsWhenPlayWasNotAllowed();
  }

  nsCOMPtr<nsIRunnable> event = new nsResolveOrRejectPendingPlayPromisesRunner(
      this, TakePendingPlayPromises(), aError);

  mMainThreadEventTarget->Dispatch(event.forget());
}

void HTMLMediaElement::GetEMEInfo(nsString& aEMEInfo) {
  if (!mMediaKeys) {
    return;
  }

  nsString keySystem;
  mMediaKeys->GetKeySystem(keySystem);

  nsString sessionsInfo;
  mMediaKeys->GetSessionsInfo(sessionsInfo);

  aEMEInfo.AppendLiteral("Key System=");
  aEMEInfo.Append(keySystem);
  aEMEInfo.AppendLiteral(" SessionsInfo=");
  aEMEInfo.Append(sessionsInfo);
}

void HTMLMediaElement::NotifyDecoderActivityChanges() const {
  if (mDecoder) {
    mDecoder->NotifyOwnerActivityChanged(!IsHidden(), mVisibilityState,
                                         IsInComposedDoc());
  }
}

Document* HTMLMediaElement::GetDocument() const { return OwnerDoc(); }

void HTMLMediaElement::ConstructMediaTracks(const MediaInfo* aInfo) {
  if (mMediaTracksConstructed || !aInfo) {
    return;
  }

  mMediaTracksConstructed = true;

  AudioTrackList* audioList = AudioTracks();
  if (audioList && aInfo->HasAudio()) {
    const TrackInfo& info = aInfo->mAudio;
    RefPtr<AudioTrack> track = MediaTrackList::CreateAudioTrack(
        audioList->GetOwnerGlobal(), info.mId, info.mKind, info.mLabel,
        info.mLanguage, info.mEnabled);

    audioList->AddTrack(track);
  }

  VideoTrackList* videoList = VideoTracks();
  if (videoList && aInfo->HasVideo()) {
    const TrackInfo& info = aInfo->mVideo;
    RefPtr<VideoTrack> track = MediaTrackList::CreateVideoTrack(
        videoList->GetOwnerGlobal(), info.mId, info.mKind, info.mLabel,
        info.mLanguage);

    videoList->AddTrack(track);
    track->SetEnabledInternal(info.mEnabled, MediaTrack::FIRE_NO_EVENTS);
  }
}

void HTMLMediaElement::RemoveMediaTracks() {
  if (mAudioTrackList) {
    mAudioTrackList->RemoveTracks();
  }

  if (mVideoTrackList) {
    mVideoTrackList->RemoveTracks();
  }

  mMediaTracksConstructed = false;
}

class MediaElementGMPCrashHelper : public GMPCrashHelper {
 public:
  explicit MediaElementGMPCrashHelper(HTMLMediaElement* aElement)
      : mElement(aElement) {
    MOZ_ASSERT(NS_IsMainThread());  // WeakPtr isn't thread safe.
  }
  already_AddRefed<nsPIDOMWindowInner> GetPluginCrashedEventTarget() override {
    MOZ_ASSERT(NS_IsMainThread());  // WeakPtr isn't thread safe.
    if (!mElement) {
      return nullptr;
    }
    return do_AddRef(mElement->OwnerDoc()->GetInnerWindow());
  }

 private:
  WeakPtr<HTMLMediaElement> mElement;
};

already_AddRefed<GMPCrashHelper> HTMLMediaElement::CreateGMPCrashHelper() {
  return MakeAndAddRef<MediaElementGMPCrashHelper>(this);
}

void HTMLMediaElement::MarkAsTainted() {
  mHasSuspendTaint = true;

  if (mDecoder) {
    mDecoder->SetSuspendTaint(true);
  }
}

bool HasDebuggerOrTabsPrivilege(JSContext* aCx, JSObject* aObj) {
  return nsContentUtils::CallerHasPermission(aCx, nsGkAtoms::debugger) ||
         nsContentUtils::CallerHasPermission(aCx, nsGkAtoms::tabs);
}

void HTMLMediaElement::AsyncResolveSeekDOMPromiseIfExists() {
  MOZ_ASSERT(NS_IsMainThread());
  if (mSeekDOMPromise) {
    RefPtr<dom::Promise> promise = mSeekDOMPromise.forget();
    nsCOMPtr<nsIRunnable> r = NS_NewRunnableFunction(
        "dom::HTMLMediaElement::AsyncResolveSeekDOMPromiseIfExists",
        [promise]() { promise->MaybeResolveWithUndefined(); });
    mAbstractMainThread->Dispatch(r.forget());
    mSeekDOMPromise = nullptr;
  }
}

void HTMLMediaElement::AsyncRejectSeekDOMPromiseIfExists() {
  MOZ_ASSERT(NS_IsMainThread());
  if (mSeekDOMPromise) {
    RefPtr<dom::Promise> promise = mSeekDOMPromise.forget();
    nsCOMPtr<nsIRunnable> r = NS_NewRunnableFunction(
        "dom::HTMLMediaElement::AsyncRejectSeekDOMPromiseIfExists",
        [promise]() { promise->MaybeReject(NS_ERROR_DOM_ABORT_ERR); });
    mAbstractMainThread->Dispatch(r.forget());
    mSeekDOMPromise = nullptr;
  }
}

void HTMLMediaElement::ReportCanPlayTelemetry() {
  LOG(LogLevel::Debug, ("%s", __func__));

  RefPtr<nsIThread> thread;
  nsresult rv = NS_NewNamedThread("MediaTelemetry", getter_AddRefs(thread));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return;
  }

  RefPtr<AbstractThread> abstractThread = mAbstractMainThread;

  thread->Dispatch(
      NS_NewRunnableFunction(
          "dom::HTMLMediaElement::ReportCanPlayTelemetry",
          [thread, abstractThread]() {
#if XP_WIN
            // Windows Media Foundation requires MSCOM to be inited.
            DebugOnly<HRESULT> hr = CoInitializeEx(0, COINIT_MULTITHREADED);
            MOZ_ASSERT(hr == S_OK);
#endif
            bool aac = MP4Decoder::IsSupportedType(
                MediaContainerType(MEDIAMIMETYPE(AUDIO_MP4)), nullptr);
            bool h264 = MP4Decoder::IsSupportedType(
                MediaContainerType(MEDIAMIMETYPE(VIDEO_MP4)), nullptr);
#if XP_WIN
            CoUninitialize();
#endif
            abstractThread->Dispatch(NS_NewRunnableFunction(
                "dom::HTMLMediaElement::ReportCanPlayTelemetry",
                [thread, aac, h264]() {
                  LOG(LogLevel::Debug,
                      ("MediaTelemetry aac=%d h264=%d", aac, h264));
                  Telemetry::Accumulate(
                      Telemetry::HistogramID::VIDEO_CAN_CREATE_AAC_DECODER,
                      aac);
                  Telemetry::Accumulate(
                      Telemetry::HistogramID::VIDEO_CAN_CREATE_H264_DECODER,
                      h264);
                  thread->AsyncShutdown();
                }));
          }),
      NS_DISPATCH_NORMAL);
}

already_AddRefed<Promise> HTMLMediaElement::SetSinkId(const nsAString& aSinkId,
                                                      ErrorResult& aRv) {
  nsCOMPtr<nsPIDOMWindowInner> win = OwnerDoc()->GetInnerWindow();
  if (!win) {
    aRv.Throw(NS_ERROR_UNEXPECTED);
    return nullptr;
  }

  RefPtr<Promise> promise = Promise::Create(win->AsGlobal(), aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  if (mSink.first().Equals(aSinkId)) {
    promise->MaybeResolveWithUndefined();
    return promise.forget();
  }

  nsString sinkId(aSinkId);
  MediaManager::Get()
      ->GetSinkDevice(win, sinkId)
      ->Then(
          mAbstractMainThread, __func__,
          [self = RefPtr<HTMLMediaElement>(this)](
              RefPtr<AudioDeviceInfo>&& aInfo) {
            // Sink found switch output device.
            MOZ_ASSERT(aInfo);
            if (self->mDecoder) {
              RefPtr<SinkInfoPromise> p = self->mDecoder->SetSink(aInfo)->Then(
                  self->mAbstractMainThread, __func__,
                  [aInfo](const GenericPromise::ResolveOrRejectValue& aValue) {
                    if (aValue.IsResolve()) {
                      return SinkInfoPromise::CreateAndResolve(aInfo, __func__);
                    }
                    return SinkInfoPromise::CreateAndReject(
                        aValue.RejectValue(), __func__);
                  });
              return p;
            }
            if (self->GetSrcMediaStream()) {
              // Set Sink Id through MSG is not supported yet.
              return SinkInfoPromise::CreateAndReject(NS_ERROR_ABORT, __func__);
            }
            // No media attached to the element save it for later.
            return SinkInfoPromise::CreateAndResolve(aInfo, __func__);
          },
          [](nsresult res) {
            // Promise is rejected, sink not found.
            return SinkInfoPromise::CreateAndReject(res, __func__);
          })
      ->Then(
          mAbstractMainThread, __func__,
          [promise, self = RefPtr<HTMLMediaElement>(this),
           sinkId](const SinkInfoPromise::ResolveOrRejectValue& aValue) {
            if (aValue.IsResolve()) {
              self->mSink = MakePair(sinkId, aValue.ResolveValue());
              promise->MaybeResolveWithUndefined();
            } else {
              switch (aValue.RejectValue()) {
                case NS_ERROR_ABORT:
                  promise->MaybeReject(NS_ERROR_DOM_ABORT_ERR);
                  break;
                case NS_ERROR_NOT_AVAILABLE: {
                  ErrorResult notFoundError;
                  notFoundError.ThrowDOMException(
                      NS_ERROR_DOM_NOT_FOUND_ERR,
                      NS_LITERAL_CSTRING("The object can not be found here."));
                  promise->MaybeReject(notFoundError);
                  break;
                }
                case NS_ERROR_DOM_MEDIA_NOT_ALLOWED_ERR:
                  promise->MaybeReject(NS_ERROR_DOM_NOT_ALLOWED_ERR);
                  break;
                default:
                  MOZ_ASSERT_UNREACHABLE("Invalid error.");
              }
            }
          });

  aRv = NS_OK;
  return promise.forget();
}

void HTMLMediaElement::NotifyTextTrackModeChanged() {
  if (mPendingTextTrackChanged) {
    return;
  }
  mPendingTextTrackChanged = true;
  mAbstractMainThread->Dispatch(
      NS_NewRunnableFunction("HTMLMediaElement::NotifyTextTrackModeChanged",
                             [this, self = RefPtr<HTMLMediaElement>(this)]() {
                               mPendingTextTrackChanged = false;
                               if (!mTextTrackManager) {
                                 return;
                               }
                               GetTextTracks()->CreateAndDispatchChangeEvent();
                               // https://html.spec.whatwg.org/multipage/media.html#text-track-model:show-poster-flag
                               if (!mShowPoster) {
                                 mTextTrackManager->TimeMarchesOn();
                               }
                             }));
}

}  // namespace dom
}  // namespace mozilla

#undef LOG
#undef LOG_EVENT
