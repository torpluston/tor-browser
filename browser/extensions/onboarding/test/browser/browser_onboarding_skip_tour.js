/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

 "use strict";

add_task(async function test_skip_onboarding_tours() {
  resetOnboardingDefaultState();

  let tourIds = TOUR_IDs;
  let expectedPrefUpdates = [
    promisePrefUpdated("browser.onboarding.notification.finished", true),
    promisePrefUpdated("browser.onboarding.state", ICON_STATE_WATERMARK),
  ];
  tourIds.forEach((id, idx) => expectedPrefUpdates.push(promisePrefUpdated(`browser.onboarding.tour.${id}.completed`, true)));

  let tab = await openTab(ABOUT_NEWTAB_URL);
  await promiseOnboardingOverlayLoaded(tab.linkedBrowser);
  await BrowserTestUtils.synthesizeMouseAtCenter("#onboarding-overlay-button", {}, tab.linkedBrowser);
  await promiseOnboardingOverlayOpened(tab.linkedBrowser);

  let overlayClosedPromise = promiseOnboardingOverlayClosed(tab.linkedBrowser);
  await BrowserTestUtils.synthesizeMouseAtCenter("#onboarding-skip-tour-button", {}, tab.linkedBrowser);
  await overlayClosedPromise;
  await Promise.all(expectedPrefUpdates);
  await assertWatermarkIconDisplayed(tab.linkedBrowser);

  BrowserTestUtils.removeTab(tab);
});

add_task(async function test_hide_skip_button_via_perf() {
  resetOnboardingDefaultState();
  Preferences.set("browser.onboarding.skip-tour-button.hide", true);

  let tab = await openTab(ABOUT_NEWTAB_URL);
  let browser = tab.linkedBrowser;
  await promiseOnboardingOverlayLoaded(browser);
  await BrowserTestUtils.synthesizeMouseAtCenter("#onboarding-overlay-button", {}, browser);
  await promiseOnboardingOverlayOpened(browser);

  let hasTourButton = await ContentTask.spawn(browser, null, () => {
    return content.document.querySelector("#onboarding-skip-tour-button") != null;
  });

  ok(!hasTourButton, "should not render the skip button");

  BrowserTestUtils.removeTab(tab);
});
