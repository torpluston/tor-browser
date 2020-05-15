/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "APZCTreeManagerTester.h"
#include "APZTestCommon.h"
#include "InputUtils.h"

TEST_F(APZCTreeManagerTester, ScrollablePaintedLayers) {
  CreateSimpleMultiLayerTree();
  ScopedLayerTreeRegistration registration(manager, LayersId{0}, root, mcc);

  // both layers have the same scrollId
  SetScrollableFrameMetrics(layers[1], ScrollableLayerGuid::START_SCROLL_ID);
  SetScrollableFrameMetrics(layers[2], ScrollableLayerGuid::START_SCROLL_ID);
  manager->UpdateHitTestingTree(LayersId{0}, root, false, LayersId{0}, 0);

  TestAsyncPanZoomController* nullAPZC = nullptr;
  // so they should have the same APZC
  EXPECT_FALSE(layers[0]->HasScrollableFrameMetrics());
  EXPECT_NE(nullAPZC, ApzcOf(layers[1]));
  EXPECT_NE(nullAPZC, ApzcOf(layers[2]));
  EXPECT_EQ(ApzcOf(layers[1]), ApzcOf(layers[2]));

  // Change the scrollId of layers[1], and verify the APZC changes
  SetScrollableFrameMetrics(layers[1],
                            ScrollableLayerGuid::START_SCROLL_ID + 1);
  manager->UpdateHitTestingTree(LayersId{0}, root, false, LayersId{0}, 0);
  EXPECT_NE(ApzcOf(layers[1]), ApzcOf(layers[2]));

  // Change the scrollId of layers[2] to match that of layers[1], ensure we get
  // the same APZC for both again
  SetScrollableFrameMetrics(layers[2],
                            ScrollableLayerGuid::START_SCROLL_ID + 1);
  manager->UpdateHitTestingTree(LayersId{0}, root, false, LayersId{0}, 0);
  EXPECT_EQ(ApzcOf(layers[1]), ApzcOf(layers[2]));
}

TEST_F(APZCTreeManagerTester, Bug1068268) {
  CreatePotentiallyLeakingTree();
  ScopedLayerTreeRegistration registration(manager, LayersId{0}, root, mcc);

  manager->UpdateHitTestingTree(LayersId{0}, root, false, LayersId{0}, 0);
  RefPtr<HitTestingTreeNode> root = manager->GetRootNode();
  RefPtr<HitTestingTreeNode> node2 = root->GetFirstChild()->GetFirstChild();
  RefPtr<HitTestingTreeNode> node5 = root->GetLastChild()->GetLastChild();

  EXPECT_EQ(ApzcOf(layers[2]), node5->GetApzc());
  EXPECT_EQ(ApzcOf(layers[2]), node2->GetApzc());
  EXPECT_EQ(ApzcOf(layers[0]), ApzcOf(layers[2])->GetParent());
  EXPECT_EQ(ApzcOf(layers[2]), ApzcOf(layers[5]));

  EXPECT_EQ(node2->GetFirstChild(), node2->GetLastChild());
  EXPECT_EQ(ApzcOf(layers[3]), node2->GetLastChild()->GetApzc());
  EXPECT_EQ(node5->GetFirstChild(), node5->GetLastChild());
  EXPECT_EQ(ApzcOf(layers[6]), node5->GetLastChild()->GetApzc());
  EXPECT_EQ(ApzcOf(layers[2]), ApzcOf(layers[3])->GetParent());
  EXPECT_EQ(ApzcOf(layers[5]), ApzcOf(layers[6])->GetParent());
}

TEST_F(APZCTreeManagerTester, Bug1194876) {
  CreateBug1194876Tree();
  ScopedLayerTreeRegistration registration(manager, LayersId{0}, root, mcc);
  manager->UpdateHitTestingTree(LayersId{0}, root, false, LayersId{0}, 0);

  uint64_t blockId;
  nsTArray<ScrollableLayerGuid> targets;

  // First touch goes down, APZCTM will hit layers[1] because it is on top of
  // layers[0], but we tell it the real target APZC is layers[0].
  MultiTouchInput mti;
  mti = CreateMultiTouchInput(MultiTouchInput::MULTITOUCH_START, mcc->Time());
  mti.mTouches.AppendElement(
      SingleTouchData(0, ParentLayerPoint(25, 50), ScreenSize(0, 0), 0, 0));
  manager->ReceiveInputEvent(mti, nullptr, &blockId);
  manager->ContentReceivedInputBlock(blockId, false);
  targets.AppendElement(ApzcOf(layers[0])->GetGuid());
  manager->SetTargetAPZC(blockId, targets);

  // Around here, the above touch will get processed by ApzcOf(layers[0])

  // Second touch goes down (first touch remains down), APZCTM will again hit
  // layers[1]. Again we tell it both touches landed on layers[0], but because
  // layers[1] is the RCD layer, it will end up being the multitouch target.
  mti.mTouches.AppendElement(
      SingleTouchData(1, ParentLayerPoint(75, 50), ScreenSize(0, 0), 0, 0));
  manager->ReceiveInputEvent(mti, nullptr, &blockId);
  manager->ContentReceivedInputBlock(blockId, false);
  targets.AppendElement(ApzcOf(layers[0])->GetGuid());
  manager->SetTargetAPZC(blockId, targets);

  // Around here, the above multi-touch will get processed by ApzcOf(layers[1]).
  // We want to ensure that ApzcOf(layers[0]) has had its state cleared, because
  // otherwise it will do things like dispatch spurious long-tap events.

  EXPECT_CALL(*mcc, HandleTap(TapType::eLongTap, _, _, _, _)).Times(0);
}

TEST_F(APZCTreeManagerTester, Bug1198900) {
  // This is just a test that cancels a wheel event to make sure it doesn't
  // crash.
  CreateSimpleDTCScrollingLayer();
  ScopedLayerTreeRegistration registration(manager, LayersId{0}, root, mcc);
  manager->UpdateHitTestingTree(LayersId{0}, root, false, LayersId{0}, 0);

  ScreenPoint origin(100, 50);
  ScrollWheelInput swi(MillisecondsSinceStartup(mcc->Time()), mcc->Time(), 0,
                       ScrollWheelInput::SCROLLMODE_INSTANT,
                       ScrollWheelInput::SCROLLDELTA_PIXEL, origin, 0, 10,
                       false, WheelDeltaAdjustmentStrategy::eNone);
  uint64_t blockId;
  manager->ReceiveInputEvent(swi, nullptr, &blockId);
  manager->ContentReceivedInputBlock(blockId, /* preventDefault= */ true);
}

// The next two tests check that APZ clamps the scroll offset it composites even
// if the main thread fails to do so. (The main thread will always clamp its
// scroll offset internally, but it may not send APZ the clamped version for
// scroll offset synchronization reasons.)
TEST_F(APZCTreeManagerTester, Bug1551582) {
  // The simple layer tree has a scrollable rect of 500x500 and a composition
  // bounds of 200x200, leading to a scroll range of (0,0,300,300).
  CreateSimpleScrollingLayer();
  ScopedLayerTreeRegistration registration(manager, LayersId{0}, root, mcc);
  manager->UpdateHitTestingTree(LayersId{0}, root, false, LayersId{0}, 0);

  // Simulate the main thread scrolling to the end of the scroll range.
  ModifyFrameMetrics(root, [](FrameMetrics& aMetrics) {
    aMetrics.SetScrollOffset(CSSPoint(300, 300));
    aMetrics.SetScrollGeneration(1);
    aMetrics.SetScrollOffsetUpdateType(FrameMetrics::eMainThread);
  });
  manager->UpdateHitTestingTree(LayersId{0}, root, false, LayersId{0}, 0);

  // Sanity check.
  RefPtr<TestAsyncPanZoomController> apzc = ApzcOf(root);
  CSSPoint compositedScrollOffset = apzc->GetCompositedScrollOffset();
  EXPECT_EQ(CSSPoint(300, 300), compositedScrollOffset);

  // Simulate the main thread shrinking the scrollable rect to 400x400 (and
  // thereby the scroll range to (0,0,200,200) without sending a new scroll
  // offset update for the clamped scroll position (200,200).
  ModifyFrameMetrics(root, [](FrameMetrics& aMetrics) {
    aMetrics.SetScrollableRect(CSSRect(0, 0, 400, 400));
  });
  manager->UpdateHitTestingTree(LayersId{0}, root, false, LayersId{0}, 0);

  // Check that APZ has clamped the scroll offset to (200,200) for us.
  compositedScrollOffset = apzc->GetCompositedScrollOffset();
  EXPECT_EQ(CSSPoint(200, 200), compositedScrollOffset);
}
TEST_F(APZCTreeManagerTester, Bug1557424) {
  // The simple layer tree has a scrollable rect of 500x500 and a composition
  // bounds of 200x200, leading to a scroll range of (0,0,300,300).
  CreateSimpleScrollingLayer();
  ScopedLayerTreeRegistration registration(manager, LayersId{0}, root, mcc);
  manager->UpdateHitTestingTree(LayersId{0}, root, false, LayersId{0}, 0);

  // Simulate the main thread scrolling to the end of the scroll range.
  ModifyFrameMetrics(root, [](FrameMetrics& aMetrics) {
    aMetrics.SetScrollOffset(CSSPoint(300, 300));
    aMetrics.SetScrollGeneration(1);
    aMetrics.SetScrollOffsetUpdateType(FrameMetrics::eMainThread);
  });
  manager->UpdateHitTestingTree(LayersId{0}, root, false, LayersId{0}, 0);

  // Sanity check.
  RefPtr<TestAsyncPanZoomController> apzc = ApzcOf(root);
  CSSPoint compositedScrollOffset = apzc->GetCompositedScrollOffset();
  EXPECT_EQ(CSSPoint(300, 300), compositedScrollOffset);

  // Simulate the main thread expanding the composition bounds to 300x300 (and
  // thereby shrinking the scroll range to (0,0,200,200) without sending a new
  // scroll offset update for the clamped scroll position (200,200).
  ModifyFrameMetrics(root, [](FrameMetrics& aMetrics) {
    aMetrics.SetCompositionBounds(ParentLayerRect(0, 0, 300, 300));
  });
  manager->UpdateHitTestingTree(LayersId{0}, root, false, LayersId{0}, 0);

  // Check that APZ has clamped the scroll offset to (200,200) for us.
  compositedScrollOffset = apzc->GetCompositedScrollOffset();
  EXPECT_EQ(CSSPoint(200, 200), compositedScrollOffset);
}
