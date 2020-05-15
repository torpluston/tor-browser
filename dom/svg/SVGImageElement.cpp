/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/SVGImageElement.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/EventStateManager.h"
#include "mozilla/EventStates.h"
#include "mozilla/gfx/2D.h"
#include "nsCOMPtr.h"
#include "nsIURI.h"
#include "nsNetUtil.h"
#include "imgINotificationObserver.h"
#include "mozilla/dom/SVGImageElementBinding.h"
#include "mozilla/dom/SVGLengthBinding.h"
#include "nsContentUtils.h"

NS_IMPL_NS_NEW_SVG_ELEMENT(Image)

using namespace mozilla::gfx;

namespace mozilla {
namespace dom {

JSObject* SVGImageElement::WrapNode(JSContext* aCx,
                                    JS::Handle<JSObject*> aGivenProto) {
  return SVGImageElement_Binding::Wrap(aCx, this, aGivenProto);
}

SVGElement::LengthInfo SVGImageElement::sLengthInfo[4] = {
    {nsGkAtoms::x, 0, SVGLength_Binding::SVG_LENGTHTYPE_NUMBER,
     SVGContentUtils::X},
    {nsGkAtoms::y, 0, SVGLength_Binding::SVG_LENGTHTYPE_NUMBER,
     SVGContentUtils::Y},
    {nsGkAtoms::width, 0, SVGLength_Binding::SVG_LENGTHTYPE_NUMBER,
     SVGContentUtils::X},
    {nsGkAtoms::height, 0, SVGLength_Binding::SVG_LENGTHTYPE_NUMBER,
     SVGContentUtils::Y},
};

SVGElement::StringInfo SVGImageElement::sStringInfo[2] = {
    {nsGkAtoms::href, kNameSpaceID_None, true},
    {nsGkAtoms::href, kNameSpaceID_XLink, true}};

//----------------------------------------------------------------------
// nsISupports methods

NS_IMPL_ISUPPORTS_INHERITED(SVGImageElement, SVGImageElementBase,
                            imgINotificationObserver, nsIImageLoadingContent)

//----------------------------------------------------------------------
// Implementation

SVGImageElement::SVGImageElement(
    already_AddRefed<mozilla::dom::NodeInfo>&& aNodeInfo)
    : SVGImageElementBase(std::move(aNodeInfo)) {
  // We start out broken
  AddStatesSilently(NS_EVENT_STATE_BROKEN);
}

SVGImageElement::~SVGImageElement() { DestroyImageLoadingContent(); }

//----------------------------------------------------------------------
// nsINode methods

NS_IMPL_ELEMENT_CLONE_WITH_INIT(SVGImageElement)

//----------------------------------------------------------------------

already_AddRefed<DOMSVGAnimatedLength> SVGImageElement::X() {
  return mLengthAttributes[ATTR_X].ToDOMAnimatedLength(this);
}

already_AddRefed<DOMSVGAnimatedLength> SVGImageElement::Y() {
  return mLengthAttributes[ATTR_Y].ToDOMAnimatedLength(this);
}

already_AddRefed<DOMSVGAnimatedLength> SVGImageElement::Width() {
  return mLengthAttributes[ATTR_WIDTH].ToDOMAnimatedLength(this);
}

already_AddRefed<DOMSVGAnimatedLength> SVGImageElement::Height() {
  return mLengthAttributes[ATTR_HEIGHT].ToDOMAnimatedLength(this);
}

already_AddRefed<DOMSVGAnimatedPreserveAspectRatio>
SVGImageElement::PreserveAspectRatio() {
  return mPreserveAspectRatio.ToDOMAnimatedPreserveAspectRatio(this);
}

already_AddRefed<DOMSVGAnimatedString> SVGImageElement::Href() {
  return mStringAttributes[HREF].IsExplicitlySet()
             ? mStringAttributes[HREF].ToDOMAnimatedString(this)
             : mStringAttributes[XLINK_HREF].ToDOMAnimatedString(this);
}

void SVGImageElement::GetDecoding(nsAString& aValue) {
  GetEnumAttr(nsGkAtoms::decoding, kDecodingTableDefault->tag, aValue);
}

already_AddRefed<Promise> SVGImageElement::Decode(ErrorResult& aRv) {
  return nsImageLoadingContent::QueueDecodeAsync(aRv);
}

//----------------------------------------------------------------------

nsresult SVGImageElement::LoadSVGImage(bool aForce, bool aNotify) {
  // resolve href attribute
  nsCOMPtr<nsIURI> baseURI = GetBaseURI();

  nsAutoString href;
  if (mStringAttributes[HREF].IsExplicitlySet()) {
    mStringAttributes[HREF].GetAnimValue(href, this);
  } else {
    mStringAttributes[XLINK_HREF].GetAnimValue(href, this);
  }
  href.Trim(" \t\n\r");

  if (baseURI && !href.IsEmpty()) NS_MakeAbsoluteURI(href, href, baseURI);

  // Mark channel as urgent-start before load image if the image load is
  // initaiated by a user interaction.
  mUseUrgentStartForChannel = EventStateManager::IsHandlingUserInput();

  return LoadImage(href, aForce, aNotify, eImageLoadType_Normal);
}

//----------------------------------------------------------------------
// EventTarget methods:

void SVGImageElement::AsyncEventRunning(AsyncEventDispatcher* aEvent) {
  nsImageLoadingContent::AsyncEventRunning(aEvent);
}

//----------------------------------------------------------------------
// nsIContent methods:

bool SVGImageElement::ParseAttribute(int32_t aNamespaceID, nsAtom* aAttribute,
                                     const nsAString& aValue,
                                     nsIPrincipal* aMaybeScriptedPrincipal,
                                     nsAttrValue& aResult) {
  if (aNamespaceID == kNameSpaceID_None) {
    if (aAttribute == nsGkAtoms::decoding) {
      return aResult.ParseEnumValue(aValue, kDecodingTable, false,
                                    kDecodingTableDefault);
    }
  }

  return SVGImageElementBase::ParseAttribute(aNamespaceID, aAttribute, aValue,
                                             aMaybeScriptedPrincipal, aResult);
}

nsresult SVGImageElement::AfterSetAttr(int32_t aNamespaceID, nsAtom* aName,
                                       const nsAttrValue* aValue,
                                       const nsAttrValue* aOldValue,
                                       nsIPrincipal* aSubjectPrincipal,
                                       bool aNotify) {
  if (aName == nsGkAtoms::href && (aNamespaceID == kNameSpaceID_None ||
                                   aNamespaceID == kNameSpaceID_XLink)) {
    if (aValue) {
      LoadSVGImage(true, aNotify);
    } else {
      CancelImageRequests(aNotify);
    }
  } else if (aName == nsGkAtoms::decoding &&
             aNamespaceID == kNameSpaceID_None) {
    // Request sync or async image decoding.
    SetSyncDecodingHint(
        aValue && static_cast<ImageDecodingType>(aValue->GetEnumValue()) ==
                      ImageDecodingType::Sync);
  }
  return SVGImageElementBase::AfterSetAttr(
      aNamespaceID, aName, aValue, aOldValue, aSubjectPrincipal, aNotify);
}

void SVGImageElement::MaybeLoadSVGImage() {
  if ((mStringAttributes[HREF].IsExplicitlySet() ||
       mStringAttributes[XLINK_HREF].IsExplicitlySet()) &&
      (NS_FAILED(LoadSVGImage(false, true)) || !LoadingEnabled())) {
    CancelImageRequests(true);
  }
}

nsresult SVGImageElement::BindToTree(Document* aDocument, nsIContent* aParent,
                                     nsIContent* aBindingParent) {
  nsresult rv =
      SVGImageElementBase::BindToTree(aDocument, aParent, aBindingParent);
  NS_ENSURE_SUCCESS(rv, rv);

  nsImageLoadingContent::BindToTree(aDocument, aParent, aBindingParent);

  if (mStringAttributes[HREF].IsExplicitlySet() ||
      mStringAttributes[XLINK_HREF].IsExplicitlySet()) {
    nsContentUtils::AddScriptRunner(
        NewRunnableMethod("dom::SVGImageElement::MaybeLoadSVGImage", this,
                          &SVGImageElement::MaybeLoadSVGImage));
  }

  return rv;
}

void SVGImageElement::UnbindFromTree(bool aDeep, bool aNullParent) {
  nsImageLoadingContent::UnbindFromTree(aDeep, aNullParent);
  SVGImageElementBase::UnbindFromTree(aDeep, aNullParent);
}

EventStates SVGImageElement::IntrinsicState() const {
  return SVGImageElementBase::IntrinsicState() |
         nsImageLoadingContent::ImageState();
}

NS_IMETHODIMP_(bool)
SVGImageElement::IsAttributeMapped(const nsAtom* name) const {
  static const MappedAttributeEntry* const map[] = {
      sViewportsMap,
  };

  return FindAttributeDependence(name, map) ||
         SVGImageElementBase::IsAttributeMapped(name);
}

//----------------------------------------------------------------------
// SVGGeometryElement methods

/* For the purposes of the update/invalidation logic pretend to
   be a rectangle. */
bool SVGImageElement::GetGeometryBounds(
    Rect* aBounds, const StrokeOptions& aStrokeOptions,
    const Matrix& aToBoundsSpace, const Matrix* aToNonScalingStrokeSpace) {
  Rect rect;
  GetAnimatedLengthValues(&rect.x, &rect.y, &rect.width, &rect.height, nullptr);

  if (rect.IsEmpty()) {
    // Rendering of the element disabled
    rect.SetEmpty();  // Make sure width/height are zero and not negative
  }

  *aBounds = aToBoundsSpace.TransformBounds(rect);
  return true;
}

already_AddRefed<Path> SVGImageElement::BuildPath(PathBuilder* aBuilder) {
  // We get called in order to get bounds for this element, and for
  // hit-testing against it. For that we just pretend to be a rectangle.

  float x, y, width, height;
  GetAnimatedLengthValues(&x, &y, &width, &height, nullptr);

  if (width <= 0 || height <= 0) {
    return nullptr;
  }

  Rect r(x, y, width, height);
  aBuilder->MoveTo(r.TopLeft());
  aBuilder->LineTo(r.TopRight());
  aBuilder->LineTo(r.BottomRight());
  aBuilder->LineTo(r.BottomLeft());
  aBuilder->Close();

  return aBuilder->Finish();
}

//----------------------------------------------------------------------
// SVGElement methods

/* virtual */
bool SVGImageElement::HasValidDimensions() const {
  return mLengthAttributes[ATTR_WIDTH].IsExplicitlySet() &&
         mLengthAttributes[ATTR_WIDTH].GetAnimValInSpecifiedUnits() > 0 &&
         mLengthAttributes[ATTR_HEIGHT].IsExplicitlySet() &&
         mLengthAttributes[ATTR_HEIGHT].GetAnimValInSpecifiedUnits() > 0;
}

SVGElement::LengthAttributesInfo SVGImageElement::GetLengthInfo() {
  return LengthAttributesInfo(mLengthAttributes, sLengthInfo,
                              ArrayLength(sLengthInfo));
}

SVGAnimatedPreserveAspectRatio*
SVGImageElement::GetAnimatedPreserveAspectRatio() {
  return &mPreserveAspectRatio;
}

SVGElement::StringAttributesInfo SVGImageElement::GetStringInfo() {
  return StringAttributesInfo(mStringAttributes, sStringInfo,
                              ArrayLength(sStringInfo));
}

nsresult SVGImageElement::CopyInnerTo(Element* aDest) {
  if (aDest->OwnerDoc()->IsStaticDocument()) {
    CreateStaticImageClone(static_cast<SVGImageElement*>(aDest));
  }
  return SVGImageElementBase::CopyInnerTo(aDest);
}

}  // namespace dom
}  // namespace mozilla
