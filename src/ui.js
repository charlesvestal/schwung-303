// 303 — Shadow UI shim.
// All parameter handling is driven by the Shadow UI via module.json's ui_hierarchy;
// this file just exists so the host has something to load for JS-visible lifecycle.

globalThis.init = function () {};
globalThis.tick = function () {};
globalThis.onMidiMessageInternal = function (_data) {};
globalThis.onMidiMessageExternal = function (_data) {};
