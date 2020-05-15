// Copyright (c) 2020, The Tor Project, Inc.

"use strict";

ChromeUtils.defineModuleGetter(
  this,
  "TorStrings",
  "resource:///modules/TorStrings.jsm"
);

/*
  Onion Services Client Authentication Preferences Code

  Code to handle init and update of onion services authentication section
  in about:preferences#privacy
*/

const OnionServicesAuthPreferences = {
  selector: {
    groupBox: "#torOnionServiceKeys",
    header: "#torOnionServiceKeys-header",
    overview: "#torOnionServiceKeys-overview",
    learnMore: "#torOnionServiceKeys-learnMore",
    savedKeysButton: "#torOnionServiceKeys-savedKeys",
  },

  init() {
    // populate XUL with localized strings
    this._populateXUL();
  },

  _populateXUL() {
    const groupbox = document.querySelector(this.selector.groupBox);

    let elem = groupbox.querySelector(this.selector.header);
    elem.textContent = TorStrings.onionServices.authPreferences.header;

    elem = groupbox.querySelector(this.selector.overview);
    elem.textContent = TorStrings.onionServices.authPreferences.overview;

    elem = groupbox.querySelector(this.selector.learnMore);
    elem.setAttribute("value", TorStrings.onionServices.learnMore);
    elem.setAttribute("href", TorStrings.onionServices.learnMoreURL);

    elem = groupbox.querySelector(this.selector.savedKeysButton);
    elem.setAttribute(
      "label",
      TorStrings.onionServices.authPreferences.savedKeys
    );
  },

  onViewSavedKeys() {
    gSubDialog.open(
      "chrome://browser/content/onionservices/savedKeysDialog.xul"
    );
  },
}; // OnionServicesAuthPreferences

Object.defineProperty(this, "OnionServicesAuthPreferences", {
  value: OnionServicesAuthPreferences,
  enumerable: true,
  writable: false,
});
