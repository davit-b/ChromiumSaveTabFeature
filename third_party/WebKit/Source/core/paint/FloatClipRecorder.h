// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FloatClipRecorder_h
#define FloatClipRecorder_h

#include "core/paint/PaintPhase.h"
#include "platform/geometry/FloatRect.h"
#include "platform/graphics/paint/DisplayItem.h"
#include "platform/wtf/Allocator.h"
#include "platform/wtf/Noncopyable.h"

namespace blink {

class FloatClipRecorder {
  USING_FAST_MALLOC(FloatClipRecorder);
  WTF_MAKE_NONCOPYABLE(FloatClipRecorder);

 public:
  FloatClipRecorder(GraphicsContext&,
                    const DisplayItemClient&,
                    PaintPhase,
                    const FloatRect&);
  FloatClipRecorder(GraphicsContext&,
                    const DisplayItemClient&,
                    DisplayItem::Type,
                    const FloatRect&);

  ~FloatClipRecorder();

 private:
  GraphicsContext& context_;
  const DisplayItemClient& client_;
  DisplayItem::Type clip_type_;
};

}  // namespace blink

#endif  // FloatClipRecorder_h
