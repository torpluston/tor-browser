/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

const { NetUtil } = ChromeUtils.import("resource://gre/modules/NetUtil.jsm");
const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
const { SearchTelemetry } = ChromeUtils.import(
  "resource:///modules/SearchTelemetry.jsm"
);
const { TelemetryTestUtils } = ChromeUtils.import(
  "resource://testing-common/TelemetryTestUtils.jsm"
);

const TESTS = [
  {
    title: "Google search access point",
    trackingUrl:
      "https://www.google.com/search?q=test&ie=utf-8&oe=utf-8&client=firefox-b-1-ab",
    expectedSearchCountEntry: "google.in-content:sap:firefox-b-1-ab",
    expectedAdKey: "google",
    adUrls: [
      "https://www.googleadservices.com/aclk=foobar",
      "https://www.googleadservices.com/pagead/aclk=foobar",
      "https://www.google.com/aclk=foobar",
      "https://www.google.com/pagead/aclk=foobar",
    ],
    nonAdUrls: [
      "https://www.googleadservices.com/?aclk=foobar",
      "https://www.googleadservices.com/bar",
      "https://www.google.com/image",
    ],
  },
  {
    title: "Google search access point follow-on",
    trackingUrl:
      "https://www.google.com/search?client=firefox-b-1-ab&ei=EI_VALUE&q=test2&oq=test2&gs_l=GS_L_VALUE",
    expectedSearchCountEntry: "google.in-content:sap-follow-on:firefox-b-1-ab",
  },
  {
    title: "Google organic",
    trackingUrl:
      "https://www.google.com/search?source=hp&ei=EI_VALUE&q=test&oq=test&gs_l=GS_L_VALUE",
    expectedSearchCountEntry: "google.in-content:organic:none",
  },
  {
    title: "Google organic UK",
    trackingUrl:
      "https://www.google.co.uk/search?source=hp&ei=EI_VALUE&q=test&oq=test&gs_l=GS_L_VALUE",
    expectedSearchCountEntry: "google.in-content:organic:none",
  },
  {
    title: "Yahoo organic",
    trackingUrl:
      "https://search.yahoo.com/search?p=test&fr=yfp-t&fp=1&toggle=1&cop=mss&ei=UTF-8",
    expectedSearchCountEntry: "yahoo.in-content:organic:none",
  },
  {
    title: "Yahoo organic UK",
    trackingUrl:
      "https://uk.search.yahoo.com/search?p=test&fr=yfp-t&fp=1&toggle=1&cop=mss&ei=UTF-8",
    expectedSearchCountEntry: "yahoo.in-content:organic:none",
  },
  {
    title: "Bing search access point",
    trackingUrl: "https://www.bing.com/search?q=test&pc=MOZI&form=MOZLBR",
    expectedSearchCountEntry: "bing.in-content:sap:MOZI",
  },
  {
    setUp() {
      Services.cookies.removeAll();
      Services.cookies.add(
        "www.bing.com",
        "/",
        "SRCHS",
        "PC=MOZ",
        false,
        false,
        false,
        Date.now() + 1000 * 60 * 60,
        {},
        Ci.nsICookie.SAMESITE_NONE
      );
    },
    tearDown() {
      Services.cookies.removeAll();
    },
    title: "Bing search access point follow-on",
    trackingUrl:
      "https://www.bing.com/search?q=test&qs=n&form=QBRE&sp=-1&pq=&sc=0-0&sk=&cvid=CVID_VALUE",
    expectedSearchCountEntry: "bing.in-content:sap-follow-on:MOZ",
  },
  {
    title: "Bing organic",
    trackingUrl:
      "https://www.bing.com/search?q=test&qs=n&form=QBLH&sp=-1&pq=&sc=0-0&sk=&cvid=CVID_VALUE",
    expectedSearchCountEntry: "bing.in-content:organic:none",
  },
  {
    title: "DuckDuckGo search access point",
    trackingUrl: "https://duckduckgo.com/?q=test&t=ffab",
    expectedSearchCountEntry: "duckduckgo.in-content:sap:ffab",
  },
  {
    title: "DuckDuckGo organic",
    trackingUrl: "https://duckduckgo.com/?q=test&t=hi&ia=news",
    expectedSearchCountEntry: "duckduckgo.in-content:organic:hi",
  },
  {
    title: "Baidu search access point",
    trackingUrl: "https://www.baidu.com/baidu?wd=test&tn=monline_7_dg&ie=utf-8",
    expectedSearchCountEntry: "baidu.in-content:sap:monline_7_dg",
  },
  {
    title: "Baidu search access point follow-on",
    trackingUrl:
      "https://www.baidu.com/s?ie=utf-8&f=8&rsv_bp=1&tn=monline_7_dg&wd=test2&oq=test&rsv_pq=RSV_PQ_VALUE&rsv_t=RSV_T_VALUE&rqlang=cn&rsv_enter=1&rsv_sug3=2&rsv_sug2=0&inputT=227&rsv_sug4=397",
    expectedSearchCountEntry: "baidu.in-content:sap-follow-on:monline_7_dg",
  },
  {
    title: "Baidu organic",
    trackingUrl:
      "https://www.baidu.com/s?ie=utf-8&f=8&rsv_bp=1&rsv_idx=1&ch=&tn=baidu&bar=&wd=test&rn=&oq=&rsv_pq=RSV_PQ_VALUE&rsv_t=RSV_T_VALUE&rqlang=cn",
    expectedSearchCountEntry: "baidu.in-content:organic:baidu",
  },
];

/**
 * This function is primarily for testing the Ad URL regexps that are triggered
 * when a URL is clicked on. These regexps are also used for the `with_ads`
 * probe. However, we test the ad_clicks route as that is easier to hit.
 *
 * @param {string} serpUrl
 *   The url to simulate where the page the click came from.
 * @param {string} adUrl
 *   The ad url to simulate being clicked.
 * @param {string} [expectedAdKey]
 *   The expected key to be logged for the scalar. Omit if no scalar should be
 *   logged.
 */
async function testAdUrlClicked(serpUrl, adUrl, expectedAdKey) {
  info(`Testing Ad URL: ${adUrl}`);
  let channel = NetUtil.newChannel({
    uri: NetUtil.newURI(adUrl),
    triggeringPrincipal: Services.scriptSecurityManager.createCodebasePrincipal(
      NetUtil.newURI(serpUrl),
      {}
    ),
    loadUsingSystemPrincipal: true,
  });
  SearchTelemetry._contentHandler.observeActivity(
    channel,
    Ci.nsIHttpActivityObserver.ACTIVITY_TYPE_HTTP_TRANSACTION,
    Ci.nsIHttpActivityObserver.ACTIVITY_SUBTYPE_TRANSACTION_CLOSE
  );
  // Since the content handler takes a moment to allow the channel information
  // to settle down, wait the same amount of time here.
  await new Promise(resolve => Services.tm.dispatchToMainThread(resolve));

  const scalars = TelemetryTestUtils.getProcessScalars("parent", true, true);
  if (!expectedAdKey) {
    Assert.ok(
      !("browser.search.ad_clicks" in scalars),
      "Should not have recorded an ad click"
    );
  } else {
    TelemetryTestUtils.assertKeyedScalar(
      scalars,
      "browser.search.ad_clicks",
      expectedAdKey,
      1
    );
  }
}

add_task(async function test_parsing_search_urls() {
  for (const test of TESTS) {
    info(`Running ${test.title}`);
    if (test.setUp) {
      test.setUp();
    }
    SearchTelemetry.updateTrackingStatus({}, test.trackingUrl);
    const hs = Services.telemetry
      .getKeyedHistogramById("SEARCH_COUNTS")
      .snapshot();
    Assert.ok(hs);
    Assert.ok(
      test.expectedSearchCountEntry in hs,
      "The histogram must contain the correct key"
    );

    if ("adUrls" in test) {
      for (const adUrl of test.adUrls) {
        await testAdUrlClicked(test.trackingUrl, adUrl, test.expectedAdKey);
      }
      for (const nonAdUrls of test.nonAdUrls) {
        await testAdUrlClicked(test.trackingUrl, nonAdUrls);
      }
    }

    if (test.tearDown) {
      test.tearDown();
    }
  }
});
