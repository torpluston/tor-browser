/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// Verify RDM opens for a container tab.

const TEST_URL = "http://example.com/";

add_task(async function() {
  // Open a tab with about:newtab in a container.
  const tab = await addTab(BROWSER_NEW_TAB_URL, {
    userContextId: 2,
  });
  is(tab.userContextId, 2, "Tab's container ID is correct");
  const browser = tab.linkedBrowser;

  // Open RDM and try to navigate
  const { ui } = await openRDM(tab);
  const loaded = waitForFrameLoad(ui, TEST_URL);
  await load(browser, TEST_URL);
  await loaded;
  ok(true, "Test URL navigated successfully");

  await closeRDM(tab);
  await removeTab(tab);
});
