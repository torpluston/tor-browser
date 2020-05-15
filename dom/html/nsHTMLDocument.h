/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef nsHTMLDocument_h___
#define nsHTMLDocument_h___

#include "mozilla/Attributes.h"
#include "nsContentList.h"
#include "mozilla/dom/Document.h"
#include "nsIHTMLDocument.h"
#include "nsIHTMLCollection.h"
#include "nsIScriptElement.h"
#include "nsTArray.h"

#include "PLDHashTable.h"
#include "nsIHttpChannel.h"
#include "nsThreadUtils.h"
#include "mozilla/dom/HTMLSharedElement.h"
#include "mozilla/dom/BindingDeclarations.h"

class nsCommandManager;
class nsIURI;
class nsIDocShell;
class nsICachingChannel;
class nsILoadGroup;

namespace mozilla {
namespace dom {
class HTMLAllCollection;
template <typename T>
struct Nullable;
class WindowProxyHolder;
}  // namespace dom
}  // namespace mozilla

class nsHTMLDocument : public mozilla::dom::Document, public nsIHTMLDocument {
 protected:
  typedef mozilla::net::ReferrerPolicy ReferrerPolicy;
  typedef mozilla::dom::Document Document;
  typedef mozilla::Encoding Encoding;
  template <typename T>
  using NotNull = mozilla::NotNull<T>;

 public:
  using Document::GetPlugins;
  using Document::SetDocumentURI;

  nsHTMLDocument();
  virtual nsresult Init() override;

  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(nsHTMLDocument, Document)

  // Document
  virtual void Reset(nsIChannel* aChannel, nsILoadGroup* aLoadGroup) override;
  virtual void ResetToURI(nsIURI* aURI, nsILoadGroup* aLoadGroup,
                          nsIPrincipal* aPrincipal,
                          nsIPrincipal* aStoragePrincipal) override;

  virtual nsresult StartDocumentLoad(const char* aCommand, nsIChannel* aChannel,
                                     nsILoadGroup* aLoadGroup,
                                     nsISupports* aContainer,
                                     nsIStreamListener** aDocListener,
                                     bool aReset = true,
                                     nsIContentSink* aSink = nullptr) override;

  virtual void BeginLoad() override;
  virtual void EndLoad() override;

 protected:
  virtual bool UseWidthDeviceWidthFallbackViewport() const override;

 public:
  // nsIHTMLDocument
  virtual void SetCompatibilityMode(nsCompatibility aMode) override;

  virtual bool IsWriting() override { return mWriteLevel != uint32_t(0); }

  virtual Element* GetUnfocusedKeyEventTarget() override;

  nsContentList* GetExistingForms() const { return mForms; }

  mozilla::dom::HTMLAllCollection* All();

  // Returns whether an object was found for aName.
  bool ResolveName(JSContext* aCx, const nsAString& aName,
                   JS::MutableHandle<JS::Value> aRetval,
                   mozilla::ErrorResult& aError);

  virtual void AddedForm() override;
  virtual void RemovedForm() override;
  virtual int32_t GetNumFormsSynchronous() override;
  virtual void TearingDownEditor() override;
  virtual void SetIsXHTML(bool aXHTML) override {
    mType = (aXHTML ? eXHTML : eHTML);
  }
  virtual void SetDocWriteDisabled(bool aDisabled) override {
    mDisableDocWrite = aDisabled;
  }

  nsresult ChangeContentEditableCount(nsIContent* aElement,
                                      int32_t aChange) override;
  void DeferredContentEditableCountChange(nsIContent* aElement);

  virtual EditingState GetEditingState() override { return mEditingState; }

  class nsAutoEditingState {
   public:
    nsAutoEditingState(nsHTMLDocument* aDoc, EditingState aState)
        : mDoc(aDoc), mSavedState(aDoc->mEditingState) {
      aDoc->mEditingState = aState;
    }
    ~nsAutoEditingState() { mDoc->mEditingState = mSavedState; }

   private:
    nsHTMLDocument* mDoc;
    EditingState mSavedState;
  };
  friend class nsAutoEditingState;

  void EndUpdate() override;

  virtual void SetMayStartLayout(bool aMayStartLayout) override;

  virtual nsresult SetEditingState(EditingState aState) override;

  virtual nsresult Clone(mozilla::dom::NodeInfo*,
                         nsINode** aResult) const override;

  virtual void RemovedFromDocShell() override;
  using mozilla::dom::DocumentOrShadowRoot::GetElementById;

  virtual void DocAddSizeOfExcludingThis(
      nsWindowSizes& aWindowSizes) const override;
  // DocAddSizeOfIncludingThis is inherited from Document.

  virtual bool WillIgnoreCharsetOverride() override;

  // WebIDL API
  virtual JSObject* WrapNode(JSContext* aCx,
                             JS::Handle<JSObject*> aGivenProto) override;
  void GetDomain(nsAString& aDomain);
  void SetDomain(const nsAString& aDomain, mozilla::ErrorResult& rv);
  bool IsRegistrableDomainSuffixOfOrEqualTo(const nsAString& aHostSuffixString,
                                            const nsACString& aOrigHost);
  void NamedGetter(JSContext* cx, const nsAString& aName, bool& aFound,
                   JS::MutableHandle<JSObject*> aRetval,
                   mozilla::ErrorResult& rv) {
    JS::Rooted<JS::Value> v(cx);
    if ((aFound = ResolveName(cx, aName, &v, rv))) {
      aRetval.set(v.toObjectOrNull());
    }
  }
  void GetSupportedNames(nsTArray<nsString>& aNames);
  Document* Open(const mozilla::dom::Optional<nsAString>& /* unused */,
                 const nsAString& /* unused */, mozilla::ErrorResult& aError);
  mozilla::dom::Nullable<mozilla::dom::WindowProxyHolder> Open(
      const nsAString& aURL, const nsAString& aName, const nsAString& aFeatures,
      bool aReplace, mozilla::ErrorResult& rv);
  void Close(mozilla::ErrorResult& rv);
  void Write(const mozilla::dom::Sequence<nsString>& aText,
             mozilla::ErrorResult& rv);
  void Writeln(const mozilla::dom::Sequence<nsString>& aText,
               mozilla::ErrorResult& rv);
  void GetDesignMode(nsAString& aDesignMode);
  void SetDesignMode(const nsAString& aDesignMode,
                     nsIPrincipal& aSubjectPrincipal, mozilla::ErrorResult& rv);
  void SetDesignMode(const nsAString& aDesignMode,
                     const mozilla::Maybe<nsIPrincipal*>& aSubjectPrincipal,
                     mozilla::ErrorResult& rv);
  MOZ_CAN_RUN_SCRIPT
  bool ExecCommand(const nsAString& aCommandID, bool aDoShowUI,
                   const nsAString& aValue, nsIPrincipal& aSubjectPrincipal,
                   mozilla::ErrorResult& rv);
  bool QueryCommandEnabled(const nsAString& aCommandID,
                           nsIPrincipal& aSubjectPrincipal,
                           mozilla::ErrorResult& rv);
  bool QueryCommandIndeterm(const nsAString& aCommandID,
                            mozilla::ErrorResult& rv);
  bool QueryCommandState(const nsAString& aCommandID, mozilla::ErrorResult& rv);
  bool QueryCommandSupported(const nsAString& aCommandID,
                             mozilla::dom::CallerType aCallerType);
  MOZ_CAN_RUN_SCRIPT
  void QueryCommandValue(const nsAString& aCommandID, nsAString& aValue,
                         mozilla::ErrorResult& rv);
  void GetFgColor(nsAString& aFgColor);
  void SetFgColor(const nsAString& aFgColor);
  void GetLinkColor(nsAString& aLinkColor);
  void SetLinkColor(const nsAString& aLinkColor);
  void GetVlinkColor(nsAString& aAvlinkColor);
  void SetVlinkColor(const nsAString& aVlinkColor);
  void GetAlinkColor(nsAString& aAlinkColor);
  void SetAlinkColor(const nsAString& aAlinkColor);
  void GetBgColor(nsAString& aBgColor);
  void SetBgColor(const nsAString& aBgColor);
  void Clear() const {
    // Deprecated
  }
  void CaptureEvents();
  void ReleaseEvents();
  // We're picking up GetLocation from Document
  already_AddRefed<mozilla::dom::Location> GetLocation() const {
    return Document::GetLocation();
  }

  static bool MatchFormControls(Element* aElement, int32_t aNamespaceID,
                                nsAtom* aAtom, void* aData);

  void GetFormsAndFormControls(nsContentList** aFormList,
                               nsContentList** aFormControlList);

  void UserInteractionForTesting();

  void SetKeyPressEventModel(uint16_t aKeyPressEventModel);

 protected:
  ~nsHTMLDocument();

  nsresult GetBodySize(int32_t* aWidth, int32_t* aHeight);

  nsIContent* MatchId(nsIContent* aContent, const nsAString& aId);

  static void DocumentWriteTerminationFunc(nsISupports* aRef);

  already_AddRefed<nsIURI> GetDomainURI();
  already_AddRefed<nsIURI> CreateInheritingURIForHost(
      const nsACString& aHostString);
  already_AddRefed<nsIURI> RegistrableDomainSuffixOfInternal(
      const nsAString& aHostSuffixString, nsIURI* aOrigHost);

  void WriteCommon(const nsAString& aText, bool aNewlineTerminate,
                   mozilla::ErrorResult& aRv);
  // A version of WriteCommon used by WebIDL bindings
  void WriteCommon(const mozilla::dom::Sequence<nsString>& aText,
                   bool aNewlineTerminate, mozilla::ErrorResult& rv);

  /**
   * Like IsEditingOn(), but will flush as needed first.
   */
  bool IsEditingOnAfterFlush();

  void* GenerateParserKey(void);

  // A helper class to keep nsContentList objects alive for a short period of
  // time. Note, when the final Release is called on an nsContentList object, it
  // removes itself from MutationObserver list.
  class ContentListHolder : public mozilla::Runnable {
   public:
    ContentListHolder(nsHTMLDocument* aDocument, nsContentList* aFormList,
                      nsContentList* aFormControlList)
        : mozilla::Runnable("ContentListHolder"),
          mDocument(aDocument),
          mFormList(aFormList),
          mFormControlList(aFormControlList) {}

    ~ContentListHolder() {
      MOZ_ASSERT(!mDocument->mContentListHolder ||
                 mDocument->mContentListHolder == this);
      mDocument->mContentListHolder = nullptr;
    }

    RefPtr<nsHTMLDocument> mDocument;
    RefPtr<nsContentList> mFormList;
    RefPtr<nsContentList> mFormControlList;
  };

  friend class ContentListHolder;
  ContentListHolder* mContentListHolder;

  RefPtr<mozilla::dom::HTMLAllCollection> mAll;

  /** # of forms in the document, synchronously set */
  int32_t mNumForms;

  static void TryHintCharset(nsIContentViewer* aContentViewer,
                             int32_t& aCharsetSource,
                             NotNull<const Encoding*>& aEncoding);
  void TryUserForcedCharset(nsIContentViewer* aCv, nsIDocShell* aDocShell,
                            int32_t& aCharsetSource,
                            NotNull<const Encoding*>& aEncoding);
  static void TryCacheCharset(nsICachingChannel* aCachingChannel,
                              int32_t& aCharsetSource,
                              NotNull<const Encoding*>& aEncoding);
  void TryParentCharset(nsIDocShell* aDocShell, int32_t& charsetSource,
                        NotNull<const Encoding*>& aEncoding);
  void TryTLD(int32_t& aCharsetSource, NotNull<const Encoding*>& aCharset);
  static void TryFallback(int32_t& aCharsetSource,
                          NotNull<const Encoding*>& aEncoding);

  /**
   * MaybeDispatchCheckKeyPressEventModelEvent() dispatches
   * "CheckKeyPressEventModel" event to check whether we should dispatch
   * keypress events in confluent model or split model.  This should be
   * called only when mEditingState is changed to eDesignMode or
   * eConentEditable at first time.
   */
  void MaybeDispatchCheckKeyPressEventModelEvent();

  // Tracks if we are currently processing any document.write calls (either
  // implicit or explicit). Note that if a write call writes out something which
  // would block the parser, then mWriteLevel will be incorrect until the parser
  // finishes processing that script.
  uint32_t mWriteLevel;

  // Load flags of the document's channel
  uint32_t mLoadFlags;

  bool mTooDeepWriteRecursion;

  bool mDisableDocWrite;

  bool mWarnedWidthHeight;

  /* Midas implementation */
  nsCommandManager* GetMidasCommandManager();

  RefPtr<nsCommandManager> mMidasCommandManager;

  nsresult TurnEditingOff();
  // MOZ_CAN_RUN_SCRIPT_BOUNDARY because this is called from all sorts
  // of places, and I'm pretty sure the exact ExecCommand call it
  // makes cannot actually run script.
  MOZ_CAN_RUN_SCRIPT_BOUNDARY nsresult EditingStateChanged();
  void MaybeEditingStateChanged();

  uint32_t mContentEditableCount;
  EditingState mEditingState;

  /**
   * Temporary flag that is set in EndUpdate() to ignore
   * MaybeEditingStateChanged() script runners from a nested scope.
   */
  bool mPendingMaybeEditingStateChanged;

  // mHasBeenEditable is set to true when mEditingState is firstly set to
  // eDesignMode or eContentEditable.
  bool mHasBeenEditable;

  /**
   * Set to true once we know that we are loading plain text content.
   */
  bool mIsPlainText;
};

namespace mozilla {
namespace dom {

inline nsHTMLDocument* Document::AsHTMLDocument() {
  MOZ_ASSERT(IsHTMLOrXHTML());
  return static_cast<nsHTMLDocument*>(this);
}

}  // namespace dom
}  // namespace mozilla

#define NS_HTML_DOCUMENT_INTERFACE_TABLE_BEGIN(_class) \
  NS_DOCUMENT_INTERFACE_TABLE_BEGIN(_class)            \
  NS_INTERFACE_TABLE_ENTRY(_class, nsIHTMLDocument)

#endif /* nsHTMLDocument_h___ */
