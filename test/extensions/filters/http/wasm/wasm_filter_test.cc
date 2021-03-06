#include <stdio.h>

#include "extensions/filters/http/wasm/wasm_filter.h"

#include "test/test_common/wasm_base.h"

using testing::_;
using testing::AtLeast;
using testing::Eq;
using testing::InSequence;
using testing::Invoke;
using testing::Return;
using testing::ReturnPointee;
using testing::ReturnRef;

MATCHER_P(MapEq, rhs, "") {
  const Envoy::ProtobufWkt::Struct& obj = arg;
  EXPECT_TRUE(rhs.size() > 0);
  for (auto const& entry : rhs) {
    EXPECT_EQ(obj.fields().at(entry.first).string_value(), entry.second);
  }
  return true;
}

using BufferFunction = std::function<void(::Envoy::Buffer::Instance&)>;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Wasm {

using envoy::config::core::v3::TrafficDirection;
using Envoy::Extensions::Common::Wasm::PluginSharedPtr;
using Envoy::Extensions::Common::Wasm::Wasm;
using Envoy::Extensions::Common::Wasm::WasmHandleSharedPtr;
using GrpcService = envoy::config::core::v3::GrpcService;
using WasmFilterConfig = envoy::extensions::filters::http::wasm::v3::Wasm;

class TestFilter : public Envoy::Extensions::Common::Wasm::Context {
public:
  TestFilter(Wasm* wasm, uint32_t root_context_id,
             Envoy::Extensions::Common::Wasm::PluginSharedPtr plugin)
      : Envoy::Extensions::Common::Wasm::Context(wasm, root_context_id, plugin) {}

  void log(const Http::RequestHeaderMap* request_headers,
           const Http::ResponseHeaderMap* response_headers,
           const Http::ResponseTrailerMap* response_trailers,
           const StreamInfo::StreamInfo& stream_info) override {
    Envoy::Extensions::Common::Wasm::Context::log(request_headers, response_headers,
                                                  response_trailers, stream_info);
  }
  MOCK_CONTEXT_LOG_;
};

class TestRoot : public Envoy::Extensions::Common::Wasm::Context {
public:
  TestRoot() {}

  MOCK_CONTEXT_LOG_;
};

class WasmHttpFilterTest
    : public Common::Wasm::WasmHttpFilterTestBase<testing::TestWithParam<std::string>> {
public:
  WasmHttpFilterTest() = default;
  ~WasmHttpFilterTest() override = default;

  void setup(const std::string& code, std::string root_id = "") {
    setupBase(GetParam(), code, std::make_unique<TestRoot>(), root_id);
  }
  void setupFilter(const std::string root_id = "") { setupFilterBase<TestFilter>(root_id); }

  TestRoot& root_context() { return *static_cast<TestRoot*>(root_context_); }
  TestFilter& filter() { return *static_cast<TestFilter*>(filter_.get()); }
};

INSTANTIATE_TEST_SUITE_P(Runtimes, WasmHttpFilterTest,
                         testing::Values("v8"
#if defined(ENVOY_WASM_WAVM)
                                         ,
                                         "wavm"
#endif
                                         ));

// Bad code in initial config.
TEST_P(WasmHttpFilterTest, BadCode) {
  EXPECT_THROW_WITH_MESSAGE(setup("bad code"), Common::Wasm::WasmException,
                            "Failed to initialize WASM code");
}

// Script touching headers only, request that is headers only.
TEST_P(WasmHttpFilterTest, HeadersOnlyRequestHeadersOnly) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/headers_cpp.wasm")));
  setupFilter();
  EXPECT_CALL(encoder_callbacks_, streamInfo()).WillRepeatedly(ReturnRef(request_stream_info_));
  EXPECT_CALL(filter(), log_(spdlog::level::debug, Eq(absl::string_view("onRequestHeaders 2"))));
  EXPECT_CALL(filter(), log_(spdlog::level::info, Eq(absl::string_view("header path /"))));
  EXPECT_CALL(filter(), log_(spdlog::level::warn, Eq(absl::string_view("onDone 2"))));
  Http::TestRequestHeaderMapImpl request_headers{{":path", "/"}, {"server", "envoy"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
  EXPECT_THAT(request_headers.get_("newheader"), Eq("newheadervalue"));
  EXPECT_THAT(request_headers.get_("server"), Eq("envoy-wasm"));
  filter_->onDestroy();
}

// Script touching headers only, request that is headers only.
TEST_P(WasmHttpFilterTest, HeadersOnlyRequestHeadersAndBody) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/headers_cpp.wasm")));
  setupFilter();
  EXPECT_CALL(filter(), log_(spdlog::level::debug, Eq(absl::string_view("onRequestHeaders 2"))));
  EXPECT_CALL(filter(), log_(spdlog::level::info, Eq(absl::string_view("header path /"))));
  EXPECT_CALL(filter(), log_(spdlog::level::err, Eq(absl::string_view("onRequestBody hello"))));
  EXPECT_CALL(filter(), log_(spdlog::level::warn, Eq(absl::string_view("onDone 2"))));
  Http::TestRequestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data, true));
  filter_->onDestroy();
}

TEST_P(WasmHttpFilterTest, HeadersStopAndContinue) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/headers_cpp.wasm")));
  setupFilter();
  EXPECT_CALL(encoder_callbacks_, streamInfo()).WillRepeatedly(ReturnRef(request_stream_info_));
  EXPECT_CALL(filter(), log_(spdlog::level::debug, Eq(absl::string_view("onRequestHeaders 2"))));
  EXPECT_CALL(filter(), log_(spdlog::level::info, Eq(absl::string_view("header path /"))));
  EXPECT_CALL(filter(), log_(spdlog::level::warn, Eq(absl::string_view("onDone 2"))));
  Http::TestRequestHeaderMapImpl request_headers{{":path", "/"}, {"server", "envoy-wasm-pause"}};
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, true));
  root_context_->onTick(0);
  EXPECT_THAT(request_headers.get_("newheader"), Eq("newheadervalue"));
  EXPECT_THAT(request_headers.get_("server"), Eq("envoy-wasm-continue"));
  filter_->onDestroy();
}

// Script that reads the body.
TEST_P(WasmHttpFilterTest, BodyRequestReadBody) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/body_cpp.wasm")));
  setupFilter();
  EXPECT_CALL(filter(), log_(spdlog::level::err, Eq(absl::string_view("onRequestBody hello"))));
  Http::TestRequestHeaderMapImpl request_headers{{":path", "/"}, {"x-test-operation", "ReadBody"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data, true));
  filter_->onDestroy();
}

// Script that prepends and appends to the body.
TEST_P(WasmHttpFilterTest, BodyRequestPrependAndAppendToBody) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/body_cpp.wasm")));
  setupFilter();
  EXPECT_CALL(filter(), log_(spdlog::level::err,
                             Eq(absl::string_view("onRequestBody prepend.hello.append"))));
  Http::TestRequestHeaderMapImpl request_headers{{":path", "/"},
                                                 {"x-test-operation", "PrependAndAppendToBody"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data, true));
  filter_->onDestroy();
}

// Script that replaces the body.
TEST_P(WasmHttpFilterTest, BodyRequestReplaceBody) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/body_cpp.wasm")));
  setupFilter();
  EXPECT_CALL(filter(), log_(spdlog::level::err, Eq(absl::string_view("onRequestBody replace"))));
  Http::TestRequestHeaderMapImpl request_headers{{":path", "/"},
                                                 {"x-test-operation", "ReplaceBody"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data, true));
  filter_->onDestroy();
}

// Script that removes the body.
TEST_P(WasmHttpFilterTest, BodyRequestRemoveBody) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/body_cpp.wasm")));
  setupFilter();
  EXPECT_CALL(filter(), log_(spdlog::level::err, Eq(absl::string_view("onRequestBody "))));
  Http::TestRequestHeaderMapImpl request_headers{{":path", "/"},
                                                 {"x-test-operation", "RemoveBody"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data, true));
  filter_->onDestroy();
}

// Script that buffers the body.
TEST_P(WasmHttpFilterTest, BodyRequestBufferBody) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/body_cpp.wasm")));
  setupFilter();

  Http::TestRequestHeaderMapImpl request_headers{{":path", "/"},
                                                 {"x-test-operation", "BufferBody"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl bufferedBody;
  EXPECT_CALL(decoder_callbacks_, decodingBuffer()).WillRepeatedly(Return(&bufferedBody));
  EXPECT_CALL(decoder_callbacks_, modifyDecodingBuffer(_))
      .WillRepeatedly(Invoke([&bufferedBody](BufferFunction f) { f(bufferedBody); }));

  Buffer::OwnedImpl data1("hello");
  bufferedBody.add(data1);
  EXPECT_CALL(filter(), log_(spdlog::level::err, Eq(absl::string_view("onRequestBody hello"))))
      .Times(1);
  EXPECT_EQ(Http::FilterDataStatus::StopIterationAndBuffer, filter_->decodeData(data1, false));

  Buffer::OwnedImpl data2(" again ");
  bufferedBody.add(data2);
  EXPECT_CALL(filter(),
              log_(spdlog::level::err, Eq(absl::string_view("onRequestBody hello again "))))
      .Times(1);
  EXPECT_EQ(Http::FilterDataStatus::StopIterationAndBuffer, filter_->decodeData(data2, false));

  EXPECT_CALL(filter(),
              log_(spdlog::level::err, Eq(absl::string_view("onRequestBody hello again hello"))))
      .Times(1);
  Buffer::OwnedImpl data3("hello");
  bufferedBody.add(data3);
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data3, true));

  // Verify that the response still works even though we buffered the request.
  Http::TestResponseHeaderMapImpl response_headers{{":status", "200"},
                                                   {"x-test-operation", "ReadBody"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));
  // Should not buffer this time
  EXPECT_CALL(filter(), log_(spdlog::level::err, Eq(absl::string_view("onRequestBody hello"))))
      .Times(2);
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(data1, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(data1, true));

  filter_->onDestroy();
}

// Script that prepends and appends to the buffered body.
TEST_P(WasmHttpFilterTest, BodyRequestPrependAndAppendToBufferedBody) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/body_cpp.wasm")));
  setupFilter();
  EXPECT_CALL(filter(), log_(spdlog::level::err,
                             Eq(absl::string_view("onRequestBody prepend.hello.append"))));
  Http::TestRequestHeaderMapImpl request_headers{
      {":path", "/"}, {"x-test-operation", "PrependAndAppendToBufferedBody"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data, true));
  filter_->onDestroy();
}

// Script that replaces the buffered body.
TEST_P(WasmHttpFilterTest, BodyRequestReplaceBufferedBody) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/body_cpp.wasm")));
  setupFilter();
  EXPECT_CALL(filter(), log_(spdlog::level::err, Eq(absl::string_view("onRequestBody replace"))));
  Http::TestRequestHeaderMapImpl request_headers{{":path", "/"},
                                                 {"x-test-operation", "ReplaceBufferedBody"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data, true));
  filter_->onDestroy();
}

// Script that removes the buffered body.
TEST_P(WasmHttpFilterTest, BodyRequestRemoveBufferedBody) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/body_cpp.wasm")));
  setupFilter();
  EXPECT_CALL(filter(), log_(spdlog::level::err, Eq(absl::string_view("onRequestBody "))));
  Http::TestRequestHeaderMapImpl request_headers{{":path", "/"},
                                                 {"x-test-operation", "RemoveBufferedBody"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data, true));
  filter_->onDestroy();
}

// Script that buffers the first part of the body and streams the rest
TEST_P(WasmHttpFilterTest, BodyRequestBufferThenStreamBody) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/body_cpp.wasm")));
  setupFilter();

  Http::TestRequestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));

  Buffer::OwnedImpl bufferedBody;
  EXPECT_CALL(decoder_callbacks_, decodingBuffer()).WillRepeatedly(Return(&bufferedBody));
  EXPECT_CALL(decoder_callbacks_, modifyDecodingBuffer(_))
      .WillRepeatedly(Invoke([&bufferedBody](BufferFunction f) { f(bufferedBody); }));

  Http::TestResponseHeaderMapImpl response_headers{{":status", "200"},
                                                   {"x-test-operation", "BufferTwoBodies"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));

  Buffer::OwnedImpl data1("hello");
  EXPECT_CALL(filter(), log_(spdlog::level::err, Eq(absl::string_view("onRequestBody hello"))))
      .Times(1);
  EXPECT_EQ(Http::FilterDataStatus::StopIterationAndBuffer, filter_->decodeData(data1, false));
  bufferedBody.add(data1);

  Buffer::OwnedImpl data2(", there, ");
  bufferedBody.add(data2);
  EXPECT_CALL(filter(),
              log_(spdlog::level::err, Eq(absl::string_view("onRequestBody hello, there, "))))
      .Times(1);
  EXPECT_EQ(Http::FilterDataStatus::StopIterationAndBuffer, filter_->decodeData(data2, false));

  // Previous callbacks returned "Buffer" so we have buffered so far
  Buffer::OwnedImpl data3("world!");
  bufferedBody.add(data3);
  EXPECT_CALL(filter(),
              log_(spdlog::level::err, Eq(absl::string_view("onRequestBody hello, there, world!"))))
      .Times(1);
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data3, false));

  // Last callback returned "continue" so we just see individual chunks.
  Buffer::OwnedImpl data4("So it's ");
  EXPECT_CALL(filter(), log_(spdlog::level::err, Eq(absl::string_view("onRequestBody So it's "))))
      .Times(1);
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data4, false));

  Buffer::OwnedImpl data5("goodbye, then!");
  EXPECT_CALL(filter(),
              log_(spdlog::level::err, Eq(absl::string_view("onRequestBody goodbye, then!"))))
      .Times(1);
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data5, true));

  filter_->onDestroy();
}

// Script testing AccessLog::Instance::log.
TEST_P(WasmHttpFilterTest, AccessLog) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/headers_cpp.wasm")));
  setupFilter();
  EXPECT_CALL(filter(), log_(spdlog::level::debug, Eq(absl::string_view("onRequestHeaders 2"))));
  EXPECT_CALL(filter(), log_(spdlog::level::info, Eq(absl::string_view("header path /"))));
  EXPECT_CALL(filter(), log_(spdlog::level::err, Eq(absl::string_view("onRequestBody hello"))));
  EXPECT_CALL(filter(), log_(spdlog::level::warn, Eq(absl::string_view("onLog 2 /"))));
  EXPECT_CALL(filter(), log_(spdlog::level::warn, Eq(absl::string_view("onDone 2"))));

  Http::TestRequestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data, true));
  filter_->onDestroy();
  StreamInfo::MockStreamInfo log_stream_info;
  filter_->log(&request_headers, nullptr, nullptr, log_stream_info);
}

TEST_P(WasmHttpFilterTest, AsyncCall) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/async_call_cpp.wasm")));
  setupFilter();

  Http::TestRequestHeaderMapImpl request_headers{{":path", "/"}};
  Http::MockAsyncClientRequest request(&cluster_manager_.async_client_);
  Http::AsyncClient::Callbacks* callbacks = nullptr;
  EXPECT_CALL(cluster_manager_, get(Eq("cluster")));
  EXPECT_CALL(cluster_manager_, httpAsyncClientForCluster("cluster"));
  EXPECT_CALL(cluster_manager_.async_client_, send_(_, _, _))
      .WillOnce(
          Invoke([&](Http::RequestMessagePtr& message, Http::AsyncClient::Callbacks& cb,
                     const Http::AsyncClient::RequestOptions&) -> Http::AsyncClient::Request* {
            EXPECT_EQ((Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                                      {":path", "/"},
                                                      {":authority", "foo"},
                                                      {"content-length", "11"}}),
                      message->headers());
            EXPECT_EQ((Http::TestRequestTrailerMapImpl{{"trail", "cow"}}), *message->trailers());
            callbacks = &cb;
            return &request;
          }));

  EXPECT_CALL(filter(), log_(spdlog::level::debug, Eq("response")));
  EXPECT_CALL(filter(), log_(spdlog::level::info, Eq(":status -> 200")));
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));

  Http::ResponseMessagePtr response_message(new Http::ResponseMessageImpl(
      Http::ResponseHeaderMapPtr{new Http::TestResponseHeaderMapImpl{{":status", "200"}}}));
  response_message->body().reset(new Buffer::OwnedImpl("response"));

  EXPECT_NE(callbacks, nullptr);
  if (callbacks) {
    callbacks->onSuccess(request, std::move(response_message));
  }
}

TEST_P(WasmHttpFilterTest, AsyncCallAfterDestroyed) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/async_call_cpp.wasm")));
  setupFilter();

  Http::TestRequestHeaderMapImpl request_headers{{":path", "/"}};
  Http::MockAsyncClientRequest request(&cluster_manager_.async_client_);
  Http::AsyncClient::Callbacks* callbacks = nullptr;
  EXPECT_CALL(cluster_manager_, get(Eq("cluster")));
  EXPECT_CALL(cluster_manager_, httpAsyncClientForCluster("cluster"));
  EXPECT_CALL(cluster_manager_.async_client_, send_(_, _, _))
      .WillOnce(
          Invoke([&](Http::RequestMessagePtr& message, Http::AsyncClient::Callbacks& cb,
                     const Http::AsyncClient::RequestOptions&) -> Http::AsyncClient::Request* {
            EXPECT_EQ((Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                                      {":path", "/"},
                                                      {":authority", "foo"},
                                                      {"content-length", "11"}}),
                      message->headers());
            EXPECT_EQ((Http::TestRequestTrailerMapImpl{{"trail", "cow"}}), *message->trailers());
            callbacks = &cb;
            return &request;
          }));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));

  EXPECT_CALL(request, cancel()).WillOnce([&]() { callbacks = nullptr; });

  // Destroy the Context, Plugin and VM.
  filter_.reset();
  plugin_.reset();
  wasm_.reset();

  Http::ResponseMessagePtr response_message(new Http::ResponseMessageImpl(
      Http::ResponseHeaderMapPtr{new Http::TestResponseHeaderMapImpl{{":status", "200"}}}));
  response_message->body().reset(new Buffer::OwnedImpl("response"));

  // (Don't) Make the callback on the destroyed VM.
  EXPECT_EQ(callbacks, nullptr);
  if (callbacks) {
    callbacks->onSuccess(request, std::move(response_message));
  }
}

TEST_P(WasmHttpFilterTest, GrpcCall) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/grpc_call_cpp.wasm")));
  setupFilter();
  Grpc::MockAsyncRequest request;
  Grpc::RawAsyncRequestCallbacks* callbacks = nullptr;
  Grpc::MockAsyncClientManager client_manager;
  auto client_factory = std::make_unique<Grpc::MockAsyncClientFactory>();
  auto async_client = std::make_unique<Grpc::MockAsyncClient>();
  Tracing::Span* parent_span{};
  EXPECT_CALL(*async_client, sendRaw(_, _, _, _, _, _))
      .WillOnce(Invoke([&](absl::string_view service_full_name, absl::string_view method_name,
                           Buffer::InstancePtr&& message, Grpc::RawAsyncRequestCallbacks& cb,
                           Tracing::Span& span, const Http::AsyncClient::RequestOptions& options)
                           -> Grpc::AsyncRequest* {
        EXPECT_EQ(service_full_name, "service");
        EXPECT_EQ(method_name, "method");
        ProtobufWkt::Value value;
        EXPECT_TRUE(value.ParseFromArray(message->linearize(message->length()), message->length()));
        EXPECT_EQ(value.string_value(), "request");
        callbacks = &cb;
        parent_span = &span;
        EXPECT_EQ(options.timeout->count(), 1000);
        return &request;
      }));
  EXPECT_CALL(*client_factory, create).WillOnce(Invoke([&]() -> Grpc::RawAsyncClientPtr {
    return std::move(async_client);
  }));
  EXPECT_CALL(cluster_manager_, grpcAsyncClientManager())
      .WillOnce(Invoke([&]() -> Grpc::AsyncClientManager& { return client_manager; }));
  EXPECT_CALL(client_manager, factoryForGrpcService(_, _, _))
      .WillOnce(Invoke([&](const GrpcService&, Stats::Scope&, bool) -> Grpc::AsyncClientFactoryPtr {
        return std::move(client_factory);
      }));
  EXPECT_CALL(root_context(), log_(spdlog::level::debug, Eq("response")));
  Http::TestRequestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));

  ProtobufWkt::Value value;
  value.set_string_value("response");
  std::string response_string;
  EXPECT_TRUE(value.SerializeToString(&response_string));
  auto response = std::make_unique<Buffer::OwnedImpl>(response_string);
  EXPECT_NE(callbacks, nullptr);
  NiceMock<Tracing::MockSpan> span;
  if (callbacks) {
    callbacks->onSuccessRaw(std::move(response), span);
  }
}

TEST_P(WasmHttpFilterTest, GrpcCallAfterDestroyed) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/grpc_call_cpp.wasm")));
  setupFilter();
  Grpc::MockAsyncRequest request;
  Grpc::RawAsyncRequestCallbacks* callbacks = nullptr;
  Grpc::MockAsyncClientManager client_manager;
  auto client_factory = std::make_unique<Grpc::MockAsyncClientFactory>();
  auto async_client = std::make_unique<Grpc::MockAsyncClient>();
  Tracing::Span* parent_span{};
  EXPECT_CALL(*async_client, sendRaw(_, _, _, _, _, _))
      .WillOnce(Invoke([&](absl::string_view service_full_name, absl::string_view method_name,
                           Buffer::InstancePtr&& message, Grpc::RawAsyncRequestCallbacks& cb,
                           Tracing::Span& span, const Http::AsyncClient::RequestOptions& options)
                           -> Grpc::AsyncRequest* {
        EXPECT_EQ(service_full_name, "service");
        EXPECT_EQ(method_name, "method");
        ProtobufWkt::Value value;
        EXPECT_TRUE(value.ParseFromArray(message->linearize(message->length()), message->length()));
        EXPECT_EQ(value.string_value(), "request");
        callbacks = &cb;
        parent_span = &span;
        EXPECT_EQ(options.timeout->count(), 1000);
        return &request;
      }));
  EXPECT_CALL(*client_factory, create).WillOnce(Invoke([&]() -> Grpc::RawAsyncClientPtr {
    return std::move(async_client);
  }));
  EXPECT_CALL(cluster_manager_, grpcAsyncClientManager())
      .WillOnce(Invoke([&]() -> Grpc::AsyncClientManager& { return client_manager; }));
  EXPECT_CALL(client_manager, factoryForGrpcService(_, _, _))
      .WillOnce(Invoke([&](const GrpcService&, Stats::Scope&, bool) -> Grpc::AsyncClientFactoryPtr {
        return std::move(client_factory);
      }));
  Http::TestRequestHeaderMapImpl request_headers{{":path", "/"}};

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));

  EXPECT_CALL(request, cancel()).WillOnce([&]() { callbacks = nullptr; });

  // Destroy the Context, Plugin and VM.
  filter_.reset();
  plugin_.reset();
  wasm_.reset();

  ProtobufWkt::Value value;
  value.set_string_value("response");
  std::string response_string;
  EXPECT_TRUE(value.SerializeToString(&response_string));
  auto response = std::make_unique<Buffer::OwnedImpl>(response_string);
  EXPECT_EQ(callbacks, nullptr);
  NiceMock<Tracing::MockSpan> span;
  if (callbacks) {
    callbacks->onSuccessRaw(std::move(response), span);
  }
}

TEST_P(WasmHttpFilterTest, Metadata) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/metadata_cpp.wasm")));
  setupFilter();
  envoy::config::core::v3::Node node_data;
  ProtobufWkt::Value node_val;
  node_val.set_string_value("wasm_node_get_value");
  (*node_data.mutable_metadata()->mutable_fields())["wasm_node_get_key"] = node_val;
  EXPECT_CALL(local_info_, node()).WillRepeatedly(ReturnRef(node_data));
  EXPECT_CALL(root_context(),
              log_(spdlog::level::debug, Eq(absl::string_view("onTick wasm_node_get_value"))));

  EXPECT_CALL(filter(),
              log_(spdlog::level::err, Eq(absl::string_view("onRequestBody wasm_node_get_value"))));
  EXPECT_CALL(filter(), log_(spdlog::level::info, Eq(absl::string_view("header path /"))));
  EXPECT_CALL(filter(), log_(spdlog::level::warn, Eq(absl::string_view("onLog 2 /"))));
  EXPECT_CALL(filter(), log_(spdlog::level::warn, Eq(absl::string_view("onDone 2"))));
  EXPECT_CALL(filter(),
              log_(spdlog::level::trace,
                   Eq(absl::string_view("Struct wasm_request_get_value wasm_request_get_value"))));
  EXPECT_CALL(filter(), log_(spdlog::level::info, Eq(absl::string_view("server is envoy-wasm"))));

  request_stream_info_.metadata_.mutable_filter_metadata()->insert(
      Protobuf::MapPair<std::string, ProtobufWkt::Struct>(
          HttpFilters::HttpFilterNames::get().Wasm,
          MessageUtil::keyValueStruct("wasm_request_get_key", "wasm_request_get_value")));

  root_context_->onTick(0);

  EXPECT_CALL(encoder_callbacks_, streamInfo()).WillRepeatedly(ReturnRef(request_stream_info_));
  absl::optional<std::chrono::nanoseconds> dur = std::chrono::nanoseconds(15000000);
  EXPECT_CALL(request_stream_info_, requestComplete()).WillRepeatedly(Return(dur));
  EXPECT_CALL(filter(), log_(spdlog::level::info, Eq(absl::string_view("duration is 15000000"))));
  EXPECT_CALL(filter(), log_(spdlog::level::info, Eq(absl::string_view("grpc service: test"))));
  Http::TestRequestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data, true));
  filter_->onDestroy();

  StreamInfo::MockStreamInfo log_stream_info;
  filter_->log(&request_headers, nullptr, nullptr, log_stream_info);

  const auto& result = request_stream_info_.filterState()->getDataReadOnly<Common::Wasm::WasmState>(
      "wasm.wasm_request_set_key");
  EXPECT_EQ("wasm_request_set_value", result.value());
}

// Null VM Plugin, headers only.
TEST_F(WasmHttpFilterTest, NullPluginRequestHeadersOnly) {
  setupBase("null", "HttpFilterTestPlugin", std::make_unique<TestRoot>());
  setupFilter();
  EXPECT_CALL(filter(), log_(spdlog::level::debug, Eq(absl::string_view("onRequestHeaders 2"))));
  EXPECT_CALL(filter(), log_(spdlog::level::info, Eq(absl::string_view("header path /"))));
  EXPECT_CALL(filter(), log_(spdlog::level::warn, Eq(absl::string_view("onDone 2"))));
  Http::TestRequestHeaderMapImpl request_headers{{":path", "/"}, {"server", "envoy"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
  EXPECT_THAT(request_headers.get_("newheader"), Eq("newheadervalue"));
  EXPECT_THAT(request_headers.get_("server"), Eq("envoy-wasm"));
  filter_->onDestroy();
}

TEST_F(WasmHttpFilterTest, NullVmResolver) {
  setupBase("null", "HttpFilterTestPlugin", std::make_unique<TestRoot>());
  setupFilter();
  envoy::config::core::v3::Node node_data;
  ProtobufWkt::Value node_val;
  node_val.set_string_value("sample_data");
  (*node_data.mutable_metadata()->mutable_fields())["istio.io/metadata"] = node_val;
  EXPECT_CALL(local_info_, node()).WillRepeatedly(ReturnRef(node_data));

  request_stream_info_.metadata_.mutable_filter_metadata()->insert(
      Protobuf::MapPair<std::string, ProtobufWkt::Struct>(
          HttpFilters::HttpFilterNames::get().Wasm,
          MessageUtil::keyValueStruct("wasm_request_get_key", "wasm_request_get_value")));
  EXPECT_CALL(request_stream_info_, responseCode()).WillRepeatedly(Return(403));
  EXPECT_CALL(encoder_callbacks_, streamInfo()).WillRepeatedly(ReturnRef(request_stream_info_));
  EXPECT_CALL(filter(), log_(spdlog::level::debug, Eq(absl::string_view("onRequestHeaders 2"))));
  EXPECT_CALL(filter(),
              log_(spdlog::level::info, Eq(absl::string_view("header path /test_context"))));

  // test outputs should match inputs
  EXPECT_CALL(filter(),
              log_(spdlog::level::warn, Eq(absl::string_view("request.path: /test_context"))));
  EXPECT_CALL(filter(),
              log_(spdlog::level::warn, Eq(absl::string_view("node.metadata: sample_data"))));
  EXPECT_CALL(filter(),
              log_(spdlog::level::warn, Eq(absl::string_view("metadata: wasm_request_get_value"))));
  EXPECT_CALL(filter(), log_(spdlog::level::warn, Eq(absl::string_view("response.code: 403"))));
  EXPECT_CALL(filter(), log_(spdlog::level::warn, Eq(absl::string_view("state: wasm_value"))));

  root_context_->onTick(0);
  Http::TestRequestHeaderMapImpl request_headers{{":path", "/test_context"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
  StreamInfo::MockStreamInfo log_stream_info;
  filter_->log(&request_headers, nullptr, nullptr, log_stream_info);
}

TEST_P(WasmHttpFilterTest, SharedData) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/shared_cpp.wasm")));
  setupFilter();
  EXPECT_CALL(filter(), log_(spdlog::level::info, Eq(absl::string_view("set CasMismatch"))));
  EXPECT_CALL(filter(),
              log_(spdlog::level::debug, Eq(absl::string_view("get 1 shared_data_value1"))));
  EXPECT_CALL(filter(),
              log_(spdlog::level::warn, Eq(absl::string_view("get 2 shared_data_value2"))));

  Http::TestRequestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
  StreamInfo::MockStreamInfo log_stream_info;
  filter_->log(&request_headers, nullptr, nullptr, log_stream_info);
}

TEST_P(WasmHttpFilterTest, SharedQueue) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/queue_cpp.wasm")));
  setupFilter();
  EXPECT_CALL(filter(),
              log_(spdlog::level::warn, Eq(absl::string_view("onRequestHeaders enqueue Ok"))));
  EXPECT_CALL(root_context(), log_(spdlog::level::info, Eq(absl::string_view("onQueueReady"))));
  EXPECT_CALL(root_context(), log_(spdlog::level::debug, Eq(absl::string_view("data data1 Ok"))));
  Http::TestRequestHeaderMapImpl request_headers{{":path", "/"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
  auto token = Common::Wasm::resolveQueueForTest("vm_id", "my_shared_queue");
  wasm_->wasm()->queueReady(root_context_->id(), token);
}

// Script using a root_id which is not registered.
TEST_P(WasmHttpFilterTest, RootIdNotRegistered) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/root_id_cpp.wasm")));
  setupFilter();
  Http::TestRequestHeaderMapImpl request_headers;
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
}

// Script using an explicit root_id which is registered.
TEST_P(WasmHttpFilterTest, RootId1) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
            "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/root_id_cpp.wasm")),
        "context1");
  setupFilter("context1");
  EXPECT_CALL(filter(), log_(spdlog::level::debug, Eq(absl::string_view("onRequestHeaders1 2"))));
  Http::TestRequestHeaderMapImpl request_headers;
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
}

// Script using an explicit root_id which is registered.
TEST_P(WasmHttpFilterTest, RootId2) {
  setup(TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
            "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/root_id_cpp.wasm")),
        "context2");
  setupFilter("context2");
  EXPECT_CALL(filter(), log_(spdlog::level::debug, Eq(absl::string_view("onRequestHeaders2 2"))));
  Http::TestRequestHeaderMapImpl request_headers;
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
}

} // namespace Wasm
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
