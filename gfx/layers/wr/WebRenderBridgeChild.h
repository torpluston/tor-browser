/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_WebRenderBridgeChild_h
#define mozilla_layers_WebRenderBridgeChild_h

#include "mozilla/layers/CompositableForwarder.h"
#include "mozilla/layers/PWebRenderBridgeChild.h"

namespace mozilla {

namespace widget {
class CompositorWidget;
}

namespace wr {
class DisplayListBuilder;
class ResourceUpdateQueue;
class IpcResourceUpdateQueue;
}  // namespace wr

namespace layers {

class CompositableClient;
class CompositorBridgeChild;
class StackingContextHelper;
class TextureForwarder;
class WebRenderLayerManager;

template <class T>
class ThreadSafeWeakPtrHashKey : public PLDHashEntryHdr {
 public:
  typedef RefPtr<T> KeyType;
  typedef const T* KeyTypePointer;

  explicit ThreadSafeWeakPtrHashKey(KeyTypePointer aKey)
      : mKey(do_AddRef(const_cast<T*>(aKey))) {}

  KeyType GetKey() const { return do_AddRef(mKey); }
  bool KeyEquals(KeyTypePointer aKey) const { return mKey == aKey; }

  static KeyTypePointer KeyToPointer(const KeyType& aKey) { return aKey.get(); }
  static PLDHashNumber HashKey(KeyTypePointer aKey) {
    return NS_PTR_TO_UINT32(aKey) >> 2;
  }
  enum { ALLOW_MEMMOVE = true };

 private:
  ThreadSafeWeakPtr<T> mKey;
};

typedef ThreadSafeWeakPtrHashKey<gfx::UnscaledFont> UnscaledFontHashKey;
typedef ThreadSafeWeakPtrHashKey<gfx::ScaledFont> ScaledFontHashKey;

class WebRenderBridgeChild final : public PWebRenderBridgeChild,
                                   public CompositableForwarder {
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(WebRenderBridgeChild, override)

  friend class PWebRenderBridgeChild;

 public:
  explicit WebRenderBridgeChild(const wr::PipelineId& aPipelineId);

  void AddWebRenderParentCommand(const WebRenderParentCommand& aCmd,
                                 wr::RenderRoot aRenderRoot);
  bool HasWebRenderParentCommands(wr::RenderRoot aRenderRoot) {
    return !mParentCommands[aRenderRoot].IsEmpty();
  }

  void UpdateResources(wr::IpcResourceUpdateQueue& aResources,
                       wr::RenderRoot aRenderRoot);
  void BeginTransaction();
  void EndTransaction(nsTArray<RenderRootDisplayListData>& aRenderRoots,
                      TransactionId aTransactionId, bool aContainsSVGroup,
                      const mozilla::VsyncId& aVsyncId,
                      const mozilla::TimeStamp& aVsyncStartTime,
                      const mozilla::TimeStamp& aRefreshStartTime,
                      const mozilla::TimeStamp& aTxnStartTime,
                      const nsCString& aTxtURL);
  void EndEmptyTransaction(const FocusTarget& aFocusTarget,
                           nsTArray<RenderRootUpdates>& aRenderRootUpdates,
                           uint32_t aPaintSequenceNumber,
                           TransactionId aTransactionId,
                           const mozilla::VsyncId& aVsyncId,
                           const mozilla::TimeStamp& aVsyncStartTime,
                           const mozilla::TimeStamp& aRefreshStartTime,
                           const mozilla::TimeStamp& aTxnStartTime,
                           const nsCString& aTxtURL);
  void ProcessWebRenderParentCommands();

  CompositorBridgeChild* GetCompositorBridgeChild();

  wr::PipelineId GetPipeline() { return mPipelineId; }

  // KnowsCompositor
  TextureForwarder* GetTextureForwarder() override;
  LayersIPCActor* GetLayersIPCActor() override;
  void SyncWithCompositor() override;
  ActiveResourceTracker* GetActiveResourceTracker() override {
    return mActiveResourceTracker.get();
  }

  void AddPipelineIdForAsyncCompositable(const wr::PipelineId& aPipelineId,
                                         const CompositableHandle& aHandlee,
                                         wr::RenderRoot aRenderRoot);
  void AddPipelineIdForCompositable(const wr::PipelineId& aPipelineId,
                                    const CompositableHandle& aHandlee,
                                    wr::RenderRoot aRenderRoot);
  void RemovePipelineIdForCompositable(const wr::PipelineId& aPipelineId,
                                       wr::RenderRoot aRenderRoot);

  /// Release TextureClient that is bounded to ImageKey.
  /// It is used for recycling TextureClient.
  void ReleaseTextureOfImage(const wr::ImageKey& aKey,
                             wr::RenderRoot aRenderRoot);

  /**
   * Clean this up, finishing with SendShutDown() which will cause __delete__
   * to be sent from the parent side.
   */
  void Destroy(bool aIsSync);
  bool IPCOpen() const { return mIPCOpen && !mDestroyed; }
  bool IsDestroyed() const { return mDestroyed; }

  uint32_t GetNextResourceId() { return ++mResourceId; }
  wr::IdNamespace GetNamespace() { return mIdNamespace; }
  void SetNamespace(wr::IdNamespace aIdNamespace) {
    mIdNamespace = aIdNamespace;
  }

  wr::FontKey GetNextFontKey() {
    return wr::FontKey{GetNamespace(), GetNextResourceId()};
  }

  wr::FontInstanceKey GetNextFontInstanceKey() {
    return wr::FontInstanceKey{GetNamespace(), GetNextResourceId()};
  }

  wr::WrImageKey GetNextImageKey() {
    return wr::WrImageKey{GetNamespace(), GetNextResourceId()};
  }

  void PushGlyphs(wr::DisplayListBuilder& aBuilder,
                  Range<const wr::GlyphInstance> aGlyphs,
                  gfx::ScaledFont* aFont, const wr::ColorF& aColor,
                  const StackingContextHelper& aSc,
                  const wr::LayoutRect& aBounds, const wr::LayoutRect& aClip,
                  bool aBackfaceVisible,
                  const wr::GlyphOptions* aGlyphOptions = nullptr);

  Maybe<wr::FontInstanceKey> GetFontKeyForScaledFont(
      gfx::ScaledFont* aScaledFont, wr::RenderRoot aRenderRoot,
      wr::IpcResourceUpdateQueue* aResources = nullptr);
  Maybe<wr::FontKey> GetFontKeyForUnscaledFont(
      gfx::UnscaledFont* aUnscaledFont, wr::RenderRoot aRenderRoot,
      wr::IpcResourceUpdateQueue* aResources = nullptr);
  void RemoveExpiredFontKeys(wr::IpcResourceUpdateQueue& aResources);

  void BeginClearCachedResources();
  void EndClearCachedResources();

  void SetWebRenderLayerManager(WebRenderLayerManager* aManager);

  ipc::IShmemAllocator* GetShmemAllocator();

  bool IsThreadSafe() const override { return false; }

  RefPtr<KnowsCompositor> GetForMedia() override;

  /// Alloc a specific type of shmem that is intended for use in
  /// IpcResourceUpdateQueue only, and cache at most one of them,
  /// when called multiple times.
  ///
  /// Do not use this for anything else.
  bool AllocResourceShmem(size_t aSize, RefCountedShmem& aShm);
  /// Dealloc shared memory that was allocated with AllocResourceShmem.
  ///
  /// Do not use this for anything else.
  void DeallocResourceShmem(RefCountedShmem& aShm);

  void Capture();

 private:
  friend class CompositorBridgeChild;

  ~WebRenderBridgeChild();

  wr::ExternalImageId GetNextExternalImageId();

  // CompositableForwarder
  void Connect(CompositableClient* aCompositable,
               ImageContainer* aImageContainer = nullptr) override;
  void UseTiledLayerBuffer(
      CompositableClient* aCompositable,
      const SurfaceDescriptorTiles& aTiledDescriptor) override;
  void UpdateTextureRegion(CompositableClient* aCompositable,
                           const ThebesBufferData& aThebesBufferData,
                           const nsIntRegion& aUpdatedRegion) override;
  void ReleaseCompositable(const CompositableHandle& aHandle) override;
  bool DestroyInTransaction(PTextureChild* aTexture) override;
  bool DestroyInTransaction(const CompositableHandle& aHandle);
  void RemoveTextureFromCompositable(
      CompositableClient* aCompositable, TextureClient* aTexture,
      const Maybe<wr::RenderRoot>& aRenderRoot) override;
  void UseTextures(CompositableClient* aCompositable,
                   const nsTArray<TimedTextureClient>& aTextures,
                   const Maybe<wr::RenderRoot>& aRenderRoot) override;
  void UseComponentAlphaTextures(CompositableClient* aCompositable,
                                 TextureClient* aClientOnBlack,
                                 TextureClient* aClientOnWhite) override;
  void UpdateFwdTransactionId() override;
  uint64_t GetFwdTransactionId() override;
  bool InForwarderThread() override;

  void ActorDestroy(ActorDestroyReason why) override;

  void DoDestroy();

  mozilla::ipc::IPCResult RecvWrUpdated(
      const wr::IdNamespace& aNewIdNamespace,
      const TextureFactoryIdentifier& textureFactoryIdentifier);
  mozilla::ipc::IPCResult RecvWrReleasedImages(
      nsTArray<wr::ExternalImageKeyPair>&& aPairs);

  void AddIPDLReference() {
    MOZ_ASSERT(mIPCOpen == false);
    mIPCOpen = true;
    AddRef();
  }
  void ReleaseIPDLReference() {
    MOZ_ASSERT(mIPCOpen == true);
    mIPCOpen = false;
    Release();
  }

  bool AddOpDestroy(const OpDestroy& aOp);

  nsTArray<OpDestroy> mDestroyedActors;
  wr::RenderRootArray<nsTArray<WebRenderParentCommand>> mParentCommands;
  nsDataHashtable<nsUint64HashKey, CompositableClient*> mCompositables;
  bool mIsInTransaction;
  bool mIsInClearCachedResources;
  wr::IdNamespace mIdNamespace;
  uint32_t mResourceId;
  wr::PipelineId mPipelineId;
  WebRenderLayerManager* mManager;

  bool mIPCOpen;
  bool mDestroyed;

  wr::RenderRootArray<uint32_t> mFontKeysDeleted;
  wr::RenderRootArray<nsDataHashtable<UnscaledFontHashKey, wr::FontKey>>
      mFontKeys;

  wr::RenderRootArray<uint32_t> mFontInstanceKeysDeleted;
  wr::RenderRootArray<nsDataHashtable<ScaledFontHashKey, wr::FontInstanceKey>>
      mFontInstanceKeys;

  UniquePtr<ActiveResourceTracker> mActiveResourceTracker;

  RefCountedShmem mResourceShm;
};

}  // namespace layers
}  // namespace mozilla

#endif  // mozilla_layers_WebRenderBridgeChild_h
