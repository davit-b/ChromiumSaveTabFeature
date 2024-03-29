// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

(async function() {
  TestRunner.addResult(
      `Tests that renaming a selector updates element styles. Bug 70018. https://bugs.webkit.org/show_bug.cgi?id=70018\n`);
  await TestRunner.loadModule('elements_test_runner');
  await TestRunner.showPanel('elements');
  await TestRunner.loadHTML(`
      <style>
      #inspected {
        color: green;
      }
      </style>
      <p>
      Tests that renaming a selector updates element styles. <a href="https://bugs.webkit.org/show_bug.cgi?id=70018">Bug 70018</a>.
      </p>

      <div id="inspected" style="color: red">Text</div>
      <div id="other"></div>
    `);

  ElementsTestRunner.selectNodeAndWaitForStyles('inspected', step1);

  function step1() {
    TestRunner.addResult('=== Before selector modification ===');
    ElementsTestRunner.dumpSelectedElementStyles(true);
    var section = ElementsTestRunner.firstMatchedStyleSection();
    section.startEditingSelector();
    section._selectorElement.textContent = 'hr, #inspected ';
    ElementsTestRunner.waitForSelectorCommitted(step2);
    section._selectorElement.dispatchEvent(TestRunner.createKeyEvent('Enter'));
  }

  function step2() {
    TestRunner.addResult('=== After non-affecting selector modification ===');
    ElementsTestRunner.dumpSelectedElementStyles(true);
    var section = ElementsTestRunner.firstMatchedStyleSection();
    section.startEditingSelector();
    section._selectorElement.textContent = '#inspectedChanged';
    ElementsTestRunner.waitForSelectorCommitted(step3);
    section._selectorElement.dispatchEvent(TestRunner.createKeyEvent('Enter'));
  }

  function step3() {
    TestRunner.addResult('=== After affecting selector modification ===');
    ElementsTestRunner.dumpSelectedElementStyles(true);
    TestRunner.completeTest();
  }
})();
