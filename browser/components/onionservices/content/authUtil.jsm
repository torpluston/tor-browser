// Copyright (c) 2020, The Tor Project, Inc.

"use strict";

var EXPORTED_SYMBOLS = [
  "OnionAuthUtil",
];

var { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");

const OnionAuthUtil = {
  topic: {
    clientAuthMissing: "tor-onion-services-clientauth-missing",
    clientAuthIncorrect: "tor-onion-services-clientauth-incorrect",
  },
  message: {
    authPromptCanceled: "Tor:OnionServicesAuthPromptCanceled",
  },
  domid: {
    anchor: "tor-clientauth-notification-icon",
    notification: "tor-clientauth",
    description: "tor-clientauth-notification-desc",
    learnMore: "tor-clientauth-notification-learnmore",
    onionNameSpan: "tor-clientauth-notification-onionname",
    keyElement: "tor-clientauth-notification-key",
    warningElement: "tor-clientauth-warning",
    checkboxElement: "tor-clientauth-persistkey-checkbox",
  },

  addCancelMessageListener(aTabContent, aDocShell) {
    aTabContent.addMessageListener(this.message.authPromptCanceled,
                                   (aMessage) => {
      // Upon cancellation of the client authentication prompt, display
      // the appropriate error page. When calling the docShell
      // displayLoadError() function, we pass undefined for the failed
      // channel so that displayLoadError() can determine that it should
      // not display the client authentication prompt a second time.
      let failedURI = Services.io.newURI(aMessage.data.failedURI);
      let reasonForPrompt = aMessage.data.reasonForPrompt;
      let errorCode =
          (reasonForPrompt === this.topic.clientAuthMissing) ?
          Cr.NS_ERROR_TOR_ONION_SVC_MISSING_CLIENT_AUTH :
          Cr.NS_ERROR_TOR_ONION_SVC_BAD_CLIENT_AUTH;
      aDocShell.displayLoadError(errorCode, failedURI, undefined, undefined);
    });
  },
};
