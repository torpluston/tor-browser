/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { DEBUG_TARGET_PANE, PREFERENCES, RUNTIMES } = require("../constants");

const Services = require("Services");

// Process target debugging is disabled by default.
function isProcessDebuggingSupported() {
  return Services.prefs.getBoolPref(
    PREFERENCES.PROCESS_DEBUGGING_ENABLED,
    false
  );
}

// Process target debugging is disabled by default.
function isLocalTabDebuggingSupported() {
  return Services.prefs.getBoolPref(
    PREFERENCES.LOCAL_TAB_DEBUGGING_ENABLED,
    false
  );
}

// Installing extensions can be disabled in enterprise policy.
// Note: Temporary Extensions are only supported when debugging This Firefox, so checking
// the local preference is acceptable here. If we enable Temporary extensions for remote
// runtimes, we should retrieve the preference from the target runtime instead.
function isTemporaryExtensionSupported() {
  return Services.prefs.getBoolPref(PREFERENCES.XPINSTALL_ENABLED, true);
}

const ALL_DEBUG_TARGET_PANES = [
  DEBUG_TARGET_PANE.INSTALLED_EXTENSION,
  ...(isProcessDebuggingSupported() ? [DEBUG_TARGET_PANE.PROCESSES] : []),
  DEBUG_TARGET_PANE.OTHER_WORKER,
  DEBUG_TARGET_PANE.SERVICE_WORKER,
  DEBUG_TARGET_PANE.SHARED_WORKER,
  DEBUG_TARGET_PANE.TAB,
  ...(isTemporaryExtensionSupported()
    ? [DEBUG_TARGET_PANE.TEMPORARY_EXTENSION]
    : []),
];

// All debug target panes except temporary extensions
const REMOTE_DEBUG_TARGET_PANES = ALL_DEBUG_TARGET_PANES.filter(
  p => p !== DEBUG_TARGET_PANE.TEMPORARY_EXTENSION
);

const THIS_FIREFOX_DEBUG_TARGET_PANES = ALL_DEBUG_TARGET_PANES
  // Main process debugging is not available for This Firefox.
  // At the moment only the main process is listed under processes, so remove the category
  // for this runtime.
  .filter(p => p !== DEBUG_TARGET_PANE.PROCESSES)
  // Showing tab targets for This Firefox is behind a preference.
  .filter(p => p !== DEBUG_TARGET_PANE.TAB || isLocalTabDebuggingSupported());

const SUPPORTED_TARGET_PANE_BY_RUNTIME = {
  [RUNTIMES.THIS_FIREFOX]: THIS_FIREFOX_DEBUG_TARGET_PANES,
  [RUNTIMES.USB]: REMOTE_DEBUG_TARGET_PANES,
  [RUNTIMES.NETWORK]: REMOTE_DEBUG_TARGET_PANES,
};

/**
 * A debug target pane is more specialized than a debug target. For instance EXTENSION is
 * a DEBUG_TARGET but INSTALLED_EXTENSION and TEMPORARY_EXTENSION are DEBUG_TARGET_PANES.
 */
function isSupportedDebugTargetPane(runtimeType, debugTargetPaneKey) {
  return SUPPORTED_TARGET_PANE_BY_RUNTIME[runtimeType].includes(
    debugTargetPaneKey
  );
}
exports.isSupportedDebugTargetPane = isSupportedDebugTargetPane;
