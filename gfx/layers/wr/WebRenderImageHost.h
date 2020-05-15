/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_WEBRENDERIMAGEHOST_H
#define MOZILLA_GFX_WEBRENDERIMAGEHOST_H

#include "CompositableHost.h"               // for CompositableHost
#include "mozilla/layers/ImageComposite.h"  // for ImageComposite
#include "mozilla/WeakPtr.h"

namespace mozilla {
namespace layers {

class WebRenderBridgeParent;

/**
 * ImageHost. Works with ImageClientSingle and ImageClientBuffered
 */
class WebRenderImageHost : public CompositableHost, public ImageComposite {
 public:
  explicit WebRenderImageHost(const TextureInfo& aTextureInfo);
  virtual ~WebRenderImageHost();

  CompositableType GetType() override { return mTextureInfo.mCompositableType; }

  void Composite(Compositor* aCompositor, LayerComposite* aLayer,
                 EffectChain& aEffectChain, float aOpacity,
                 const gfx::Matrix4x4& aTransform,
                 const gfx::SamplingFilter aSamplingFilter,
                 const gfx::IntRect& aClipRect,
                 const nsIntRegion* aVisibleRegion = nullptr,
                 const Maybe<gfx::Polygon>& aGeometry = Nothing()) override;

  void UseTextureHost(const nsTArray<TimedTexture>& aTextures) override;
  void UseComponentAlphaTextures(TextureHost* aTextureOnBlack,
                                 TextureHost* aTextureOnWhite) override;
  void RemoveTextureHost(TextureHost* aTexture) override;

  TextureHost* GetAsTextureHost(gfx::IntRect* aPictureRect = nullptr) override;

  void Attach(Layer* aLayer, TextureSourceProvider* aProvider,
              AttachFlags aFlags = NO_FLAGS) override;

  void SetTextureSourceProvider(TextureSourceProvider* aProvider) override;

  gfx::IntSize GetImageSize() override;

  void PrintInfo(std::stringstream& aStream, const char* aPrefix) override;

  void Dump(std::stringstream& aStream, const char* aPrefix = "",
            bool aDumpHtml = false) override;

  already_AddRefed<gfx::DataSourceSurface> GetAsSurface() override;

  bool Lock() override;

  void Unlock() override;

  void CleanupResources() override;

  uint32_t GetDroppedFrames() override { return GetDroppedFramesAndReset(); }

  WebRenderImageHost* AsWebRenderImageHost() override { return this; }

  TextureHost* GetAsTextureHostForComposite();

  void SetWrBridge(WebRenderBridgeParent* aWrBridge);

  void ClearWrBridge(WebRenderBridgeParent* aWrBridge);

  TextureHost* GetCurrentTextureHost() { return mCurrentTextureHost; }

 protected:
  // ImageComposite
  TimeStamp GetCompositionTime() const override;

  void SetCurrentTextureHost(TextureHost* aTexture);

  WeakPtr<WebRenderBridgeParent> mWrBridge;

  uint32_t mWrBridgeBindings;

  CompositableTextureHostRef mCurrentTextureHost;
};

}  // namespace layers
}  // namespace mozilla

#endif  // MOZILLA_GFX_WEBRENDERIMAGEHOST_H
