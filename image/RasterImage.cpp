/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Must #include ImageLogging.h before any IPDL-generated files or other files
// that #include prlog.h
#include "ImageLogging.h"

#include "RasterImage.h"

#include "gfxPlatform.h"
#include "nsComponentManagerUtils.h"
#include "nsError.h"
#include "DecodePool.h"
#include "Decoder.h"
#include "prenv.h"
#include "prsystem.h"
#include "IDecodingTask.h"
#include "ImageRegion.h"
#include "Layers.h"
#include "LookupResult.h"
#include "nsIConsoleService.h"
#include "nsIInputStream.h"
#include "nsIScriptError.h"
#include "nsISupportsPrimitives.h"
#include "nsMemory.h"
#include "nsPresContext.h"
#include "SourceBuffer.h"
#include "SurfaceCache.h"
#include "FrameAnimator.h"

#include "gfxContext.h"

#include "mozilla/gfx/2D.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Likely.h"
#include "mozilla/RefPtr.h"
#include "mozilla/Move.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/SizeOfState.h"
#include <stdint.h>
#include "mozilla/Telemetry.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/Tuple.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/gfx/Scale.h"

#include "GeckoProfiler.h"
#include "gfx2DGlue.h"
#include "gfxPrefs.h"
#include "nsProperties.h"
#include <algorithm>

namespace mozilla {

using namespace gfx;
using namespace layers;

namespace image {

using std::ceil;
using std::min;

#ifndef DEBUG
NS_IMPL_ISUPPORTS(RasterImage, imgIContainer, nsIProperties)
#else
NS_IMPL_ISUPPORTS(RasterImage, imgIContainer, nsIProperties, imgIContainerDebug)
#endif

//******************************************************************************
RasterImage::RasterImage(nsIURI* aURI /* = nullptr */)
    : ImageResource(aURI),  // invoke superclass's constructor
      mSize(0, 0),
      mLockCount(0),
      mDecoderType(DecoderType::UNKNOWN),
      mDecodeCount(0),
#ifdef DEBUG
      mFramesNotified(0),
#endif
      mSourceBuffer(MakeNotNull<SourceBuffer*>()),
      mHasSize(false),
      mTransient(false),
      mSyncLoad(false),
      mDiscardable(false),
      mSomeSourceData(false),
      mAllSourceData(false),
      mHasBeenDecoded(false),
      mPendingAnimation(false),
      mAnimationFinished(false),
      mWantFullDecode(false) {
}

//******************************************************************************
RasterImage::~RasterImage() {
  // Make sure our SourceBuffer is marked as complete. This will ensure that any
  // outstanding decoders terminate.
  if (!mSourceBuffer->IsComplete()) {
    mSourceBuffer->Complete(NS_ERROR_ABORT);
  }

  // Release all frames from the surface cache.
  SurfaceCache::RemoveImage(ImageKey(this));

  // Record Telemetry.
  Telemetry::Accumulate(Telemetry::IMAGE_DECODE_COUNT, mDecodeCount);
}

nsresult RasterImage::Init(const char* aMimeType, uint32_t aFlags) {
  // We don't support re-initialization
  if (mInitialized) {
    return NS_ERROR_ILLEGAL_VALUE;
  }

  // Not sure an error can happen before init, but be safe
  if (mError) {
    return NS_ERROR_FAILURE;
  }

  // We want to avoid redecodes for transient images.
  MOZ_ASSERT_IF(aFlags & INIT_FLAG_TRANSIENT,
                !(aFlags & INIT_FLAG_DISCARDABLE));

  // Store initialization data
  mDiscardable = !!(aFlags & INIT_FLAG_DISCARDABLE);
  mWantFullDecode = !!(aFlags & INIT_FLAG_DECODE_IMMEDIATELY);
  mTransient = !!(aFlags & INIT_FLAG_TRANSIENT);
  mSyncLoad = !!(aFlags & INIT_FLAG_SYNC_LOAD);

  // Use the MIME type to select a decoder type, and make sure there *is* a
  // decoder for this MIME type.
  NS_ENSURE_ARG_POINTER(aMimeType);
  mDecoderType = DecoderFactory::GetDecoderType(aMimeType);
  if (mDecoderType == DecoderType::UNKNOWN) {
    return NS_ERROR_FAILURE;
  }

  // Lock this image's surfaces in the SurfaceCache if we're not discardable.
  if (!mDiscardable) {
    mLockCount++;
    SurfaceCache::LockImage(ImageKey(this));
  }

  // Mark us as initialized
  mInitialized = true;

  return NS_OK;
}

//******************************************************************************
NS_IMETHODIMP_(void)
RasterImage::RequestRefresh(const TimeStamp& aTime) {
  if (HadRecentRefresh(aTime)) {
    return;
  }

  EvaluateAnimation();

  if (!mAnimating) {
    return;
  }

  RefreshResult res;
  if (mAnimationState) {
    MOZ_ASSERT(mFrameAnimator);
    res = mFrameAnimator->RequestRefresh(*mAnimationState, aTime,
                                         mAnimationFinished);
  }

  if (res.mFrameAdvanced) {
// Notify listeners that our frame has actually changed, but do this only
// once for all frames that we've now passed (if AdvanceFrame() was called
// more than once).
#ifdef DEBUG
    mFramesNotified++;
#endif

    NotifyProgress(NoProgress, res.mDirtyRect);
  }

  if (res.mAnimationFinished) {
    mAnimationFinished = true;
    EvaluateAnimation();
  }
}

//******************************************************************************
NS_IMETHODIMP
RasterImage::GetWidth(int32_t* aWidth) {
  NS_ENSURE_ARG_POINTER(aWidth);

  if (mError) {
    *aWidth = 0;
    return NS_ERROR_FAILURE;
  }

  *aWidth = mSize.width;
  return NS_OK;
}

//******************************************************************************
NS_IMETHODIMP
RasterImage::GetHeight(int32_t* aHeight) {
  NS_ENSURE_ARG_POINTER(aHeight);

  if (mError) {
    *aHeight = 0;
    return NS_ERROR_FAILURE;
  }

  *aHeight = mSize.height;
  return NS_OK;
}

//******************************************************************************
nsresult RasterImage::GetNativeSizes(nsTArray<IntSize>& aNativeSizes) const {
  if (mError) {
    return NS_ERROR_FAILURE;
  }

  if (mNativeSizes.IsEmpty()) {
    aNativeSizes.Clear();
    aNativeSizes.AppendElement(mSize);
  } else {
    aNativeSizes = mNativeSizes;
  }

  return NS_OK;
}

//******************************************************************************
size_t RasterImage::GetNativeSizesLength() const {
  if (mError || !mHasSize) {
    return 0;
  }

  if (mNativeSizes.IsEmpty()) {
    return 1;
  }

  return mNativeSizes.Length();
}

//******************************************************************************
NS_IMETHODIMP
RasterImage::GetIntrinsicSize(nsSize* aSize) {
  if (mError) {
    return NS_ERROR_FAILURE;
  }

  *aSize = nsSize(nsPresContext::CSSPixelsToAppUnits(mSize.width),
                  nsPresContext::CSSPixelsToAppUnits(mSize.height));
  return NS_OK;
}

//******************************************************************************
Maybe<AspectRatio> RasterImage::GetIntrinsicRatio() {
  if (mError) {
    return Nothing();
  }

  return Some(AspectRatio::FromSize(mSize.width, mSize.height));
}

NS_IMETHODIMP_(Orientation)
RasterImage::GetOrientation() { return mOrientation; }

//******************************************************************************
NS_IMETHODIMP
RasterImage::GetType(uint16_t* aType) {
  NS_ENSURE_ARG_POINTER(aType);

  *aType = imgIContainer::TYPE_RASTER;
  return NS_OK;
}

NS_IMETHODIMP
RasterImage::GetProducerId(uint32_t* aId) {
  NS_ENSURE_ARG_POINTER(aId);

  *aId = ImageResource::GetImageProducerId();
  return NS_OK;
}

LookupResult RasterImage::LookupFrameInternal(const IntSize& aSize,
                                              uint32_t aFlags,
                                              PlaybackType aPlaybackType,
                                              bool aMarkUsed) {
  if (mAnimationState && aPlaybackType == PlaybackType::eAnimated) {
    MOZ_ASSERT(mFrameAnimator);
    MOZ_ASSERT(ToSurfaceFlags(aFlags) == DefaultSurfaceFlags(),
               "Can't composite frames with non-default surface flags");
    return mFrameAnimator->GetCompositedFrame(*mAnimationState, aMarkUsed);
  }

  SurfaceFlags surfaceFlags = ToSurfaceFlags(aFlags);

  // We don't want any substitution for sync decodes, and substitution would be
  // illegal when high quality downscaling is disabled, so we use
  // SurfaceCache::Lookup in this case.
  if ((aFlags & FLAG_SYNC_DECODE) || !(aFlags & FLAG_HIGH_QUALITY_SCALING)) {
    return SurfaceCache::Lookup(
        ImageKey(this),
        RasterSurfaceKey(aSize, surfaceFlags, PlaybackType::eStatic),
        aMarkUsed);
  }

  // We'll return the best match we can find to the requested frame.
  return SurfaceCache::LookupBestMatch(
      ImageKey(this),
      RasterSurfaceKey(aSize, surfaceFlags, PlaybackType::eStatic), aMarkUsed);
}

LookupResult RasterImage::LookupFrame(const IntSize& aSize, uint32_t aFlags,
                                      PlaybackType aPlaybackType,
                                      bool aMarkUsed) {
  MOZ_ASSERT(NS_IsMainThread());

  // If we're opaque, we don't need to care about premultiplied alpha, because
  // that can only matter for frames with transparency.
  if (IsOpaque()) {
    aFlags &= ~FLAG_DECODE_NO_PREMULTIPLY_ALPHA;
  }

  IntSize requestedSize =
      CanDownscaleDuringDecode(aSize, aFlags) ? aSize : mSize;
  if (requestedSize.IsEmpty()) {
    // Can't decode to a surface of zero size.
    return LookupResult(MatchType::NOT_FOUND);
  }

  LookupResult result =
      LookupFrameInternal(requestedSize, aFlags, aPlaybackType, aMarkUsed);

  if (!result && !mHasSize) {
    // We can't request a decode without knowing our intrinsic size. Give up.
    return LookupResult(MatchType::NOT_FOUND);
  }

  const bool syncDecode = aFlags & FLAG_SYNC_DECODE;
  const bool avoidRedecode = aFlags & FLAG_AVOID_REDECODE_FOR_SIZE;
  if (result.Type() == MatchType::NOT_FOUND ||
      (result.Type() == MatchType::SUBSTITUTE_BECAUSE_NOT_FOUND &&
       !avoidRedecode) ||
      (syncDecode && !avoidRedecode && !result)) {
    // We don't have a copy of this frame, and there's no decoder working on
    // one. (Or we're sync decoding and the existing decoder hasn't even started
    // yet.) Trigger decoding so it'll be available next time.
    MOZ_ASSERT(aPlaybackType != PlaybackType::eAnimated ||
                   gfxPrefs::ImageMemAnimatedDiscardable() ||
                   !mAnimationState || mAnimationState->KnownFrameCount() < 1,
               "Animated frames should be locked");

    // The surface cache may suggest the preferred size we are supposed to
    // decode at. This should only happen if we accept substitutions.
    if (!result.SuggestedSize().IsEmpty()) {
      MOZ_ASSERT(!syncDecode && (aFlags & FLAG_HIGH_QUALITY_SCALING));
      requestedSize = result.SuggestedSize();
    }

    bool ranSync = Decode(requestedSize, aFlags, aPlaybackType);

    // If we can or did sync decode, we should already have the frame.
    if (ranSync || syncDecode) {
      result =
          LookupFrameInternal(requestedSize, aFlags, aPlaybackType, aMarkUsed);
    }
  }

  if (!result) {
    // We still weren't able to get a frame. Give up.
    return result;
  }

  // Sync decoding guarantees that we got the frame, but if it's owned by an
  // async decoder that's currently running, the contents of the frame may not
  // be available yet. Make sure we get everything.
  if (mAllSourceData && syncDecode) {
    result.Surface()->WaitUntilFinished();
  }

  // If we could have done some decoding in this function we need to check if
  // that decoding encountered an error and hence aborted the surface. We want
  // to avoid calling IsAborted if we weren't passed any sync decode flag
  // because IsAborted acquires the monitor for the imgFrame.
  if (aFlags & (FLAG_SYNC_DECODE | FLAG_SYNC_DECODE_IF_FAST) &&
      result.Surface()->IsAborted()) {
    DrawableSurface tmp = std::move(result.Surface());
    return result;
  }

  return result;
}

bool RasterImage::IsOpaque() {
  if (mError) {
    return false;
  }

  Progress progress = mProgressTracker->GetProgress();

  // If we haven't yet finished decoding, the safe answer is "not opaque".
  if (!(progress & FLAG_DECODE_COMPLETE)) {
    return false;
  }

  // Other, we're opaque if FLAG_HAS_TRANSPARENCY is not set.
  return !(progress & FLAG_HAS_TRANSPARENCY);
}

NS_IMETHODIMP_(bool)
RasterImage::WillDrawOpaqueNow() {
  if (!IsOpaque()) {
    return false;
  }

  if (mAnimationState) {
    if (!gfxPrefs::ImageMemAnimatedDiscardable()) {
      // We never discard frames of animated images.
      return true;
    } else {
      if (mAnimationState->GetCompositedFrameInvalid()) {
        // We're not going to draw anything at all.
        return false;
      }
    }
  }

  // If we are not locked our decoded data could get discard at any time (ie
  // between the call to this function and when we are asked to draw), so we
  // have to return false if we are unlocked.
  if (mLockCount == 0) {
    return false;
  }

  LookupResult result = SurfaceCache::LookupBestMatch(
      ImageKey(this),
      RasterSurfaceKey(mSize, DefaultSurfaceFlags(), PlaybackType::eStatic),
      /* aMarkUsed = */ false);
  MatchType matchType = result.Type();
  if (matchType == MatchType::NOT_FOUND || matchType == MatchType::PENDING ||
      !result.Surface()->IsFinished()) {
    return false;
  }

  return true;
}

void RasterImage::OnSurfaceDiscarded(const SurfaceKey& aSurfaceKey) {
  MOZ_ASSERT(mProgressTracker);

  bool animatedFramesDiscarded =
      mAnimationState && aSurfaceKey.Playback() == PlaybackType::eAnimated;

  nsCOMPtr<nsIEventTarget> eventTarget;
  if (mProgressTracker) {
    eventTarget = mProgressTracker->GetEventTarget();
  } else {
    eventTarget = do_GetMainThread();
  }

  RefPtr<RasterImage> image = this;
  nsCOMPtr<nsIRunnable> ev =
      NS_NewRunnableFunction("RasterImage::OnSurfaceDiscarded", [=]() -> void {
        image->OnSurfaceDiscardedInternal(animatedFramesDiscarded);
      });
  eventTarget->Dispatch(ev.forget(), NS_DISPATCH_NORMAL);
}

void RasterImage::OnSurfaceDiscardedInternal(bool aAnimatedFramesDiscarded) {
  MOZ_ASSERT(NS_IsMainThread());

  if (aAnimatedFramesDiscarded && mAnimationState) {
    MOZ_ASSERT(gfxPrefs::ImageMemAnimatedDiscardable());
    ReleaseImageContainer();
    gfx::IntRect rect =
        mAnimationState->UpdateState(mAnimationFinished, this, mSize);
    NotifyProgress(NoProgress, rect);
  }

  if (mProgressTracker) {
    mProgressTracker->OnDiscard();
  }
}

//******************************************************************************
NS_IMETHODIMP
RasterImage::GetAnimated(bool* aAnimated) {
  if (mError) {
    return NS_ERROR_FAILURE;
  }

  NS_ENSURE_ARG_POINTER(aAnimated);

  // If we have an AnimationState, we can know for sure.
  if (mAnimationState) {
    *aAnimated = true;
    return NS_OK;
  }

  // Otherwise, we need to have been decoded to know for sure, since if we were
  // decoded at least once mAnimationState would have been created for animated
  // images. This is true even though we check for animation during the
  // metadata decode, because we may still discover animation only during the
  // full decode for corrupt images.
  if (!mHasBeenDecoded) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  // We know for sure
  *aAnimated = false;

  return NS_OK;
}

//******************************************************************************
NS_IMETHODIMP_(int32_t)
RasterImage::GetFirstFrameDelay() {
  if (mError) {
    return -1;
  }

  bool animated = false;
  if (NS_FAILED(GetAnimated(&animated)) || !animated) {
    return -1;
  }

  MOZ_ASSERT(mAnimationState, "Animated images should have an AnimationState");
  return mAnimationState->FirstFrameTimeout().AsEncodedValueDeprecated();
}

NS_IMETHODIMP_(already_AddRefed<SourceSurface>)
RasterImage::GetFrame(uint32_t aWhichFrame, uint32_t aFlags) {
  return GetFrameAtSize(mSize, aWhichFrame, aFlags);
}

NS_IMETHODIMP_(already_AddRefed<SourceSurface>)
RasterImage::GetFrameAtSize(const IntSize& aSize, uint32_t aWhichFrame,
                            uint32_t aFlags) {
#ifdef DEBUG
  NotifyDrawingObservers();
#endif

  auto result = GetFrameInternal(aSize, Nothing(), aWhichFrame, aFlags);
  return mozilla::Get<2>(result).forget();
}

Tuple<ImgDrawResult, IntSize, RefPtr<SourceSurface>>
RasterImage::GetFrameInternal(const IntSize& aSize,
                              const Maybe<SVGImageContext>& aSVGContext,
                              uint32_t aWhichFrame, uint32_t aFlags) {
  MOZ_ASSERT(aWhichFrame <= FRAME_MAX_VALUE);

  if (aSize.IsEmpty() || aWhichFrame > FRAME_MAX_VALUE) {
    return MakeTuple(ImgDrawResult::BAD_ARGS, aSize, RefPtr<SourceSurface>());
  }

  if (mError) {
    return MakeTuple(ImgDrawResult::BAD_IMAGE, aSize, RefPtr<SourceSurface>());
  }

  // Get the frame. If it's not there, it's probably the caller's fault for
  // not waiting for the data to be loaded from the network or not passing
  // FLAG_SYNC_DECODE.
  LookupResult result = LookupFrame(aSize, aFlags, ToPlaybackType(aWhichFrame),
                                    /* aMarkUsed = */ true);

  // The surface cache may have suggested we use a different size than the
  // given size in the future. This may or may not be accompanied by an
  // actual surface, depending on what it has in its cache.
  IntSize suggestedSize =
      result.SuggestedSize().IsEmpty() ? aSize : result.SuggestedSize();
  MOZ_ASSERT_IF(result.Type() == MatchType::SUBSTITUTE_BECAUSE_BEST,
                suggestedSize != aSize);

  if (!result) {
    // The OS threw this frame away and we couldn't redecode it.
    return MakeTuple(ImgDrawResult::TEMPORARY_ERROR, suggestedSize,
                     RefPtr<SourceSurface>());
  }

  RefPtr<SourceSurface> surface = result.Surface()->GetSourceSurface();
  if (!result.Surface()->IsFinished()) {
    return MakeTuple(ImgDrawResult::INCOMPLETE, suggestedSize,
                     std::move(surface));
  }

  return MakeTuple(ImgDrawResult::SUCCESS, suggestedSize, std::move(surface));
}

Tuple<ImgDrawResult, IntSize> RasterImage::GetImageContainerSize(
    LayerManager* aManager, const IntSize& aSize, uint32_t aFlags) {
  if (!mHasSize) {
    return MakeTuple(ImgDrawResult::NOT_READY, IntSize(0, 0));
  }

  if (aSize.IsEmpty()) {
    return MakeTuple(ImgDrawResult::BAD_ARGS, IntSize(0, 0));
  }

  // We check the minimum size because while we support downscaling, we do not
  // support upscaling. If aSize > mSize, we will never give a larger surface
  // than mSize. If mSize > aSize, and mSize > maxTextureSize, we still want to
  // use image containers if aSize <= maxTextureSize.
  int32_t maxTextureSize = aManager->GetMaxTextureSize();
  if (min(mSize.width, aSize.width) > maxTextureSize ||
      min(mSize.height, aSize.height) > maxTextureSize) {
    return MakeTuple(ImgDrawResult::NOT_SUPPORTED, IntSize(0, 0));
  }

  if (!CanDownscaleDuringDecode(aSize, aFlags)) {
    return MakeTuple(ImgDrawResult::SUCCESS, mSize);
  }

  return MakeTuple(ImgDrawResult::SUCCESS, aSize);
}

NS_IMETHODIMP_(bool)
RasterImage::IsImageContainerAvailable(LayerManager* aManager,
                                       uint32_t aFlags) {
  return IsImageContainerAvailableAtSize(aManager, mSize, aFlags);
}

NS_IMETHODIMP_(already_AddRefed<ImageContainer>)
RasterImage::GetImageContainer(LayerManager* aManager, uint32_t aFlags) {
  RefPtr<ImageContainer> container;
  ImgDrawResult drawResult = GetImageContainerImpl(
      aManager, mSize, Nothing(), aFlags, getter_AddRefs(container));

  // We silence the unused warning here because anything that needs the draw
  // result should be using GetImageContainerAtSize, not GetImageContainer.
  (void)drawResult;
  return container.forget();
}

NS_IMETHODIMP_(bool)
RasterImage::IsImageContainerAvailableAtSize(LayerManager* aManager,
                                             const IntSize& aSize,
                                             uint32_t aFlags) {
  // We check the minimum size because while we support downscaling, we do not
  // support upscaling. If aSize > mSize, we will never give a larger surface
  // than mSize. If mSize > aSize, and mSize > maxTextureSize, we still want to
  // use image containers if aSize <= maxTextureSize.
  int32_t maxTextureSize = aManager->GetMaxTextureSize();
  if (!mHasSize || aSize.IsEmpty() ||
      min(mSize.width, aSize.width) > maxTextureSize ||
      min(mSize.height, aSize.height) > maxTextureSize) {
    return false;
  }

  return true;
}

NS_IMETHODIMP_(ImgDrawResult)
RasterImage::GetImageContainerAtSize(layers::LayerManager* aManager,
                                     const gfx::IntSize& aSize,
                                     const Maybe<SVGImageContext>& aSVGContext,
                                     uint32_t aFlags,
                                     layers::ImageContainer** aOutContainer) {
  // We do not pass in the given SVG context because in theory it could differ
  // between calls, but actually have no impact on the actual contents of the
  // image container.
  return GetImageContainerImpl(aManager, aSize, Nothing(), aFlags,
                               aOutContainer);
}

size_t RasterImage::SizeOfSourceWithComputedFallback(
    SizeOfState& aState) const {
  return mSourceBuffer->SizeOfIncludingThisWithComputedFallback(
      aState.mMallocSizeOf);
}

void RasterImage::CollectSizeOfSurfaces(
    nsTArray<SurfaceMemoryCounter>& aCounters,
    MallocSizeOf aMallocSizeOf) const {
  SurfaceCache::CollectSizeOfSurfaces(ImageKey(this), aCounters, aMallocSizeOf);
}

bool RasterImage::SetMetadata(const ImageMetadata& aMetadata,
                              bool aFromMetadataDecode) {
  MOZ_ASSERT(NS_IsMainThread());

  if (mError) {
    return true;
  }

  if (aMetadata.HasSize()) {
    IntSize size = aMetadata.GetSize();
    if (size.width < 0 || size.height < 0) {
      NS_WARNING("Image has negative intrinsic size");
      DoError();
      return true;
    }

    MOZ_ASSERT(aMetadata.HasOrientation());
    Orientation orientation = aMetadata.GetOrientation();

    // If we already have a size, check the new size against the old one.
    if (mHasSize && (size != mSize || orientation != mOrientation)) {
      NS_WARNING(
          "Image changed size or orientation on redecode! "
          "This should not happen!");
      DoError();
      return true;
    }

    // Set the size and flag that we have it.
    mSize = size;
    mOrientation = orientation;
    mNativeSizes = aMetadata.GetNativeSizes();
    mHasSize = true;
  }

  if (mHasSize && aMetadata.HasAnimation() && !mAnimationState) {
    // We're becoming animated, so initialize animation stuff.
    mAnimationState.emplace(mAnimationMode);
    mFrameAnimator = MakeUnique<FrameAnimator>(this, mSize);

    if (!gfxPrefs::ImageMemAnimatedDiscardable()) {
      // We don't support discarding animated images (See bug 414259).
      // Lock the image and throw away the key.
      LockImage();
    }

    if (!aFromMetadataDecode) {
      // The metadata decode reported that this image isn't animated, but we
      // discovered that it actually was during the full decode. This is a
      // rare failure that only occurs for corrupt images. To recover, we need
      // to discard all existing surfaces and redecode.
      return false;
    }
  }

  if (mAnimationState) {
    mAnimationState->SetLoopCount(aMetadata.GetLoopCount());
    mAnimationState->SetFirstFrameTimeout(aMetadata.GetFirstFrameTimeout());

    if (aMetadata.HasLoopLength()) {
      mAnimationState->SetLoopLength(aMetadata.GetLoopLength());
    }
    if (aMetadata.HasFirstFrameRefreshArea()) {
      mAnimationState->SetFirstFrameRefreshArea(
          aMetadata.GetFirstFrameRefreshArea());
    }
  }

  if (aMetadata.HasHotspot()) {
    IntPoint hotspot = aMetadata.GetHotspot();

    nsCOMPtr<nsISupportsPRUint32> intwrapx =
        do_CreateInstance(NS_SUPPORTS_PRUINT32_CONTRACTID);
    nsCOMPtr<nsISupportsPRUint32> intwrapy =
        do_CreateInstance(NS_SUPPORTS_PRUINT32_CONTRACTID);
    intwrapx->SetData(hotspot.x);
    intwrapy->SetData(hotspot.y);

    Set("hotspotX", intwrapx);
    Set("hotspotY", intwrapy);
  }

  return true;
}

NS_IMETHODIMP
RasterImage::SetAnimationMode(uint16_t aAnimationMode) {
  if (mAnimationState) {
    mAnimationState->SetAnimationMode(aAnimationMode);
  }
  return SetAnimationModeInternal(aAnimationMode);
}

//******************************************************************************

nsresult RasterImage::StartAnimation() {
  if (mError) {
    return NS_ERROR_FAILURE;
  }

  MOZ_ASSERT(ShouldAnimate(), "Should not animate!");

  // If we're not ready to animate, then set mPendingAnimation, which will cause
  // us to start animating if and when we do become ready.
  mPendingAnimation =
      !mAnimationState || mAnimationState->KnownFrameCount() < 1;
  if (mPendingAnimation) {
    return NS_OK;
  }

  // Don't bother to animate if we're displaying the first frame forever.
  if (mAnimationState->GetCurrentAnimationFrameIndex() == 0 &&
      mAnimationState->FirstFrameTimeout() == FrameTimeout::Forever()) {
    mAnimationFinished = true;
    return NS_ERROR_ABORT;
  }

  // We need to set the time that this initial frame was first displayed, as
  // this is used in AdvanceFrame().
  mAnimationState->InitAnimationFrameTimeIfNecessary();

  return NS_OK;
}

//******************************************************************************
nsresult RasterImage::StopAnimation() {
  MOZ_ASSERT(mAnimating, "Should be animating!");

  nsresult rv = NS_OK;
  if (mError) {
    rv = NS_ERROR_FAILURE;
  } else {
    mAnimationState->SetAnimationFrameTime(TimeStamp());
  }

  mAnimating = false;
  return rv;
}

//******************************************************************************
NS_IMETHODIMP
RasterImage::ResetAnimation() {
  if (mError) {
    return NS_ERROR_FAILURE;
  }

  mPendingAnimation = false;

  if (mAnimationMode == kDontAnimMode || !mAnimationState ||
      mAnimationState->GetCurrentAnimationFrameIndex() == 0) {
    return NS_OK;
  }

  mAnimationFinished = false;

  if (mAnimating) {
    StopAnimation();
  }

  MOZ_ASSERT(mAnimationState, "Should have AnimationState");
  MOZ_ASSERT(mFrameAnimator, "Should have FrameAnimator");
  mFrameAnimator->ResetAnimation(*mAnimationState);

  NotifyProgress(NoProgress, mAnimationState->FirstFrameRefreshArea());

  // Start the animation again. It may not have been running before, if
  // mAnimationFinished was true before entering this function.
  EvaluateAnimation();

  return NS_OK;
}

//******************************************************************************
NS_IMETHODIMP_(void)
RasterImage::SetAnimationStartTime(const TimeStamp& aTime) {
  if (mError || mAnimationMode == kDontAnimMode || mAnimating ||
      !mAnimationState) {
    return;
  }

  mAnimationState->SetAnimationFrameTime(aTime);
}

NS_IMETHODIMP_(float)
RasterImage::GetFrameIndex(uint32_t aWhichFrame) {
  MOZ_ASSERT(aWhichFrame <= FRAME_MAX_VALUE, "Invalid argument");
  return (aWhichFrame == FRAME_FIRST || !mAnimationState)
             ? 0.0f
             : mAnimationState->GetCurrentAnimationFrameIndex();
}

NS_IMETHODIMP_(IntRect)
RasterImage::GetImageSpaceInvalidationRect(const IntRect& aRect) {
  return aRect;
}

nsresult RasterImage::OnImageDataComplete(nsIRequest*, nsISupports*,
                                          nsresult aStatus, bool aLastPart) {
  MOZ_ASSERT(NS_IsMainThread());

  // Record that we have all the data we're going to get now.
  mAllSourceData = true;

  // Let decoders know that there won't be any more data coming.
  mSourceBuffer->Complete(aStatus);

  // Allow a synchronous metadata decode if mSyncLoad was set, or if we're
  // running on a single thread (in which case waiting for the async metadata
  // decoder could delay this image's load event quite a bit), or if this image
  // is transient.
  bool canSyncDecodeMetadata =
      mSyncLoad || mTransient || DecodePool::NumberOfCores() < 2;

  if (canSyncDecodeMetadata && !mHasSize) {
    // We're loading this image synchronously, so it needs to be usable after
    // this call returns.  Since we haven't gotten our size yet, we need to do a
    // synchronous metadata decode here.
    DecodeMetadata(FLAG_SYNC_DECODE);
  }

  // Determine our final status, giving precedence to Necko failure codes. We
  // check after running the metadata decode in case it triggered an error.
  nsresult finalStatus = mError ? NS_ERROR_FAILURE : NS_OK;
  if (NS_FAILED(aStatus)) {
    finalStatus = aStatus;
  }

  // If loading failed, report an error.
  if (NS_FAILED(finalStatus)) {
    DoError();
  }

  Progress loadProgress = LoadCompleteProgress(aLastPart, mError, finalStatus);

  if (!mHasSize && !mError) {
    // We don't have our size yet, so we'll fire the load event in SetSize().
    MOZ_ASSERT(!canSyncDecodeMetadata,
               "Firing load async after metadata sync decode?");
    mLoadProgress = Some(loadProgress);
    return finalStatus;
  }

  NotifyForLoadEvent(loadProgress);

  return finalStatus;
}

void RasterImage::NotifyForLoadEvent(Progress aProgress) {
  MOZ_ASSERT(mHasSize || mError, "Need to know size before firing load event");
  MOZ_ASSERT(
      !mHasSize || (mProgressTracker->GetProgress() & FLAG_SIZE_AVAILABLE),
      "Should have notified that the size is available if we have it");

  // If we encountered an error, make sure we notify for that as well.
  if (mError) {
    aProgress |= FLAG_HAS_ERROR;
  }

  // Notify our listeners, which will fire this image's load event.
  NotifyProgress(aProgress);
}

nsresult RasterImage::OnImageDataAvailable(nsIRequest*, nsISupports*,
                                           nsIInputStream* aInputStream,
                                           uint64_t, uint32_t aCount) {
  nsresult rv = mSourceBuffer->AppendFromInputStream(aInputStream, aCount);
  if (NS_SUCCEEDED(rv) && !mSomeSourceData) {
    mSomeSourceData = true;
    if (!mSyncLoad) {
      // Create an async metadata decoder and verify we succeed in doing so.
      rv = DecodeMetadata(DECODE_FLAGS_DEFAULT);
    }
  }

  if (NS_FAILED(rv)) {
    DoError();
  }
  return rv;
}

nsresult RasterImage::SetSourceSizeHint(uint32_t aSizeHint) {
  if (aSizeHint == 0) {
    return NS_OK;
  }

  nsresult rv = mSourceBuffer->ExpectLength(aSizeHint);
  if (rv == NS_ERROR_OUT_OF_MEMORY) {
    // Flush memory, try to get some back, and try again.
    rv = nsMemory::HeapMinimize(true);
    if (NS_SUCCEEDED(rv)) {
      rv = mSourceBuffer->ExpectLength(aSizeHint);
    }
  }

  return rv;
}

/********* Methods to implement lazy allocation of nsIProperties object *******/
NS_IMETHODIMP
RasterImage::Get(const char* prop, const nsIID& iid, void** result) {
  if (!mProperties) {
    return NS_ERROR_FAILURE;
  }
  return mProperties->Get(prop, iid, result);
}

NS_IMETHODIMP
RasterImage::Set(const char* prop, nsISupports* value) {
  if (!mProperties) {
    mProperties = new nsProperties();
  }
  return mProperties->Set(prop, value);
}

NS_IMETHODIMP
RasterImage::Has(const char* prop, bool* _retval) {
  NS_ENSURE_ARG_POINTER(_retval);
  if (!mProperties) {
    *_retval = false;
    return NS_OK;
  }
  return mProperties->Has(prop, _retval);
}

NS_IMETHODIMP
RasterImage::Undefine(const char* prop) {
  if (!mProperties) {
    return NS_ERROR_FAILURE;
  }
  return mProperties->Undefine(prop);
}

NS_IMETHODIMP
RasterImage::GetKeys(nsTArray<nsCString>& keys) {
  if (!mProperties) {
    keys.Clear();
    return NS_OK;
  }
  return mProperties->GetKeys(keys);
}

void RasterImage::Discard() {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(CanDiscard(), "Asked to discard but can't");
  MOZ_ASSERT(!mAnimationState || gfxPrefs::ImageMemAnimatedDiscardable(),
             "Asked to discard for animated image");

  // Delete all the decoded frames.
  SurfaceCache::RemoveImage(ImageKey(this));

  if (mAnimationState) {
    ReleaseImageContainer();
    gfx::IntRect rect =
        mAnimationState->UpdateState(mAnimationFinished, this, mSize);
    NotifyProgress(NoProgress, rect);
  }

  // Notify that we discarded.
  if (mProgressTracker) {
    mProgressTracker->OnDiscard();
  }
}

bool RasterImage::CanDiscard() {
  return mAllSourceData &&
         // Can discard animated images if the pref is set
         (!mAnimationState || gfxPrefs::ImageMemAnimatedDiscardable());
}

NS_IMETHODIMP
RasterImage::StartDecoding(uint32_t aFlags, uint32_t aWhichFrame) {
  if (mError) {
    return NS_ERROR_FAILURE;
  }

  if (!mHasSize) {
    mWantFullDecode = true;
    return NS_OK;
  }

  uint32_t flags = (aFlags & FLAG_ASYNC_NOTIFY) | FLAG_SYNC_DECODE_IF_FAST |
                   FLAG_HIGH_QUALITY_SCALING;
  return RequestDecodeForSize(mSize, flags, aWhichFrame);
}

bool RasterImage::StartDecodingWithResult(uint32_t aFlags,
                                          uint32_t aWhichFrame) {
  if (mError) {
    return false;
  }

  if (!mHasSize) {
    mWantFullDecode = true;
    return false;
  }

  uint32_t flags = (aFlags & FLAG_ASYNC_NOTIFY) | FLAG_SYNC_DECODE_IF_FAST |
                   FLAG_HIGH_QUALITY_SCALING;
  DrawableSurface surface =
      RequestDecodeForSizeInternal(mSize, flags, aWhichFrame);
  return surface && surface->IsFinished();
}

bool RasterImage::RequestDecodeWithResult(uint32_t aFlags,
                                          uint32_t aWhichFrame) {
  MOZ_ASSERT(NS_IsMainThread());

  if (mError) {
    return false;
  }

  uint32_t flags = aFlags | FLAG_ASYNC_NOTIFY;
  DrawableSurface surface =
      RequestDecodeForSizeInternal(mSize, flags, aWhichFrame);
  return surface && surface->IsFinished();
}

NS_IMETHODIMP
RasterImage::RequestDecodeForSize(const IntSize& aSize, uint32_t aFlags,
                                  uint32_t aWhichFrame) {
  MOZ_ASSERT(NS_IsMainThread());

  if (mError) {
    return NS_ERROR_FAILURE;
  }

  RequestDecodeForSizeInternal(aSize, aFlags, aWhichFrame);

  return NS_OK;
}

DrawableSurface RasterImage::RequestDecodeForSizeInternal(
    const IntSize& aSize, uint32_t aFlags, uint32_t aWhichFrame) {
  MOZ_ASSERT(NS_IsMainThread());

  if (aWhichFrame > FRAME_MAX_VALUE) {
    return DrawableSurface();
  }

  if (mError) {
    return DrawableSurface();
  }

  if (!mHasSize) {
    mWantFullDecode = true;
    return DrawableSurface();
  }

  // Decide whether to sync decode images we can decode quickly. Here we are
  // explicitly trading off flashing for responsiveness in the case that we're
  // redecoding an image (see bug 845147).
  bool shouldSyncDecodeIfFast =
      !mHasBeenDecoded && (aFlags & FLAG_SYNC_DECODE_IF_FAST);

  uint32_t flags =
      shouldSyncDecodeIfFast ? aFlags : aFlags & ~FLAG_SYNC_DECODE_IF_FAST;

  // Perform a frame lookup, which will implicitly start decoding if needed.
  LookupResult result = LookupFrame(aSize, flags, ToPlaybackType(aWhichFrame),
                                    /* aMarkUsed = */ false);
  return std::move(result.Surface());
}

static bool LaunchDecodingTask(IDecodingTask* aTask, RasterImage* aImage,
                               uint32_t aFlags, bool aHaveSourceData) {
  if (aHaveSourceData) {
    nsCString uri(aImage->GetURIString());

    // If we have all the data, we can sync decode if requested.
    if (aFlags & imgIContainer::FLAG_SYNC_DECODE) {
      DecodePool::Singleton()->SyncRunIfPossible(aTask, uri);
      return true;
    }

    if (aFlags & imgIContainer::FLAG_SYNC_DECODE_IF_FAST) {
      return DecodePool::Singleton()->SyncRunIfPreferred(aTask, uri);
    }
  }

  // Perform an async decode. We also take this path if we don't have all the
  // source data yet, since sync decoding is impossible in that situation.
  DecodePool::Singleton()->AsyncRun(aTask);
  return false;
}

bool RasterImage::Decode(const IntSize& aSize, uint32_t aFlags,
                         PlaybackType aPlaybackType) {
  MOZ_ASSERT(NS_IsMainThread());

  if (mError) {
    return false;
  }

  // If we don't have a size yet, we can't do any other decoding.
  if (!mHasSize) {
    mWantFullDecode = true;
    return false;
  }

  // We're about to decode again, which may mean that some of the previous sizes
  // we've decoded at aren't useful anymore. We can allow them to expire from
  // the cache by unlocking them here. When the decode finishes, it will send an
  // invalidation that will cause all instances of this image to redraw. If this
  // image is locked, any surfaces that are still useful will become locked
  // again when LookupFrame touches them, and the remainder will eventually
  // expire.
  SurfaceCache::UnlockEntries(ImageKey(this));

  // Determine which flags we need to decode this image with.
  DecoderFlags decoderFlags = DefaultDecoderFlags();
  if (aFlags & FLAG_ASYNC_NOTIFY) {
    decoderFlags |= DecoderFlags::ASYNC_NOTIFY;
  }
  if (mTransient) {
    decoderFlags |= DecoderFlags::IMAGE_IS_TRANSIENT;
  }
  if (mHasBeenDecoded) {
    decoderFlags |= DecoderFlags::IS_REDECODE;
  }
  if ((aFlags & FLAG_SYNC_DECODE) || !(aFlags & FLAG_HIGH_QUALITY_SCALING)) {
    // Used SurfaceCache::Lookup instead of SurfaceCache::LookupBestMatch. That
    // means the caller can handle a differently sized surface to be returned
    // at any point.
    decoderFlags |= DecoderFlags::CANNOT_SUBSTITUTE;
  }

  SurfaceFlags surfaceFlags = ToSurfaceFlags(aFlags);
  if (IsOpaque()) {
    // If there's no transparency, it doesn't matter whether we premultiply
    // alpha or not.
    surfaceFlags &= ~SurfaceFlags::NO_PREMULTIPLY_ALPHA;
  }

  // Create a decoder.
  RefPtr<IDecodingTask> task;
  nsresult rv;
  bool animated = mAnimationState && aPlaybackType == PlaybackType::eAnimated;
  if (animated) {
    size_t currentFrame = mAnimationState->GetCurrentAnimationFrameIndex();
    rv = DecoderFactory::CreateAnimationDecoder(
        mDecoderType, WrapNotNull(this), mSourceBuffer, mSize, decoderFlags,
        surfaceFlags, currentFrame, getter_AddRefs(task));
  } else {
    rv = DecoderFactory::CreateDecoder(
        mDecoderType, WrapNotNull(this), mSourceBuffer, mSize, aSize,
        decoderFlags, surfaceFlags, getter_AddRefs(task));
  }

  if (rv == NS_ERROR_ALREADY_INITIALIZED) {
    // We raced with an already pending decoder, and it finished before we
    // managed to insert the new decoder. Pretend we did a sync call to make
    // the caller lookup in the surface cache again.
    MOZ_ASSERT(!task);
    return true;
  }

  if (animated) {
    // We pass false for aAllowInvalidation because we may be asked to use
    // async notifications. Any potential invalidation here will be sent when
    // RequestRefresh is called, or NotifyDecodeComplete.
#ifdef DEBUG
    gfx::IntRect rect =
#endif
        mAnimationState->UpdateState(mAnimationFinished, this, mSize, false);
    MOZ_ASSERT(rect.IsEmpty());
  }

  // Make sure DecoderFactory was able to create a decoder successfully.
  if (NS_FAILED(rv)) {
    MOZ_ASSERT(!task);
    return false;
  }

  MOZ_ASSERT(task);
  mDecodeCount++;

  // We're ready to decode; start the decoder.
  return LaunchDecodingTask(task, this, aFlags, mAllSourceData);
}

NS_IMETHODIMP
RasterImage::DecodeMetadata(uint32_t aFlags) {
  if (mError) {
    return NS_ERROR_FAILURE;
  }

  MOZ_ASSERT(!mHasSize, "Should not do unnecessary metadata decodes");

  // Create a decoder.
  RefPtr<IDecodingTask> task = DecoderFactory::CreateMetadataDecoder(
      mDecoderType, WrapNotNull(this), mSourceBuffer);

  // Make sure DecoderFactory was able to create a decoder successfully.
  if (!task) {
    return NS_ERROR_FAILURE;
  }

  // We're ready to decode; start the decoder.
  LaunchDecodingTask(task, this, aFlags, mAllSourceData);
  return NS_OK;
}

void RasterImage::RecoverFromInvalidFrames(const IntSize& aSize,
                                           uint32_t aFlags) {
  if (!mHasSize) {
    return;
  }

  NS_WARNING("A RasterImage's frames became invalid. Attempting to recover...");

  // Discard all existing frames, since they're probably all now invalid.
  SurfaceCache::RemoveImage(ImageKey(this));

  // Relock the image if it's supposed to be locked.
  if (mLockCount > 0) {
    SurfaceCache::LockImage(ImageKey(this));
  }

  // Animated images require some special handling, because we normally require
  // that they never be discarded.
  if (mAnimationState) {
    Decode(mSize, aFlags | FLAG_SYNC_DECODE, PlaybackType::eAnimated);
    ResetAnimation();
    return;
  }

  // For non-animated images, it's fine to recover using an async decode.
  Decode(aSize, aFlags, PlaybackType::eStatic);
}

static bool HaveSkia() {
#ifdef MOZ_ENABLE_SKIA
  return true;
#else
  return false;
#endif
}

bool RasterImage::CanDownscaleDuringDecode(const IntSize& aSize,
                                           uint32_t aFlags) {
  // Check basic requirements: downscale-during-decode is enabled, Skia is
  // available, this image isn't transient, we have all the source data and know
  // our size, and the flags allow us to do it.
  if (!mHasSize || mTransient || !HaveSkia() ||
      !gfxPrefs::ImageDownscaleDuringDecodeEnabled() ||
      !(aFlags & imgIContainer::FLAG_HIGH_QUALITY_SCALING)) {
    return false;
  }

  // We don't downscale animated images during decode.
  if (mAnimationState) {
    return false;
  }

  // Never upscale.
  if (aSize.width >= mSize.width || aSize.height >= mSize.height) {
    return false;
  }

  // Zero or negative width or height is unacceptable.
  if (aSize.width < 1 || aSize.height < 1) {
    return false;
  }

  // There's no point in scaling if we can't store the result.
  if (!SurfaceCache::CanHold(aSize)) {
    return false;
  }

  return true;
}

ImgDrawResult RasterImage::DrawInternal(DrawableSurface&& aSurface,
                                        gfxContext* aContext,
                                        const IntSize& aSize,
                                        const ImageRegion& aRegion,
                                        SamplingFilter aSamplingFilter,
                                        uint32_t aFlags, float aOpacity) {
  gfxContextMatrixAutoSaveRestore saveMatrix(aContext);
  ImageRegion region(aRegion);
  bool frameIsFinished = aSurface->IsFinished();

#ifdef DEBUG
  NotifyDrawingObservers();
#endif

  // By now we may have a frame with the requested size. If not, we need to
  // adjust the drawing parameters accordingly.
  IntSize finalSize = aSurface->GetSize();
  bool couldRedecodeForBetterFrame = false;
  if (finalSize != aSize) {
    gfx::Size scale(double(aSize.width) / finalSize.width,
                    double(aSize.height) / finalSize.height);
    aContext->Multiply(gfxMatrix::Scaling(scale.width, scale.height));
    region.Scale(1.0 / scale.width, 1.0 / scale.height);

    couldRedecodeForBetterFrame = CanDownscaleDuringDecode(aSize, aFlags);
  }

  if (!aSurface->Draw(aContext, region, aSamplingFilter, aFlags, aOpacity)) {
    RecoverFromInvalidFrames(aSize, aFlags);
    return ImgDrawResult::TEMPORARY_ERROR;
  }
  if (!frameIsFinished) {
    return ImgDrawResult::INCOMPLETE;
  }
  if (couldRedecodeForBetterFrame) {
    return ImgDrawResult::WRONG_SIZE;
  }
  return ImgDrawResult::SUCCESS;
}

//******************************************************************************
NS_IMETHODIMP_(ImgDrawResult)
RasterImage::Draw(gfxContext* aContext, const IntSize& aSize,
                  const ImageRegion& aRegion, uint32_t aWhichFrame,
                  SamplingFilter aSamplingFilter,
                  const Maybe<SVGImageContext>& /*aSVGContext - ignored*/,
                  uint32_t aFlags, float aOpacity) {
  if (aWhichFrame > FRAME_MAX_VALUE) {
    return ImgDrawResult::BAD_ARGS;
  }

  if (mError) {
    return ImgDrawResult::BAD_IMAGE;
  }

  // Illegal -- you can't draw with non-default decode flags.
  // (Disabling colorspace conversion might make sense to allow, but
  // we don't currently.)
  if (ToSurfaceFlags(aFlags) != DefaultSurfaceFlags()) {
    return ImgDrawResult::BAD_ARGS;
  }

  if (!aContext) {
    return ImgDrawResult::BAD_ARGS;
  }

  if (mAnimationConsumers == 0) {
    SendOnUnlockedDraw(aFlags);
  }

  // If we're not using SamplingFilter::GOOD, we shouldn't high-quality scale or
  // downscale during decode.
  uint32_t flags = aSamplingFilter == SamplingFilter::GOOD
                       ? aFlags
                       : aFlags & ~FLAG_HIGH_QUALITY_SCALING;

  LookupResult result = LookupFrame(aSize, flags, ToPlaybackType(aWhichFrame),
                                    /* aMarkUsed = */ true);
  if (!result) {
    // Getting the frame (above) touches the image and kicks off decoding.
    if (mDrawStartTime.IsNull()) {
      mDrawStartTime = TimeStamp::Now();
    }
    return ImgDrawResult::NOT_READY;
  }

  bool shouldRecordTelemetry =
      !mDrawStartTime.IsNull() && result.Surface()->IsFinished();

  auto drawResult = DrawInternal(std::move(result.Surface()), aContext, aSize,
                                 aRegion, aSamplingFilter, flags, aOpacity);

  if (shouldRecordTelemetry) {
    TimeDuration drawLatency = TimeStamp::Now() - mDrawStartTime;
    Telemetry::Accumulate(Telemetry::IMAGE_DECODE_ON_DRAW_LATENCY,
                          int32_t(drawLatency.ToMicroseconds()));
    mDrawStartTime = TimeStamp();
  }

  return drawResult;
}

//******************************************************************************

NS_IMETHODIMP
RasterImage::LockImage() {
  MOZ_ASSERT(NS_IsMainThread(),
             "Main thread to encourage serialization with UnlockImage");
  if (mError) {
    return NS_ERROR_FAILURE;
  }

  // Increment the lock count
  mLockCount++;

  // Lock this image's surfaces in the SurfaceCache.
  if (mLockCount == 1) {
    SurfaceCache::LockImage(ImageKey(this));
  }

  return NS_OK;
}

//******************************************************************************

NS_IMETHODIMP
RasterImage::UnlockImage() {
  MOZ_ASSERT(NS_IsMainThread(),
             "Main thread to encourage serialization with LockImage");
  if (mError) {
    return NS_ERROR_FAILURE;
  }

  // It's an error to call this function if the lock count is 0
  MOZ_ASSERT(mLockCount > 0, "Calling UnlockImage with mLockCount == 0!");
  if (mLockCount == 0) {
    return NS_ERROR_ABORT;
  }

  // Decrement our lock count
  mLockCount--;

  // Unlock this image's surfaces in the SurfaceCache.
  if (mLockCount == 0) {
    SurfaceCache::UnlockImage(ImageKey(this));
  }

  return NS_OK;
}

//******************************************************************************

NS_IMETHODIMP
RasterImage::RequestDiscard() {
  if (mDiscardable &&     // Enabled at creation time...
      mLockCount == 0 &&  // ...not temporarily disabled...
      CanDiscard()) {
    Discard();
  }

  return NS_OK;
}

// Indempotent error flagging routine. If a decoder is open, shuts it down.
void RasterImage::DoError() {
  // If we've flagged an error before, we have nothing to do
  if (mError) {
    return;
  }

  // We can't safely handle errors off-main-thread, so dispatch a worker to
  // do it.
  if (!NS_IsMainThread()) {
    HandleErrorWorker::DispatchIfNeeded(this);
    return;
  }

  // Put the container in an error state.
  mError = true;

  // Stop animation and release our FrameAnimator.
  if (mAnimating) {
    StopAnimation();
  }
  mAnimationState = Nothing();
  mFrameAnimator = nullptr;

  // Release all locks.
  mLockCount = 0;
  SurfaceCache::UnlockImage(ImageKey(this));

  // Release all frames from the surface cache.
  SurfaceCache::RemoveImage(ImageKey(this));

  // Invalidate to get rid of any partially-drawn image content.
  NotifyProgress(NoProgress, IntRect(0, 0, mSize.width, mSize.height));

  MOZ_LOG(gImgLog, LogLevel::Error,
          ("RasterImage: [this=%p] Error detected for image\n", this));
}

/* static */
void RasterImage::HandleErrorWorker::DispatchIfNeeded(RasterImage* aImage) {
  RefPtr<HandleErrorWorker> worker = new HandleErrorWorker(aImage);
  NS_DispatchToMainThread(worker);
}

RasterImage::HandleErrorWorker::HandleErrorWorker(RasterImage* aImage)
    : Runnable("image::RasterImage::HandleErrorWorker"), mImage(aImage) {
  MOZ_ASSERT(mImage, "Should have image");
}

NS_IMETHODIMP
RasterImage::HandleErrorWorker::Run() {
  mImage->DoError();

  return NS_OK;
}

bool RasterImage::ShouldAnimate() {
  return ImageResource::ShouldAnimate() && mAnimationState &&
         mAnimationState->KnownFrameCount() >= 1 && !mAnimationFinished;
}

#ifdef DEBUG
NS_IMETHODIMP
RasterImage::GetFramesNotified(uint32_t* aFramesNotified) {
  NS_ENSURE_ARG_POINTER(aFramesNotified);

  *aFramesNotified = mFramesNotified;

  return NS_OK;
}
#endif

void RasterImage::NotifyProgress(
    Progress aProgress, const IntRect& aInvalidRect /* = IntRect() */,
    const Maybe<uint32_t>& aFrameCount /* = Nothing() */,
    DecoderFlags aDecoderFlags
    /* = DefaultDecoderFlags() */,
    SurfaceFlags aSurfaceFlags
    /* = DefaultSurfaceFlags() */) {
  MOZ_ASSERT(NS_IsMainThread());

  // Ensure that we stay alive long enough to finish notifying.
  RefPtr<RasterImage> image = this;

  const bool wasDefaultFlags = aSurfaceFlags == DefaultSurfaceFlags();

  if (!aInvalidRect.IsEmpty() && wasDefaultFlags) {
    // Update our image container since we're invalidating.
    UpdateImageContainer(Some(aInvalidRect));
  }

  if (!(aDecoderFlags & DecoderFlags::FIRST_FRAME_ONLY)) {
    // We may have decoded new animation frames; update our animation state.
    MOZ_ASSERT_IF(aFrameCount && *aFrameCount > 1, mAnimationState || mError);
    if (mAnimationState && aFrameCount) {
      mAnimationState->UpdateKnownFrameCount(*aFrameCount);
    }

    // If we should start animating right now, do so.
    if (mAnimationState && aFrameCount == Some(1u) && mPendingAnimation &&
        ShouldAnimate()) {
      StartAnimation();
    }
  }

  // Tell the observers what happened.
  image->mProgressTracker->SyncNotifyProgress(aProgress, aInvalidRect);
}

void RasterImage::NotifyDecodeComplete(
    const DecoderFinalStatus& aStatus, const ImageMetadata& aMetadata,
    const DecoderTelemetry& aTelemetry, Progress aProgress,
    const IntRect& aInvalidRect, const Maybe<uint32_t>& aFrameCount,
    DecoderFlags aDecoderFlags, SurfaceFlags aSurfaceFlags) {
  MOZ_ASSERT(NS_IsMainThread());

  // If the decoder detected an error, log it to the error console.
  if (aStatus.mShouldReportError) {
    ReportDecoderError();
  }

  // Record all the metadata the decoder gathered about this image.
  bool metadataOK = SetMetadata(aMetadata, aStatus.mWasMetadataDecode);
  if (!metadataOK) {
    // This indicates a serious error that requires us to discard all existing
    // surfaces and redecode to recover. We'll drop the results from this
    // decoder on the floor, since they aren't valid.
    RecoverFromInvalidFrames(mSize, FromSurfaceFlags(aSurfaceFlags));
    return;
  }

  MOZ_ASSERT(mError || mHasSize || !aMetadata.HasSize(),
             "SetMetadata should've gotten a size");

  if (!aStatus.mWasMetadataDecode && aStatus.mFinished) {
    // Flag that we've been decoded before.
    mHasBeenDecoded = true;
  }

  // Send out any final notifications.
  NotifyProgress(aProgress, aInvalidRect, aFrameCount, aDecoderFlags,
                 aSurfaceFlags);

  if (!(aDecoderFlags & DecoderFlags::FIRST_FRAME_ONLY) && mHasBeenDecoded &&
      mAnimationState) {
    // We've finished a full decode of all animation frames and our
    // AnimationState has been notified about them all, so let it know not to
    // expect anymore.
    mAnimationState->NotifyDecodeComplete();
    gfx::IntRect rect =
        mAnimationState->UpdateState(mAnimationFinished, this, mSize);
    if (!rect.IsEmpty()) {
      NotifyProgress(NoProgress, rect);
    }
  }

  // Do some telemetry if this isn't a metadata decode.
  if (!aStatus.mWasMetadataDecode) {
    if (aTelemetry.mChunkCount) {
      Telemetry::Accumulate(Telemetry::IMAGE_DECODE_CHUNKS,
                            aTelemetry.mChunkCount);
    }

    if (aStatus.mFinished) {
      Telemetry::Accumulate(Telemetry::IMAGE_DECODE_TIME,
                            int32_t(aTelemetry.mDecodeTime.ToMicroseconds()));

      if (aTelemetry.mSpeedHistogram && aTelemetry.mBytesDecoded) {
        Telemetry::Accumulate(*aTelemetry.mSpeedHistogram, aTelemetry.Speed());
      }
    }
  }

  // Only act on errors if we have no usable frames from the decoder.
  if (aStatus.mHadError &&
      (!mAnimationState || mAnimationState->KnownFrameCount() == 0)) {
    DoError();
  } else if (aStatus.mWasMetadataDecode && !mHasSize) {
    DoError();
  }

  // XXX(aosmond): Can we get this far without mFinished == true?
  if (aStatus.mFinished && aStatus.mWasMetadataDecode) {
    // If we were waiting to fire the load event, go ahead and fire it now.
    if (mLoadProgress) {
      NotifyForLoadEvent(*mLoadProgress);
      mLoadProgress = Nothing();
    }

    // If we were a metadata decode and a full decode was requested, do it.
    if (mWantFullDecode) {
      mWantFullDecode = false;
      RequestDecodeForSize(mSize,
                           DECODE_FLAGS_DEFAULT | FLAG_HIGH_QUALITY_SCALING,
                           FRAME_CURRENT);
    }
  }
}

void RasterImage::ReportDecoderError() {
  nsCOMPtr<nsIConsoleService> consoleService =
      do_GetService(NS_CONSOLESERVICE_CONTRACTID);
  nsCOMPtr<nsIScriptError> errorObject =
      do_CreateInstance(NS_SCRIPTERROR_CONTRACTID);

  if (consoleService && errorObject) {
    nsAutoString msg(NS_LITERAL_STRING("Image corrupt or truncated."));
    nsAutoString src;
    if (GetURI()) {
      nsAutoCString uri;
      if (!GetSpecTruncatedTo1k(uri)) {
        msg += NS_LITERAL_STRING(" URI in this note truncated due to length.");
      }
      src = NS_ConvertUTF8toUTF16(uri);
    }
    if (NS_SUCCEEDED(errorObject->InitWithWindowID(msg, src, EmptyString(), 0,
                                                   0, nsIScriptError::errorFlag,
                                                   "Image", InnerWindowID()))) {
      consoleService->LogMessage(errorObject);
    }
  }
}

already_AddRefed<imgIContainer> RasterImage::Unwrap() {
  nsCOMPtr<imgIContainer> self(this);
  return self.forget();
}

void RasterImage::PropagateUseCounters(dom::Document*) {
  // No use counters.
}

IntSize RasterImage::OptimalImageSizeForDest(const gfxSize& aDest,
                                             uint32_t aWhichFrame,
                                             SamplingFilter aSamplingFilter,
                                             uint32_t aFlags) {
  MOZ_ASSERT(aDest.width >= 0 || ceil(aDest.width) <= INT32_MAX ||
                 aDest.height >= 0 || ceil(aDest.height) <= INT32_MAX,
             "Unexpected destination size");

  if (mSize.IsEmpty() || aDest.IsEmpty()) {
    return IntSize(0, 0);
  }

  IntSize destSize = IntSize::Ceil(aDest.width, aDest.height);

  if (aSamplingFilter == SamplingFilter::GOOD &&
      CanDownscaleDuringDecode(destSize, aFlags)) {
    return destSize;
  }

  // We can't scale to this size. Use our intrinsic size for now.
  return mSize;
}

}  // namespace image
}  // namespace mozilla
