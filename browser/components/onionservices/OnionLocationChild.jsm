// Copyright (c) 2020, The Tor Project, Inc.

"use strict";

var EXPORTED_SYMBOLS = ["OnionLocationChild"];

const { ActorChild } = ChromeUtils.import(
  "resource://gre/modules/ActorChild.jsm"
);

class OnionLocationChild extends ActorChild {
  handleEvent(event) {
    this.onPageShow(event);
  }

  onPageShow(event) {
    if (event.target != this.content.document) {
      return;
    }
    const onionLocationURI = this.content.document.onionLocationURI;
    if (onionLocationURI) {
      this.mm.sendAsyncMessage("OnionLocation:Set");
    }
  }

  receiveMessage(aMessage) {
    if (aMessage.name == "OnionLocation:Refresh") {
      const doc = this.content.document;
      const docShell = this.mm.docShell;
      const onionLocationURI = doc.onionLocationURI;
      const refreshURI = docShell.QueryInterface(Ci.nsIRefreshURI);
      if (onionLocationURI && refreshURI) {
        refreshURI.refreshURI(
          onionLocationURI,
          doc.nodePrincipal,
          0,
          false,
          true
        );
      }
    }
  }
}
