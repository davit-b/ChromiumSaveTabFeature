/*
 * Copyright (C) 2006 Apple Computer, Inc.  All rights reserved.
 * Copyright (C) 2009 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "platform/loader/fetch/ResourceError.h"

#include "net/base/net_errors.h"
#include "platform/loader/fetch/ResourceRequest.h"
#include "public/platform/Platform.h"
#include "public/platform/WebString.h"
#include "public/platform/WebURL.h"
#include "public/platform/WebURLError.h"

namespace blink {

namespace {
constexpr char kThrottledErrorDescription[] =
    "Request throttled. Visit http://dev.chromium.org/throttling for more "
    "information.";
}  // namespace

int ResourceError::BlockedByXSSAuditorErrorCode() {
  return net::ERR_BLOCKED_BY_XSS_AUDITOR;
}

ResourceError ResourceError::CancelledError(const KURL& url) {
  return ResourceError(net::ERR_ABORTED, url, WTF::nullopt);
}

ResourceError ResourceError::CancelledDueToAccessCheckError(
    const KURL& url,
    ResourceRequestBlockedReason blocked_reason) {
  ResourceError error = CancelledError(url);
  error.is_access_check_ = true;
  error.should_collapse_initiator_ =
      blocked_reason == ResourceRequestBlockedReason::kSubresourceFilter;
  return error;
}

ResourceError ResourceError::CancelledDueToAccessCheckError(
    const KURL& url,
    ResourceRequestBlockedReason blocked_reason,
    const String& localized_description) {
  ResourceError error = CancelledDueToAccessCheckError(url, blocked_reason);
  error.localized_description_ = localized_description;
  return error;
}

ResourceError ResourceError::CacheMissError(const KURL& url) {
  return ResourceError(net::ERR_CACHE_MISS, url, WTF::nullopt);
}

ResourceError ResourceError::TimeoutError(const KURL& url) {
  return ResourceError(net::ERR_TIMED_OUT, url, WTF::nullopt);
}

ResourceError ResourceError::Failure(const KURL& url) {
  return ResourceError(net::ERR_FAILED, url, WTF::nullopt);
}

ResourceError::ResourceError(
    int error_code,
    const KURL& url,
    WTF::Optional<network::CORSErrorStatus> cors_error_status)
    : error_code_(error_code),
      failing_url_(url),
      cors_error_status_(cors_error_status) {
  DCHECK_NE(error_code_, 0);
  InitializeDescription();
}

ResourceError::ResourceError(const WebURLError& error)
    : error_code_(error.reason()),
      failing_url_(error.url()),
      is_access_check_(error.is_web_security_violation()),
      has_copy_in_cache_(error.has_copy_in_cache()),
      cors_error_status_(error.cors_error_status()) {
  DCHECK_NE(error_code_, 0);
  InitializeDescription();
}

ResourceError ResourceError::Copy() const {
  ResourceError error_copy(error_code_, failing_url_.Copy(),
                           cors_error_status_);
  error_copy.has_copy_in_cache_ = has_copy_in_cache_;
  error_copy.localized_description_ = localized_description_.IsolatedCopy();
  error_copy.is_access_check_ = is_access_check_;
  return error_copy;
}

ResourceError::operator WebURLError() const {
  WebURLError::HasCopyInCache has_copy_in_cache =
      has_copy_in_cache_ ? WebURLError::HasCopyInCache::kTrue
                         : WebURLError::HasCopyInCache::kFalse;

  if (cors_error_status_) {
    DCHECK_EQ(net::ERR_FAILED, error_code_);
    return WebURLError(*cors_error_status_, has_copy_in_cache, failing_url_);
  }

  return WebURLError(error_code_, has_copy_in_cache,
                     is_access_check_
                         ? WebURLError::IsWebSecurityViolation::kTrue
                         : WebURLError::IsWebSecurityViolation::kFalse,
                     failing_url_);
}

bool ResourceError::Compare(const ResourceError& a, const ResourceError& b) {
  if (a.ErrorCode() != b.ErrorCode())
    return false;

  if (a.FailingURL() != b.FailingURL())
    return false;

  if (a.LocalizedDescription() != b.LocalizedDescription())
    return false;

  if (a.IsAccessCheck() != b.IsAccessCheck())
    return false;

  if (a.HasCopyInCache() != b.HasCopyInCache())
    return false;

  if (a.CORSErrorStatus() != b.CORSErrorStatus())
    return false;

  return true;
}

bool ResourceError::IsTimeout() const {
  return error_code_ == net::ERR_TIMED_OUT;
}

bool ResourceError::IsCancellation() const {
  return error_code_ == net::ERR_ABORTED;
}

bool ResourceError::IsCacheMiss() const {
  return error_code_ == net::ERR_CACHE_MISS;
}

bool ResourceError::WasBlockedByResponse() const {
  return error_code_ == net::ERR_BLOCKED_BY_RESPONSE;
}

void ResourceError::InitializeDescription() {
  if (error_code_ == net::ERR_TEMPORARILY_THROTTLED) {
    localized_description_ = WebString::FromASCII(kThrottledErrorDescription);
  } else {
    localized_description_ =
        WebString::FromASCII(net::ErrorToString(error_code_));
  }
}

}  // namespace blink