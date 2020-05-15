// Copyright (c) 2020, The Tor Project, Inc.

"use strict";

ChromeUtils.defineModuleGetter(
  this,
  "TorStrings",
  "resource:///modules/TorStrings.jsm"
);

ChromeUtils.defineModuleGetter(
  this,
  "controller",
  "resource://torbutton/modules/tor-control-port.js"
);

var gOnionServicesSavedKeysDialog = {
  selector: {
    dialog: "#onionservices-savedkeys-dialog",
    intro: "#onionservices-savedkeys-intro",
    tree: "#onionservices-savedkeys-tree",
    onionSiteCol: "#onionservices-savedkeys-siteCol",
    onionKeyCol: "#onionservices-savedkeys-keyCol",
    errorIcon: "#onionservices-savedkeys-errorIcon",
    errorMessage: "#onionservices-savedkeys-errorMessage",
    removeButton: "#onionservices-savedkeys-remove",
    removeAllButton: "#onionservices-savedkeys-removeall",
  },

  _tree: undefined,
  _isBusy: false, // true when loading data, deleting a key, etc.

  // Public functions (called from outside this file).
  async deleteSelectedKeys() {
    this._setBusyState(true);

    const indexesToDelete = [];
    const count = this._tree.view.selection.getRangeCount();
    for (let i = 0; i < count; ++i) {
      const minObj = {};
      const maxObj = {};
      this._tree.view.selection.getRangeAt(i, minObj, maxObj);
      for (let idx = minObj.value; idx <= maxObj.value; ++idx) {
        indexesToDelete.push(idx);
      }
    }

    if (indexesToDelete.length > 0) {
      const controllerFailureMsg =
        TorStrings.onionServices.authPreferences.failedToRemoveKey;
      try {
        const torController = controller(aError => {
          this._showError(controllerFailureMsg);
        });

        // Remove in reverse index order to avoid issues caused by index changes.
        for (let i = indexesToDelete.length - 1; i >= 0; --i) {
          await this._deleteOneKey(torController, indexesToDelete[i]);
        }
      } catch (e) {
        if (e.torMessage) {
          this._showError(e.torMessage);
        } else {
          this._showError(controllerFailureMsg);
        }
      }
    }

    this._setBusyState(false);
  },

  async deleteAllKeys() {
    this._tree.view.selection.selectAll();
    await this.deleteSelectedKeys();
  },

  updateButtonsState() {
    const haveSelection = this._tree.view.selection.getRangeCount() > 0;
    const dialog = document.querySelector(this.selector.dialog);
    const removeSelectedBtn = dialog.querySelector(this.selector.removeButton);
    removeSelectedBtn.disabled = this._isBusy || !haveSelection;
    const removeAllBtn = dialog.querySelector(this.selector.removeAllButton);
    removeAllBtn.disabled = this._isBusy || this.rowCount === 0;
  },

  // Private functions.
  _onLoad() {
    document.mozSubdialogReady = this._init();
  },

  async _init() {
    await this._populateXUL();

    window.addEventListener("keypress", this._onWindowKeyPress.bind(this));

    // We don't use await here because we want _loadSavedKeys() to run
    // in the background and not block loading of this dialog.
    this._loadSavedKeys();
  },

  async _populateXUL() {
    const dialog = document.querySelector(this.selector.dialog);
    const authPrefStrings = TorStrings.onionServices.authPreferences;
    dialog.setAttribute("title", authPrefStrings.dialogTitle);

    let elem = dialog.querySelector(this.selector.intro);
    elem.textContent = authPrefStrings.dialogIntro;

    elem = dialog.querySelector(this.selector.onionSiteCol);
    elem.setAttribute("label", authPrefStrings.onionSite);

    elem = dialog.querySelector(this.selector.onionKeyCol);
    elem.setAttribute("label", authPrefStrings.onionKey);

    elem = dialog.querySelector(this.selector.removeButton);
    elem.setAttribute("label", authPrefStrings.remove);

    elem = dialog.querySelector(this.selector.removeAllButton);
    elem.setAttribute("label", authPrefStrings.removeAll);

    this._tree = dialog.querySelector(this.selector.tree);
  },

  async _loadSavedKeys() {
    const controllerFailureMsg =
      TorStrings.onionServices.authPreferences.failedToGetKeys;
    this._setBusyState(true);

    try {
      this._tree.view = this;

      const torController = controller(aError => {
        this._showError(controllerFailureMsg);
      });

      const keyInfoList = await torController.onionAuthViewKeys();
      if (keyInfoList) {
        // Filter out temporary keys.
        this._keyInfoList = keyInfoList.filter(aKeyInfo => {
          if (!aKeyInfo.Flags) {
            return false;
          }

          const flags = aKeyInfo.Flags.split(",");
          return flags.includes("Permanent");
        });

        // Sort by the .onion address.
        this._keyInfoList.sort((aObj1, aObj2) => {
          const hsAddr1 = aObj1.hsAddress.toLowerCase();
          const hsAddr2 = aObj2.hsAddress.toLowerCase();
          if (hsAddr1 < hsAddr2) {
            return -1;
          }
          return hsAddr1 > hsAddr2 ? 1 : 0;
        });
      }

      // Render the tree content.
      this._tree.rowCountChanged(0, this.rowCount);
    } catch (e) {
      if (e.torMessage) {
        this._showError(e.torMessage);
      } else {
        this._showError(controllerFailureMsg);
      }
    }

    this._setBusyState(false);
  },

  // This method may throw; callers should catch errors.
  async _deleteOneKey(aTorController, aIndex) {
    const keyInfoObj = this._keyInfoList[aIndex];
    await aTorController.onionAuthRemove(keyInfoObj.hsAddress);
    this._tree.view.selection.clearRange(aIndex, aIndex);
    this._keyInfoList.splice(aIndex, 1);
    this._tree.rowCountChanged(aIndex + 1, -1);
  },

  _setBusyState(aIsBusy) {
    this._isBusy = aIsBusy;
    this.updateButtonsState();
  },

  _onWindowKeyPress(event) {
    if (event.keyCode === KeyEvent.DOM_VK_ESCAPE) {
      window.close();
    } else if (event.keyCode === KeyEvent.DOM_VK_DELETE) {
      this.deleteSelectedKeys();
    }
  },

  _showError(aMessage) {
    const dialog = document.querySelector(this.selector.dialog);
    const errorIcon = dialog.querySelector(this.selector.errorIcon);
    errorIcon.style.visibility = aMessage ? "visible" : "hidden";
    const errorDesc = dialog.querySelector(this.selector.errorMessage);
    errorDesc.textContent = aMessage ? aMessage : "";
  },

  // XUL tree widget view implementation.
  get rowCount() {
    return this._keyInfoList ? this._keyInfoList.length : 0;
  },

  getCellText(aRow, aCol) {
    let val = "";
    if (this._keyInfoList && aRow < this._keyInfoList.length) {
      const keyInfo = this._keyInfoList[aRow];
      if (aCol.id.endsWith("-siteCol")) {
        val = keyInfo.hsAddress;
      } else if (aCol.id.endsWith("-keyCol")) {
        val = keyInfo.typeAndKey;
        // Omit keyType because it is always "x25519".
        const idx = val.indexOf(":");
        if (idx > 0) {
          val = val.substring(idx + 1);
        }
      }
    }

    return val;
  },

  isSeparator(index) {
    return false;
  },

  isSorted() {
    return false;
  },

  isContainer(index) {
    return false;
  },

  setTree(tree) {},

  getImageSrc(row, column) {},

  getCellValue(row, column) {},

  cycleHeader(column) {},

  getRowProperties(row) {
    return "";
  },

  getColumnProperties(column) {
    return "";
  },

  getCellProperties(row, column) {
    return "";
  },
};

window.addEventListener("load", () => gOnionServicesSavedKeysDialog._onLoad());
