// Copyright (c) 2020, The Tor Project, Inc.

"use strict";

var EXPORTED_SYMBOLS = ["OnionLocationParent"];

const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
const { TorStrings } = ChromeUtils.import("resource:///modules/TorStrings.jsm");

// Prefs
const NOTIFICATION_PREF = "privacy.prioritizeonions.showNotification";
const PRIORITIZE_ONIONS_PREF = "privacy.prioritizeonions.enabled";

// Element IDs
const ONIONLOCATION_BOX_ID = "onion-location-box";
const ONIONLOCATION_BUTTON_ID = "onion-location-button";
const ONIONLOCATION_LABEL_ID = "onion-label";

// Notification IDs
const NOTIFICATION_ID = "onion-location";
const NOTIFICATION_ANCHOR_ID = "onionlocation";

// Strings
const STRING_ONION_AVAILABLE = TorStrings.onionLocation.onionAvailable;
const NOTIFICATION_CANCEL_LABEL = TorStrings.onionLocation.notNow;
const NOTIFICATION_CANCEL_ACCESSKEY = TorStrings.onionLocation.notNowAccessKey;
const NOTIFICATION_OK_LABEL = TorStrings.onionLocation.alwaysPrioritize;
const NOTIFICATION_OK_ACCESSKEY =
  TorStrings.onionLocation.alwaysPrioritizeAccessKey;
const NOTIFICATION_TITLE = TorStrings.onionLocation.tryThis;
const NOTIFICATION_DESCRIPTION = TorStrings.onionLocation.description;
const NOTIFICATION_LEARN_MORE_URL = TorStrings.onionLocation.learnMoreURL;

var OnionLocationParent = {
  // Listeners are added in BrowserGlue.jsm
  receiveMessage(aMsg) {
    switch (aMsg.name) {
      case "OnionLocation:Set":
        this.setOnionLocation(aMsg.target);
        break;
    }
  },

  buttonClick(event) {
    if (event.button != 0) {
      return;
    }
    const win = event.target.ownerGlobal;
    const browser = win.gBrowser.selectedBrowser;
    this.redirect(browser);
  },

  redirect(browser) {
    browser.messageManager.sendAsyncMessage("OnionLocation:Refresh");
    this.setDisabled(browser);
  },

  onStateChange(browser) {
    delete browser._onionLocation;
    this.hideNotification(browser);
  },

  setOnionLocation(browser) {
    const win = browser.ownerGlobal;
    browser._onionLocation = true;
    if (browser === win.gBrowser.selectedBrowser) {
      this.updateOnionLocationBadge(browser);
    }
  },

  hideNotification(browser) {
    const win = browser.ownerGlobal;
    if (browser._onionLocationPrompt) {
      win.PopupNotifications.remove(browser._onionLocationPrompt);
    }
  },

  showNotification(browser) {
    const mustShow = Services.prefs.getBoolPref(NOTIFICATION_PREF, true);
    if (!mustShow) {
      return;
    }

    const win = browser.ownerGlobal;
    Services.prefs.setBoolPref(NOTIFICATION_PREF, false);

    const mainAction = {
      label: NOTIFICATION_OK_LABEL,
      accessKey: NOTIFICATION_OK_ACCESSKEY,
      callback() {
        Services.prefs.setBoolPref(PRIORITIZE_ONIONS_PREF, true);
        OnionLocationParent.redirect(browser);
        win.openPreferences("privacy-onionservices");
      },
    };

    const cancelAction = {
      label: NOTIFICATION_CANCEL_LABEL,
      accessKey: NOTIFICATION_CANCEL_ACCESSKEY,
      callback: () => {},
    };

    const options = {
      autofocus: true,
      persistent: true,
      removeOnDismissal: false,
      eventCallback(aTopic) {
        if (aTopic === "removed") {
          delete browser._onionLocationPrompt;
          delete browser.onionpopupnotificationanchor;
        }
      },
      learnMoreURL: NOTIFICATION_LEARN_MORE_URL,
      displayURI: {
        hostPort: NOTIFICATION_TITLE, // This is hacky, but allows us to have a title without extra markup/css.
      },
      hideClose: true,
      popupIconClass: "onionlocation-notification-icon",
    };

    // A hacky way of setting the popup anchor outside the usual url bar icon box
    // onionlocationpopupnotificationanchor comes from `${ANCHOR_ID}popupnotificationanchor`
    // From https://searchfox.org/mozilla-esr68/rev/080f9ed47742644d2ff84f7aa0b10aea5c44301a/browser/components/newtab/lib/CFRPageActions.jsm#488
    browser.onionlocationpopupnotificationanchor = win.document.getElementById(
      ONIONLOCATION_BUTTON_ID
    );

    browser._onionLocationPrompt = win.PopupNotifications.show(
      browser,
      NOTIFICATION_ID,
      NOTIFICATION_DESCRIPTION,
      NOTIFICATION_ANCHOR_ID,
      mainAction,
      [cancelAction],
      options
    );
  },

  setEnabled(browser) {
    const win = browser.ownerGlobal;
    const label = win.document.getElementById(ONIONLOCATION_LABEL_ID);
    label.textContent = STRING_ONION_AVAILABLE;
    const elem = win.document.getElementById(ONIONLOCATION_BOX_ID);
    elem.removeAttribute("hidden");
  },

  setDisabled(browser) {
    const win = browser.ownerGlobal;
    const elem = win.document.getElementById(ONIONLOCATION_BOX_ID);
    elem.setAttribute("hidden", true);
  },

  updateOnionLocationBadge(browser) {
    if (browser._onionLocation) {
      this.setEnabled(browser);
      this.showNotification(browser);
    } else {
      this.setDisabled(browser);
    }
  },
};
