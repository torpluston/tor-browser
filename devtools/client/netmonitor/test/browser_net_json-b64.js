/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

/**
 * Tests if JSON responses encoded in base64 are handled correctly.
 */

add_task(async function() {
  const { L10N } = require("devtools/client/netmonitor/src/utils/l10n");
  const { tab, monitor } = await initNetMonitor(JSON_B64_URL);
  info("Starting test... ");

  const { document, store, windowRequire } = monitor.panelWin;
  const Actions = windowRequire("devtools/client/netmonitor/src/actions/index");

  store.dispatch(Actions.batchEnable(false));

  // Execute requests.
  await performRequests(monitor, tab, 1);

  wait = waitForDOM(document, "#response-panel .CodeMirror-code");
  store.dispatch(Actions.toggleNetworkDetails());
  EventUtils.sendMouseEvent(
    { type: "click" },
    document.querySelector("#response-tab")
  );
  await wait;

  const tabpanel = document.querySelector("#response-panel");

  is(
    tabpanel.querySelector(".response-error-header") === null,
    true,
    "The response error header doesn't have the intended visibility."
  );
  const jsonView = tabpanel.querySelector(".tree-section .treeLabel") || {};
  is(
    jsonView.textContent === L10N.getStr("jsonScopeName"),
    true,
    "The response json view has the intended visibility."
  );
  is(
    tabpanel.querySelector(".CodeMirror-code") === null,
    false,
    "The response editor has the intended visibility."
  );
  is(
    tabpanel.querySelector(".response-image-box") === null,
    true,
    "The response image box doesn't have the intended visibility."
  );

  is(
    tabpanel.querySelectorAll(".tree-section").length,
    2,
    "There should be 2 tree sections displayed in this tabpanel."
  );
  is(
    tabpanel.querySelectorAll(".treeRow:not(.tree-section)").length,
    1,
    "There should be 1 json properties displayed in this tabpanel."
  );
  is(
    tabpanel.querySelectorAll(".empty-notice").length,
    0,
    "The empty notice should not be displayed in this tabpanel."
  );

  const labels = tabpanel.querySelectorAll(
    "tr:not(.tree-section) .treeLabelCell .treeLabel"
  );
  const values = tabpanel.querySelectorAll(
    "tr:not(.tree-section) .treeValueCell .objectBox"
  );

  is(
    labels[0].textContent,
    "greeting",
    "The first json property name was incorrect."
  );
  is(
    values[0].textContent,
    "This is a base 64 string.",
    "The first json property value was incorrect."
  );

  await teardown(monitor);
});
