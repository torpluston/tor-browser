/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

/* import-globals-from ../../../shared/test/shared-head.js */
/* import-globals-from ../../../shared/test/telemetry-test-helpers.js */

// shared-head.js handles imports, constants, and utility functions
Services.scriptloader.loadSubScript(
  "chrome://mochitests/content/browser/devtools/client/shared/test/shared-head.js",
  this
);

// So that PERFHERDER data can be extracted from the logs.
SimpleTest.requestCompleteLog();

function getFilteredModules(filter, loaders) {
  let modules = [];
  for (const l of loaders) {
    const loaderModulesMap = l.modules;
    const loaderModulesPaths = Object.keys(loaderModulesMap);
    modules = modules.concat(loaderModulesPaths);
  }
  return modules.filter(url => url.includes(filter));
}

function countCharsInModules(modules) {
  return modules.reduce((sum, uri) => {
    try {
      return sum + require("raw!" + uri).length;
    } catch (e) {
      // Ignore failures
      return sum;
    }
  }, 0);
}

/**
 * Record module loading data.
 *
 * @param {Object}
 * - filterString {String} path to use to filter modules specific to the current panel
 * - loaders {Array} Array of Loaders to check for modules
 * - panelName {String} reused in identifiers for perfherder data
 */
function runMetricsTest({ filterString, loaders, panelName }) {
  const allModules = getFilteredModules("", loaders);
  const panelModules = getFilteredModules(filterString, loaders);

  const allModulesCount = allModules.length;
  const panelModulesCount = panelModules.length;

  const allModulesChars = countCharsInModules(allModules);
  const panelModulesChars = countCharsInModules(panelModules);

  const PERFHERDER_DATA = {
    framework: {
      name: "devtools",
    },
    suites: [
      {
        name: panelName + "-metrics",
        value: allModulesChars,
        subtests: [
          {
            name: panelName + "-modules",
            value: panelModulesCount,
          },
          {
            name: panelName + "-chars",
            value: panelModulesChars,
          },
          {
            name: "all-modules",
            value: allModulesCount,
          },
          {
            name: "all-chars",
            value: allModulesChars,
          },
        ],
      },
    ],
  };
  info("PERFHERDER_DATA: " + JSON.stringify(PERFHERDER_DATA));

  // Simply check that we found valid values.
  ok(
    allModulesCount > panelModulesCount && panelModulesCount > 0,
    "Successfully recorded module count for " + panelName
  );
  ok(
    allModulesChars > panelModulesChars && panelModulesChars > 0,
    "Successfully recorded char count for " + panelName
  );
}
