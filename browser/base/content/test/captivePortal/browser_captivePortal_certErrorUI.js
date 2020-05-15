/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

const BAD_CERT_PAGE = "https://expired.example.com/";

async function setupCaptivePortalTab() {
  let captivePortalStatePropagated = TestUtils.topicObserved(
    "ipc:network:captive-portal-set-state"
  );
  Services.obs.notifyObservers(null, "captive-portal-login");
  info(
    "Waiting for captive portal state to be propagated to the content process."
  );
  await captivePortalStatePropagated;

  // Open a page with a cert error.
  let browser;
  let certErrorLoaded;
  let errorTab = await BrowserTestUtils.openNewForegroundTab(
    gBrowser,
    () => {
      let tab = BrowserTestUtils.addTab(gBrowser, BAD_CERT_PAGE);
      gBrowser.selectedTab = tab;
      browser = gBrowser.selectedBrowser;
      certErrorLoaded = BrowserTestUtils.waitForErrorPage(browser);
      return tab;
    },
    false
  );
  info("Waiting for cert error page to load");
  await certErrorLoaded;
  return errorTab;
}

add_task(async function setup() {
  await SpecialPowers.pushPrefEnv({
    set: [
      ["captivedetect.canonicalURL", CANONICAL_URL],
      ["captivedetect.canonicalContent", CANONICAL_CONTENT],
    ],
  });
});

// This tests the alternate cert error UI when we are behind a captive portal.
add_task(async function checkCaptivePortalCertErrorUI() {
  info(
    "Checking that the alternate cert error UI is shown when we are behind a captive portal"
  );

  let tab = await setupCaptivePortalTab();
  let browser = tab.linkedBrowser;
  let portalTabPromise = BrowserTestUtils.waitForNewTab(
    gBrowser,
    CANONICAL_URL
  );

  await ContentTask.spawn(browser, null, async () => {
    const EventUtils = ContentTaskUtils.getEventUtils(content);
    let doc = content.document;
    let loginButton = doc.getElementById("openPortalLoginPageButton");
    await ContentTaskUtils.waitForCondition(
      () => ContentTaskUtils.is_visible(loginButton),
      "Captive portal error page UI is visible"
    );

    is(
      loginButton.getAttribute("autofocus"),
      "true",
      "openPortalLoginPageButton has autofocus"
    );
    info("Clicking the Open Login Page button");
    await EventUtils.synthesizeMouseAtCenter(loginButton, {}, content);
  });

  let portalTab = await portalTabPromise;
  is(
    gBrowser.selectedTab,
    portalTab,
    "Login page should be open in a new foreground tab."
  );

  // Make sure clicking the "Open Login Page" button again focuses the existing portal tab.
  await BrowserTestUtils.switchTab(gBrowser, tab);
  // Passing an empty function to BrowserTestUtils.switchTab lets us wait for an arbitrary
  // tab switch.
  portalTabPromise = BrowserTestUtils.switchTab(gBrowser, () => {});
  await ContentTask.spawn(browser, null, async () => {
    const EventUtils = ContentTaskUtils.getEventUtils(content);
    info("Clicking the Open Login Page button.");
    let loginButton = content.document.getElementById(
      "openPortalLoginPageButton"
    );
    await EventUtils.synthesizeMouseAtCenter(loginButton, {}, content);
  });

  info("Opening captive portal login page");
  let portalTab2 = await portalTabPromise;
  is(portalTab2, portalTab, "The existing portal tab should be focused.");

  let portalTabClosing = BrowserTestUtils.waitForTabClosing(portalTab);
  let errorTabReloaded = BrowserTestUtils.waitForErrorPage(browser);

  Services.obs.notifyObservers(null, "captive-portal-login-success");
  await portalTabClosing;

  info(
    "Waiting for error tab to be reloaded after the captive portal was freed."
  );
  await errorTabReloaded;
  await ContentTask.spawn(browser, null, () => {
    let doc = content.document;
    ok(
      !doc.body.classList.contains("captiveportal"),
      "Captive portal error page UI is not visible."
    );
  });

  await BrowserTestUtils.removeTab(tab);
});

add_task(async function testCaptivePortalAdvancedPanel() {
  info(
    "Checking that the advanced section of the about:certerror UI is shown when we are behind a captive portal."
  );
  let tab = await setupCaptivePortalTab();
  let browser = tab.linkedBrowser;

  await ContentTask.spawn(browser, BAD_CERT_PAGE, async expectedURL => {
    const EventUtils = ContentTaskUtils.getEventUtils(content);
    let doc = content.document;
    let advancedButton = doc.getElementById("advancedButton");
    await ContentTaskUtils.waitForCondition(
      () => ContentTaskUtils.is_visible(advancedButton),
      "Captive portal UI is visible"
    );

    info("Clicking on the advanced button");
    await EventUtils.synthesizeMouseAtCenter(advancedButton, {}, content);
    let advPanelContainer = doc.getElementById("advancedPanelContainer");
    ok(
      ContentTaskUtils.is_visible(advPanelContainer),
      "Advanced panel is now visible"
    );

    let advPanelContent = doc.getElementById("badCertTechnicalInfo");
    ok(
      ContentTaskUtils.is_visible(advPanelContent) &&
        advPanelContent.textContent.includes("expired.example.com"),
      "Advanced panel text content is visible"
    );

    let advPanelErrorCode = doc.getElementById("errorCode");
    ok(
      advPanelErrorCode.textContent,
      "Cert error code is visible in the advanced panel"
    );

    let advPanelExceptionButton = doc.getElementById("exceptionDialogButton");
    await EventUtils.synthesizeMouseAtCenter(
      advPanelExceptionButton,
      {},
      content
    );
    ok(
      doc.location.href.startsWith(expectedURL),
      "Accept the risk and continue button works on the captive portal page"
    );
  });

  // Clear the certificate exception.
  let certOverrideService = Cc[
    "@mozilla.org/security/certoverride;1"
  ].getService(Ci.nsICertOverrideService);
  certOverrideService.clearValidityOverride("expired.example.com", -1);
  await BrowserTestUtils.removeTab(tab);
});
