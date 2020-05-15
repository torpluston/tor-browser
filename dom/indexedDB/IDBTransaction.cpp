/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "IDBTransaction.h"

#include "BackgroundChildImpl.h"
#include "IDBDatabase.h"
#include "IDBEvents.h"
#include "IDBObjectStore.h"
#include "IDBRequest.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/EventDispatcher.h"
#include "mozilla/HoldDropJSObjects.h"
#include "mozilla/dom/DOMException.h"
#include "mozilla/dom/DOMStringList.h"
#include "mozilla/dom/WorkerRef.h"
#include "mozilla/dom/WorkerPrivate.h"
#include "mozilla/ipc/BackgroundChild.h"
#include "mozilla/ScopeExit.h"
#include "nsAutoPtr.h"
#include "nsPIDOMWindow.h"
#include "nsQueryObject.h"
#include "nsServiceManagerUtils.h"
#include "nsTHashtable.h"
#include "ProfilerHelpers.h"
#include "ReportInternalError.h"

// Include this last to avoid path problems on Windows.
#include "ActorsChild.h"

namespace mozilla {
namespace dom {

using namespace mozilla::dom::indexedDB;
using namespace mozilla::ipc;

IDBTransaction::IDBTransaction(IDBDatabase* aDatabase,
                               const nsTArray<nsString>& aObjectStoreNames,
                               Mode aMode)
    : DOMEventTargetHelper(aDatabase),
      mDatabase(aDatabase),
      mObjectStoreNames(aObjectStoreNames),
      mLoggingSerialNumber(0),
      mNextObjectStoreId(0),
      mNextIndexId(0),
      mAbortCode(NS_OK),
      mPendingRequestCount(0),
      mLineNo(0),
      mColumn(0),
      mReadyState(IDBTransaction::INITIAL),
      mMode(aMode),
      mCreating(false),
      mRegistered(false),
      mAbortedByScript(false),
      mNotedActiveTransaction(false)
#ifdef DEBUG
      ,
      mSentCommitOrAbort(false),
      mFiredCompleteOrAbort(false)
#endif
{
  MOZ_ASSERT(aDatabase);
  aDatabase->AssertIsOnOwningThread();

  mBackgroundActor.mNormalBackgroundActor = nullptr;

  BackgroundChildImpl::ThreadLocal* threadLocal =
      BackgroundChildImpl::GetThreadLocalForCurrentThread();
  MOZ_ASSERT(threadLocal);

  ThreadLocal* idbThreadLocal = threadLocal->mIndexedDBThreadLocal;
  MOZ_ASSERT(idbThreadLocal);

  const_cast<int64_t&>(mLoggingSerialNumber) =
      idbThreadLocal->NextTransactionSN(aMode);

#ifdef DEBUG
  if (!aObjectStoreNames.IsEmpty()) {
    nsTArray<nsString> sortedNames(aObjectStoreNames);
    sortedNames.Sort();

    const uint32_t count = sortedNames.Length();
    MOZ_ASSERT(count == aObjectStoreNames.Length());

    // Make sure the array is properly sorted.
    for (uint32_t index = 0; index < count; index++) {
      MOZ_ASSERT(aObjectStoreNames[index] == sortedNames[index]);
    }

    // Make sure there are no duplicates in our objectStore names.
    for (uint32_t index = 0; index < count - 1; index++) {
      MOZ_ASSERT(sortedNames[index] != sortedNames[index + 1]);
    }
  }
#endif

  mozilla::HoldJSObjects(this);
}

IDBTransaction::~IDBTransaction() {
  AssertIsOnOwningThread();
  MOZ_ASSERT(!mPendingRequestCount);
  MOZ_ASSERT(!mCreating);
  MOZ_ASSERT(!mNotedActiveTransaction);
  MOZ_ASSERT(mSentCommitOrAbort);
  MOZ_ASSERT_IF(
      mMode == VERSION_CHANGE && mBackgroundActor.mVersionChangeBackgroundActor,
      mFiredCompleteOrAbort);
  MOZ_ASSERT_IF(
      mMode != VERSION_CHANGE && mBackgroundActor.mNormalBackgroundActor,
      mFiredCompleteOrAbort);

  if (mRegistered) {
    mDatabase->UnregisterTransaction(this);
#ifdef DEBUG
    mRegistered = false;
#endif
  }

  if (mMode == VERSION_CHANGE) {
    if (auto* actor = mBackgroundActor.mVersionChangeBackgroundActor) {
      actor->SendDeleteMeInternal(/* aFailedConstructor */ false);

      MOZ_ASSERT(!mBackgroundActor.mVersionChangeBackgroundActor,
                 "SendDeleteMeInternal should have cleared!");
    }
  } else if (auto* actor = mBackgroundActor.mNormalBackgroundActor) {
    actor->SendDeleteMeInternal();

    MOZ_ASSERT(!mBackgroundActor.mNormalBackgroundActor,
               "SendDeleteMeInternal should have cleared!");
  }

  ReleaseWrapper(this);
  mozilla::DropJSObjects(this);
}

// static
already_AddRefed<IDBTransaction> IDBTransaction::CreateVersionChange(
    IDBDatabase* aDatabase, BackgroundVersionChangeTransactionChild* aActor,
    IDBOpenDBRequest* aOpenRequest, int64_t aNextObjectStoreId,
    int64_t aNextIndexId) {
  MOZ_ASSERT(aDatabase);
  aDatabase->AssertIsOnOwningThread();
  MOZ_ASSERT(aActor);
  MOZ_ASSERT(aOpenRequest);
  MOZ_ASSERT(aNextObjectStoreId > 0);
  MOZ_ASSERT(aNextIndexId > 0);

  nsTArray<nsString> emptyObjectStoreNames;

  RefPtr<IDBTransaction> transaction =
      new IDBTransaction(aDatabase, emptyObjectStoreNames, VERSION_CHANGE);
  aOpenRequest->GetCallerLocation(transaction->mFilename, &transaction->mLineNo,
                                  &transaction->mColumn);

  transaction->NoteActiveTransaction();

  transaction->mBackgroundActor.mVersionChangeBackgroundActor = aActor;
  transaction->mNextObjectStoreId = aNextObjectStoreId;
  transaction->mNextIndexId = aNextIndexId;

  aDatabase->RegisterTransaction(transaction);
  transaction->mRegistered = true;

  return transaction.forget();
}

// static
already_AddRefed<IDBTransaction> IDBTransaction::Create(
    JSContext* aCx, IDBDatabase* aDatabase,
    const nsTArray<nsString>& aObjectStoreNames, Mode aMode) {
  MOZ_ASSERT(aDatabase);
  aDatabase->AssertIsOnOwningThread();
  MOZ_ASSERT(!aObjectStoreNames.IsEmpty());
  MOZ_ASSERT(aMode == READ_ONLY || aMode == READ_WRITE ||
             aMode == READ_WRITE_FLUSH || aMode == CLEANUP);

  RefPtr<IDBTransaction> transaction =
      new IDBTransaction(aDatabase, aObjectStoreNames, aMode);
  IDBRequest::CaptureCaller(aCx, transaction->mFilename, &transaction->mLineNo,
                            &transaction->mColumn);

  if (!NS_IsMainThread()) {
    WorkerPrivate* workerPrivate = GetCurrentThreadWorkerPrivate();
    MOZ_ASSERT(workerPrivate);

    workerPrivate->AssertIsOnWorkerThread();

    RefPtr<StrongWorkerRef> workerRef = StrongWorkerRef::Create(
        workerPrivate, "IDBTransaction", [transaction]() {
          transaction->AssertIsOnOwningThread();
          if (!transaction->IsCommittingOrDone()) {
            IDB_REPORT_INTERNAL_ERR();
            transaction->AbortInternal(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR,
                                       nullptr);
          }
        });
    if (NS_WARN_IF(!workerRef)) {
      // Silence the destructor assertion if we never made this object live.
#ifdef DEBUG
      MOZ_ASSERT(!transaction->mSentCommitOrAbort);
      transaction->mSentCommitOrAbort = true;
#endif
      return nullptr;
    }

    transaction->mWorkerRef = std::move(workerRef);
  }

  nsCOMPtr<nsIRunnable> runnable = do_QueryObject(transaction);
  nsContentUtils::AddPendingIDBTransaction(runnable.forget());

  transaction->mCreating = true;

  aDatabase->RegisterTransaction(transaction);
  transaction->mRegistered = true;

  return transaction.forget();
}

// static
IDBTransaction* IDBTransaction::GetCurrent() {
  using namespace mozilla::ipc;

  MOZ_ASSERT(BackgroundChild::GetForCurrentThread());

  BackgroundChildImpl::ThreadLocal* threadLocal =
      BackgroundChildImpl::GetThreadLocalForCurrentThread();
  MOZ_ASSERT(threadLocal);

  ThreadLocal* idbThreadLocal = threadLocal->mIndexedDBThreadLocal;
  MOZ_ASSERT(idbThreadLocal);

  return idbThreadLocal->GetCurrentTransaction();
}

#ifdef DEBUG

void IDBTransaction::AssertIsOnOwningThread() const {
  MOZ_ASSERT(mDatabase);
  mDatabase->AssertIsOnOwningThread();
}

#endif  // DEBUG

void IDBTransaction::SetBackgroundActor(
    indexedDB::BackgroundTransactionChild* aBackgroundActor) {
  AssertIsOnOwningThread();
  MOZ_ASSERT(aBackgroundActor);
  MOZ_ASSERT(!mBackgroundActor.mNormalBackgroundActor);
  MOZ_ASSERT(mMode != VERSION_CHANGE);

  NoteActiveTransaction();

  mBackgroundActor.mNormalBackgroundActor = aBackgroundActor;
}

BackgroundRequestChild* IDBTransaction::StartRequest(
    IDBRequest* aRequest, const RequestParams& aParams) {
  AssertIsOnOwningThread();
  MOZ_ASSERT(aRequest);
  MOZ_ASSERT(aParams.type() != RequestParams::T__None);

  BackgroundRequestChild* actor = new BackgroundRequestChild(aRequest);

  if (mMode == VERSION_CHANGE) {
    MOZ_ASSERT(mBackgroundActor.mVersionChangeBackgroundActor);

    mBackgroundActor.mVersionChangeBackgroundActor
        ->SendPBackgroundIDBRequestConstructor(actor, aParams);
  } else {
    MOZ_ASSERT(mBackgroundActor.mNormalBackgroundActor);

    mBackgroundActor.mNormalBackgroundActor
        ->SendPBackgroundIDBRequestConstructor(actor, aParams);
  }

  MOZ_ASSERT(actor->GetActorEventTarget(),
             "The event target shall be inherited from its manager actor.");

  // Balanced in BackgroundRequestChild::Recv__delete__().
  OnNewRequest();

  return actor;
}

void IDBTransaction::OpenCursor(BackgroundCursorChild* aBackgroundActor,
                                const OpenCursorParams& aParams) {
  AssertIsOnOwningThread();
  MOZ_ASSERT(aBackgroundActor);
  MOZ_ASSERT(aParams.type() != OpenCursorParams::T__None);

  if (mMode == VERSION_CHANGE) {
    MOZ_ASSERT(mBackgroundActor.mVersionChangeBackgroundActor);

    mBackgroundActor.mVersionChangeBackgroundActor
        ->SendPBackgroundIDBCursorConstructor(aBackgroundActor, aParams);
  } else {
    MOZ_ASSERT(mBackgroundActor.mNormalBackgroundActor);

    mBackgroundActor.mNormalBackgroundActor
        ->SendPBackgroundIDBCursorConstructor(aBackgroundActor, aParams);
  }

  MOZ_ASSERT(aBackgroundActor->GetActorEventTarget(),
             "The event target shall be inherited from its manager actor.");

  // Balanced in BackgroundCursorChild::RecvResponse().
  OnNewRequest();
}

void IDBTransaction::RefreshSpec(bool aMayDelete) {
  AssertIsOnOwningThread();

  for (uint32_t count = mObjectStores.Length(), index = 0; index < count;
       index++) {
    mObjectStores[index]->RefreshSpec(aMayDelete);
  }

  for (uint32_t count = mDeletedObjectStores.Length(), index = 0; index < count;
       index++) {
    mDeletedObjectStores[index]->RefreshSpec(false);
  }
}

void IDBTransaction::OnNewRequest() {
  AssertIsOnOwningThread();

  if (!mPendingRequestCount) {
    MOZ_ASSERT(INITIAL == mReadyState);
    mReadyState = LOADING;
  }

  ++mPendingRequestCount;
}

void IDBTransaction::OnRequestFinished(bool aActorDestroyedNormally) {
  AssertIsOnOwningThread();
  MOZ_ASSERT(mPendingRequestCount);

  --mPendingRequestCount;

  if (!mPendingRequestCount) {
    mReadyState = COMMITTING;

    if (aActorDestroyedNormally) {
      if (NS_SUCCEEDED(mAbortCode)) {
        SendCommit();
      } else {
        SendAbort(mAbortCode);
      }
    } else {
      // Don't try to send any more messages to the parent if the request actor
      // was killed.
#ifdef DEBUG
      MOZ_ASSERT(!mSentCommitOrAbort);
      mSentCommitOrAbort = true;
#endif
      IDB_LOG_MARK(
          "IndexedDB %s: Child  Transaction[%lld]: "
          "Request actor was killed, transaction will be aborted",
          "IndexedDB %s: C T[%lld]: IDBTransaction abort", IDB_LOG_ID_STRING(),
          LoggingSerialNumber());
    }
  }
}

void IDBTransaction::SendCommit() {
  AssertIsOnOwningThread();
  MOZ_ASSERT(NS_SUCCEEDED(mAbortCode));
  MOZ_ASSERT(IsCommittingOrDone());
  MOZ_ASSERT(!mSentCommitOrAbort);
  MOZ_ASSERT(!mPendingRequestCount);

  // Don't do this in the macro because we always need to increment the serial
  // number to keep in sync with the parent.
  const uint64_t requestSerialNumber = IDBRequest::NextSerialNumber();

  IDB_LOG_MARK(
      "IndexedDB %s: Child  Transaction[%lld] Request[%llu]: "
      "All requests complete, committing transaction",
      "IndexedDB %s: C T[%lld] R[%llu]: IDBTransaction commit",
      IDB_LOG_ID_STRING(), LoggingSerialNumber(), requestSerialNumber);

  if (mMode == VERSION_CHANGE) {
    MOZ_ASSERT(mBackgroundActor.mVersionChangeBackgroundActor);
    mBackgroundActor.mVersionChangeBackgroundActor->SendCommit();
  } else {
    MOZ_ASSERT(mBackgroundActor.mNormalBackgroundActor);
    mBackgroundActor.mNormalBackgroundActor->SendCommit();
  }

#ifdef DEBUG
  mSentCommitOrAbort = true;
#endif
}

void IDBTransaction::SendAbort(nsresult aResultCode) {
  AssertIsOnOwningThread();
  MOZ_ASSERT(NS_FAILED(aResultCode));
  MOZ_ASSERT(IsCommittingOrDone());
  MOZ_ASSERT(!mSentCommitOrAbort);

  // Don't do this in the macro because we always need to increment the serial
  // number to keep in sync with the parent.
  const uint64_t requestSerialNumber = IDBRequest::NextSerialNumber();

  IDB_LOG_MARK(
      "IndexedDB %s: Child  Transaction[%lld] Request[%llu]: "
      "Aborting transaction with result 0x%x",
      "IndexedDB %s: C T[%lld] R[%llu]: IDBTransaction abort (0x%x)",
      IDB_LOG_ID_STRING(), LoggingSerialNumber(), requestSerialNumber,
      aResultCode);

  if (mMode == VERSION_CHANGE) {
    MOZ_ASSERT(mBackgroundActor.mVersionChangeBackgroundActor);
    mBackgroundActor.mVersionChangeBackgroundActor->SendAbort(aResultCode);
  } else {
    MOZ_ASSERT(mBackgroundActor.mNormalBackgroundActor);
    mBackgroundActor.mNormalBackgroundActor->SendAbort(aResultCode);
  }

#ifdef DEBUG
  mSentCommitOrAbort = true;
#endif
}

void IDBTransaction::NoteActiveTransaction() {
  AssertIsOnOwningThread();
  MOZ_ASSERT(!mNotedActiveTransaction);

  mDatabase->NoteActiveTransaction();
  mNotedActiveTransaction = true;
}

void IDBTransaction::MaybeNoteInactiveTransaction() {
  AssertIsOnOwningThread();

  if (mNotedActiveTransaction) {
    mDatabase->NoteInactiveTransaction();
    mNotedActiveTransaction = false;
  }
}

bool IDBTransaction::IsOpen() const {
  AssertIsOnOwningThread();

  // If we haven't started anything then we're open.
  if (mReadyState == IDBTransaction::INITIAL) {
    return true;
  }

  // If we've already started then we need to check to see if we still have the
  // mCreating flag set. If we do (i.e. we haven't returned to the event loop
  // from the time we were created) then we are open. Otherwise check the
  // currently running transaction to see if it's the same. We only allow other
  // requests to be made if this transaction is currently running.
  if (mReadyState == IDBTransaction::LOADING &&
      (mCreating || GetCurrent() == this)) {
    return true;
  }

  return false;
}

void IDBTransaction::GetCallerLocation(nsAString& aFilename, uint32_t* aLineNo,
                                       uint32_t* aColumn) const {
  AssertIsOnOwningThread();
  MOZ_ASSERT(aLineNo);
  MOZ_ASSERT(aColumn);

  aFilename = mFilename;
  *aLineNo = mLineNo;
  *aColumn = mColumn;
}

already_AddRefed<IDBObjectStore> IDBTransaction::CreateObjectStore(
    const ObjectStoreSpec& aSpec) {
  AssertIsOnOwningThread();
  MOZ_ASSERT(aSpec.metadata().id());
  MOZ_ASSERT(VERSION_CHANGE == mMode);
  MOZ_ASSERT(mBackgroundActor.mVersionChangeBackgroundActor);
  MOZ_ASSERT(IsOpen());

#ifdef DEBUG
  {
    const nsString& name = aSpec.metadata().name();

    for (uint32_t count = mObjectStores.Length(), index = 0; index < count;
         index++) {
      MOZ_ASSERT(mObjectStores[index]->Name() != name);
    }
  }
#endif

  MOZ_ALWAYS_TRUE(
      mBackgroundActor.mVersionChangeBackgroundActor->SendCreateObjectStore(
          aSpec.metadata()));

  RefPtr<IDBObjectStore> objectStore = IDBObjectStore::Create(this, aSpec);
  MOZ_ASSERT(objectStore);

  mObjectStores.AppendElement(objectStore);

  return objectStore.forget();
}

void IDBTransaction::DeleteObjectStore(int64_t aObjectStoreId) {
  AssertIsOnOwningThread();
  MOZ_ASSERT(aObjectStoreId);
  MOZ_ASSERT(VERSION_CHANGE == mMode);
  MOZ_ASSERT(mBackgroundActor.mVersionChangeBackgroundActor);
  MOZ_ASSERT(IsOpen());

  MOZ_ALWAYS_TRUE(
      mBackgroundActor.mVersionChangeBackgroundActor->SendDeleteObjectStore(
          aObjectStoreId));

  for (uint32_t count = mObjectStores.Length(), index = 0; index < count;
       index++) {
    RefPtr<IDBObjectStore>& objectStore = mObjectStores[index];

    if (objectStore->Id() == aObjectStoreId) {
      objectStore->NoteDeletion();

      RefPtr<IDBObjectStore>* deletedObjectStore =
          mDeletedObjectStores.AppendElement();
      deletedObjectStore->swap(mObjectStores[index]);

      mObjectStores.RemoveElementAt(index);
      break;
    }
  }
}

void IDBTransaction::RenameObjectStore(int64_t aObjectStoreId,
                                       const nsAString& aName) {
  AssertIsOnOwningThread();
  MOZ_ASSERT(aObjectStoreId);
  MOZ_ASSERT(VERSION_CHANGE == mMode);
  MOZ_ASSERT(mBackgroundActor.mVersionChangeBackgroundActor);
  MOZ_ASSERT(IsOpen());

  MOZ_ALWAYS_TRUE(
      mBackgroundActor.mVersionChangeBackgroundActor->SendRenameObjectStore(
          aObjectStoreId, nsString(aName)));
}

void IDBTransaction::CreateIndex(IDBObjectStore* aObjectStore,
                                 const indexedDB::IndexMetadata& aMetadata) {
  AssertIsOnOwningThread();
  MOZ_ASSERT(aObjectStore);
  MOZ_ASSERT(aMetadata.id());
  MOZ_ASSERT(VERSION_CHANGE == mMode);
  MOZ_ASSERT(mBackgroundActor.mVersionChangeBackgroundActor);
  MOZ_ASSERT(IsOpen());

  MOZ_ALWAYS_TRUE(
      mBackgroundActor.mVersionChangeBackgroundActor->SendCreateIndex(
          aObjectStore->Id(), aMetadata));
}

void IDBTransaction::DeleteIndex(IDBObjectStore* aObjectStore,
                                 int64_t aIndexId) {
  AssertIsOnOwningThread();
  MOZ_ASSERT(aObjectStore);
  MOZ_ASSERT(aIndexId);
  MOZ_ASSERT(VERSION_CHANGE == mMode);
  MOZ_ASSERT(mBackgroundActor.mVersionChangeBackgroundActor);
  MOZ_ASSERT(IsOpen());

  MOZ_ALWAYS_TRUE(
      mBackgroundActor.mVersionChangeBackgroundActor->SendDeleteIndex(
          aObjectStore->Id(), aIndexId));
}

void IDBTransaction::RenameIndex(IDBObjectStore* aObjectStore, int64_t aIndexId,
                                 const nsAString& aName) {
  AssertIsOnOwningThread();
  MOZ_ASSERT(aObjectStore);
  MOZ_ASSERT(aIndexId);
  MOZ_ASSERT(VERSION_CHANGE == mMode);
  MOZ_ASSERT(mBackgroundActor.mVersionChangeBackgroundActor);
  MOZ_ASSERT(IsOpen());

  MOZ_ALWAYS_TRUE(
      mBackgroundActor.mVersionChangeBackgroundActor->SendRenameIndex(
          aObjectStore->Id(), aIndexId, nsString(aName)));
}

void IDBTransaction::AbortInternal(nsresult aAbortCode,
                                   already_AddRefed<DOMException> aError) {
  AssertIsOnOwningThread();
  MOZ_ASSERT(NS_FAILED(aAbortCode));
  MOZ_ASSERT(!IsCommittingOrDone());

  RefPtr<DOMException> error = aError;

  const bool isVersionChange = mMode == VERSION_CHANGE;
  const bool isInvalidated = mDatabase->IsInvalidated();
  bool needToSendAbort = mReadyState == INITIAL;

  mAbortCode = aAbortCode;
  mReadyState = DONE;
  mError = error.forget();

  if (isVersionChange) {
    // If a version change transaction is aborted, we must revert the world
    // back to its previous state unless we're being invalidated after the
    // transaction already completed.
    if (!isInvalidated) {
      mDatabase->RevertToPreviousState();
    }

    // We do the reversion only for the mObjectStores/mDeletedObjectStores but
    // not for the mIndexes/mDeletedIndexes of each IDBObjectStore because it's
    // time-consuming(O(m*n)) and mIndexes/mDeletedIndexes won't be used anymore
    // in IDBObjectStore::(Create|Delete)Index() and IDBObjectStore::Index() in
    // which all the executions are returned earlier by !transaction->IsOpen().

    const nsTArray<ObjectStoreSpec>& specArray =
        mDatabase->Spec()->objectStores();

    if (specArray.IsEmpty()) {
      mObjectStores.Clear();
      mDeletedObjectStores.Clear();
    } else {
      nsTHashtable<nsUint64HashKey> validIds(specArray.Length());

      for (uint32_t specCount = specArray.Length(), specIndex = 0;
           specIndex < specCount; specIndex++) {
        const int64_t objectStoreId = specArray[specIndex].metadata().id();
        MOZ_ASSERT(objectStoreId);

        validIds.PutEntry(uint64_t(objectStoreId));
      }

      for (uint32_t objCount = mObjectStores.Length(), objIndex = 0;
           objIndex < objCount;
           /* incremented conditionally */) {
        const int64_t objectStoreId = mObjectStores[objIndex]->Id();
        MOZ_ASSERT(objectStoreId);

        if (validIds.Contains(uint64_t(objectStoreId))) {
          objIndex++;
        } else {
          mObjectStores.RemoveElementAt(objIndex);
          objCount--;
        }
      }

      if (!mDeletedObjectStores.IsEmpty()) {
        for (uint32_t objCount = mDeletedObjectStores.Length(), objIndex = 0;
             objIndex < objCount; objIndex++) {
          const int64_t objectStoreId = mDeletedObjectStores[objIndex]->Id();
          MOZ_ASSERT(objectStoreId);

          if (validIds.Contains(uint64_t(objectStoreId))) {
            RefPtr<IDBObjectStore>* objectStore = mObjectStores.AppendElement();
            objectStore->swap(mDeletedObjectStores[objIndex]);
          }
        }
        mDeletedObjectStores.Clear();
      }
    }
  }

  // Fire the abort event if there are no outstanding requests. Otherwise the
  // abort event will be fired when all outstanding requests finish.
  if (needToSendAbort) {
    SendAbort(aAbortCode);
  }

  if (isVersionChange) {
    mDatabase->Close();
  }
}

void IDBTransaction::Abort(IDBRequest* aRequest) {
  AssertIsOnOwningThread();
  MOZ_ASSERT(aRequest);

  if (IsCommittingOrDone()) {
    // Already started (and maybe finished) the commit or abort so there is
    // nothing to do here.
    return;
  }

  ErrorResult rv;
  RefPtr<DOMException> error = aRequest->GetError(rv);

  AbortInternal(aRequest->GetErrorCode(), error.forget());
}

void IDBTransaction::Abort(nsresult aErrorCode) {
  AssertIsOnOwningThread();

  if (IsCommittingOrDone()) {
    // Already started (and maybe finished) the commit or abort so there is
    // nothing to do here.
    return;
  }

  RefPtr<DOMException> error = DOMException::Create(aErrorCode);
  AbortInternal(aErrorCode, error.forget());
}

void IDBTransaction::Abort(ErrorResult& aRv) {
  AssertIsOnOwningThread();

  if (IsCommittingOrDone()) {
    aRv = NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR;
    return;
  }

  AbortInternal(NS_ERROR_DOM_INDEXEDDB_ABORT_ERR, nullptr);

  MOZ_ASSERT(!mAbortedByScript);
  mAbortedByScript = true;
}

void IDBTransaction::FireCompleteOrAbortEvents(nsresult aResult) {
  AssertIsOnOwningThread();
  MOZ_ASSERT(!mFiredCompleteOrAbort);

  mReadyState = DONE;

#ifdef DEBUG
  mFiredCompleteOrAbort = true;
#endif

  // Make sure we drop the WorkerRef when this function completes.
  auto scopeExit = MakeScopeExit([&] { mWorkerRef = nullptr; });

  RefPtr<Event> event;
  if (NS_SUCCEEDED(aResult)) {
    event = CreateGenericEvent(this, nsDependentString(kCompleteEventType),
                               eDoesNotBubble, eNotCancelable);
    MOZ_ASSERT(event);
  } else {
    if (aResult == NS_ERROR_DOM_INDEXEDDB_QUOTA_ERR) {
      mDatabase->SetQuotaExceeded();
    }

    if (!mError && !mAbortedByScript) {
      mError = DOMException::Create(aResult);
    }

    event = CreateGenericEvent(this, nsDependentString(kAbortEventType),
                               eDoesBubble, eNotCancelable);
    MOZ_ASSERT(event);
  }

  if (NS_SUCCEEDED(mAbortCode)) {
    IDB_LOG_MARK(
        "IndexedDB %s: Child  Transaction[%lld]: "
        "Firing 'complete' event",
        "IndexedDB %s: C T[%lld]: IDBTransaction 'complete' event",
        IDB_LOG_ID_STRING(), mLoggingSerialNumber);
  } else {
    IDB_LOG_MARK(
        "IndexedDB %s: Child  Transaction[%lld]: "
        "Firing 'abort' event with error 0x%x",
        "IndexedDB %s: C T[%lld]: IDBTransaction 'abort' event (0x%x)",
        IDB_LOG_ID_STRING(), mLoggingSerialNumber, mAbortCode);
  }

  IgnoredErrorResult rv;
  DispatchEvent(*event, rv);
  if (rv.Failed()) {
    NS_WARNING("DispatchEvent failed!");
  }

  // Normally, we note inactive transaction here instead of
  // IDBTransaction::ClearBackgroundActor() because here is the earliest place
  // to know that it becomes non-blocking to allow the scheduler to start the
  // preemption as soon as it can.
  // Note: If the IDBTransaction object is held by the script,
  // ClearBackgroundActor() will be done in ~IDBTransaction() until garbage
  // collected after its window is closed which prevents us to preempt its
  // window immediately after committed.
  MaybeNoteInactiveTransaction();
}

int64_t IDBTransaction::NextObjectStoreId() {
  AssertIsOnOwningThread();
  MOZ_ASSERT(VERSION_CHANGE == mMode);

  return mNextObjectStoreId++;
}

int64_t IDBTransaction::NextIndexId() {
  AssertIsOnOwningThread();
  MOZ_ASSERT(VERSION_CHANGE == mMode);

  return mNextIndexId++;
}

nsIGlobalObject* IDBTransaction::GetParentObject() const {
  AssertIsOnOwningThread();

  return mDatabase->GetParentObject();
}

IDBTransactionMode IDBTransaction::GetMode(ErrorResult& aRv) const {
  AssertIsOnOwningThread();

  switch (mMode) {
    case READ_ONLY:
      return IDBTransactionMode::Readonly;

    case READ_WRITE:
      return IDBTransactionMode::Readwrite;

    case READ_WRITE_FLUSH:
      return IDBTransactionMode::Readwriteflush;

    case CLEANUP:
      return IDBTransactionMode::Cleanup;

    case VERSION_CHANGE:
      return IDBTransactionMode::Versionchange;

    case MODE_INVALID:
    default:
      MOZ_CRASH("Bad mode!");
  }
}

DOMException* IDBTransaction::GetError() const {
  AssertIsOnOwningThread();

  return mError;
}

already_AddRefed<DOMStringList> IDBTransaction::ObjectStoreNames() const {
  AssertIsOnOwningThread();

  if (mMode == IDBTransaction::VERSION_CHANGE) {
    return mDatabase->ObjectStoreNames();
  }

  RefPtr<DOMStringList> list = new DOMStringList();
  list->StringArray() = mObjectStoreNames;
  return list.forget();
}

already_AddRefed<IDBObjectStore> IDBTransaction::ObjectStore(
    const nsAString& aName, ErrorResult& aRv) {
  AssertIsOnOwningThread();

  if (IsCommittingOrDone()) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }

  const ObjectStoreSpec* spec = nullptr;

  if (IDBTransaction::VERSION_CHANGE == mMode ||
      mObjectStoreNames.Contains(aName)) {
    const nsTArray<ObjectStoreSpec>& objectStores =
        mDatabase->Spec()->objectStores();

    for (uint32_t count = objectStores.Length(), index = 0; index < count;
         index++) {
      const ObjectStoreSpec& objectStore = objectStores[index];
      if (objectStore.metadata().name() == aName) {
        spec = &objectStore;
        break;
      }
    }
  }

  if (!spec) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_FOUND_ERR);
    return nullptr;
  }

  const int64_t desiredId = spec->metadata().id();

  RefPtr<IDBObjectStore> objectStore;

  for (uint32_t count = mObjectStores.Length(), index = 0; index < count;
       index++) {
    RefPtr<IDBObjectStore>& existingObjectStore = mObjectStores[index];

    if (existingObjectStore->Id() == desiredId) {
      objectStore = existingObjectStore;
      break;
    }
  }

  if (!objectStore) {
    objectStore = IDBObjectStore::Create(this, *spec);
    MOZ_ASSERT(objectStore);

    mObjectStores.AppendElement(objectStore);
  }

  return objectStore.forget();
}

NS_IMPL_ADDREF_INHERITED(IDBTransaction, DOMEventTargetHelper)
NS_IMPL_RELEASE_INHERITED(IDBTransaction, DOMEventTargetHelper)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(IDBTransaction)
  NS_INTERFACE_MAP_ENTRY(nsIRunnable)
NS_INTERFACE_MAP_END_INHERITING(DOMEventTargetHelper)

NS_IMPL_CYCLE_COLLECTION_CLASS(IDBTransaction)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(IDBTransaction,
                                                  DOMEventTargetHelper)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mDatabase)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mError)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mObjectStores)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mDeletedObjectStores)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(IDBTransaction,
                                                DOMEventTargetHelper)
  // Don't unlink mDatabase!
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mError)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mObjectStores)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mDeletedObjectStores)
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

JSObject* IDBTransaction::WrapObject(JSContext* aCx,
                                     JS::Handle<JSObject*> aGivenProto) {
  AssertIsOnOwningThread();

  return IDBTransaction_Binding::Wrap(aCx, this, aGivenProto);
}

void IDBTransaction::GetEventTargetParent(EventChainPreVisitor& aVisitor) {
  AssertIsOnOwningThread();

  aVisitor.mCanHandle = true;
  aVisitor.SetParentTarget(mDatabase, false);
}

NS_IMETHODIMP
IDBTransaction::Run() {
  AssertIsOnOwningThread();

  // We're back at the event loop, no longer newborn.
  mCreating = false;

  // Maybe commit if there were no requests generated.
  if (mReadyState == IDBTransaction::INITIAL) {
    mReadyState = DONE;

    SendCommit();
  }

  return NS_OK;
}

}  // namespace dom
}  // namespace mozilla
