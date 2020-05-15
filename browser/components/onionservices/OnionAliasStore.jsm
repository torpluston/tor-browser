// Copyright (c) 2020, The Tor Project, Inc.

"use strict";

const EXPORTED_SYMBOLS = ["OnionAliasStore"];

const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
const { XPCOMUtils } = ChromeUtils.import(
  "resource://gre/modules/XPCOMUtils.jsm"
);
const { setTimeout, clearTimeout } = ChromeUtils.import(
  "resource://gre/modules/Timer.jsm"
);
const { HttpsEverywhereControl } = ChromeUtils.import(
  "resource:///modules/HttpsEverywhereControl.jsm"
);

// Logger adapted from CustomizableUI.jsm
const kPrefOnionAliasDebug = "browser.onionalias.debug";
XPCOMUtils.defineLazyPreferenceGetter(
  this,
  "gDebuggingEnabled",
  kPrefOnionAliasDebug,
  false,
  (pref, oldVal, newVal) => {
    if (typeof log != "undefined") {
      log.maxLogLevel = newVal ? "all" : "log";
    }
  }
);
XPCOMUtils.defineLazyGetter(this, "log", () => {
  let scope = {};
  ChromeUtils.import("resource://gre/modules/Console.jsm", scope);
  let consoleOptions = {
    maxLogLevel: gDebuggingEnabled ? "all" : "log",
    prefix: "OnionAlias",
  };
  return new scope.ConsoleAPI(consoleOptions);
});

function observe(topic, callback) {
  let observer = {
    observe(aSubject, aTopic, aData) {
      if (topic === aTopic) {
        callback(aSubject, aData);
      }
    },
  };
  Services.obs.addObserver(observer, topic);
  return () => Services.obs.removeObserver(observer, topic);
}

class _OnionAliasStore {
  static get RULESET_CHECK_INTERVAL() {
    return 1000 * 60; // 1 minute
  }

  static get RULESET_CHECK_INTERVAL_FAST() {
    return 1000 * 5; // 5 seconds
  }

  constructor() {
    this._onionMap = new Map();
    this._rulesetTimeout = null;
    this._removeObserver = () => {};
    this._canLoadRules = false;
    this._rulesetTimestamp = null;
  }

  async _periodicRulesetCheck() {
    // TODO: it would probably be preferable to listen to some message broadcasted by
    // the https-everywhere extension when some update channel is updated, instead of
    // polling every N seconds.
    log.debug("Checking for new rules");
    const ts = await this.httpsEverywhereControl.getRulesetTimestamp();
    log.debug(
      `Found ruleset timestamp ${ts}, current is ${this._rulesetTimestamp}`
    );
    if (ts !== this._rulesetTimestamp) {
      this._rulesetTimestamp = ts;
      log.debug("New rules found, updating");
      // We clear the mappings even if we cannot load the rules from https-everywhere,
      // since we cannot be sure if the stored mappings are correct anymore.
      this._clear();
      if (this._canLoadRules) {
        await this._loadRules();
      }
    }
    // If the timestamp is 0, that means the update channel was not yet updated, so
    // we schedule a check soon.
    this._rulesetTimeout = setTimeout(
      () => this._periodicRulesetCheck(),
      ts === 0
        ? _OnionAliasStore.RULESET_CHECK_INTERVAL_FAST
        : _OnionAliasStore.RULESET_CHECK_INTERVAL
    );
  }

  async init() {
    this.httpsEverywhereControl = new HttpsEverywhereControl();

    // Install update channel
    await this.httpsEverywhereControl.installTorOnionUpdateChannel();

    // Setup .tor.onion rule loading.
    // The http observer is a fallback, and is removed in _loadRules() as soon as we are able
    // to load some rules from HTTPS Everywhere.
    this._loadHttpObserver();
    try {
      await this.httpsEverywhereControl.getTorOnionRules();
      this._canLoadRules = true;
    } catch (e) {
      // Loading rules did not work, probably because "get_simple_rules_ending_with" is not yet
      // working in https-everywhere. Use an http observer as a fallback for learning the rules.
      log.debug("Could not load rules, using http observer as fallback");
    }

    // Setup checker for https-everywhere ruleset updates
    this._periodicRulesetCheck();
  }

  /**
   * Loads the .tor.onion mappings from https-everywhere.
   */
  async _loadRules() {
    const rules = await this.httpsEverywhereControl.getTorOnionRules();
    // Remove http observer if we are able to load some rules directly.
    if (rules.length) {
      this._removeObserver();
      this._removeObserver = () => {};
    }
    this._clear();
    log.debug(`Loading ${rules.length} rules`, rules);
    for (const rule of rules) {
      // Here we are trusting that the securedrop ruleset follows some conventions so that we can
      // assume there is a host mapping from `rule.host` to the hostname of the URL in `rule.to`.
      try {
        const url = new URL(rule.to);
        const shortHost = rule.host;
        const longHost = url.hostname;
        this._addMapping(shortHost, longHost);
      } catch (e) {
        log.error("Could not process rule:", rule);
      }
    }
  }

  /**
   * Loads a http observer to listen for local redirects for populating
   * the .tor.onion -> .onion mappings. Should only be used if we cannot ask https-everywhere
   * directly for the mappings.
   */
  _loadHttpObserver() {
    this._removeObserver = observe("http-on-before-connect", channel => {
      if (
        channel.isMainDocumentChannel &&
        channel.originalURI.host.endsWith(".tor.onion")
      ) {
        this._addMapping(channel.originalURI.host, channel.URI.host);
      }
    });
  }

  uninit() {
    this._clear();
    this._removeObserver();
    this._removeObserver = () => {};
    if (this.httpsEverywhereControl) {
      this.httpsEverywhereControl.unload();
      delete this.httpsEverywhereControl;
    }
    clearTimeout(this._rulesetTimeout);
    this._rulesetTimeout = null;
    this._rulesetTimestamp = null;
  }

  _clear() {
    this._onionMap.clear();
  }

  _addMapping(shortOnionHost, longOnionHost) {
    this._onionMap.set(longOnionHost, shortOnionHost);
  }

  getShortURI(onionURI) {
    if (
      (onionURI.schemeIs("http") || onionURI.schemeIs("https")) &&
      this._onionMap.has(onionURI.host)
    ) {
      return onionURI
        .mutate()
        .setHost(this._onionMap.get(onionURI.host))
        .finalize();
    }
    return null;
  }
}

let OnionAliasStore = new _OnionAliasStore();
