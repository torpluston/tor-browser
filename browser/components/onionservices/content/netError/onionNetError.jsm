// Copyright (c) 2020, The Tor Project, Inc.

"use strict";

var EXPORTED_SYMBOLS = ["OnionServicesAboutNetError"];

ChromeUtils.defineModuleGetter(
  this,
  "TorStrings",
  "resource:///modules/TorStrings.jsm"
);

var OnionServicesAboutNetError = {
  _selector: {
    header: ".title-text",
    longDesc: "#errorLongDesc",
    learnMoreContainer: "#learnMoreContainer",
    learnMoreLink: "#learnMoreLink",
    contentContainer: "#errorLongContent",
    tryAgainButtonContainer: "#netErrorButtonContainer",
  },
  _status: {
    ok: "ok",
    error: "error",
  },

  _diagramInfoMap: undefined,

  // Public functions (called from outside this file).
  //
  // This initPage() function may need to be updated if the structure of
  // browser/base/content/aboutNetError.xhtml changes. Specifically, it
  // references the following elements:
  //   query string parameter e
  //   class title-text
  //   id errorLongDesc
  //   id learnMoreContainer
  //   id learnMoreLink
  //   id errorLongContent
  initPage(aDoc) {
    const searchParams = new URLSearchParams(aDoc.documentURI.split("?")[1]);
    const err = searchParams.get("e");

    const errPrefix = "onionServices.";
    if (!err.startsWith(errPrefix)) {
      return; // This is not an onion services error; nothing for us to do.
    }

    const errName = err.substring(errPrefix.length);

    const stringsObj = TorStrings.onionServices[errName];
    if (!stringsObj) {
      return;
    }

    this._insertStylesheet(aDoc);

    const pageTitle = stringsObj.pageTitle;
    const header = stringsObj.header;
    const longDescription = stringsObj.longDescription; // optional
    const learnMoreURL = stringsObj.learnMoreURL;

    if (pageTitle) {
      aDoc.title = pageTitle;
    }

    if (header) {
      const headerElem = aDoc.querySelector(this._selector.header);
      if (headerElem) {
        headerElem.textContent = header;
      }
    }

    const ld = aDoc.querySelector(this._selector.longDesc);
    if (ld) {
      if (longDescription) {
        const hexErr = this._hexErrorFromName(errName);
        // eslint-disable-next-line no-unsanitized/property
        ld.innerHTML = longDescription.replace("%S", hexErr);
      } else {
        // This onion service error does not have a long description. Since
        // it is set to a generic error string by the code in
        // browser/base/content/aboutNetError.js, hide it here.
        ld.style.display = "none";
      }
    }

    if (learnMoreURL) {
      const lmContainer = aDoc.querySelector(this._selector.learnMoreContainer);
      if (lmContainer) {
        lmContainer.style.display = "block";
      }
      const lmLink = lmContainer.querySelector(this._selector.learnMoreLink);
      if (lmLink) {
        lmLink.setAttribute("href", learnMoreURL);
      }
    }

    // Remove the "Try Again" button if the user made a typo in the .onion
    // address since it is not useful in that case.
    if (errName === "badAddress") {
      const tryAgainButton = aDoc.querySelector(
        this._selector.tryAgainButtonContainer
      );
      if (tryAgainButton) {
        tryAgainButton.style.display = "none";
      }
    }

    this._insertDiagram(aDoc, errName);
  }, // initPage()

  _insertStylesheet(aDoc) {
    const url =
      "chrome://browser/content/onionservices/netError/onionNetError.css";
    let linkElem = aDoc.createElement("link");
    linkElem.rel = "stylesheet";
    linkElem.href = url;
    linkElem.type = "text/css";
    aDoc.head.appendChild(linkElem);
  },

  _insertDiagram(aDoc, aErrorName) {
    // The onion error diagram consists of a grid of div elements.
    // The first row contains three images (Browser, Network, Onionsite) and
    // the second row contains labels for the images that are in the first row.
    // The _diagramInfoMap describes for each type of onion service error
    // whether a small ok or error status icon is overlaid on top of the main
    // Browser/Network/Onionsite images.
    if (!this._diagramInfoMap) {
      this._diagramInfoMap = new Map();
      this._diagramInfoMap.set("descNotFound", {
        browser: this._status.ok,
        network: this._status.ok,
        onionSite: this._status.error,
      });
      this._diagramInfoMap.set("descInvalid", {
        browser: this._status.ok,
        network: this._status.error,
      });
      this._diagramInfoMap.set("introFailed", {
        browser: this._status.ok,
        network: this._status.error,
      });
      this._diagramInfoMap.set("rendezvousFailed", {
        browser: this._status.ok,
        network: this._status.error,
      });
      this._diagramInfoMap.set("clientAuthMissing", {
        browser: this._status.error,
      });
      this._diagramInfoMap.set("clientAuthIncorrect", {
        browser: this._status.error,
      });
      this._diagramInfoMap.set("badAddress", {
        browser: this._status.error,
      });
      this._diagramInfoMap.set("introTimedOut", {
        browser: this._status.ok,
        network: this._status.error,
      });
    }

    const diagramInfo = this._diagramInfoMap.get(aErrorName);

    const container = this._createDiv(aDoc, "onionErrorDiagramContainer");
    const imageClass = "onionErrorImage";

    const browserImage = this._createDiv(
      aDoc,
      "onionErrorBrowserImage",
      imageClass,
      container
    );
    if (diagramInfo && diagramInfo.browser) {
      browserImage.setAttribute("status", diagramInfo.browser);
    }

    const networkImage = this._createDiv(
      aDoc,
      "onionErrorNetworkImage",
      imageClass,
      container
    );
    if (diagramInfo && diagramInfo.network) {
      networkImage.setAttribute("status", diagramInfo.network);
    }

    const onionSiteImage = this._createDiv(
      aDoc,
      "onionErrorOnionSiteImage",
      imageClass,
      container
    );
    if (diagramInfo && diagramInfo.onionSite) {
      onionSiteImage.setAttribute("status", diagramInfo.onionSite);
    }

    let labelDiv = this._createDiv(aDoc, undefined, undefined, container);
    labelDiv.textContent = TorStrings.onionServices.errorPage.browser;
    labelDiv = this._createDiv(aDoc, undefined, undefined, container);
    labelDiv.textContent = TorStrings.onionServices.errorPage.network;
    labelDiv = this._createDiv(aDoc, undefined, undefined, container);
    labelDiv.textContent = TorStrings.onionServices.errorPage.onionSite;

    const contentContainer = aDoc.querySelector(
      this._selector.contentContainer
    );
    if (contentContainer) {
      contentContainer.insertBefore(container, contentContainer.firstChild);
    }
  }, // _insertDiagram()

  _createDiv(aDoc, aID, aClass, aParentElem) {
    const div = aDoc.createElement("div");
    if (aID) {
      div.id = aID;
    }
    if (aClass) {
      div.setAttribute("class", aClass);
    }
    if (aParentElem) {
      aParentElem.appendChild(div);
    }

    return div;
  },

  _hexErrorFromName(aErrorName) {
    // We do not have access to the original Tor SOCKS error code here, so
    // perform a reverse mapping from the error name.
    switch (aErrorName) {
      case "descNotFound":
        return "0xF0";
      case "descInvalid":
        return "0xF1";
      case "introFailed":
        return "0xF2";
      case "rendezvousFailed":
        return "0xF3";
      case "clientAuthMissing":
        return "0xF4";
      case "clientAuthIncorrect":
        return "0xF5";
      case "badAddress":
        return "0xF6";
      case "introTimedOut":
        return "0xF7";
    }

    return "";
  },
};
