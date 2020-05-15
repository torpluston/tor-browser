// Copyright (c) 2020, The Tor Project, Inc.

"use strict";

XPCOMUtils.defineLazyModuleGetters(this, {
  OnionAuthUtil: "chrome://browser/content/onionservices/authUtil.jsm",
  CommonUtils: "resource://services-common/utils.js",
  TorStrings: "resource:///modules/TorStrings.jsm",
});

const OnionAuthPrompt = (function() {
  // OnionServicesAuthPrompt objects run within the main/chrome process.
  // aReason is the topic passed within the observer notification that is
  // causing this auth prompt to be displayed.
  function OnionServicesAuthPrompt(aBrowser, aFailedURI, aReason, aOnionName) {
    this._browser = aBrowser;
    this._failedURI = aFailedURI;
    this._reasonForPrompt = aReason;
    this._onionName = aOnionName;
  }

  OnionServicesAuthPrompt.prototype = {
    show(aWarningMessage) {
      let mainAction = {
        label: TorStrings.onionServices.authPrompt.done,
        accessKey: TorStrings.onionServices.authPrompt.doneAccessKey,
        leaveOpen: true, // Callback is responsible for closing the notification.
        callback: this._onDone.bind(this),
      };

      let dialogBundle = Services.strings.createBundle(
                           "chrome://global/locale/dialog.properties");

      let cancelAccessKey = dialogBundle.GetStringFromName("accesskey-cancel");
      if (!cancelAccessKey)
        cancelAccessKey = "c"; // required by PopupNotifications.show()

      let cancelAction = {
        label: dialogBundle.GetStringFromName("button-cancel"),
        accessKey: cancelAccessKey,
        callback: this._onCancel.bind(this),
      };

      let _this = this;
      let options = {
        autofocus: true,
        hideClose: true,
        persistent: true,
        removeOnDismissal: false,
        eventCallback(aTopic) {
          if (aTopic === "showing") {
            _this._onPromptShowing(aWarningMessage);
          } else if (aTopic === "shown") {
            _this._onPromptShown();
          } else if (aTopic === "removed") {
            _this._onPromptRemoved();
          }
        }
      };

      this._prompt = PopupNotifications.show(this._browser,
                       OnionAuthUtil.domid.notification, "",
                       OnionAuthUtil.domid.anchor,
                       mainAction, [cancelAction], options);
    },

    _onPromptShowing(aWarningMessage) {
      let xulDoc = this._browser.ownerDocument;
      let descElem = xulDoc.getElementById(OnionAuthUtil.domid.description);
      if (descElem) {
        // Handle replacement of the onion name within the localized
        // string ourselves so we can show the onion name as bold text.
        // We do this by splitting the localized string and creating
        // several HTML <span> elements.
        while (descElem.firstChild)
          descElem.removeChild(descElem.firstChild);

        let fmtString = TorStrings.onionServices.authPrompt.description;
        let prefix = "";
        let suffix = "";
        const kToReplace = "%S";
        let idx = fmtString.indexOf(kToReplace);
        if (idx < 0) {
          prefix = fmtString;
        } else {
          prefix = fmtString.substring(0, idx);
          suffix = fmtString.substring(idx + kToReplace.length);
        }

        const kHTMLNS = "http://www.w3.org/1999/xhtml";
        let span = xulDoc.createElementNS(kHTMLNS, "span");
        span.textContent = prefix;
        descElem.appendChild(span);
        span = xulDoc.createElementNS(kHTMLNS, "span");
        span.id = OnionAuthUtil.domid.onionNameSpan;
        span.textContent = this._onionName;
        descElem.appendChild(span);
        span = xulDoc.createElementNS(kHTMLNS, "span");
        span.textContent = suffix;
        descElem.appendChild(span);
      }

      // Set "Learn More" label and href.
      let learnMoreElem = xulDoc.getElementById(OnionAuthUtil.domid.learnMore);
      if (learnMoreElem) {
        learnMoreElem.setAttribute("value", TorStrings.onionServices.learnMore);
        learnMoreElem.setAttribute("href", TorStrings.onionServices.learnMoreURL);
      }

      this._showWarning(aWarningMessage);
      let checkboxElem = this._getCheckboxElement();
      if (checkboxElem) {
        checkboxElem.checked = false;
      }
    },

    _onPromptShown() {
      let keyElem = this._getKeyElement();
      if (keyElem) {
        keyElem.setAttribute("placeholder",
                          TorStrings.onionServices.authPrompt.keyPlaceholder);
        this._boundOnKeyFieldKeyPress = this._onKeyFieldKeyPress.bind(this);
        this._boundOnKeyFieldInput = this._onKeyFieldInput.bind(this);
        keyElem.addEventListener("keypress", this._boundOnKeyFieldKeyPress);
        keyElem.addEventListener("input", this._boundOnKeyFieldInput);
        keyElem.focus();
      }
    },

    _onPromptRemoved() {
      if (this._boundOnKeyFieldKeyPress) {
        let keyElem = this._getKeyElement();
        if (keyElem) {
          keyElem.value = "";
          keyElem.removeEventListener("keypress",
                                      this._boundOnKeyFieldKeyPress);
          this._boundOnKeyFieldKeyPress = undefined;
          keyElem.removeEventListener("input", this._boundOnKeyFieldInput);
          this._boundOnKeyFieldInput = undefined;
        }
      }
    },

    _onKeyFieldKeyPress(aEvent) {
      if (aEvent.keyCode == aEvent.DOM_VK_RETURN) {
        this._onDone();
      } else if (aEvent.keyCode == aEvent.DOM_VK_ESCAPE) {
        this._prompt.remove();
        this._onCancel();
      }
    },

    _onKeyFieldInput(aEvent) {
      this._showWarning(undefined); // Remove the warning.
    },

    _onDone() {
      let keyElem = this._getKeyElement();
      if (!keyElem)
        return;

      let base64key = this._keyToBase64(keyElem.value);
      if (!base64key) {
        this._showWarning(TorStrings.onionServices.authPrompt.invalidKey);
        return;
      }

      this._prompt.remove();

      // Use Torbutton's controller module to add the private key to Tor.
      let controllerFailureMsg =
        TorStrings.onionServices.authPrompt.failedToSetKey;
      try {
        let { controller } =
            Cu.import("resource://torbutton/modules/tor-control-port.js", {});
        let torController = controller(aError => {
          this.show(controllerFailureMsg);
        });
        let onionAddr = this._onionName.toLowerCase().replace(/\.onion$/, "");
        let checkboxElem = this._getCheckboxElement();
        let isPermanent = (checkboxElem && checkboxElem.checked);
        torController.onionAuthAdd(onionAddr, base64key, isPermanent)
        .then(aResponse => {
          // Success! Reload the page.
          this._browser.messageManager.sendAsyncMessage("Browser:Reload", {});
        })
        .catch(aError => {
          if (aError.torMessage)
            this.show(aError.torMessage);
          else
            this.show(controllerFailureMsg);
        });
      } catch (e) {
        this.show(controllerFailureMsg);
      }
    },

    _onCancel() {
      // Arrange for an error page to be displayed.
      this._browser.messageManager.sendAsyncMessage(
                               OnionAuthUtil.message.authPromptCanceled,
                               {failedURI: this._failedURI.spec,
                                reasonForPrompt: this._reasonForPrompt});
    },

    _getKeyElement() {
      let xulDoc = this._browser.ownerDocument;
      return xulDoc.getElementById(OnionAuthUtil.domid.keyElement);
    },

    _getCheckboxElement() {
      let xulDoc = this._browser.ownerDocument;
      return xulDoc.getElementById(OnionAuthUtil.domid.checkboxElement);
    },

    _showWarning(aWarningMessage) {
      let xulDoc = this._browser.ownerDocument;
      let warningElem =
                 xulDoc.getElementById(OnionAuthUtil.domid.warningElement);
      let keyElem = this._getKeyElement();
      if (warningElem) {
        if (aWarningMessage) {
          warningElem.textContent = aWarningMessage;
          warningElem.removeAttribute("hidden");
          if (keyElem)
            keyElem.className = "invalid";
        } else {
          warningElem.setAttribute("hidden", "true");
          if (keyElem)
            keyElem.className = "";
        }
      }
    },

    // Returns undefined if the key is the wrong length or format.
    _keyToBase64(aKeyString) {
      if (!aKeyString)
        return undefined;

      let base64key;
      if (aKeyString.length == 52) {
        // The key is probably base32-encoded. Attempt to decode.
        // Although base32 specifies uppercase letters, we accept lowercase
        // as well because users may type in lowercase or copy a key out of
        // a tor onion-auth file (which uses lowercase).
        let rawKey;
        try {
          rawKey = CommonUtils.decodeBase32(aKeyString.toUpperCase());
        } catch (e) {}

        if (rawKey) try {
          base64key = btoa(rawKey);
        } catch (e) {}
      } else if ((aKeyString.length == 44) &&
                 /^[a-zA-Z0-9+/]*=*$/.test(aKeyString)) {
        // The key appears to be a correctly formatted base64 value. If not,
        // tor will return an error when we try to add the key via the
        // control port.
        base64key = aKeyString;
      }

      return base64key;
    },
  };

  let retval = {
    init() {
      Services.obs.addObserver(this, OnionAuthUtil.topic.clientAuthMissing);
      Services.obs.addObserver(this, OnionAuthUtil.topic.clientAuthIncorrect);
    },

    uninit() {
      Services.obs.removeObserver(this, OnionAuthUtil.topic.clientAuthMissing);
      Services.obs.removeObserver(this, OnionAuthUtil.topic.clientAuthIncorrect);
    },

    // aSubject is the DOM Window or browser where the prompt should be shown.
    // aData contains the .onion name.
    observe(aSubject, aTopic, aData) {
      if ((aTopic != OnionAuthUtil.topic.clientAuthMissing) &&
          (aTopic != OnionAuthUtil.topic.clientAuthIncorrect)) {
        return;
      }

      let browser;
      if (aSubject instanceof Ci.nsIDOMWindow) {
        let contentWindow = aSubject.QueryInterface(Ci.nsIDOMWindow);
        browser = contentWindow.docShell.chromeEventHandler;
      } else {
        browser = aSubject.QueryInterface(Ci.nsIBrowser);
      }

      if (!gBrowser.browsers.some(aBrowser => aBrowser == browser)) {
        return; // This window does not contain the subject browser; ignore.
      }

      let failedURI = browser.currentURI;
      let authPrompt = new OnionServicesAuthPrompt(browser, failedURI,
                                                   aTopic, aData);
      authPrompt.show(undefined);
    }
  };

  return retval;
})(); /* OnionAuthPrompt */


Object.defineProperty(this, "OnionAuthPrompt", {
  value: OnionAuthPrompt,
  enumerable: true,
  writable: false
});
