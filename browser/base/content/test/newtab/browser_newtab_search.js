/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

// See browser/components/search/test/browser_*_behavior.js for tests of actual
// searches.

const ENGINE_LOGO = "searchEngineLogo.xml";
const ENGINE_NO_LOGO = "searchEngineNoLogo.xml";

const SERVICE_EVENT_NAME = "ContentSearchService";

const LOGO_LOW_DPI_SIZE = [65, 26];
const LOGO_HIGH_DPI_SIZE = [130, 52];

// The test has an expected search event queue and a search event listener.
// Search events that are expected to happen are added to the queue, and the
// listener consumes the queue and ensures that each event it receives is at
// the head of the queue.
//
// Each item in the queue is an object { type, deferred }.  type is the
// expected search event type.  deferred is a Promise.defer() value that is
// resolved when the event is consumed.
var gExpectedSearchEventQueue = [];

var gNewEngines = [];

function runTests() {
  let oldCurrentEngine = Services.search.currentEngine;

  yield addNewTabPageTab();
  yield whenSearchInitDone();

  // The tab is removed at the end of the test, so there's no need to remove
  // this listener at the end of the test.
  info("Adding search event listener");
  getContentWindow().addEventListener(SERVICE_EVENT_NAME, searchEventListener);

  let panel = searchPanel();
  is(panel.state, "closed", "Search panel should be closed initially");

  // The panel's animation often is not finished when the test clicks on panel
  // children, which makes the test click the wrong children, so disable it.
  panel.setAttribute("animate", "false");

  // Add the two test engines.
  let logoEngine = null;
  yield promiseNewSearchEngine(true).then(engine => {
    logoEngine = engine;
    TestRunner.next();
  });
  ok(!!logoEngine.getIconURLBySize(...LOGO_LOW_DPI_SIZE),
     "Sanity check: engine should have 1x logo");
  ok(!!logoEngine.getIconURLBySize(...LOGO_HIGH_DPI_SIZE),
     "Sanity check: engine should have 2x logo");

  let noLogoEngine = null;
  yield promiseNewSearchEngine(false).then(engine => {
    noLogoEngine = engine;
    TestRunner.next();
  });
  ok(!noLogoEngine.getIconURLBySize(...LOGO_LOW_DPI_SIZE),
     "Sanity check: engine should not have 1x logo");
  ok(!noLogoEngine.getIconURLBySize(...LOGO_HIGH_DPI_SIZE),
     "Sanity check: engine should not have 2x logo");

  // Use the search service to change the current engine to the logo engine.
  Services.search.currentEngine = logoEngine;
  yield promiseSearchEvents(["CurrentEngine"]).then(TestRunner.next);
  checkCurrentEngine(ENGINE_LOGO);

  // Click the logo to open the search panel.
  yield Promise.all([
    promisePanelShown(panel),
    promiseClick(logoImg()),
  ]).then(TestRunner.next);

  // In the search panel, click the no-logo engine.  It should become the
  // current engine.
  let noLogoBox = null;
  for (let box of panel.childNodes) {
    if (box.getAttribute("engine") == noLogoEngine.name) {
      noLogoBox = box;
      break;
    }
  }
  ok(noLogoBox, "Search panel should contain the no-logo engine");
  yield Promise.all([
    promiseSearchEvents(["CurrentEngine"]),
    promiseClick(noLogoBox),
  ]).then(TestRunner.next);

  checkCurrentEngine(ENGINE_NO_LOGO);

  // Switch back to the logo engine.
  Services.search.currentEngine = logoEngine;
  yield promiseSearchEvents(["CurrentEngine"]).then(TestRunner.next);
  checkCurrentEngine(ENGINE_LOGO);

  // Open the panel again.
  yield Promise.all([
    promisePanelShown(panel),
    promiseClick(logoImg()),
  ]).then(TestRunner.next);

  // In the search panel, click the Manage Engines box.
  let manageBox = $("manage");
  ok(!!manageBox, "The Manage Engines box should be present in the document");
  yield Promise.all([
    promiseManagerOpen(),
    promiseClick(manageBox),
  ]).then(TestRunner.next);

  // Done.  Revert the current engine and remove the new engines.
  Services.search.currentEngine = oldCurrentEngine;
  yield promiseSearchEvents(["CurrentEngine"]).then(TestRunner.next);

  let events = [];
  for (let engine of gNewEngines) {
    Services.search.removeEngine(engine);
    events.push("State");
  }
  yield promiseSearchEvents(events).then(TestRunner.next);
}

function searchEventListener(event) {
  info("Got search event " + event.detail.type);
  let passed = false;
  let nonempty = gExpectedSearchEventQueue.length > 0;
  ok(nonempty, "Expected search event queue should be nonempty");
  if (nonempty) {
    let { type, deferred } = gExpectedSearchEventQueue.shift();
    is(event.detail.type, type, "Got expected search event " + type);
    if (event.detail.type == type) {
      passed = true;
      // Let gSearch respond to the event before continuing.
      executeSoon(() => deferred.resolve());
    }
  }
  if (!passed) {
    info("Didn't get expected event, stopping the test");
    getContentWindow().removeEventListener(SERVICE_EVENT_NAME,
                                           searchEventListener);
    // Set next() to a no-op so the test really does stop.
    TestRunner.next = function () {};
    TestRunner.finish();
  }
}

function $(idSuffix) {
  return getContentDocument().getElementById("newtab-search-" + idSuffix);
}

function promiseSearchEvents(events) {
  info("Expecting search events: " + events);
  events = events.map(e => ({ type: e, deferred: Promise.defer() }));
  gExpectedSearchEventQueue.push(...events);
  return Promise.all(events.map(e => e.deferred.promise));
}

function promiseNewSearchEngine(withLogo) {
  let basename = withLogo ? ENGINE_LOGO : ENGINE_NO_LOGO;
  info("Waiting for engine to be added: " + basename);

  // Wait for the search events triggered by adding the new engine.
  // engine-added engine-loaded
  let expectedSearchEvents = ["State", "State"];
  if (withLogo) {
    // an engine-changed for each of the two logos
    expectedSearchEvents.push("State", "State");
  }
  let eventPromise = promiseSearchEvents(expectedSearchEvents);

  // Wait for addEngine().
  let addDeferred = Promise.defer();
  let url = getRootDirectory(gTestPath) + basename;
  Services.search.addEngine(url, Ci.nsISearchEngine.TYPE_MOZSEARCH, "", false, {
    onSuccess: function (engine) {
      info("Search engine added: " + basename);
      gNewEngines.push(engine);
      addDeferred.resolve(engine);
    },
    onError: function (errCode) {
      ok(false, "addEngine failed with error code " + errCode);
      addDeferred.reject();
    },
  });

  // Make a new promise that wraps the previous promises.  The only point of
  // this is to pass the new engine to the yielder via deferred.resolve(),
  // which is a little nicer than passing an array whose first element is the
  // new engine.
  let deferred = Promise.defer();
  Promise.all([addDeferred.promise, eventPromise]).then(values => {
    let newEngine = values[0];
    deferred.resolve(newEngine);
  }, () => deferred.reject());
  return deferred.promise;
}

function checkCurrentEngine(basename) {
  let engine = Services.search.currentEngine;
  ok(engine.name.contains(basename),
     "Sanity check: current engine: engine.name=" + engine.name +
     " basename=" + basename);

  // gSearch.currentEngineName
  is(gSearch().currentEngineName, engine.name,
     "currentEngineName: " + engine.name);

  // search bar logo
  let logoSize = [px * window.devicePixelRatio for (px of LOGO_LOW_DPI_SIZE)];
  let logoURI = engine.getIconURLBySize(...logoSize);
  let logo = logoImg();
  is(logo.hidden, !logoURI,
     "Logo should be visible iff engine has a logo: " + engine.name);
  if (logoURI) {
    is(logo.style.backgroundImage, 'url("' + logoURI + '")', "Logo URI");
  }

  // "selected" attributes of engines in the panel
  let panel = searchPanel();
  for (let engineBox of panel.childNodes) {
    let engineName = engineBox.getAttribute("engine");
    if (engineName == engine.name) {
      is(engineBox.getAttribute("selected"), "true",
         "Engine box's selected attribute should be true for " +
         "selected engine: " + engineName);
    }
    else {
      ok(!engineBox.hasAttribute("selected"),
         "Engine box's selected attribute should be absent for " +
         "non-selected engine: " + engineName);
    }
  }
}

function promisePanelShown(panel) {
  let deferred = Promise.defer();
  info("Waiting for popupshown");
  panel.addEventListener("popupshown", function onEvent() {
    panel.removeEventListener("popupshown", onEvent);
    is(panel.state, "open", "Panel state");
    executeSoon(() => deferred.resolve());
  });
  return deferred.promise;
}

function promiseClick(node) {
  let deferred = Promise.defer();
  let win = getContentWindow();
  SimpleTest.waitForFocus(() => {
    EventUtils.synthesizeMouseAtCenter(node, {}, win);
    deferred.resolve();
  }, win);
  return deferred.promise;
}

function promiseManagerOpen() {
  info("Waiting for the search manager window to open...");
  let deferred = Promise.defer();
  let winWatcher = Cc["@mozilla.org/embedcomp/window-watcher;1"].
                   getService(Ci.nsIWindowWatcher);
  winWatcher.registerNotification(function onWin(subj, topic, data) {
    if (topic == "domwindowopened" && subj instanceof Ci.nsIDOMWindow) {
      subj.addEventListener("load", function onLoad() {
        subj.removeEventListener("load", onLoad);
        if (subj.document.documentURI ==
            "chrome://browser/content/search/engineManager.xul") {
          winWatcher.unregisterNotification(onWin);
          ok(true, "Observed search manager window opened");
          is(subj.opener, gWindow,
             "Search engine manager opener should be the chrome browser " +
             "window containing the newtab page");
          executeSoon(() => {
            subj.close();
            deferred.resolve();
          });
        }
      });
    }
  });
  return deferred.promise;
}

function searchPanel() {
  return $("panel");
}

function logoImg() {
  return $("logo");
}

function gSearch() {
  return getContentWindow().gSearch;
}
