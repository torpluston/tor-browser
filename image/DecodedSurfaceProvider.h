/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * An ISurfaceProvider implemented for single-frame decoded surfaces.
 */

#ifndef mozilla_image_DecodedSurfaceProvider_h
#define mozilla_image_DecodedSurfaceProvider_h

#include "IDecodingTask.h"
#include "ISurfaceProvider.h"
#include "SurfaceCache.h"

namespace mozilla {
namespace image {

/**
 * An ISurfaceProvider that manages the decoding of a single-frame image and
 * stores the resulting surface.
 */
class DecodedSurfaceProvider final : public ISurfaceProvider,
                                     public IDecodingTask {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(DecodedSurfaceProvider, override)

  DecodedSurfaceProvider(NotNull<RasterImage*> aImage,
                         const SurfaceKey& aSurfaceKey,
                         NotNull<Decoder*> aDecoder);

  //////////////////////////////////////////////////////////////////////////////
  // ISurfaceProvider implementation.
  //////////////////////////////////////////////////////////////////////////////

 public:
  bool IsFinished() const override;
  size_t LogicalSizeInBytes() const override;

 protected:
  DrawableFrameRef DrawableRef(size_t aFrame) override;
  bool IsLocked() const override { return bool(mLockRef); }
  void SetLocked(bool aLocked) override;

  //////////////////////////////////////////////////////////////////////////////
  // IDecodingTask implementation.
  //////////////////////////////////////////////////////////////////////////////

 public:
  void Run() override;
  bool ShouldPreferSyncRun() const override;

  // Full decodes are low priority compared to metadata decodes because they
  // don't block layout or page load.
  TaskPriority Priority() const override { return TaskPriority::eLow; }

 private:
  virtual ~DecodedSurfaceProvider();

  void DropImageReference();
  void CheckForNewSurface();
  void FinishDecoding();

  /// The image associated with our decoder. Dropped after decoding.
  RefPtr<RasterImage> mImage;

  /// Mutex protecting access to mDecoder.
  Mutex mMutex;

  /// The decoder that will generate our surface. Dropped after decoding.
  RefPtr<Decoder> mDecoder;

  /// Our surface. Initially null until it's generated by the decoder.
  RefPtr<imgFrame> mSurface;

  /// A drawable reference to our service; used for locking.
  DrawableFrameRef mLockRef;
};

}  // namespace image
}  // namespace mozilla

#endif  // mozilla_image_DecodedSurfaceProvider_h
