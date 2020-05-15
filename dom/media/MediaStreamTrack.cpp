/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-*/
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MediaStreamTrack.h"

#include "DOMMediaStream.h"
#include "MediaStreamError.h"
#include "MediaStreamGraph.h"
#include "MediaStreamListener.h"
#include "mozilla/BasePrincipal.h"
#include "mozilla/dom/Promise.h"
#include "nsContentUtils.h"
#include "nsIUUIDGenerator.h"
#include "nsServiceManagerUtils.h"
#include "systemservices/MediaUtils.h"

#ifdef LOG
#  undef LOG
#endif

static mozilla::LazyLogModule gMediaStreamTrackLog("MediaStreamTrack");
#define LOG(type, msg) MOZ_LOG(gMediaStreamTrackLog, type, msg)

using namespace mozilla::media;

namespace mozilla {
namespace dom {

NS_IMPL_CYCLE_COLLECTING_ADDREF(MediaStreamTrackSource)
NS_IMPL_CYCLE_COLLECTING_RELEASE(MediaStreamTrackSource)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(MediaStreamTrackSource)
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTION_CLASS(MediaStreamTrackSource)

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(MediaStreamTrackSource)
  tmp->Destroy();
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mPrincipal)
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(MediaStreamTrackSource)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mPrincipal)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

auto MediaStreamTrackSource::ApplyConstraints(
    const dom::MediaTrackConstraints& aConstraints, CallerType aCallerType)
    -> RefPtr<ApplyConstraintsPromise> {
  return ApplyConstraintsPromise::CreateAndReject(
      MakeRefPtr<MediaMgrError>(MediaMgrError::Name::OverconstrainedError,
                                NS_LITERAL_STRING("")),
      __func__);
}

/**
 * MSGListener monitors state changes of the media flowing through the
 * MediaStreamGraph.
 *
 *
 * For changes to PrincipalHandle the following applies:
 *
 * When the main thread principal for a MediaStreamTrack changes, its principal
 * will be set to the combination of the previous principal and the new one.
 *
 * As a PrincipalHandle change later happens on the MediaStreamGraph thread, we
 * will be notified. If the latest principal on main thread matches the
 * PrincipalHandle we just saw on MSG thread, we will set the track's principal
 * to the new one.
 *
 * We know at this point that the old principal has been flushed out and data
 * under it cannot leak to consumers.
 *
 * In case of multiple changes to the main thread state, the track's principal
 * will be a combination of its old principal and all the new ones until the
 * latest main thread principal matches the PrincipalHandle on the MSG thread.
 */
class MediaStreamTrack::MSGListener : public MediaStreamTrackListener {
 public:
  explicit MSGListener(MediaStreamTrack* aTrack)
      : mGraph(aTrack->GraphImpl()), mTrack(aTrack) {
    MOZ_ASSERT(mGraph);
  }

  void DoNotifyPrincipalHandleChanged(
      const PrincipalHandle& aNewPrincipalHandle) {
    MOZ_ASSERT(NS_IsMainThread());

    if (!mTrack) {
      return;
    }

    mTrack->NotifyPrincipalHandleChanged(aNewPrincipalHandle);
  }

  void NotifyPrincipalHandleChanged(
      MediaStreamGraph* aGraph,
      const PrincipalHandle& aNewPrincipalHandle) override {
    mGraph->DispatchToMainThreadStableState(
        NewRunnableMethod<StoreCopyPassByConstLRef<PrincipalHandle>>(
            "dom::MediaStreamTrack::MSGListener::"
            "DoNotifyPrincipalHandleChanged",
            this, &MSGListener::DoNotifyPrincipalHandleChanged,
            aNewPrincipalHandle));
  }

  void NotifyRemoved() override {
    // `mTrack` is a WeakPtr and must be destroyed on main thread.
    // We dispatch ourselves to main thread here in case the MediaStreamGraph
    // is holding the last reference to us.
    mGraph->DispatchToMainThreadStableState(
        NS_NewRunnableFunction("MediaStreamTrack::MSGListener::mTrackReleaser",
                               [self = RefPtr<MSGListener>(this)]() {}));
  }

  void DoNotifyEnded() {
    MOZ_ASSERT(NS_IsMainThread());

    if (!mTrack) {
      return;
    }

    mGraph->AbstractMainThread()->Dispatch(
        NewRunnableMethod("MediaStreamTrack::OverrideEnded", mTrack.get(),
                          &MediaStreamTrack::OverrideEnded));
  }

  void NotifyEnded() override {
    mGraph->DispatchToMainThreadStableState(
        NewRunnableMethod("MediaStreamTrack::MSGListener::DoNotifyEnded", this,
                          &MSGListener::DoNotifyEnded));
  }

 protected:
  const RefPtr<MediaStreamGraphImpl> mGraph;

  // Main thread only.
  WeakPtr<MediaStreamTrack> mTrack;
};

class TrackSink : public MediaStreamTrackSource::Sink {
 public:
  explicit TrackSink(MediaStreamTrack* aTrack) : mTrack(aTrack) {}

  /**
   * Keep the track source alive. This track and any clones are controlling the
   * lifetime of the source by being registered as its sinks.
   */
  bool KeepsSourceAlive() const override { return true; }

  bool Enabled() const override {
    if (!mTrack) {
      return false;
    }
    return mTrack->Enabled();
  }

  void PrincipalChanged() override {
    if (mTrack) {
      mTrack->PrincipalChanged();
    }
  }

  void MutedChanged(bool aNewState) override {
    if (mTrack) {
      mTrack->MutedChanged(aNewState);
    }
  }

 private:
  WeakPtr<MediaStreamTrack> mTrack;
};

MediaStreamTrack::MediaStreamTrack(DOMMediaStream* aStream, TrackID aTrackID,
                                   TrackID aInputTrackID,
                                   MediaStreamTrackSource* aSource,
                                   const MediaTrackConstraints& aConstraints)
    : mOwningStream(aStream),
      mTrackID(aTrackID),
      mInputTrackID(aInputTrackID),
      mSource(aSource),
      mSink(MakeUnique<TrackSink>(this)),
      mPrincipal(aSource->GetPrincipal()),
      mReadyState(MediaStreamTrackState::Live),
      mEnabled(true),
      mMuted(false),
      mConstraints(aConstraints) {
  GetSource().RegisterSink(mSink.get());

  if (GetOwnedStream()) {
    mMSGListener = new MSGListener(this);
    AddListener(mMSGListener);
  }

  nsresult rv;
  nsCOMPtr<nsIUUIDGenerator> uuidgen =
      do_GetService("@mozilla.org/uuid-generator;1", &rv);

  nsID uuid;
  memset(&uuid, 0, sizeof(uuid));
  if (uuidgen) {
    uuidgen->GenerateUUIDInPlace(&uuid);
  }

  char chars[NSID_LENGTH];
  uuid.ToProvidedString(chars);
  mID = NS_ConvertASCIItoUTF16(chars);
}

MediaStreamTrack::~MediaStreamTrack() { Destroy(); }

void MediaStreamTrack::Destroy() {
  mReadyState = MediaStreamTrackState::Ended;
  if (mSource) {
    mSource->UnregisterSink(mSink.get());
  }
  if (mMSGListener) {
    if (GetOwnedStream()) {
      RemoveListener(mMSGListener);
    }
    mMSGListener = nullptr;
  }
  // Remove all listeners -- avoid iterating over the list we're removing from
  const nsTArray<RefPtr<MediaStreamTrackListener>> trackListeners(
      mTrackListeners);
  for (auto listener : trackListeners) {
    RemoveListener(listener);
  }
  // Do the same as above for direct listeners
  const nsTArray<RefPtr<DirectMediaStreamTrackListener>> directTrackListeners(
      mDirectTrackListeners);
  for (auto listener : directTrackListeners) {
    RemoveDirectListener(listener);
  }
}

NS_IMPL_CYCLE_COLLECTION_CLASS(MediaStreamTrack)

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(MediaStreamTrack,
                                                DOMEventTargetHelper)
  tmp->Destroy();
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mOwningStream)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mSource)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mOriginalTrack)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mPrincipal)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mPendingPrincipal)
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(MediaStreamTrack,
                                                  DOMEventTargetHelper)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mOwningStream)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mSource)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mOriginalTrack)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mPrincipal)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mPendingPrincipal)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_ADDREF_INHERITED(MediaStreamTrack, DOMEventTargetHelper)
NS_IMPL_RELEASE_INHERITED(MediaStreamTrack, DOMEventTargetHelper)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(MediaStreamTrack)
NS_INTERFACE_MAP_END_INHERITING(DOMEventTargetHelper)

nsPIDOMWindowInner* MediaStreamTrack::GetParentObject() const {
  MOZ_RELEASE_ASSERT(mOwningStream);
  return mOwningStream->GetParentObject();
}

JSObject* MediaStreamTrack::WrapObject(JSContext* aCx,
                                       JS::Handle<JSObject*> aGivenProto) {
  return MediaStreamTrack_Binding::Wrap(aCx, this, aGivenProto);
}

void MediaStreamTrack::GetId(nsAString& aID) const { aID = mID; }

void MediaStreamTrack::SetEnabled(bool aEnabled) {
  LOG(LogLevel::Info,
      ("MediaStreamTrack %p %s", this, aEnabled ? "Enabled" : "Disabled"));

  if (mEnabled == aEnabled) {
    return;
  }

  mEnabled = aEnabled;
  GetOwnedStream()->SetTrackEnabled(
      mTrackID,
      mEnabled ? DisabledTrackMode::ENABLED : DisabledTrackMode::SILENCE_BLACK);
  GetSource().SinkEnabledStateChanged();
}

void MediaStreamTrack::Stop() {
  LOG(LogLevel::Info, ("MediaStreamTrack %p Stop()", this));

  if (Ended()) {
    LOG(LogLevel::Warning, ("MediaStreamTrack %p Already ended", this));
    return;
  }

  if (!mSource) {
    MOZ_ASSERT(false);
    return;
  }

  mSource->UnregisterSink(mSink.get());

  MOZ_ASSERT(mOwningStream,
             "Every MediaStreamTrack needs an owning DOMMediaStream");
  DOMMediaStream::TrackPort* port = mOwningStream->FindOwnedTrackPort(*this);
  MOZ_ASSERT(port,
             "A MediaStreamTrack must exist in its owning DOMMediaStream");
  Unused << port->BlockSourceTrackId(mInputTrackID, BlockingMode::CREATION);

  mReadyState = MediaStreamTrackState::Ended;

  NotifyEnded();
}

void MediaStreamTrack::GetConstraints(dom::MediaTrackConstraints& aResult) {
  aResult = mConstraints;
}

void MediaStreamTrack::GetSettings(dom::MediaTrackSettings& aResult,
                                   CallerType aCallerType) {
  GetSource().GetSettings(aResult);

  // Spoof values when privacy.resistFingerprinting is true.
  if (!nsContentUtils::ResistFingerprinting(aCallerType)) {
    return;
  }
  if (aResult.mFacingMode.WasPassed()) {
    aResult.mFacingMode.Value().Assign(NS_ConvertASCIItoUTF16(
        VideoFacingModeEnumValues::strings[uint8_t(VideoFacingModeEnum::User)]
            .value));
  }
}

already_AddRefed<Promise> MediaStreamTrack::ApplyConstraints(
    const MediaTrackConstraints& aConstraints, CallerType aCallerType,
    ErrorResult& aRv) {
  if (MOZ_LOG_TEST(gMediaStreamTrackLog, LogLevel::Info)) {
    nsString str;
    aConstraints.ToJSON(str);

    LOG(LogLevel::Info, ("MediaStreamTrack %p ApplyConstraints() with "
                         "constraints %s",
                         this, NS_ConvertUTF16toUTF8(str).get()));
  }

  nsPIDOMWindowInner* window = mOwningStream->GetParentObject();
  nsIGlobalObject* go = window ? window->AsGlobal() : nullptr;

  RefPtr<Promise> promise = Promise::Create(go, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  // Forward constraints to the source.
  //
  // After GetSource().ApplyConstraints succeeds (after it's been to
  // media-thread and back), and no sooner, do we set mConstraints to the newly
  // applied values.

  // Keep a reference to this, to make sure it's still here when we get back.
  RefPtr<MediaStreamTrack> self(this);
  GetSource()
      .ApplyConstraints(aConstraints, aCallerType)
      ->Then(
          GetCurrentThreadSerialEventTarget(), __func__,
          [this, self, promise, aConstraints](bool aDummy) {
            nsPIDOMWindowInner* window = mOwningStream->GetParentObject();
            if (!window || !window->IsCurrentInnerWindow()) {
              return;  // Leave Promise pending after navigation by design.
            }
            mConstraints = aConstraints;
            promise->MaybeResolve(false);
          },
          [this, self, promise](const RefPtr<MediaMgrError>& aError) {
            nsPIDOMWindowInner* window = mOwningStream->GetParentObject();
            if (!window || !window->IsCurrentInnerWindow()) {
              return;  // Leave Promise pending after navigation by design.
            }
            promise->MaybeReject(MakeRefPtr<MediaStreamError>(window, *aError));
          });
  return promise.forget();
}

MediaStreamGraph* MediaStreamTrack::Graph() {
  return GetOwnedStream()->Graph();
}

MediaStreamGraphImpl* MediaStreamTrack::GraphImpl() {
  return GetOwnedStream()->GraphImpl();
}

void MediaStreamTrack::SetPrincipal(nsIPrincipal* aPrincipal) {
  if (aPrincipal == mPrincipal) {
    return;
  }
  mPrincipal = aPrincipal;

  LOG(LogLevel::Info,
      ("MediaStreamTrack %p principal changed to %p. Now: "
       "null=%d, codebase=%d, expanded=%d, system=%d",
       this, mPrincipal.get(), mPrincipal->GetIsNullPrincipal(),
       mPrincipal->GetIsCodebasePrincipal(),
       mPrincipal->GetIsExpandedPrincipal(), mPrincipal->IsSystemPrincipal()));
  for (PrincipalChangeObserver<MediaStreamTrack>* observer :
       mPrincipalChangeObservers) {
    observer->PrincipalChanged(this);
  }
}

void MediaStreamTrack::PrincipalChanged() {
  mPendingPrincipal = GetSource().GetPrincipal();
  nsCOMPtr<nsIPrincipal> newPrincipal = mPrincipal;
  LOG(LogLevel::Info, ("MediaStreamTrack %p Principal changed on main thread "
                       "to %p (pending). Combining with existing principal %p.",
                       this, mPendingPrincipal.get(), mPrincipal.get()));
  if (nsContentUtils::CombineResourcePrincipals(&newPrincipal,
                                                mPendingPrincipal)) {
    SetPrincipal(newPrincipal);
  }
}

void MediaStreamTrack::NotifyPrincipalHandleChanged(
    const PrincipalHandle& aNewPrincipalHandle) {
  PrincipalHandle handle(aNewPrincipalHandle);
  LOG(LogLevel::Info, ("MediaStreamTrack %p principalHandle changed on "
                       "MediaStreamGraph thread to %p. Current principal: %p, "
                       "pending: %p",
                       this, GetPrincipalFromHandle(handle), mPrincipal.get(),
                       mPendingPrincipal.get()));
  if (PrincipalHandleMatches(handle, mPendingPrincipal)) {
    SetPrincipal(mPendingPrincipal);
    mPendingPrincipal = nullptr;
  }
}

void MediaStreamTrack::MutedChanged(bool aNewState) {
  MOZ_ASSERT(NS_IsMainThread());

  /**
   * 4.3.1 Life-cycle and Media flow - Media flow
   * To set a track's muted state to newState, the User Agent MUST run the
   * following steps:
   *  1. Let track be the MediaStreamTrack in question.
   *  2. Set track's muted attribute to newState.
   *  3. If newState is true let eventName be mute, otherwise unmute.
   *  4. Fire a simple event named eventName on track.
   */

  if (mMuted == aNewState) {
    MOZ_ASSERT_UNREACHABLE("Muted state didn't actually change");
    return;
  }

  LOG(LogLevel::Info,
      ("MediaStreamTrack %p became %s", this, aNewState ? "muted" : "unmuted"));

  mMuted = aNewState;
  nsString eventName =
      aNewState ? NS_LITERAL_STRING("mute") : NS_LITERAL_STRING("unmute");
  DispatchTrustedEvent(eventName);
}

void MediaStreamTrack::NotifyEnded() {
  MOZ_ASSERT(mReadyState == MediaStreamTrackState::Ended);

  auto consumers(mConsumers);
  for (const auto& consumer : consumers) {
    if (consumer) {
      consumer->NotifyEnded(this);
    } else {
      MOZ_ASSERT_UNREACHABLE("A consumer was not explicitly removed");
      mConsumers.RemoveElement(consumer);
    }
  }
}

bool MediaStreamTrack::AddPrincipalChangeObserver(
    PrincipalChangeObserver<MediaStreamTrack>* aObserver) {
  return mPrincipalChangeObservers.AppendElement(aObserver) != nullptr;
}

bool MediaStreamTrack::RemovePrincipalChangeObserver(
    PrincipalChangeObserver<MediaStreamTrack>* aObserver) {
  return mPrincipalChangeObservers.RemoveElement(aObserver);
}

void MediaStreamTrack::AddConsumer(MediaStreamTrackConsumer* aConsumer) {
  MOZ_ASSERT(!mConsumers.Contains(aConsumer));
  mConsumers.AppendElement(aConsumer);

  // Remove destroyed consumers for cleanliness
  while (mConsumers.RemoveElement(nullptr)) {
    MOZ_ASSERT_UNREACHABLE("A consumer was not explicitly removed");
  }
}

void MediaStreamTrack::RemoveConsumer(MediaStreamTrackConsumer* aConsumer) {
  mConsumers.RemoveElement(aConsumer);

  // Remove destroyed consumers for cleanliness
  while (mConsumers.RemoveElement(nullptr)) {
    MOZ_ASSERT_UNREACHABLE("A consumer was not explicitly removed");
  }
}

already_AddRefed<MediaStreamTrack> MediaStreamTrack::Clone() {
  // MediaStreamTracks are currently governed by streams, so we need a dummy
  // DOMMediaStream to own our track clone.
  RefPtr<DOMMediaStream> newStream =
      new DOMMediaStream(mOwningStream->GetParentObject());

  MediaStreamGraph* graph = Graph();
  newStream->InitOwnedStreamCommon(graph);
  newStream->InitPlaybackStreamCommon(graph);

  return newStream->CloneDOMTrack(*this, mTrackID);
}

void MediaStreamTrack::SetReadyState(MediaStreamTrackState aState) {
  MOZ_ASSERT(!(mReadyState == MediaStreamTrackState::Ended &&
               aState == MediaStreamTrackState::Live),
             "We don't support overriding the ready state from ended to live");

  if (mReadyState == MediaStreamTrackState::Live &&
      aState == MediaStreamTrackState::Ended && mSource) {
    mSource->UnregisterSink(mSink.get());
  }

  mReadyState = aState;
}

void MediaStreamTrack::OverrideEnded() {
  MOZ_ASSERT(NS_IsMainThread());

  if (Ended()) {
    return;
  }

  LOG(LogLevel::Info, ("MediaStreamTrack %p ended", this));

  if (!mSource) {
    MOZ_ASSERT(false);
    return;
  }

  mSource->UnregisterSink(mSink.get());

  if (mMSGListener) {
    RemoveListener(mMSGListener);
  }
  mMSGListener = nullptr;

  mReadyState = MediaStreamTrackState::Ended;

  NotifyEnded();

  DispatchTrustedEvent(NS_LITERAL_STRING("ended"));
}

DOMMediaStream* MediaStreamTrack::GetInputDOMStream() {
  MediaStreamTrack* originalTrack =
      mOriginalTrack ? mOriginalTrack.get() : this;
  MOZ_RELEASE_ASSERT(originalTrack->mOwningStream);
  return originalTrack->mOwningStream;
}

MediaStream* MediaStreamTrack::GetInputStream() {
  DOMMediaStream* inputDOMStream = GetInputDOMStream();
  MOZ_RELEASE_ASSERT(inputDOMStream->GetInputStream());
  return inputDOMStream->GetInputStream();
}

ProcessedMediaStream* MediaStreamTrack::GetOwnedStream() {
  if (!mOwningStream) {
    return nullptr;
  }

  return mOwningStream->GetOwnedStream();
}

void MediaStreamTrack::AddListener(MediaStreamTrackListener* aListener) {
  LOG(LogLevel::Debug,
      ("MediaStreamTrack %p adding listener %p", this, aListener));
  MOZ_ASSERT(GetOwnedStream());

  GetOwnedStream()->AddTrackListener(aListener, mTrackID);
  mTrackListeners.AppendElement(aListener);
}

void MediaStreamTrack::RemoveListener(MediaStreamTrackListener* aListener) {
  LOG(LogLevel::Debug,
      ("MediaStreamTrack %p removing listener %p", this, aListener));

  if (GetOwnedStream()) {
    GetOwnedStream()->RemoveTrackListener(aListener, mTrackID);
    mTrackListeners.RemoveElement(aListener);
  }
}

void MediaStreamTrack::AddDirectListener(
    DirectMediaStreamTrackListener* aListener) {
  LOG(LogLevel::Debug, ("MediaStreamTrack %p (%s) adding direct listener %p to "
                        "stream %p, track %d",
                        this, AsAudioStreamTrack() ? "audio" : "video",
                        aListener, GetOwnedStream(), mTrackID));
  MOZ_ASSERT(GetOwnedStream());

  GetOwnedStream()->AddDirectTrackListener(aListener, mTrackID);
  mDirectTrackListeners.AppendElement(aListener);
}

void MediaStreamTrack::RemoveDirectListener(
    DirectMediaStreamTrackListener* aListener) {
  LOG(LogLevel::Debug,
      ("MediaStreamTrack %p removing direct listener %p from stream %p", this,
       aListener, GetOwnedStream()));

  if (GetOwnedStream()) {
    GetOwnedStream()->RemoveDirectTrackListener(aListener, mTrackID);
    mDirectTrackListeners.RemoveElement(aListener);
  }
}

already_AddRefed<MediaInputPort> MediaStreamTrack::ForwardTrackContentsTo(
    ProcessedMediaStream* aStream, TrackID aDestinationTrackID) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_RELEASE_ASSERT(aStream);
  RefPtr<MediaInputPort> port = aStream->AllocateInputPort(
      GetOwnedStream(), mTrackID, aDestinationTrackID);
  return port.forget();
}

bool MediaStreamTrack::IsForwardedThrough(MediaInputPort* aPort) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aPort);
  if (!aPort) {
    return false;
  }
  return aPort->GetSource() == GetOwnedStream() &&
         aPort->PassTrackThrough(mTrackID);
}

}  // namespace dom
}  // namespace mozilla
