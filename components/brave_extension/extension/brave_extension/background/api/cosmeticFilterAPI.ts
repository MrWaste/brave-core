/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const uuidv4 = require('uuid/v4');

import shieldsPanelActions from '../actions/shieldsPanelActions'

const informTabOfCosmeticRulesToConsider = (tabId: number, hideRules: string[], customStyleRules: any) => {
  if (hideRules.length !== 0 || (customStyleRules && customStyleRules !== {})) {
    const message = {
      type: 'cosmeticFilterConsiderNewRules',
      hideRules,
      customStyleRules
    }
    const options = {
      frameId: 0
    }
    chrome.tabs.sendMessage(tabId, message, options)
  }
}

export const injectClassIdStylesheet = (tabId: number, classes: string[], ids: string[], exceptions: string[]) => {
  chrome.braveShields.classIdStylesheet(classes, ids, exceptions, (jsonSelectors) => {
    const hideSelectors = JSON.parse(jsonSelectors)
    informTabOfCosmeticRulesToConsider(tabId, hideSelectors, null)
  })
}

export const applyAdblockCosmeticFilters = (tabId: number, hostname: string) => {
  chrome.braveShields.hostnameCosmeticResources(hostname, async (resources) => {
    if (chrome.runtime.lastError) {
      console.warn('Unable to get cosmetic filter data for the current host')
      return
    }

    informTabOfCosmeticRulesToConsider(tabId, resources.hide_selectors, resources.style_selectors)

    if (resources.injected_script) {
      chrome.tabs.executeScript(tabId, {
        code: resources.injected_script,
        runAt: 'document_start'
      })
    }

    const randomizedClassName = 'b' + uuidv4().split('-').slice(0, 2).join('')

    chrome.tabs.insertCSS(tabId, {
      code: `.${randomizedClassName} {display: none !important;}`,
      cssOrigin: 'user',
      runAt: 'document_start'
    })

    shieldsPanelActions.cosmeticFilterRuleExceptions(tabId, resources.exceptions, randomizedClassName)
  })
}

// User generated cosmetic filtering below
export const applyCSSCosmeticFilters = (tabId: number, hostname: string) => {
  chrome.storage.local.get('cosmeticFilterList', (storeData = {}) => {
    if (!storeData.cosmeticFilterList) {
      if (process.env.NODE_ENV === 'shields_development') {
        console.log('applySiteFilters: no cosmetic filter store yet')
      }
      return
    }
    if (storeData.cosmeticFilterList[hostname] !== undefined) {
      storeData.cosmeticFilterList[hostname].map((rule: string) => {
        if (process.env.NODE_ENV === 'shields_development') {
          console.log('applying rule', rule)
        }
        chrome.tabs.insertCSS(tabId, { // https://github.com/brave/brave-browser/wiki/Cosmetic-Filtering
          code: `${rule} {display: none !important;}`,
          cssOrigin: 'user',
          runAt: 'document_start'
        })
      })
    }
  })
}

export const removeAllFilters = () => {
  chrome.storage.local.set({ 'cosmeticFilterList': {} })
}

export const addSiteCosmeticFilter = async (origin: string, cssfilter: string) => {
  chrome.storage.local.get('cosmeticFilterList', (storeData = {}) => {
    let storeList = Object.assign({}, storeData.cosmeticFilterList)
    if (storeList[origin] === undefined || storeList[origin].length === 0) { // nothing in filter list for origin
      storeList[origin] = [cssfilter]
    } else { // add entry
      storeList[origin].push(cssfilter)
    }
    chrome.storage.local.set({ 'cosmeticFilterList': storeList })
  })
}

export const removeSiteFilter = (origin: string) => {
  chrome.storage.local.get('cosmeticFilterList', (storeData = {}) => {
    let storeList = Object.assign({}, storeData.cosmeticFilterList)
    delete storeList[origin]
    chrome.storage.local.set({ 'cosmeticFilterList': storeList })
  })
}

