/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { ConsoleCommand } = require("devtools/client/webconsole/types");

function JSTerm(webConsoleUI) {
  this.ui = webConsoleUI;
  this.hudId = this.ui.hudId;
  this.historyLoaded = new Promise(r => {
    r();
  });
}

JSTerm.prototype = {
  SELECTED_FRAME: -1,

  get webConsoleClient() {
    return this.hud.webConsoleClient;
  },

  openVariablesView() {},

  init() {
    this.doc = this.hud.window.document;
    this.root = this.doc.createElement("div");
    this.root.className = "jsterm-input-container";
    this.inputNode = this.doc.createElement("input");
    this.inputNode.className = "jsterm-input-node jsterm-input-node-html";
    this.root.appendChild(this.inputNode);
    this.doc.querySelector("#app-wrapper").appendChild(this.root);

    this.inputNode.onkeypress = e => {
      if (e.key === "Enter") {
        this.execute();
      }
    };
  },

  /**
   * Sets the value of the input field (command line), and resizes the field to
   * fit its contents. This method is preferred over setting "inputNode.value"
   * directly, because it correctly resizes the field.
   *
   * @param string newValue
   *        The new value to set.
   * @returns void
   */
  _setValue(newValue) {
    this.inputNode.value = newValue;
    // this.resizeInput();
  },

  /**
   * Gets the value from the input field
   * @returns string
   */
  _getValue() {
    return this.inputNode.value || "";
  },

  execute(executeString) {
    return new Promise(resolve => {
      // attempt to execute the content of the inputNode
      executeString = executeString || this._getValue();
      if (!executeString) {
        return;
      }

      const message = new ConsoleCommand({
        messageText: executeString,
      });
      this.ui.proxy.dispatchMessageAdd(message);

      let selectedNodeActor = null;
      const inspectorSelection = this.ui.owner.getInspectorSelection();
      if (inspectorSelection && inspectorSelection.nodeFront) {
        selectedNodeActor = inspectorSelection.nodeFront.actorID;
      }

      const onResult = response => {
        if (response.error) {
          console.error(
            "Evaluation error " + response.error + ": " + response.message
          );
          return;
        }
        this.ui.wrapper.dispatchMessageAdd(response, true).then(resolve);
      };

      const options = {
        frame: this.SELECTED_FRAME,
        selectedNodeActor: selectedNodeActor,
      };

      this.requestEvaluation(executeString, options).then(onResult, onResult);
      this._setValue("");
    });
  },

  /**
   * Request a JavaScript string evaluation from the server.
   *
   * @param string str
   *        String to execute.
   * @param object [options]
   *        Options for evaluation:
   *        - bindObjectActor: tells the ObjectActor ID for which you want to do
   *        the evaluation. The Debugger.Object of the OA will be bound to
   *        |_self| during evaluation, such that it's usable in the string you
   *        execute.
   *        - frame: tells the stackframe depth to evaluate the string in. If
   *        the jsdebugger is paused, you can pick the stackframe to be used for
   *        evaluation. Use |this.SELECTED_FRAME| to always pick the
   *        user-selected stackframe.
   *        If you do not provide a |frame| the string will be evaluated in the
   *        global content window.
   *        - selectedNodeActor: tells the NodeActor ID of the current selection
   *        in the Inspector, if such a selection exists. This is used by
   *        helper functions that can evaluate on the current selection.
   * @return object
   *         A promise object that is resolved when the server response is
   *         received.
   */
  requestEvaluation(str, options = {}) {
    let frameActor = null;
    if ("frame" in options) {
      frameActor = this.getFrameActor(options.frame);
    }
    const evalOptions = {
      bindObjectActor: options.bindObjectActor,
      frameActor: frameActor,
      selectedNodeActor: options.selectedNodeActor,
      selectedObjectActor: options.selectedObjectActor,
    };

    return this.webConsoleClient.evaluateJSAsync(str, evalOptions);
  },

  /**
   * Retrieve the FrameActor ID given a frame depth.
   *
   * @param number frame
   *        Frame depth.
   * @return string|null
   *         The FrameActor ID for the given frame depth.
   */
  getFrameActor(frame) {
    const state = this.ui.owner.getDebuggerFrames();
    if (!state) {
      return null;
    }

    let grip;
    if (frame == this.SELECTED_FRAME) {
      grip = state.frames[state.selected];
    } else {
      grip = state.frames[frame];
    }

    return grip ? grip.actor : null;
  },

  focus() {
    if (this.inputNode) {
      this.inputNode.focus();
    }
  },
};

module.exports.JSTerm = JSTerm;
