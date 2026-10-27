/* Private management page: no hosts, content scripts or external messaging. */
'use strict';
const inventory = () => new Promise((resolve, reject) => {
  chrome.management.getAll(extensions => {
    const error = chrome.runtime.lastError;
    if (error) reject(new Error(error.message));
    else resolve(extensions);
  });
});
globalThis.GnosisExtensionManager = Object.freeze({
  inventory,
  async setEnabled(id, enabled) {
    await new Promise((resolve, reject) => {
      chrome.management.setEnabled(id, enabled, () => {
        const error = chrome.runtime.lastError;
        if (error) reject(new Error(error.message));
        else resolve();
      });
    });
    return inventory();
  }
});
