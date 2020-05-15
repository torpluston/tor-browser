/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StructuredCloneHolder.h"

#include "ImageContainer.h"
#include "mozilla/AutoRestore.h"
#include "mozilla/dom/BlobBinding.h"
#include "mozilla/dom/CryptoKey.h"
#include "mozilla/dom/StructuredCloneBlob.h"
#include "mozilla/dom/Directory.h"
#include "mozilla/dom/DirectoryBinding.h"
#include "mozilla/dom/DOMPrefs.h"
#include "mozilla/dom/File.h"
#include "mozilla/dom/FileList.h"
#include "mozilla/dom/FileListBinding.h"
#include "mozilla/dom/FormData.h"
#include "mozilla/dom/ImageBitmap.h"
#include "mozilla/dom/ImageBitmapBinding.h"
#include "mozilla/dom/ImageData.h"
#include "mozilla/dom/ImageDataBinding.h"
#include "mozilla/dom/StructuredClone.h"
#include "mozilla/dom/MessagePort.h"
#include "mozilla/dom/MessagePortBinding.h"
#include "mozilla/dom/OffscreenCanvas.h"
#include "mozilla/dom/OffscreenCanvasBinding.h"
#include "mozilla/dom/PMessagePort.h"
#include "mozilla/dom/StructuredCloneTags.h"
#include "mozilla/dom/StructuredCloneTester.h"
#include "mozilla/dom/StructuredCloneTesterBinding.h"
#include "mozilla/dom/SubtleCryptoBinding.h"
#include "mozilla/dom/ToJSValue.h"
#include "mozilla/dom/URLSearchParams.h"
#include "mozilla/dom/URLSearchParamsBinding.h"
#include "mozilla/dom/WebCryptoCommon.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/ipc/BackgroundChild.h"
#include "mozilla/ipc/BackgroundUtils.h"
#include "mozilla/ipc/PBackgroundSharedTypes.h"
#include "MultipartBlobImpl.h"
#include "nsQueryObject.h"

#ifdef MOZ_WEBRTC
#  include "mozilla/dom/RTCCertificate.h"
#  include "mozilla/dom/RTCCertificateBinding.h"
#endif

using namespace mozilla::ipc;

namespace mozilla {
namespace dom {

namespace {

JSObject* StructuredCloneCallbacksRead(JSContext* aCx,
                                       JSStructuredCloneReader* aReader,
                                       uint32_t aTag, uint32_t aIndex,
                                       void* aClosure) {
  StructuredCloneHolderBase* holder =
      static_cast<StructuredCloneHolderBase*>(aClosure);
  MOZ_ASSERT(holder);
  return holder->CustomReadHandler(aCx, aReader, aTag, aIndex);
}

bool StructuredCloneCallbacksWrite(JSContext* aCx,
                                   JSStructuredCloneWriter* aWriter,
                                   JS::Handle<JSObject*> aObj, void* aClosure) {
  StructuredCloneHolderBase* holder =
      static_cast<StructuredCloneHolderBase*>(aClosure);
  MOZ_ASSERT(holder);
  return holder->CustomWriteHandler(aCx, aWriter, aObj);
}

bool StructuredCloneCallbacksReadTransfer(
    JSContext* aCx, JSStructuredCloneReader* aReader, uint32_t aTag,
    void* aContent, uint64_t aExtraData, void* aClosure,
    JS::MutableHandleObject aReturnObject) {
  StructuredCloneHolderBase* holder =
      static_cast<StructuredCloneHolderBase*>(aClosure);
  MOZ_ASSERT(holder);
  return holder->CustomReadTransferHandler(aCx, aReader, aTag, aContent,
                                           aExtraData, aReturnObject);
}

bool StructuredCloneCallbacksWriteTransfer(
    JSContext* aCx, JS::Handle<JSObject*> aObj, void* aClosure,
    // Output:
    uint32_t* aTag, JS::TransferableOwnership* aOwnership, void** aContent,
    uint64_t* aExtraData) {
  StructuredCloneHolderBase* holder =
      static_cast<StructuredCloneHolderBase*>(aClosure);
  MOZ_ASSERT(holder);
  return holder->CustomWriteTransferHandler(aCx, aObj, aTag, aOwnership,
                                            aContent, aExtraData);
}

void StructuredCloneCallbacksFreeTransfer(uint32_t aTag,
                                          JS::TransferableOwnership aOwnership,
                                          void* aContent, uint64_t aExtraData,
                                          void* aClosure) {
  StructuredCloneHolderBase* holder =
      static_cast<StructuredCloneHolderBase*>(aClosure);
  MOZ_ASSERT(holder);
  return holder->CustomFreeTransferHandler(aTag, aOwnership, aContent,
                                           aExtraData);
}

bool StructuredCloneCallbacksCanTransfer(JSContext* aCx,
                                         JS::Handle<JSObject*> aObject,
                                         void* aClosure) {
  StructuredCloneHolderBase* holder =
      static_cast<StructuredCloneHolderBase*>(aClosure);
  MOZ_ASSERT(holder);
  return holder->CustomCanTransferHandler(aCx, aObject);
}

void StructuredCloneCallbacksError(JSContext* aCx, uint32_t aErrorId) {
  NS_WARNING("Failed to clone data.");
}

}  // anonymous namespace

const JSStructuredCloneCallbacks StructuredCloneHolder::sCallbacks = {
    StructuredCloneCallbacksRead,          StructuredCloneCallbacksWrite,
    StructuredCloneCallbacksError,         StructuredCloneCallbacksReadTransfer,
    StructuredCloneCallbacksWriteTransfer, StructuredCloneCallbacksFreeTransfer,
    StructuredCloneCallbacksCanTransfer,
};

// StructuredCloneHolderBase class

StructuredCloneHolderBase::StructuredCloneHolderBase(
    StructuredCloneScope aScope)
    : mStructuredCloneScope(aScope)
#ifdef DEBUG
      ,
      mClearCalled(false)
#endif
{
}

StructuredCloneHolderBase::~StructuredCloneHolderBase() {
#ifdef DEBUG
  MOZ_ASSERT(mClearCalled);
#endif
}

void StructuredCloneHolderBase::Clear() {
#ifdef DEBUG
  mClearCalled = true;
#endif

  mBuffer = nullptr;
}

bool StructuredCloneHolderBase::Write(JSContext* aCx,
                                      JS::Handle<JS::Value> aValue) {
  return Write(aCx, aValue, JS::UndefinedHandleValue,
               JS::CloneDataPolicy().denySharedArrayBuffer());
}

bool StructuredCloneHolderBase::Write(JSContext* aCx,
                                      JS::Handle<JS::Value> aValue,
                                      JS::Handle<JS::Value> aTransfer,
                                      JS::CloneDataPolicy cloneDataPolicy) {
  MOZ_ASSERT(!mBuffer, "Double Write is not allowed");
  MOZ_ASSERT(!mClearCalled, "This method cannot be called after Clear.");

  mBuffer = MakeUnique<JSAutoStructuredCloneBuffer>(
      mStructuredCloneScope, &StructuredCloneHolder::sCallbacks, this);

  if (!mBuffer->write(aCx, aValue, aTransfer, cloneDataPolicy,
                      &StructuredCloneHolder::sCallbacks, this)) {
    mBuffer = nullptr;
    return false;
  }

  return true;
}

bool StructuredCloneHolderBase::Read(JSContext* aCx,
                                     JS::MutableHandle<JS::Value> aValue) {
  MOZ_ASSERT(mBuffer, "Read() without Write() is not allowed.");
  MOZ_ASSERT(!mClearCalled, "This method cannot be called after Clear.");

  bool ok =
      mBuffer->read(aCx, aValue, &StructuredCloneHolder::sCallbacks, this);
  return ok;
}

bool StructuredCloneHolderBase::CustomReadTransferHandler(
    JSContext* aCx, JSStructuredCloneReader* aReader, uint32_t aTag,
    void* aContent, uint64_t aExtraData,
    JS::MutableHandleObject aReturnObject) {
  MOZ_CRASH("Nothing to read.");
  return false;
}

bool StructuredCloneHolderBase::CustomWriteTransferHandler(
    JSContext* aCx, JS::Handle<JSObject*> aObj, uint32_t* aTag,
    JS::TransferableOwnership* aOwnership, void** aContent,
    uint64_t* aExtraData) {
  // No transfers are supported by default.
  return false;
}

void StructuredCloneHolderBase::CustomFreeTransferHandler(
    uint32_t aTag, JS::TransferableOwnership aOwnership, void* aContent,
    uint64_t aExtraData) {
  MOZ_CRASH("Nothing to free.");
}

bool StructuredCloneHolderBase::CustomCanTransferHandler(
    JSContext* aCx, JS::Handle<JSObject*> aObj) {
  return false;
}

// StructuredCloneHolder class

StructuredCloneHolder::StructuredCloneHolder(
    CloningSupport aSupportsCloning, TransferringSupport aSupportsTransferring,
    StructuredCloneScope aScope)
    : StructuredCloneHolderBase(aScope),
      mSupportsCloning(aSupportsCloning == CloningSupported),
      mSupportsTransferring(aSupportsTransferring == TransferringSupported),
      mParent(nullptr)
#ifdef DEBUG
      ,
      mCreationEventTarget(GetCurrentThreadEventTarget())
#endif
{
}

StructuredCloneHolder::~StructuredCloneHolder() {
  Clear();
  MOZ_ASSERT(mTransferredPorts.IsEmpty());
}

void StructuredCloneHolder::Write(JSContext* aCx, JS::Handle<JS::Value> aValue,
                                  ErrorResult& aRv) {
  Write(aCx, aValue, JS::UndefinedHandleValue,
        JS::CloneDataPolicy().denySharedArrayBuffer(), aRv);
}

void StructuredCloneHolder::Write(JSContext* aCx, JS::Handle<JS::Value> aValue,
                                  JS::Handle<JS::Value> aTransfer,
                                  JS::CloneDataPolicy cloneDataPolicy,
                                  ErrorResult& aRv) {
  MOZ_ASSERT_IF(
      mStructuredCloneScope == StructuredCloneScope::SameProcessSameThread,
      mCreationEventTarget->IsOnCurrentThread());

  if (!StructuredCloneHolderBase::Write(aCx, aValue, aTransfer,
                                        cloneDataPolicy)) {
    aRv.Throw(NS_ERROR_DOM_DATA_CLONE_ERR);
    return;
  }
}

void StructuredCloneHolder::Read(nsISupports* aParent, JSContext* aCx,
                                 JS::MutableHandle<JS::Value> aValue,
                                 ErrorResult& aRv) {
  MOZ_ASSERT_IF(
      mStructuredCloneScope == StructuredCloneScope::SameProcessSameThread,
      mCreationEventTarget->IsOnCurrentThread());
  MOZ_ASSERT(aParent);

  mozilla::AutoRestore<nsISupports*> guard(mParent);
  mParent = aParent;

  if (!StructuredCloneHolderBase::Read(aCx, aValue)) {
    JS_ClearPendingException(aCx);
    aRv.Throw(NS_ERROR_DOM_DATA_CLONE_ERR);
    return;
  }

  // If we are tranferring something, we cannot call 'Read()' more than once.
  if (mSupportsTransferring) {
    mBlobImplArray.Clear();
    mWasmModuleArray.Clear();
    mClonedSurfaces.Clear();
    mInputStreamArray.Clear();
    Clear();
  }
}

void StructuredCloneHolder::ReadFromBuffer(nsISupports* aParent, JSContext* aCx,
                                           JSStructuredCloneData& aBuffer,
                                           JS::MutableHandle<JS::Value> aValue,
                                           ErrorResult& aRv) {
  ReadFromBuffer(aParent, aCx, aBuffer, JS_STRUCTURED_CLONE_VERSION, aValue,
                 aRv);
}

void StructuredCloneHolder::ReadFromBuffer(nsISupports* aParent, JSContext* aCx,
                                           JSStructuredCloneData& aBuffer,
                                           uint32_t aAlgorithmVersion,
                                           JS::MutableHandle<JS::Value> aValue,
                                           ErrorResult& aRv) {
  MOZ_ASSERT_IF(
      mStructuredCloneScope == StructuredCloneScope::SameProcessSameThread,
      mCreationEventTarget->IsOnCurrentThread());

  MOZ_ASSERT(!mBuffer, "ReadFromBuffer() must be called without a Write().");

  mozilla::AutoRestore<nsISupports*> guard(mParent);
  mParent = aParent;

  if (!JS_ReadStructuredClone(aCx, aBuffer, aAlgorithmVersion,
                              mStructuredCloneScope, aValue, &sCallbacks,
                              this)) {
    JS_ClearPendingException(aCx);
    aRv.Throw(NS_ERROR_DOM_DATA_CLONE_ERR);
  }
}

/* static */
JSObject* StructuredCloneHolder::ReadFullySerializableObjects(
    JSContext* aCx, JSStructuredCloneReader* aReader, uint32_t aTag) {
  if (aTag == SCTAG_DOM_IMAGEDATA) {
    return ReadStructuredCloneImageData(aCx, aReader);
  }

  if (aTag == SCTAG_DOM_WEBCRYPTO_KEY || aTag == SCTAG_DOM_URLSEARCHPARAMS) {
    nsIGlobalObject* global = xpc::CurrentNativeGlobal(aCx);
    if (!global) {
      return nullptr;
    }

    // Prevent the return value from being trashed by a GC during ~nsRefPtr.
    JS::Rooted<JSObject*> result(aCx);
    {
      if (aTag == SCTAG_DOM_WEBCRYPTO_KEY) {
        RefPtr<CryptoKey> key = new CryptoKey(global);
        if (!key->ReadStructuredClone(aReader)) {
          result = nullptr;
        } else {
          result = key->WrapObject(aCx, nullptr);
        }
      } else if (aTag == SCTAG_DOM_URLSEARCHPARAMS) {
        RefPtr<URLSearchParams> usp = new URLSearchParams(global);
        if (!usp->ReadStructuredClone(aReader)) {
          result = nullptr;
        } else {
          result = usp->WrapObject(aCx, nullptr);
        }
      }
    }
    return result;
  }

  if (aTag == SCTAG_DOM_NULL_PRINCIPAL || aTag == SCTAG_DOM_SYSTEM_PRINCIPAL ||
      aTag == SCTAG_DOM_CONTENT_PRINCIPAL ||
      aTag == SCTAG_DOM_EXPANDED_PRINCIPAL) {
    JSPrincipals* prin;
    if (!nsJSPrincipals::ReadKnownPrincipalType(aCx, aReader, aTag, &prin)) {
      return nullptr;
    }

    JS::RootedValue result(aCx);
    {
      // nsJSPrincipals::ReadKnownPrincipalType addrefs for us, but because of
      // the casting between JSPrincipals* and nsIPrincipal* we can't use
      // getter_AddRefs above and have to already_AddRefed here.
      nsCOMPtr<nsIPrincipal> principal =
        already_AddRefed<nsIPrincipal>(nsJSPrincipals::get(prin));

      nsresult rv = nsContentUtils::WrapNative(
        aCx, principal, &NS_GET_IID(nsIPrincipal), &result);
      if (NS_FAILED(rv)) {
        xpc::Throw(aCx, NS_ERROR_DOM_DATA_CLONE_ERR);
        return nullptr;
      }
    }
    return result.toObjectOrNull();
  }

#ifdef MOZ_WEBRTC
  if (aTag == SCTAG_DOM_RTC_CERTIFICATE) {
    if (!NS_IsMainThread()) {
      return nullptr;
    }

    nsIGlobalObject* global = xpc::CurrentNativeGlobal(aCx);
    if (!global) {
      return nullptr;
    }

    // Prevent the return value from being trashed by a GC during ~nsRefPtr.
    JS::Rooted<JSObject*> result(aCx);
    {
      RefPtr<RTCCertificate> cert = new RTCCertificate(global);
      if (!cert->ReadStructuredClone(aReader)) {
        result = nullptr;
      } else {
        result = cert->WrapObject(aCx, nullptr);
      }
    }
    return result;
  }
#endif

  if (aTag == SCTAG_DOM_STRUCTURED_CLONE_TESTER) {
    return StructuredCloneTester::ReadStructuredClone(aCx, aReader);
  }

  // Don't know what this is. Bail.
  xpc::Throw(aCx, NS_ERROR_DOM_DATA_CLONE_ERR);
  return nullptr;
}

/* static */
bool StructuredCloneHolder::WriteFullySerializableObjects(
    JSContext* aCx, JSStructuredCloneWriter* aWriter,
    JS::Handle<JSObject*> aObj) {
  JS::Rooted<JSObject*> obj(aCx, aObj);

  // See if this is a ImageData object.
  {
    ImageData* imageData = nullptr;
    if (NS_SUCCEEDED(UNWRAP_OBJECT(ImageData, &obj, imageData))) {
      return WriteStructuredCloneImageData(aCx, aWriter, imageData);
    }
  }

  // Handle URLSearchParams cloning
  {
    URLSearchParams* usp = nullptr;
    if (NS_SUCCEEDED(UNWRAP_OBJECT(URLSearchParams, &obj, usp))) {
      return JS_WriteUint32Pair(aWriter, SCTAG_DOM_URLSEARCHPARAMS, 0) &&
             usp->WriteStructuredClone(aWriter);
    }
  }

  // Handle Key cloning
  {
    CryptoKey* key = nullptr;
    if (NS_SUCCEEDED(UNWRAP_OBJECT(CryptoKey, &obj, key))) {
      return JS_WriteUint32Pair(aWriter, SCTAG_DOM_WEBCRYPTO_KEY, 0) &&
             key->WriteStructuredClone(aWriter);
    }
  }

#ifdef MOZ_WEBRTC
  {
    // Handle WebRTC Certificate cloning
    RTCCertificate* cert = nullptr;
    if (NS_SUCCEEDED(UNWRAP_OBJECT(RTCCertificate, &obj, cert))) {
      MOZ_ASSERT(NS_IsMainThread());
      return JS_WriteUint32Pair(aWriter, SCTAG_DOM_RTC_CERTIFICATE, 0) &&
             cert->WriteStructuredClone(aWriter);
    }
  }
#endif

  // StructuredCloneTester - testing only
  {
    StructuredCloneTester* sct = nullptr;
    if (NS_SUCCEEDED(UNWRAP_OBJECT(StructuredCloneTester, &obj, sct))) {
      MOZ_ASSERT(StaticPrefs::dom_testing_structuredclonetester_enabled());

      // "Fail" serialization
      if (!sct->Serializable()) {
        xpc::Throw(aCx, NS_ERROR_DOM_DATA_CLONE_ERR);
        return false;
      }

      return sct->WriteStructuredClone(aWriter);
    }
  }

  if (NS_IsMainThread() && xpc::IsReflector(obj, aCx)) {
    // We only care about principals, so ReflectorToISupportsStatic is fine.
    nsCOMPtr<nsISupports> base = xpc::ReflectorToISupportsStatic(obj);
    nsCOMPtr<nsIPrincipal> principal = do_QueryInterface(base);
    if (principal) {
      auto nsjsprincipals = nsJSPrincipals::get(principal);
      return nsjsprincipals->write(aCx, aWriter);
    }
  }

  // Don't know what this is
  xpc::Throw(aCx, NS_ERROR_DOM_DATA_CLONE_ERR);
  return false;
}

namespace {

JSObject* ReadBlob(JSContext* aCx, uint32_t aIndex,
                   StructuredCloneHolder* aHolder) {
  MOZ_ASSERT(aHolder);
#ifdef FUZZING
  if (aIndex >= aHolder->BlobImpls().Length()) {
    return nullptr;
  }
#endif
  MOZ_ASSERT(aIndex < aHolder->BlobImpls().Length());
  JS::Rooted<JS::Value> val(aCx);
  {
    // RefPtr<File> and RefPtr<BlobImpl> need to go out of scope before
    // toObject() is called because the static analysis thinks releasing XPCOM
    // objects can GC (because in some cases it can!), and a return statement
    // with a JSObject* type means that JSObject* is on the stack as a raw
    // pointer while destructors are running.
    RefPtr<BlobImpl> blobImpl = aHolder->BlobImpls()[aIndex];

    MOZ_ALWAYS_SUCCEEDS(blobImpl->SetMutable(false));

    RefPtr<Blob> blob = Blob::Create(aHolder->ParentDuringRead(), blobImpl);
    if (!ToJSValue(aCx, blob, &val)) {
      return nullptr;
    }
  }

  return &val.toObject();
}

bool WriteBlob(JSStructuredCloneWriter* aWriter, Blob* aBlob,
               StructuredCloneHolder* aHolder) {
  MOZ_ASSERT(aWriter);
  MOZ_ASSERT(aBlob);
  MOZ_ASSERT(aHolder);

  if (JS_GetStructuredCloneScope(aWriter) !=
          JS::StructuredCloneScope::SameProcessSameThread &&
      !aBlob->Impl()->MayBeClonedToOtherThreads()) {
    return false;
  }

  RefPtr<BlobImpl> blobImpl = aBlob->Impl();
  MOZ_ALWAYS_SUCCEEDS(blobImpl->SetMutable(false));

  // We store the position of the blobImpl in the array as index.
  if (JS_WriteUint32Pair(aWriter, SCTAG_DOM_BLOB,
                         aHolder->BlobImpls().Length())) {
    aHolder->BlobImpls().AppendElement(blobImpl);
    return true;
  }

  return false;
}

// A directory is serialized as:
// - pair of ints: SCTAG_DOM_DIRECTORY, path length
// - path as string
bool WriteDirectory(JSStructuredCloneWriter* aWriter, Directory* aDirectory) {
  MOZ_ASSERT(aWriter);
  MOZ_ASSERT(aDirectory);

  nsAutoString path;
  aDirectory->GetFullRealPath(path);

  size_t charSize = sizeof(nsString::char_type);
  return JS_WriteUint32Pair(aWriter, SCTAG_DOM_DIRECTORY, path.Length()) &&
         JS_WriteBytes(aWriter, path.get(), path.Length() * charSize);
}

already_AddRefed<Directory> ReadDirectoryInternal(
    JSStructuredCloneReader* aReader, uint32_t aPathLength,
    StructuredCloneHolder* aHolder) {
  MOZ_ASSERT(aReader);
  MOZ_ASSERT(aHolder);

  nsAutoString path;
  if (NS_WARN_IF(!path.SetLength(aPathLength, fallible))) {
    return nullptr;
  }
  size_t charSize = sizeof(nsString::char_type);
  if (!JS_ReadBytes(aReader, (void*)path.BeginWriting(),
                    aPathLength * charSize)) {
    return nullptr;
  }

  nsCOMPtr<nsIFile> file;
  nsresult rv = NS_NewLocalFile(path, true, getter_AddRefs(file));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return nullptr;
  }

  RefPtr<Directory> directory =
      Directory::Create(aHolder->ParentDuringRead(), file);
  return directory.forget();
}

JSObject* ReadDirectory(JSContext* aCx, JSStructuredCloneReader* aReader,
                        uint32_t aPathLength, StructuredCloneHolder* aHolder) {
  MOZ_ASSERT(aCx);
  MOZ_ASSERT(aReader);
  MOZ_ASSERT(aHolder);

  // RefPtr<Directory> needs to go out of scope before toObject() is
  // called because the static analysis thinks dereferencing XPCOM objects
  // can GC (because in some cases it can!), and a return statement with a
  // JSObject* type means that JSObject* is on the stack as a raw pointer
  // while destructors are running.
  JS::Rooted<JS::Value> val(aCx);
  {
    RefPtr<Directory> directory =
        ReadDirectoryInternal(aReader, aPathLength, aHolder);
    if (!directory) {
      return nullptr;
    }

    if (!ToJSValue(aCx, directory, &val)) {
      return nullptr;
    }
  }

  return &val.toObject();
}

// Read the WriteFileList for the format.
JSObject* ReadFileList(JSContext* aCx, JSStructuredCloneReader* aReader,
                       uint32_t aCount, StructuredCloneHolder* aHolder) {
  MOZ_ASSERT(aCx);
  MOZ_ASSERT(aReader);

  JS::Rooted<JS::Value> val(aCx);
  {
    RefPtr<FileList> fileList = new FileList(aHolder->ParentDuringRead());

    uint32_t zero, index;
    // |index| is the index of the first blobImpl.
    if (!JS_ReadUint32Pair(aReader, &zero, &index)) {
      return nullptr;
    }

    MOZ_ASSERT(zero == 0);

    // |aCount| is the number of BlobImpls to use from the |index|.
    for (uint32_t i = 0; i < aCount; ++i) {
      uint32_t pos = index + i;
#ifdef FUZZING
      if (pos >= aHolder->BlobImpls().Length()) {
        return nullptr;
      }
#endif
      MOZ_ASSERT(pos < aHolder->BlobImpls().Length());

      RefPtr<BlobImpl> blobImpl = aHolder->BlobImpls()[pos];
      MOZ_ASSERT(blobImpl->IsFile());

      MOZ_ALWAYS_SUCCEEDS(blobImpl->SetMutable(false));

      RefPtr<File> file = File::Create(aHolder->ParentDuringRead(), blobImpl);
      if (!fileList->Append(file)) {
        return nullptr;
      }
    }

    if (!ToJSValue(aCx, fileList, &val)) {
      return nullptr;
    }
  }

  return &val.toObject();
}

// The format of the FileList serialization is:
// - pair of ints: SCTAG_DOM_FILELIST, Length of the FileList
// - pair of ints: 0, The offset of the BlobImpl array
bool WriteFileList(JSStructuredCloneWriter* aWriter, FileList* aFileList,
                   StructuredCloneHolder* aHolder) {
  MOZ_ASSERT(aWriter);
  MOZ_ASSERT(aFileList);
  MOZ_ASSERT(aHolder);

  // A FileList is serialized writing the X number of elements and the offset
  // from mBlobImplArray. The Read will take X elements from mBlobImplArray
  // starting from the offset.
  if (!JS_WriteUint32Pair(aWriter, SCTAG_DOM_FILELIST, aFileList->Length()) ||
      !JS_WriteUint32Pair(aWriter, 0, aHolder->BlobImpls().Length())) {
    return false;
  }

  nsTArray<RefPtr<BlobImpl>> blobImpls;

  for (uint32_t i = 0; i < aFileList->Length(); ++i) {
    RefPtr<BlobImpl> blobImpl = aFileList->Item(i)->Impl();
    MOZ_ALWAYS_SUCCEEDS(blobImpl->SetMutable(false));
    blobImpls.AppendElement(blobImpl);
  }

  aHolder->BlobImpls().AppendElements(blobImpls);
  return true;
}

// Read the WriteFormData for the format.
JSObject* ReadFormData(JSContext* aCx, JSStructuredCloneReader* aReader,
                       uint32_t aCount, StructuredCloneHolder* aHolder) {
  MOZ_ASSERT(aCx);
  MOZ_ASSERT(aReader);
  MOZ_ASSERT(aHolder);

  // See the serialization of the FormData for the format.
  JS::Rooted<JS::Value> val(aCx);
  {
    RefPtr<FormData> formData = new FormData(aHolder->ParentDuringRead());

    Optional<nsAString> thirdArg;
    for (uint32_t i = 0; i < aCount; ++i) {
      nsAutoString name;
      if (!ReadString(aReader, name)) {
        return nullptr;
      }

      uint32_t tag, indexOrLengthOfString;
      if (!JS_ReadUint32Pair(aReader, &tag, &indexOrLengthOfString)) {
        return nullptr;
      }

      if (tag == SCTAG_DOM_BLOB) {
#ifdef FUZZING
        if (indexOrLengthOfString >= aHolder->BlobImpls().Length()) {
          return nullptr;
        }
#endif
        MOZ_ASSERT(indexOrLengthOfString < aHolder->BlobImpls().Length());

        RefPtr<BlobImpl> blobImpl = aHolder->BlobImpls()[indexOrLengthOfString];
        MOZ_ALWAYS_SUCCEEDS(blobImpl->SetMutable(false));

        RefPtr<Blob> blob = Blob::Create(aHolder->ParentDuringRead(), blobImpl);
        MOZ_ASSERT(blob);

        ErrorResult rv;
        formData->Append(name, *blob, thirdArg, rv);
        if (NS_WARN_IF(rv.Failed())) {
          rv.SuppressException();
          return nullptr;
        }

      } else if (tag == SCTAG_DOM_DIRECTORY) {
        RefPtr<Directory> directory =
            ReadDirectoryInternal(aReader, indexOrLengthOfString, aHolder);
        if (!directory) {
          return nullptr;
        }

        formData->Append(name, directory);

      } else {
        MOZ_ASSERT(tag == 0);

        nsAutoString value;
        if (NS_WARN_IF(!value.SetLength(indexOrLengthOfString, fallible))) {
          return nullptr;
        }
        size_t charSize = sizeof(nsString::char_type);
        if (!JS_ReadBytes(aReader, (void*)value.BeginWriting(),
                          indexOrLengthOfString * charSize)) {
          return nullptr;
        }

        ErrorResult rv;
        formData->Append(name, value, rv);
        if (NS_WARN_IF(rv.Failed())) {
          rv.SuppressException();
          return nullptr;
        }
      }
    }

    if (!ToJSValue(aCx, formData, &val)) {
      return nullptr;
    }
  }

  return &val.toObject();
}

// The format of the FormData serialization is:
// - pair of ints: SCTAG_DOM_FORMDATA, Length of the FormData elements
// - for each Element element:
//   - name string
//   - if it's a blob:
//     - pair of ints: SCTAG_DOM_BLOB, index of the BlobImpl in the array
//       mBlobImplArray.
//   - if it's a directory (See WriteDirectory):
//     - pair of ints: SCTAG_DOM_DIRECTORY, path length
//     - path as string
//   - else:
//     - pair of ints: 0, string length
//     - value string
bool WriteFormData(JSStructuredCloneWriter* aWriter, FormData* aFormData,
                   StructuredCloneHolder* aHolder) {
  MOZ_ASSERT(aWriter);
  MOZ_ASSERT(aFormData);
  MOZ_ASSERT(aHolder);

  if (!JS_WriteUint32Pair(aWriter, SCTAG_DOM_FORMDATA, aFormData->Length())) {
    return false;
  }

  class MOZ_STACK_CLASS Closure final {
    JSStructuredCloneWriter* mWriter;
    StructuredCloneHolder* mHolder;

   public:
    Closure(JSStructuredCloneWriter* aWriter, StructuredCloneHolder* aHolder)
        : mWriter(aWriter), mHolder(aHolder) {}

    static bool Write(const nsString& aName,
                      const OwningBlobOrDirectoryOrUSVString& aValue,
                      void* aClosure) {
      Closure* closure = static_cast<Closure*>(aClosure);
      if (!WriteString(closure->mWriter, aName)) {
        return false;
      }

      if (aValue.IsBlob()) {
        if (!JS_WriteUint32Pair(closure->mWriter, SCTAG_DOM_BLOB,
                                closure->mHolder->BlobImpls().Length())) {
          return false;
        }

        RefPtr<BlobImpl> blobImpl = aValue.GetAsBlob()->Impl();
        MOZ_ALWAYS_SUCCEEDS(blobImpl->SetMutable(false));

        closure->mHolder->BlobImpls().AppendElement(blobImpl);
        return true;
      }

      if (aValue.IsDirectory()) {
        Directory* directory = aValue.GetAsDirectory();
        return WriteDirectory(closure->mWriter, directory);
      }

      size_t charSize = sizeof(nsString::char_type);
      if (!JS_WriteUint32Pair(closure->mWriter, 0,
                              aValue.GetAsUSVString().Length()) ||
          !JS_WriteBytes(closure->mWriter, aValue.GetAsUSVString().get(),
                         aValue.GetAsUSVString().Length() * charSize)) {
        return false;
      }

      return true;
    }
  };
  Closure closure(aWriter, aHolder);
  return aFormData->ForEach(Closure::Write, &closure);
}

JSObject* ReadWasmModule(JSContext* aCx, uint32_t aIndex,
                         StructuredCloneHolder* aHolder) {
  MOZ_ASSERT(aHolder);
  MOZ_ASSERT(
      aHolder->CloneScope() ==
          StructuredCloneHolder::StructuredCloneScope::SameProcessSameThread ||
      aHolder->CloneScope() == StructuredCloneHolder::StructuredCloneScope::
                                   SameProcessDifferentThread);
#ifdef FUZZING
  if (aIndex >= aHolder->WasmModules().Length()) {
    return nullptr;
  }
#endif
  MOZ_ASSERT(aIndex < aHolder->WasmModules().Length());

  return aHolder->WasmModules()[aIndex]->createObject(aCx);
}

bool WriteWasmModule(JSStructuredCloneWriter* aWriter,
                     JS::WasmModule* aWasmModule,
                     StructuredCloneHolder* aHolder) {
  MOZ_ASSERT(aWriter);
  MOZ_ASSERT(aWasmModule);
  MOZ_ASSERT(aHolder);
  MOZ_ASSERT(
      aHolder->CloneScope() ==
          StructuredCloneHolder::StructuredCloneScope::SameProcessSameThread ||
      aHolder->CloneScope() == StructuredCloneHolder::StructuredCloneScope::
                                   SameProcessDifferentThread);

  // We store the position of the wasmModule in the array as index.
  if (JS_WriteUint32Pair(aWriter, SCTAG_DOM_WASM,
                         aHolder->WasmModules().Length())) {
    aHolder->WasmModules().AppendElement(aWasmModule);
    return true;
  }

  return false;
}

JSObject* ReadInputStream(JSContext* aCx, uint32_t aIndex,
                          StructuredCloneHolder* aHolder) {
  MOZ_ASSERT(aHolder);
#ifdef FUZZING
  if (aIndex >= aHolder->InputStreams().Length()) {
    return nullptr;
  }
#endif
  MOZ_ASSERT(aIndex < aHolder->InputStreams().Length());
  JS::RootedValue result(aCx);
  {
    nsCOMPtr<nsIInputStream> inputStream = aHolder->InputStreams()[aIndex];

    nsresult rv = nsContentUtils::WrapNative(
      aCx, inputStream, &NS_GET_IID(nsIInputStream), &result);
    if (NS_FAILED(rv)) {
      return nullptr;
    }
  }

  return &result.toObject();
}

bool WriteInputStream(JSStructuredCloneWriter* aWriter,
                      nsIInputStream* aInputStream,
                      StructuredCloneHolder* aHolder) {
  MOZ_ASSERT(aWriter);
  MOZ_ASSERT(aInputStream);
  MOZ_ASSERT(aHolder);

  // We store the position of the inputStream in the array as index.
  if (JS_WriteUint32Pair(aWriter, SCTAG_DOM_INPUTSTREAM,
                         aHolder->InputStreams().Length())) {
    aHolder->InputStreams().AppendElement(aInputStream);
    return true;
  }

  return false;
}

}  // anonymous namespace

JSObject* StructuredCloneHolder::CustomReadHandler(
    JSContext* aCx, JSStructuredCloneReader* aReader, uint32_t aTag,
    uint32_t aIndex) {
  MOZ_ASSERT(mSupportsCloning);

  if (aTag == SCTAG_DOM_BLOB) {
    return ReadBlob(aCx, aIndex, this);
  }

  if (aTag == SCTAG_DOM_DIRECTORY) {
    return ReadDirectory(aCx, aReader, aIndex, this);
  }

  if (aTag == SCTAG_DOM_FILELIST) {
    return ReadFileList(aCx, aReader, aIndex, this);
  }

  if (aTag == SCTAG_DOM_FORMDATA) {
    return ReadFormData(aCx, aReader, aIndex, this);
  }

  if (aTag == SCTAG_DOM_IMAGEBITMAP &&
      (mStructuredCloneScope == StructuredCloneScope::SameProcessSameThread ||
       mStructuredCloneScope ==
           StructuredCloneScope::SameProcessDifferentThread)) {
    // Get the current global object.
    // This can be null.
    JS::RootedObject result(aCx);
    {
      nsCOMPtr<nsIGlobalObject> parent = do_QueryInterface(mParent);
      // aIndex is the index of the cloned image.
      result = ImageBitmap::ReadStructuredClone(aCx, aReader, parent,
                                                GetSurfaces(), aIndex);
    }
    return result;
  }

  if (aTag == SCTAG_DOM_STRUCTURED_CLONE_HOLDER) {
    return StructuredCloneBlob::ReadStructuredClone(aCx, aReader, this);
  }

  if (aTag == SCTAG_DOM_WASM &&
      (mStructuredCloneScope == StructuredCloneScope::SameProcessSameThread ||
       mStructuredCloneScope ==
           StructuredCloneScope::SameProcessDifferentThread)) {
    return ReadWasmModule(aCx, aIndex, this);
  }

  if (aTag == SCTAG_DOM_INPUTSTREAM) {
    return ReadInputStream(aCx, aIndex, this);
  }

  return ReadFullySerializableObjects(aCx, aReader, aTag);
}

bool StructuredCloneHolder::CustomWriteHandler(JSContext* aCx,
                                               JSStructuredCloneWriter* aWriter,
                                               JS::Handle<JSObject*> aObj) {
  if (!mSupportsCloning) {
    return false;
  }

  JS::Rooted<JSObject*> obj(aCx, aObj);

  // See if this is a File/Blob object.
  {
    Blob* blob = nullptr;
    if (NS_SUCCEEDED(UNWRAP_OBJECT(Blob, &obj, blob))) {
      return WriteBlob(aWriter, blob, this);
    }
  }

  // See if this is a Directory object.
  {
    Directory* directory = nullptr;
    if (NS_SUCCEEDED(UNWRAP_OBJECT(Directory, &obj, directory))) {
      return WriteDirectory(aWriter, directory);
    }
  }

  // See if this is a FileList object.
  {
    FileList* fileList = nullptr;
    if (NS_SUCCEEDED(UNWRAP_OBJECT(FileList, &obj, fileList))) {
      return WriteFileList(aWriter, fileList, this);
    }
  }

  // See if this is a FormData object.
  {
    FormData* formData = nullptr;
    if (NS_SUCCEEDED(UNWRAP_OBJECT(FormData, &obj, formData))) {
      return WriteFormData(aWriter, formData, this);
    }
  }

  // See if this is an ImageBitmap object.
  if (mStructuredCloneScope == StructuredCloneScope::SameProcessSameThread ||
      mStructuredCloneScope ==
          StructuredCloneScope::SameProcessDifferentThread) {
    ImageBitmap* imageBitmap = nullptr;
    if (NS_SUCCEEDED(UNWRAP_OBJECT(ImageBitmap, &obj, imageBitmap))) {
      return ImageBitmap::WriteStructuredClone(aWriter, GetSurfaces(),
                                               imageBitmap);
    }
  }

  // See if this is a StructuredCloneBlob object.
  {
    StructuredCloneBlob* holder = nullptr;
    if (NS_SUCCEEDED(UNWRAP_OBJECT(StructuredCloneHolder, &obj, holder))) {
      return holder->WriteStructuredClone(aCx, aWriter, this);
    }
  }

  // See if this is a WasmModule.
  if ((mStructuredCloneScope == StructuredCloneScope::SameProcessSameThread ||
       mStructuredCloneScope ==
           StructuredCloneScope::SameProcessDifferentThread) &&
      JS::IsWasmModuleObject(obj)) {
    RefPtr<JS::WasmModule> module = JS::GetWasmModule(obj);
    MOZ_ASSERT(module);

    return WriteWasmModule(aWriter, module, this);
  }

  {
    // We only care about streams, so ReflectorToISupportsStatic is fine.
    nsCOMPtr<nsISupports> base = xpc::ReflectorToISupportsStatic(aObj);
    nsCOMPtr<nsIInputStream> inputStream = do_QueryInterface(base);
    if (inputStream) {
      return WriteInputStream(aWriter, inputStream, this);
    }
  }

  return WriteFullySerializableObjects(aCx, aWriter, aObj);
}

bool StructuredCloneHolder::CustomReadTransferHandler(
    JSContext* aCx, JSStructuredCloneReader* aReader, uint32_t aTag,
    void* aContent, uint64_t aExtraData,
    JS::MutableHandleObject aReturnObject) {
  MOZ_ASSERT(mSupportsTransferring);

  if (aTag == SCTAG_DOM_MAP_MESSAGEPORT) {
#ifdef FUZZING
    if (aExtraData >= mPortIdentifiers.Length()) {
      return false;
    }
#endif
    MOZ_ASSERT(aExtraData < mPortIdentifiers.Length());
    const MessagePortIdentifier& portIdentifier = mPortIdentifiers[aExtraData];

    nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(mParent);

    ErrorResult rv;
    RefPtr<MessagePort> port = MessagePort::Create(global, portIdentifier, rv);
    if (NS_WARN_IF(rv.Failed())) {
      rv.SuppressException();
      return false;
    }

    mTransferredPorts.AppendElement(port);

    JS::Rooted<JS::Value> value(aCx);
    if (!GetOrCreateDOMReflector(aCx, port, &value)) {
      JS_ClearPendingException(aCx);
      return false;
    }

    aReturnObject.set(&value.toObject());
    return true;
  }

  if (aTag == SCTAG_DOM_CANVAS &&
      (mStructuredCloneScope == StructuredCloneScope::SameProcessSameThread ||
       mStructuredCloneScope ==
           StructuredCloneScope::SameProcessDifferentThread)) {
    MOZ_ASSERT(aContent);
    OffscreenCanvasCloneData* data =
        static_cast<OffscreenCanvasCloneData*>(aContent);
    nsCOMPtr<nsIGlobalObject> parent = do_QueryInterface(mParent);
    RefPtr<OffscreenCanvas> canvas =
        OffscreenCanvas::CreateFromCloneData(parent, data);
    delete data;

    JS::Rooted<JS::Value> value(aCx);
    if (!GetOrCreateDOMReflector(aCx, canvas, &value)) {
      JS_ClearPendingException(aCx);
      return false;
    }

    aReturnObject.set(&value.toObject());
    return true;
  }

  if (aTag == SCTAG_DOM_IMAGEBITMAP &&
      (mStructuredCloneScope == StructuredCloneScope::SameProcessSameThread ||
       mStructuredCloneScope ==
           StructuredCloneScope::SameProcessDifferentThread)) {
    MOZ_ASSERT(aContent);
    ImageBitmapCloneData* data = static_cast<ImageBitmapCloneData*>(aContent);
    nsCOMPtr<nsIGlobalObject> parent = do_QueryInterface(mParent);
    RefPtr<ImageBitmap> bitmap = ImageBitmap::CreateFromCloneData(parent, data);
    delete data;

    JS::Rooted<JS::Value> value(aCx);
    if (!GetOrCreateDOMReflector(aCx, bitmap, &value)) {
      JS_ClearPendingException(aCx);
      return false;
    }

    aReturnObject.set(&value.toObject());
    return true;
  }

  return false;
}

bool StructuredCloneHolder::CustomWriteTransferHandler(
    JSContext* aCx, JS::Handle<JSObject*> aObj, uint32_t* aTag,
    JS::TransferableOwnership* aOwnership, void** aContent,
    uint64_t* aExtraData) {
  if (!mSupportsTransferring) {
    return false;
  }

  JS::Rooted<JSObject*> obj(aCx, aObj);

  {
    MessagePort* port = nullptr;
    nsresult rv = UNWRAP_OBJECT(MessagePort, &obj, port);
    if (NS_SUCCEEDED(rv)) {
      // We use aExtraData to store the index of this new port identifier.
      *aExtraData = mPortIdentifiers.Length();
      MessagePortIdentifier* identifier = mPortIdentifiers.AppendElement();

      if (!port->CanBeCloned()) {
        return false;
      }

      port->CloneAndDisentangle(*identifier);

      *aTag = SCTAG_DOM_MAP_MESSAGEPORT;
      *aOwnership = JS::SCTAG_TMO_CUSTOM;
      *aContent = nullptr;

      return true;
    }

    if (mStructuredCloneScope == StructuredCloneScope::SameProcessSameThread ||
        mStructuredCloneScope ==
            StructuredCloneScope::SameProcessDifferentThread) {
      OffscreenCanvas* canvas = nullptr;
      rv = UNWRAP_OBJECT(OffscreenCanvas, &obj, canvas);
      if (NS_SUCCEEDED(rv)) {
        MOZ_ASSERT(canvas);

        if (canvas->IsNeutered()) {
          return false;
        }

        *aExtraData = 0;
        *aTag = SCTAG_DOM_CANVAS;
        *aOwnership = JS::SCTAG_TMO_CUSTOM;
        *aContent = canvas->ToCloneData();
        MOZ_ASSERT(*aContent);
        canvas->SetNeutered();

        return true;
      }

      ImageBitmap* bitmap = nullptr;
      rv = UNWRAP_OBJECT(ImageBitmap, &obj, bitmap);
      if (NS_SUCCEEDED(rv)) {
        MOZ_ASSERT(bitmap);

        *aExtraData = 0;
        *aTag = SCTAG_DOM_IMAGEBITMAP;
        *aOwnership = JS::SCTAG_TMO_CUSTOM;

        UniquePtr<ImageBitmapCloneData> clonedBitmap = bitmap->ToCloneData();
        if (!clonedBitmap) {
          return false;
        }

        *aContent = clonedBitmap.release();
        MOZ_ASSERT(*aContent);
        bitmap->Close();

        return true;
      }
    }
  }

  return false;
}

void StructuredCloneHolder::CustomFreeTransferHandler(
    uint32_t aTag, JS::TransferableOwnership aOwnership, void* aContent,
    uint64_t aExtraData) {
  MOZ_ASSERT(mSupportsTransferring);

  if (aTag == SCTAG_DOM_MAP_MESSAGEPORT) {
    MOZ_ASSERT(!aContent);
#ifdef FUZZING
    if (aExtraData >= mPortIdentifiers.Length()) {
      return;
    }
#endif
    MOZ_ASSERT(aExtraData < mPortIdentifiers.Length());
    MessagePort::ForceClose(mPortIdentifiers[aExtraData]);
    return;
  }

  if (aTag == SCTAG_DOM_CANVAS &&
      (mStructuredCloneScope == StructuredCloneScope::SameProcessSameThread ||
       mStructuredCloneScope ==
           StructuredCloneScope::SameProcessDifferentThread)) {
    MOZ_ASSERT(aContent);
    OffscreenCanvasCloneData* data =
        static_cast<OffscreenCanvasCloneData*>(aContent);
    delete data;
    return;
  }

  if (aTag == SCTAG_DOM_IMAGEBITMAP &&
      (mStructuredCloneScope == StructuredCloneScope::SameProcessSameThread ||
       mStructuredCloneScope ==
           StructuredCloneScope::SameProcessDifferentThread)) {
    MOZ_ASSERT(aContent);
    ImageBitmapCloneData* data = static_cast<ImageBitmapCloneData*>(aContent);
    delete data;
    return;
  }
}

bool StructuredCloneHolder::CustomCanTransferHandler(
    JSContext* aCx, JS::Handle<JSObject*> aObj) {
  if (!mSupportsTransferring) {
    return false;
  }

  JS::Rooted<JSObject*> obj(aCx, aObj);

  {
    MessagePort* port = nullptr;
    nsresult rv = UNWRAP_OBJECT(MessagePort, &obj, port);
    if (NS_SUCCEEDED(rv)) {
      return true;
    }

    if (mStructuredCloneScope == StructuredCloneScope::SameProcessSameThread ||
        mStructuredCloneScope ==
            StructuredCloneScope::SameProcessDifferentThread) {
      OffscreenCanvas* canvas = nullptr;
      rv = UNWRAP_OBJECT(OffscreenCanvas, &obj, canvas);
      if (NS_SUCCEEDED(rv)) {
        return true;
      }

      ImageBitmap* bitmap = nullptr;
      rv = UNWRAP_OBJECT(ImageBitmap, &obj, bitmap);
      if (NS_SUCCEEDED(rv)) {
        return true;
      }
    }
  }

  return false;
}

bool StructuredCloneHolder::TakeTransferredPortsAsSequence(
    Sequence<OwningNonNull<mozilla::dom::MessagePort>>& aPorts) {
  nsTArray<RefPtr<MessagePort>> ports = TakeTransferredPorts();

  aPorts.Clear();
  for (uint32_t i = 0, len = ports.Length(); i < len; ++i) {
    if (!aPorts.AppendElement(ports[i].forget(), fallible)) {
      return false;
    }
  }

  return true;
}

}  // namespace dom
}  // namespace mozilla
