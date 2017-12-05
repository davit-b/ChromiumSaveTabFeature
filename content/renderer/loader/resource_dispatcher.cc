// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// See http://dev.chromium.org/developers/design-documents/multi-process-resource-loading

#include "content/renderer/loader/resource_dispatcher.h"

#include <utility>

#include "base/atomic_sequence_num.h"
#include "base/bind.h"
#include "base/compiler_specific.h"
#include "base/debug/alias.h"
#include "base/debug/stack_trace.h"
#include "base/files/file_path.h"
#include "base/memory/ptr_util.h"
#include "base/memory/shared_memory.h"
#include "base/message_loop/message_loop.h"
#include "base/metrics/histogram_macros.h"
#include "base/strings/string_util.h"
#include "base/synchronization/waitable_event.h"
#include "base/task_scheduler/post_task.h"
#include "build/build_config.h"
#include "content/common/inter_process_time_ticks_converter.h"
#include "content/common/navigation_params.h"
#include "content/common/resource_messages.h"
#include "content/common/throttling_url_loader.h"
#include "content/public/common/content_features.h"
#include "content/public/common/resource_request.h"
#include "content/public/common/resource_response.h"
#include "content/public/common/resource_type.h"
#include "content/public/renderer/fixed_received_data.h"
#include "content/public/renderer/request_peer.h"
#include "content/public/renderer/resource_dispatcher_delegate.h"
#include "content/renderer/loader/request_extra_data.h"
#include "content/renderer/loader/resource_scheduling_filter.h"
#include "content/renderer/loader/shared_memory_received_data_factory.h"
#include "content/renderer/loader/site_isolation_stats_gatherer.h"
#include "content/renderer/loader/sync_load_context.h"
#include "content/renderer/loader/sync_load_response.h"
#include "content/renderer/loader/url_loader_client_impl.h"
#include "content/renderer/render_frame_impl.h"
#include "net/base/net_errors.h"
#include "net/base/request_priority.h"
#include "net/http/http_response_headers.h"
#include "services/network/public/cpp/url_loader_completion_status.h"

namespace content {

namespace {

// Converts |time| from a remote to local TimeTicks, overwriting the original
// value.
void RemoteToLocalTimeTicks(
    const InterProcessTimeTicksConverter& converter,
    base::TimeTicks* time) {
  RemoteTimeTicks remote_time = RemoteTimeTicks::FromTimeTicks(*time);
  *time = converter.ToLocalTimeTicks(remote_time).ToTimeTicks();
}

void CrashOnMapFailure() {
#if defined(OS_WIN)
  DWORD last_err = GetLastError();
  base::debug::Alias(&last_err);
#endif
  CHECK(false);
}

void CheckSchemeForReferrerPolicy(const ResourceRequest& request) {
  if ((request.referrer_policy == blink::kWebReferrerPolicyDefault ||
       request.referrer_policy ==
           blink::kWebReferrerPolicyNoReferrerWhenDowngrade) &&
      request.referrer.SchemeIsCryptographic() &&
      !request.url.SchemeIsCryptographic()) {
    LOG(FATAL) << "Trying to send secure referrer for insecure request "
               << "without an appropriate referrer policy.\n"
               << "URL = " << request.url << "\n"
               << "Referrer = " << request.referrer;
  }
}

void NotifySubresourceStarted(
    scoped_refptr<base::SingleThreadTaskRunner> thread_task_runner,
    int render_frame_id,
    const GURL& url,
    const GURL& referrer,
    const std::string& method,
    ResourceType resource_type,
    const std::string& ip,
    uint32_t cert_status) {
  if (!thread_task_runner)
    return;

  if (!thread_task_runner->BelongsToCurrentThread()) {
    thread_task_runner->PostTask(
        FROM_HERE, base::BindOnce(NotifySubresourceStarted, thread_task_runner,
                                  render_frame_id, url, referrer, method,
                                  resource_type, ip, cert_status));
    return;
  }

  RenderFrameImpl* render_frame =
      RenderFrameImpl::FromRoutingID(render_frame_id);
  if (!render_frame)
    return;

  render_frame->GetFrameHost()->SubresourceResponseStarted(
      url, referrer, method, resource_type, ip, cert_status);
}

}  // namespace

// static
int ResourceDispatcher::MakeRequestID() {
  // NOTE: The resource_dispatcher_host also needs probably unique
  // request_ids, so they count down from -2 (-1 is a special "we're
  // screwed value"), while the renderer process counts up.
  static base::AtomicSequenceNumber sequence;
  return sequence.GetNext();  // We start at zero.
}

ResourceDispatcher::ResourceDispatcher(
    IPC::Sender* sender,
    scoped_refptr<base::SingleThreadTaskRunner> thread_task_runner)
    : message_sender_(sender),
      delegate_(nullptr),
      io_timestamp_(base::TimeTicks()),
      thread_task_runner_(thread_task_runner),
      weak_factory_(this) {}

ResourceDispatcher::~ResourceDispatcher() {
}

bool ResourceDispatcher::OnMessageReceived(const IPC::Message& message) {
  if (!IsResourceDispatcherMessage(message)) {
    return false;
  }

  int request_id;

  base::PickleIterator iter(message);
  if (!iter.ReadInt(&request_id)) {
    NOTREACHED() << "malformed resource message";
    return true;
  }

  PendingRequestInfo* request_info = GetPendingRequestInfo(request_id);
  if (!request_info) {
    // Release resources in the message if it is a data message.
    ReleaseResourcesInDataMessage(message);
    return true;
  }

  if (request_info->is_deferred) {
    request_info->deferred_message_queue.push_back(new IPC::Message(message));
    return true;
  }

  // Make sure any deferred messages are dispatched before we dispatch more.
  if (!request_info->deferred_message_queue.empty()) {
    request_info->deferred_message_queue.push_back(new IPC::Message(message));
    FlushDeferredMessages(request_id);
    return true;
  }

  DispatchMessage(message);
  return true;
}

ResourceDispatcher::PendingRequestInfo*
ResourceDispatcher::GetPendingRequestInfo(int request_id) {
  PendingRequestMap::iterator it = pending_requests_.find(request_id);
  if (it == pending_requests_.end()) {
    // This might happen for kill()ed requests on the webkit end.
    return nullptr;
  }
  return it->second.get();
}

void ResourceDispatcher::OnUploadProgress(int request_id,
                                          int64_t position,
                                          int64_t size) {
  PendingRequestInfo* request_info = GetPendingRequestInfo(request_id);
  if (!request_info)
    return;

  request_info->peer->OnUploadProgress(position, size);

  // URLLoaderClientImpl has its own acknowledgement, and doesn't need the IPC
  // message here.
  if (!request_info->url_loader) {
    // Acknowledge receipt
    message_sender_->Send(new ResourceHostMsg_UploadProgress_ACK(request_id));
  }
}

void ResourceDispatcher::OnReceivedResponse(
    int request_id, const ResourceResponseHead& response_head) {
  TRACE_EVENT0("loader", "ResourceDispatcher::OnReceivedResponse");
  PendingRequestInfo* request_info = GetPendingRequestInfo(request_id);
  if (!request_info)
    return;
  request_info->response_start = ConsumeIOTimestamp();

  if (delegate_) {
    std::unique_ptr<RequestPeer> new_peer = delegate_->OnReceivedResponse(
        std::move(request_info->peer), response_head.mime_type,
        request_info->url);
    DCHECK(new_peer);
    request_info->peer = std::move(new_peer);
  }

  if (!IsResourceTypeFrame(request_info->resource_type)) {
    NotifySubresourceStarted(
        RenderThreadImpl::GetMainTaskRunner(), request_info->render_frame_id,
        request_info->response_url, request_info->response_referrer,
        request_info->response_method, request_info->resource_type,
        response_head.socket_address.host(), response_head.cert_status);
  }

  ResourceResponseInfo renderer_response_info;
  ToResourceResponseInfo(*request_info, response_head, &renderer_response_info);
  request_info->site_isolation_metadata =
      SiteIsolationStatsGatherer::OnReceivedResponse(
          request_info->frame_origin, request_info->response_url,
          request_info->resource_type, renderer_response_info);
  request_info->peer->OnReceivedResponse(renderer_response_info);
}

void ResourceDispatcher::OnReceivedCachedMetadata(
    int request_id,
    const std::vector<uint8_t>& data) {
  PendingRequestInfo* request_info = GetPendingRequestInfo(request_id);
  if (!request_info)
    return;

  if (data.size()) {
    request_info->peer->OnReceivedCachedMetadata(
        reinterpret_cast<const char*>(&data.front()), data.size());
  }
}

void ResourceDispatcher::OnSetDataBuffer(int request_id,
                                         base::SharedMemoryHandle shm_handle,
                                         int shm_size) {
  TRACE_EVENT0("loader", "ResourceDispatcher::OnSetDataBuffer");
  PendingRequestInfo* request_info = GetPendingRequestInfo(request_id);
  if (!request_info)
    return;

  bool shm_valid = base::SharedMemory::IsHandleValid(shm_handle);
  CHECK((shm_valid && shm_size > 0) || (!shm_valid && !shm_size));

  request_info->buffer.reset(
      new base::SharedMemory(shm_handle, true));  // read only
  request_info->received_data_factory =
      base::MakeRefCounted<SharedMemoryReceivedDataFactory>(
          message_sender_, request_id, request_info->buffer);

  bool ok = request_info->buffer->Map(shm_size);
  if (!ok) {
    base::SharedMemoryHandle shm_handle_copy = shm_handle;
    base::debug::Alias(&shm_handle_copy);

    CrashOnMapFailure();
    return;
  }

  // TODO(erikchen): Temporary debugging. http://crbug.com/527588.
  CHECK_GE(shm_size, 0);
  CHECK_LE(shm_size, 512 * 1024);
  request_info->buffer_size = shm_size;
}

void ResourceDispatcher::OnReceivedData(int request_id,
                                        int data_offset,
                                        int data_length,
                                        int encoded_data_length) {
  TRACE_EVENT0("loader", "ResourceDispatcher::OnReceivedData");
  DCHECK_GT(data_length, 0);
  PendingRequestInfo* request_info = GetPendingRequestInfo(request_id);
  bool send_ack = true;
  if (request_info && data_length > 0) {
    CHECK(base::SharedMemory::IsHandleValid(request_info->buffer->handle()));
    CHECK_GE(request_info->buffer_size, data_offset + data_length);

    const char* data_start = static_cast<char*>(request_info->buffer->memory());
    CHECK(data_start);
    CHECK(data_start + data_offset);
    const char* data_ptr = data_start + data_offset;

    // Check whether this response data is compliant with our cross-site
    // document blocking policy. We only do this for the first chunk of data.
    if (request_info->site_isolation_metadata.get()) {
      SiteIsolationStatsGatherer::OnReceivedFirstChunk(
          request_info->site_isolation_metadata, data_ptr, data_length);
      request_info->site_isolation_metadata.reset();
    }

    std::unique_ptr<RequestPeer::ReceivedData> data =
        request_info->received_data_factory->Create(data_offset, data_length);
    // |data| takes care of ACKing.
    send_ack = false;
    request_info->peer->OnReceivedData(std::move(data));
  }

  // Get the request info again as the client callback may modify the info.
  request_info = GetPendingRequestInfo(request_id);
  if (request_info && encoded_data_length > 0)
    request_info->peer->OnTransferSizeUpdated(encoded_data_length);

  // Acknowledge the reception of this data.
  if (send_ack)
    message_sender_->Send(new ResourceHostMsg_DataReceived_ACK(request_id));
}

void ResourceDispatcher::OnDownloadedData(int request_id,
                                          int data_len,
                                          int encoded_data_length) {
  PendingRequestInfo* request_info = GetPendingRequestInfo(request_id);
  if (!request_info)
    return;

  request_info->peer->OnDownloadedData(data_len, encoded_data_length);
}

void ResourceDispatcher::OnReceivedRedirect(
    int request_id,
    const net::RedirectInfo& redirect_info,
    const ResourceResponseHead& response_head) {
  TRACE_EVENT0("loader", "ResourceDispatcher::OnReceivedRedirect");
  PendingRequestInfo* request_info = GetPendingRequestInfo(request_id);
  if (!request_info)
    return;
  request_info->response_start = ConsumeIOTimestamp();

  ResourceResponseInfo renderer_response_info;
  ToResourceResponseInfo(*request_info, response_head, &renderer_response_info);
  if (request_info->peer->OnReceivedRedirect(redirect_info,
                                             renderer_response_info)) {
    // Double-check if the request is still around. The call above could
    // potentially remove it.
    request_info = GetPendingRequestInfo(request_id);
    if (!request_info)
      return;
    // We update the response_url here so that we can send it to
    // SiteIsolationStatsGatherer later when OnReceivedResponse is called.
    request_info->response_url = redirect_info.new_url;
    request_info->response_method = redirect_info.new_method;
    request_info->response_referrer = GURL(redirect_info.new_referrer);
    request_info->pending_redirect_message.reset(
        new ResourceHostMsg_FollowRedirect(request_id));
    if (!request_info->is_deferred) {
      FollowPendingRedirect(request_info);
    }
  } else {
    Cancel(request_id);
  }
}

void ResourceDispatcher::FollowPendingRedirect(
    PendingRequestInfo* request_info) {
  IPC::Message* msg = request_info->pending_redirect_message.release();
  if (msg) {
    if (request_info->url_loader) {
      request_info->url_loader->FollowRedirect();
      delete msg;
    } else {
      message_sender_->Send(msg);
    }
  }
}

void ResourceDispatcher::OnRequestComplete(
    int request_id,
    const network::URLLoaderCompletionStatus& status) {
  TRACE_EVENT0("loader", "ResourceDispatcher::OnRequestComplete");

  PendingRequestInfo* request_info = GetPendingRequestInfo(request_id);
  if (!request_info)
    return;
  request_info->completion_time = ConsumeIOTimestamp();
  request_info->buffer.reset();
  if (request_info->received_data_factory)
    request_info->received_data_factory->Stop();
  request_info->received_data_factory = nullptr;
  request_info->buffer_size = 0;

  RequestPeer* peer = request_info->peer.get();

  if (delegate_) {
    std::unique_ptr<RequestPeer> new_peer = delegate_->OnRequestComplete(
        std::move(request_info->peer), request_info->resource_type,
        status.error_code);
    DCHECK(new_peer);
    request_info->peer = std::move(new_peer);
  }

  // The request ID will be removed from our pending list in the destructor.
  // Normally, dispatching this message causes the reference-counted request to
  // die immediately.
  // TODO(kinuko): Revisit here. This probably needs to call request_info->peer
  // but the past attempt to change it seems to have caused crashes.
  // (crbug.com/547047)
  network::URLLoaderCompletionStatus renderer_status(status);
  renderer_status.completion_time =
      ToRendererCompletionTime(*request_info, status.completion_time);
  peer->OnCompletedRequest(renderer_status);
}

bool ResourceDispatcher::RemovePendingRequest(int request_id) {
  PendingRequestMap::iterator it = pending_requests_.find(request_id);
  if (it == pending_requests_.end())
    return false;

  PendingRequestInfo* request_info = it->second.get();

  // |url_loader_client| releases the downloaded file. Otherwise (i.e., we
  // are using Chrome IPC), we should release it here.
  bool release_downloaded_file =
      request_info->download_to_file && !it->second->url_loader_client;

  ReleaseResourcesInMessageQueue(&request_info->deferred_message_queue);

  // Cancel loading.
  it->second->url_loader = nullptr;
  // Clear URLLoaderClient to stop receiving further Mojo IPC from the browser
  // process.
  it->second->url_loader_client = nullptr;

  // Always delete the pending_request asyncly so that cancelling the request
  // doesn't delete the request context info while its response is still being
  // handled.
  thread_task_runner_->DeleteSoon(FROM_HERE, it->second.release());
  pending_requests_.erase(it);

  if (release_downloaded_file) {
    message_sender_->Send(
        new ResourceHostMsg_ReleaseDownloadedFile(request_id));
  }

  if (resource_scheduling_filter_.get())
    resource_scheduling_filter_->ClearRequestIdTaskRunner(request_id);

  return true;
}

void ResourceDispatcher::Cancel(int request_id) {
  PendingRequestMap::iterator it = pending_requests_.find(request_id);
  if (it == pending_requests_.end()) {
    DVLOG(1) << "unknown request";
    return;
  }

  // Cancel the request if it didn't complete, and clean it up so the bridge
  // will receive no more messages.
  const PendingRequestInfo& info = *it->second;
  if (info.completion_time.is_null() && !info.url_loader)
    message_sender_->Send(new ResourceHostMsg_CancelRequest(request_id));
  RemovePendingRequest(request_id);
}

void ResourceDispatcher::SetDefersLoading(int request_id, bool value) {
  PendingRequestInfo* request_info = GetPendingRequestInfo(request_id);
  if (!request_info) {
    DLOG(ERROR) << "unknown request";
    return;
  }
  if (value) {
    request_info->is_deferred = value;
    if (request_info->url_loader_client)
      request_info->url_loader_client->SetDefersLoading();
  } else if (request_info->is_deferred) {
    request_info->is_deferred = false;

    if (request_info->url_loader_client)
      request_info->url_loader_client->UnsetDefersLoading();

    FollowPendingRedirect(request_info);

    thread_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(&ResourceDispatcher::FlushDeferredMessages,
                                  weak_factory_.GetWeakPtr(), request_id));
  }
}

void ResourceDispatcher::DidChangePriority(int request_id,
                                           net::RequestPriority new_priority,
                                           int intra_priority_value) {
  PendingRequestInfo* request_info = GetPendingRequestInfo(request_id);
  DCHECK(request_info);
  if (request_info->url_loader) {
    request_info->url_loader->SetPriority(new_priority, intra_priority_value);
  } else {
    message_sender_->Send(new ResourceHostMsg_DidChangePriority(
        request_id, new_priority, intra_priority_value));
  }
}

void ResourceDispatcher::OnTransferSizeUpdated(int request_id,
                                               int32_t transfer_size_diff) {
  DCHECK_GT(transfer_size_diff, 0);
  PendingRequestInfo* request_info = GetPendingRequestInfo(request_id);
  if (!request_info)
    return;

  // TODO(yhirano): Consider using int64_t in
  // RequestPeer::OnTransferSizeUpdated.
  request_info->peer->OnTransferSizeUpdated(transfer_size_diff);
}

ResourceDispatcher::PendingRequestInfo::PendingRequestInfo(
    std::unique_ptr<RequestPeer> peer,
    ResourceType resource_type,
    int render_frame_id,
    const url::Origin& frame_origin,
    const GURL& request_url,
    const std::string& method,
    const GURL& referrer,
    bool download_to_file)
    : peer(std::move(peer)),
      resource_type(resource_type),
      render_frame_id(render_frame_id),
      url(request_url),
      frame_origin(frame_origin),
      response_url(request_url),
      response_method(method),
      response_referrer(referrer),
      download_to_file(download_to_file),
      request_start(base::TimeTicks::Now()) {}

ResourceDispatcher::PendingRequestInfo::~PendingRequestInfo() {
}

void ResourceDispatcher::DispatchMessage(const IPC::Message& message) {
  IPC_BEGIN_MESSAGE_MAP(ResourceDispatcher, message)
    IPC_MESSAGE_HANDLER(ResourceMsg_UploadProgress, OnUploadProgress)
    IPC_MESSAGE_HANDLER(ResourceMsg_ReceivedResponse, OnReceivedResponse)
    IPC_MESSAGE_HANDLER(ResourceMsg_ReceivedCachedMetadata,
                        OnReceivedCachedMetadata)
    IPC_MESSAGE_HANDLER(ResourceMsg_ReceivedRedirect, OnReceivedRedirect)
    IPC_MESSAGE_HANDLER(ResourceMsg_SetDataBuffer, OnSetDataBuffer)
    IPC_MESSAGE_HANDLER(ResourceMsg_DataReceived, OnReceivedData)
    IPC_MESSAGE_HANDLER(ResourceMsg_DataDownloaded, OnDownloadedData)
    IPC_MESSAGE_HANDLER(ResourceMsg_RequestComplete, OnRequestComplete)
  IPC_END_MESSAGE_MAP()
}

void ResourceDispatcher::FlushDeferredMessages(int request_id) {
  PendingRequestInfo* request_info = GetPendingRequestInfo(request_id);
  if (!request_info || request_info->is_deferred)
    return;

  if (request_info->url_loader) {
    DCHECK(request_info->deferred_message_queue.empty());
    request_info->url_loader_client->FlushDeferredMessages();
    return;
  }

  // Because message handlers could result in request_info being destroyed,
  // we need to work with a stack reference to the deferred queue.
  MessageQueue q;
  q.swap(request_info->deferred_message_queue);
  while (!q.empty()) {
    IPC::Message* m = q.front();
    q.pop_front();
    DispatchMessage(*m);
    delete m;
    // We need to find the request again in the list as it may have completed
    // by now and the request_info instance above may be invalid.
    request_info = GetPendingRequestInfo(request_id);
    if (!request_info) {
      // The recipient is gone, the messages won't be handled and
      // resources they might hold won't be released. Explicitly release
      // them from here so that they won't leak.
      ReleaseResourcesInMessageQueue(&q);
      return;
    }
    // If this request is deferred in the context of the above message, then
    // we should honor the same and stop dispatching further messages.
    if (request_info->is_deferred) {
      request_info->deferred_message_queue.swap(q);
      return;
    }
  }
}

void ResourceDispatcher::StartSync(
    std::unique_ptr<ResourceRequest> request,
    int routing_id,
    const url::Origin& frame_origin,
    const net::NetworkTrafficAnnotationTag& traffic_annotation,
    SyncLoadResponse* response,
    blink::WebURLRequest::LoadingIPCType ipc_type,
    mojom::URLLoaderFactory* url_loader_factory,
    std::vector<std::unique_ptr<URLLoaderThrottle>> throttles) {
  CheckSchemeForReferrerPolicy(*request);

  if (ipc_type == blink::WebURLRequest::LoadingIPCType::kMojo) {
    mojom::URLLoaderFactoryPtrInfo url_loader_factory_copy;
    url_loader_factory->Clone(mojo::MakeRequest(&url_loader_factory_copy));
    base::WaitableEvent event(base::WaitableEvent::ResetPolicy::MANUAL,
                              base::WaitableEvent::InitialState::NOT_SIGNALED);

    // Prepare the configured throttles for use on a separate thread.
    for (const auto& throttle : throttles)
      throttle->DetachFromCurrentSequence();

    // A task is posted to a separate thread to execute the request so that
    // this thread may block on a waitable event. It is safe to pass raw
    // pointers to |sync_load_response| and |event| as this stack frame will
    // survive until the request is complete.
    base::CreateSingleThreadTaskRunnerWithTraits({})->PostTask(
        FROM_HERE,
        base::BindOnce(&SyncLoadContext::StartAsyncWithWaitableEvent,
                       std::move(request), routing_id, frame_origin,
                       traffic_annotation, std::move(url_loader_factory_copy),
                       std::move(throttles), base::Unretained(response),
                       base::Unretained(&event)));

    event.Wait();
  } else {
    SyncLoadResult result;
    IPC::SyncMessage* msg = new ResourceHostMsg_SyncLoad(
        routing_id, MakeRequestID(), *request, &result);

    // NOTE: This may pump events (see RenderThread::Send).
    if (!message_sender_->Send(msg)) {
      response->error_code = net::ERR_FAILED;
      return;
    }

    response->error_code = result.error_code;
    response->url = result.final_url;
    response->headers = result.headers;
    response->mime_type = result.mime_type;
    response->charset = result.charset;
    response->request_time = result.request_time;
    response->response_time = result.response_time;
    response->load_timing = result.load_timing;
    response->devtools_info = result.devtools_info;
    response->data.swap(result.data);
    response->download_file_path = result.download_file_path;
    response->socket_address = result.socket_address;
    response->encoded_data_length = result.encoded_data_length;
    response->encoded_body_length = result.encoded_body_length;
  }
}

int ResourceDispatcher::StartAsync(
    std::unique_ptr<ResourceRequest> request,
    int routing_id,
    scoped_refptr<base::SingleThreadTaskRunner> loading_task_runner,
    const url::Origin& frame_origin,
    const net::NetworkTrafficAnnotationTag& traffic_annotation,
    bool is_sync,
    std::unique_ptr<RequestPeer> peer,
    blink::WebURLRequest::LoadingIPCType ipc_type,
    mojom::URLLoaderFactory* url_loader_factory,
    std::vector<std::unique_ptr<URLLoaderThrottle>> throttles,
    mojo::ScopedDataPipeConsumerHandle consumer_handle) {
  CheckSchemeForReferrerPolicy(*request);

  // Compute a unique request_id for this renderer process.
  int request_id = MakeRequestID();
  pending_requests_[request_id] = std::make_unique<PendingRequestInfo>(
      std::move(peer), request->resource_type, request->render_frame_id,
      frame_origin, request->url, request->method, request->referrer,
      request->download_to_file);

  if (resource_scheduling_filter_.get() && loading_task_runner) {
    resource_scheduling_filter_->SetRequestIdTaskRunner(request_id,
                                                        loading_task_runner);
  }

  scoped_refptr<base::SingleThreadTaskRunner> task_runner =
      loading_task_runner ? loading_task_runner : thread_task_runner_;

  if (consumer_handle.is_valid()) {
    pending_requests_[request_id]->url_loader_client =
        std::make_unique<URLLoaderClientImpl>(request_id, this, task_runner);

    task_runner->PostTask(
        FROM_HERE, base::BindOnce(&ResourceDispatcher::ContinueForNavigation,
                                  weak_factory_.GetWeakPtr(), request_id,
                                  base::Passed(std::move(consumer_handle))));

    return request_id;
  }

  if (ipc_type == blink::WebURLRequest::LoadingIPCType::kMojo) {
    scoped_refptr<base::SingleThreadTaskRunner> task_runner =
        loading_task_runner ? loading_task_runner : thread_task_runner_;
    std::unique_ptr<URLLoaderClientImpl> client(
        new URLLoaderClientImpl(request_id, this, task_runner));

    uint32_t options = mojom::kURLLoadOptionNone;
    // TODO(jam): use this flag for ResourceDispatcherHost code path once
    // MojoLoading is the only IPC code path.
    if (base::FeatureList::IsEnabled(features::kNetworkService) &&
        request->fetch_request_context_type != REQUEST_CONTEXT_TYPE_FETCH) {
      // MIME sniffing should be disabled for a request initiated by fetch().
      options |= mojom::kURLLoadOptionSniffMimeType;
    }
    if (is_sync)
      options |= mojom::kURLLoadOptionSynchronous;

    std::unique_ptr<ThrottlingURLLoader> url_loader =
        ThrottlingURLLoader::CreateLoaderAndStart(
            url_loader_factory, std::move(throttles), routing_id, request_id,
            options, *request, client.get(), traffic_annotation,
            std::move(task_runner));
    pending_requests_[request_id]->url_loader = std::move(url_loader);
    pending_requests_[request_id]->url_loader_client = std::move(client);
  } else {
    message_sender_->Send(new ResourceHostMsg_RequestResource(
        routing_id, request_id, *request,
        net::MutableNetworkTrafficAnnotationTag(traffic_annotation)));
  }

  return request_id;
}

void ResourceDispatcher::ToResourceResponseInfo(
    const PendingRequestInfo& request_info,
    const ResourceResponseHead& browser_info,
    ResourceResponseInfo* renderer_info) const {
  *renderer_info = browser_info;
  if (base::TimeTicks::IsConsistentAcrossProcesses() ||
      request_info.request_start.is_null() ||
      request_info.response_start.is_null() ||
      browser_info.request_start.is_null() ||
      browser_info.response_start.is_null() ||
      browser_info.load_timing.request_start.is_null()) {
    return;
  }
  InterProcessTimeTicksConverter converter(
      LocalTimeTicks::FromTimeTicks(request_info.request_start),
      LocalTimeTicks::FromTimeTicks(request_info.response_start),
      RemoteTimeTicks::FromTimeTicks(browser_info.request_start),
      RemoteTimeTicks::FromTimeTicks(browser_info.response_start));

  net::LoadTimingInfo* load_timing = &renderer_info->load_timing;
  RemoteToLocalTimeTicks(converter, &load_timing->request_start);
  RemoteToLocalTimeTicks(converter, &load_timing->proxy_resolve_start);
  RemoteToLocalTimeTicks(converter, &load_timing->proxy_resolve_end);
  RemoteToLocalTimeTicks(converter, &load_timing->connect_timing.dns_start);
  RemoteToLocalTimeTicks(converter, &load_timing->connect_timing.dns_end);
  RemoteToLocalTimeTicks(converter, &load_timing->connect_timing.connect_start);
  RemoteToLocalTimeTicks(converter, &load_timing->connect_timing.connect_end);
  RemoteToLocalTimeTicks(converter, &load_timing->connect_timing.ssl_start);
  RemoteToLocalTimeTicks(converter, &load_timing->connect_timing.ssl_end);
  RemoteToLocalTimeTicks(converter, &load_timing->send_start);
  RemoteToLocalTimeTicks(converter, &load_timing->send_end);
  RemoteToLocalTimeTicks(converter, &load_timing->receive_headers_end);
  RemoteToLocalTimeTicks(converter, &load_timing->push_start);
  RemoteToLocalTimeTicks(converter, &load_timing->push_end);
  RemoteToLocalTimeTicks(converter, &renderer_info->service_worker_start_time);
  RemoteToLocalTimeTicks(converter, &renderer_info->service_worker_ready_time);
}

base::TimeTicks ResourceDispatcher::ToRendererCompletionTime(
    const PendingRequestInfo& request_info,
    const base::TimeTicks& browser_completion_time) const {
  if (request_info.completion_time.is_null()) {
    return browser_completion_time;
  }

  // TODO(simonjam): The optimal lower bound should be the most recent value of
  // TimeTicks::Now() returned to WebKit. Is it worth trying to cache that?
  // Until then, |response_start| is used as it is the most recent value
  // returned for this request.
  int64_t result = std::max(browser_completion_time.ToInternalValue(),
                            request_info.response_start.ToInternalValue());
  result = std::min(result, request_info.completion_time.ToInternalValue());
  return base::TimeTicks::FromInternalValue(result);
}

base::TimeTicks ResourceDispatcher::ConsumeIOTimestamp() {
  if (io_timestamp_ == base::TimeTicks())
    return base::TimeTicks::Now();
  base::TimeTicks result = io_timestamp_;
  io_timestamp_ = base::TimeTicks();
  return result;
}

void ResourceDispatcher::ContinueForNavigation(
    int request_id,
    mojo::ScopedDataPipeConsumerHandle consumer_handle) {
  PendingRequestInfo* request_info = GetPendingRequestInfo(request_id);
  if (!request_info)
    return;

  URLLoaderClientImpl* client_ptr = request_info->url_loader_client.get();

  // Short circuiting call to OnReceivedResponse to immediately start
  // the request. ResourceResponseHead can be empty here because we
  // pull the StreamOverride's one in
  // WebURLLoaderImpl::Context::OnReceivedResponse.
  client_ptr->OnReceiveResponse(ResourceResponseHead(), base::nullopt,
                                mojom::DownloadedTempFilePtr());

  // Abort if the request is cancelled.
  if (!GetPendingRequestInfo(request_id))
    return;

  // Start streaming now.
  client_ptr->OnStartLoadingResponseBody(std::move(consumer_handle));

  // Abort if the request is cancelled.
  if (!GetPendingRequestInfo(request_id))
    return;

  // Call OnComplete now too, as it won't get called on the client.
  // TODO(kinuko): Fill this properly.
  network::URLLoaderCompletionStatus status;
  status.error_code = net::OK;
  status.exists_in_cache = false;
  status.completion_time = base::TimeTicks::Now();
  status.encoded_data_length = -1;
  status.encoded_body_length = -1;
  status.decoded_body_length = -1;
  client_ptr->OnComplete(status);
}

// static
bool ResourceDispatcher::IsResourceDispatcherMessage(
    const IPC::Message& message) {
  switch (message.type()) {
    case ResourceMsg_UploadProgress::ID:
    case ResourceMsg_ReceivedResponse::ID:
    case ResourceMsg_ReceivedCachedMetadata::ID:
    case ResourceMsg_ReceivedRedirect::ID:
    case ResourceMsg_SetDataBuffer::ID:
    case ResourceMsg_DataReceived::ID:
    case ResourceMsg_DataDownloaded::ID:
    case ResourceMsg_RequestComplete::ID:
      return true;

    default:
      break;
  }

  return false;
}

// static
void ResourceDispatcher::ReleaseResourcesInDataMessage(
    const IPC::Message& message) {
  base::PickleIterator iter(message);
  int request_id;
  if (!iter.ReadInt(&request_id)) {
    NOTREACHED() << "malformed resource message";
    return;
  }

  // If the message contains a shared memory handle, we should close the handle
  // or there will be a memory leak.
  if (message.type() == ResourceMsg_SetDataBuffer::ID) {
    base::SharedMemoryHandle shm_handle;
    if (IPC::ParamTraits<base::SharedMemoryHandle>::Read(&message,
                                                         &iter,
                                                         &shm_handle)) {
      if (base::SharedMemory::IsHandleValid(shm_handle))
        base::SharedMemory::CloseHandle(shm_handle);
    }
  }
}

// static
void ResourceDispatcher::ReleaseResourcesInMessageQueue(MessageQueue* queue) {
  while (!queue->empty()) {
    IPC::Message* message = queue->front();
    ReleaseResourcesInDataMessage(*message);
    queue->pop_front();
    delete message;
  }
}

void ResourceDispatcher::SetResourceSchedulingFilter(
    scoped_refptr<ResourceSchedulingFilter> resource_scheduling_filter) {
  resource_scheduling_filter_ = resource_scheduling_filter;
}

}  // namespace content