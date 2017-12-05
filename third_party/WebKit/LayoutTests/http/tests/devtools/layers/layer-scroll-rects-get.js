// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

(async function() {
  TestRunner.addResult(`Tests scroll rectangles support in in Layers3DViewxScroll rectangles\n`);
  await TestRunner.loadModule('layers_test_runner');
  await TestRunner.loadHTML(`
      <div style="transform: translateZ(100px);" onmousewheel=""></div>
      <div id="touchable" style="transform:translateZ(100px);height:20px;width:20px;overflow:scroll;">
          <div style="height:40px;width:40px;"></div>
      </div>
    `);
  await TestRunner.evaluateInPagePromise(`
          var element = document.getElementById('touchable');
          element.addEventListener("touchstart", () => {}, false);
    `);

  await LayersTestRunner.requestLayers();

  TestRunner.addResult('Scroll rectangles');
  LayersTestRunner.layerTreeModel().layerTree().forEachLayer(layer => {
    if (layer._scrollRects.length > 0)
      TestRunner.addObject(layer._scrollRects);
  });
  TestRunner.completeTest();

})();