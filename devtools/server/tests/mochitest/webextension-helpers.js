/* exported attachAddon, setWebExtensionOOPMode, waitForFramesUpdated, reloadAddon,
   collectFrameUpdates, generateWebExtensionXPI, promiseInstallFile,
   promiseWebExtensionStartup, promiseWebExtensionShutdown
 */

"use strict";

const { require, loader } = ChromeUtils.import(
  "resource://devtools/shared/Loader.jsm"
);
const { DebuggerClient } = require("devtools/shared/client/debugger-client");
const { DebuggerServer } = require("devtools/server/main");

const {
  AddonTestUtils,
} = require("resource://testing-common/AddonTestUtils.jsm");
const {
  ExtensionTestCommon,
} = require("resource://testing-common/ExtensionTestCommon.jsm");

loader.lazyImporter(
  this,
  "ExtensionParent",
  "resource://gre/modules/ExtensionParent.jsm"
);

// Initialize a minimal DebuggerServer and connect to the webextension addon actor.
if (!DebuggerServer.initialized) {
  DebuggerServer.init();
  DebuggerServer.registerAllActors();
  SimpleTest.registerCleanupFunction(function() {
    DebuggerServer.destroy();
  });
}

SimpleTest.registerCleanupFunction(function() {
  const { hiddenXULWindow } = ExtensionParent.DebugUtils;
  const debugBrowserMapSize =
    ExtensionParent.DebugUtils.debugBrowserPromises.size;

  if (debugBrowserMapSize > 0) {
    is(
      debugBrowserMapSize,
      0,
      "ExtensionParent DebugUtils debug browsers have not been released"
    );
  }

  if (hiddenXULWindow) {
    ok(
      false,
      "ExtensionParent DebugUtils hiddenXULWindow has not been destroyed"
    );
  }
});

// Test helpers related to the webextensions debugging RDP actors.

function setWebExtensionOOPMode(oopMode) {
  return SpecialPowers.pushPrefEnv({
    set: [["extensions.webextensions.remote", oopMode]],
  });
}

function waitForFramesUpdated(target, matchFn) {
  return new Promise(resolve => {
    const listener = data => {
      if (typeof matchFn === "function" && !matchFn(data)) {
        return;
      } else if (!data.frames) {
        return;
      }

      target.off("frameUpdate", listener);
      resolve(data.frames);
    };
    target.on("frameUpdate", listener);
  });
}

function collectFrameUpdates({ client }, matchFn) {
  const collected = [];

  const listener = (evt, data) => {
    if (matchFn(data)) {
      collected.push(data);
    }
  };

  client.addListener("frameUpdate", listener);
  let unsubscribe = () => {
    unsubscribe = null;
    client.removeListener("frameUpdate", listener);
    return collected;
  };

  SimpleTest.registerCleanupFunction(function() {
    if (unsubscribe) {
      unsubscribe();
    }
  });

  return unsubscribe;
}

async function attachAddon(addonId) {
  const transport = DebuggerServer.connectPipe();
  const client = new DebuggerClient(transport);

  await client.connect();

  const addonFront = await client.mainRoot.getAddon({ id: addonId });
  const addonTarget = await addonFront.connect();

  if (!addonTarget) {
    client.close();
    throw new Error(`No WebExtension Actor found for ${addonId}`);
  }

  return addonTarget;
}

async function reloadAddon({ client }, addonId) {
  const addonTargetFront = await client.mainRoot.getAddon({ id: addonId });

  if (!addonTargetFront) {
    client.close();
    throw new Error(`No WebExtension Actor found for ${addonId}`);
  }

  await addonTargetFront.reload();
}

// Test helpers related to the AddonManager.

function generateWebExtensionXPI(extDetails) {
  return ExtensionTestCommon.generateXPI(extDetails);
}

let {
  promiseInstallFile,
  promiseWebExtensionStartup,
  promiseWebExtensionShutdown,
} = AddonTestUtils;
