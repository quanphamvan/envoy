#include "envoy/api/v2/eds.pb.h"
#include "envoy/api/v2/filter/network/http_connection_manager.pb.h"

#include "common/config/metadata.h"
#include "common/config/resources.h"
#include "common/protobuf/protobuf.h"

#include "test/integration/http_integration.h"
#include "test/test_common/network_utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace {
void disableHeaderValueOptionAppend(
    Protobuf::RepeatedPtrField<envoy::api::v2::HeaderValueOption>& header_value_options) {
  for (auto& i : header_value_options) {
    i.mutable_append()->set_value(false);
  }
}

const std::string http_connection_mgr_config = R"EOF(
http_filters:
  - name: envoy.router
codec_type: HTTP1
stat_prefix: header_test
route_config:
  virtual_hosts:
    - name: no-headers
      domains: ["no-headers.com"]
      routes:
        - match: { prefix: "/" }
          route: { cluster: "cluster_0" }
    - name: vhost-headers
      domains: ["vhost-headers.com"]
      request_headers_to_add:
        - header:
            key: "x-vhost-request"
            value: "vhost"
      response_headers_to_add:
        - header:
            key: "x-vhost-response"
            value: "vhost"
      response_headers_to_remove: ["x-vhost-response-remove"]
      routes:
        - match: { prefix: "/vhost-only" }
          route: { cluster: "cluster_0" }
        - match: { prefix: "/vhost-and-route" }
          route:
            cluster: cluster_0
            request_headers_to_add:
              - header:
                  key: "x-route-request"
                  value: "route"
            response_headers_to_add:
              - header:
                  key: "x-route-response"
                  value: "route"
            response_headers_to_remove: ["x-route-response-remove"]
    - name: route-headers
      domains: ["route-headers.com"]
      routes:
        - match: { prefix: "/route-only" }
          route:
            cluster: cluster_0
            request_headers_to_add:
              - header:
                  key: "x-route-request"
                  value: "route"
            response_headers_to_add:
              - header:
                  key: "x-route-response"
                  value: "route"
            response_headers_to_remove: ["x-route-response-remove"]
)EOF";

} // namespace

class HeaderIntegrationTest : public HttpIntegrationTest,
                              public testing::TestWithParam<Network::Address::IpVersion> {
public:
  HeaderIntegrationTest() : HttpIntegrationTest(Http::CodecClient::Type::HTTP1, GetParam()) {}

  enum HeaderMode {
    Append = 1,
    Replace = 2,
  };

  void TearDown() override {
    test_server_.reset();
    fake_upstreams_.clear();
  }

  void addHeader(Protobuf::RepeatedPtrField<envoy::api::v2::HeaderValueOption>* field,
                 const std::string& key, const std::string& value, bool append) {
    envoy::api::v2::HeaderValueOption* header_value_option = field->Add();
    auto* mutable_header = header_value_option->mutable_header();
    mutable_header->set_key(key);
    mutable_header->set_value(value);
    header_value_option->mutable_append()->set_value(append);
  }

  void prepareEDS() {
    config_helper_.addConfigModifier([&](envoy::config::bootstrap::v2::Bootstrap& bootstrap) {
      auto* static_resources = bootstrap.mutable_static_resources();
      ASSERT(static_resources->clusters_size() == 1);

      static_resources->mutable_clusters(0)->CopyFrom(
          TestUtility::parseYaml<envoy::api::v2::Cluster>(
              R"EOF(
                  name: cluster_0
                  type: EDS
                  eds_cluster_config:
                    eds_config:
                      api_config_source:
                        cluster_names: "eds-cluster"
                        api_type: GRPC
              )EOF"));

      // TODO(zuercher): Make ConfigHelper EDS-aware and get rid of this hack:
      // ConfigHelper expects the number of ports assigned to upstreams to match the number of
      // static hosts assigned ports. So give it a place to put the port for our EDS host. This
      // host must come before the eds-cluster's host to keep the upstreams and ports in the same
      // order.
      static_resources->add_clusters()->CopyFrom(
          TestUtility::parseYaml<envoy::api::v2::Cluster>(fmt::format(
              R"EOF(
                      name: unused-cluster
                      type: STATIC
                      lb_policy: ROUND_ROBIN
                      hosts:
                        - socket_address:
                            address: {}
                            port_value: 0
                  )EOF",
              Network::Test::getLoopbackAddressString(version_))));

      static_resources->add_clusters()->CopyFrom(
          TestUtility::parseYaml<envoy::api::v2::Cluster>(fmt::format(
              R"EOF(
                      name: eds-cluster
                      type: STATIC
                      lb_policy: ROUND_ROBIN
                      http2_protocol_options: {{}}
                      connect_timeout: 5s
                      hosts:
                        - socket_address:
                            address: {}
                            port_value: 0
                  )EOF",
              Network::Test::getLoopbackAddressString(version_))));
    });

    use_eds_ = true;
  }

  void initializeFilter(HeaderMode mode, bool include_route_config_headers) {
    config_helper_.addConfigModifier(
        [&](envoy::api::v2::filter::network::HttpConnectionManager& hcm) {
          // Overwrite default config with our own.
          MessageUtil::loadFromYaml(http_connection_mgr_config, hcm);

          const bool append = mode == HeaderMode::Append;

          auto* route_config = hcm.mutable_route_config();
          if (include_route_config_headers) {
            // Configure route config level headers.
            addHeader(route_config->mutable_response_headers_to_add(), "x-routeconfig-response",
                      "routeconfig", append);
            route_config->add_response_headers_to_remove("x-routeconfig-response-remove");
            addHeader(route_config->mutable_request_headers_to_add(), "x-routeconfig-request",
                      "routeconfig", append);
          }

          if (use_eds_) {
            addHeader(route_config->mutable_response_headers_to_add(), "x-routeconfig-dynamic",
                      "%UPSTREAM_METADATA([\"test.namespace\", \"key\"])%", append);

            // Iterate over VirtualHosts and nested Routes, adding a dynamic response header.
            for (auto& vhost : *route_config->mutable_virtual_hosts()) {
              addHeader(vhost.mutable_response_headers_to_add(), "x-vhost-dynamic",
                        "%UPSTREAM_METADATA([\"test.namespace\", \"key\"])%", append);

              for (auto& rte : *vhost.mutable_routes()) {
                if (rte.has_route()) {
                  addHeader(rte.mutable_route()->mutable_response_headers_to_add(),
                            "x-route-dynamic", "%UPSTREAM_METADATA([\"test.namespace\", \"key\"])%",
                            append);
                }
              }
            }
          }

          if (append) {
            // The config specifies append by default: no modifications needed.
            return;
          }

          // Iterate over VirtualHosts and nested Routes, disabling header append.
          for (auto& vhost : *route_config->mutable_virtual_hosts()) {
            disableHeaderValueOptionAppend(*vhost.mutable_request_headers_to_add());
            disableHeaderValueOptionAppend(*vhost.mutable_response_headers_to_add());

            for (auto& rte : *vhost.mutable_routes()) {
              if (rte.has_route()) {
                disableHeaderValueOptionAppend(
                    *rte.mutable_route()->mutable_request_headers_to_add());
                disableHeaderValueOptionAppend(
                    *rte.mutable_route()->mutable_response_headers_to_add());
              }
            }
          }
        });

    initialize();
  }

  void createUpstreams() override {
    HttpIntegrationTest::createUpstreams();

    if (use_eds_) {
      fake_upstreams_.emplace_back(new FakeUpstream(0, FakeHttpConnection::Type::HTTP2, version_));
    }
  }

  void initialize() override {
    if (use_eds_) {
      pre_worker_start_test_steps_ = [this]() {
        eds_connection_ = fake_upstreams_[1]->waitForHttpConnection(*dispatcher_);
        eds_stream_ = eds_connection_->waitForNewStream(*dispatcher_);
        eds_stream_->startGrpcStream();

        envoy::api::v2::DiscoveryRequest discovery_request;
        eds_stream_->waitForGrpcMessage(*dispatcher_, discovery_request);

        envoy::api::v2::DiscoveryResponse discovery_response;
        discovery_response.set_version_info("1");
        discovery_response.set_type_url(Config::TypeUrl::get().ClusterLoadAssignment);

        envoy::api::v2::ClusterLoadAssignment cluster_load_assignment =
            TestUtility::parseYaml<envoy::api::v2::ClusterLoadAssignment>(fmt::format(
                R"EOF(
                cluster_name: cluster_0
                endpoints:
                - lb_endpoints:
                  - endpoint:
                      address:
                        socket_address:
                          address: {}
                          port_value: {}
                    metadata:
                      filter_metadata:
                        test.namespace:
                          key: metadata-value
              )EOF",
                Network::Test::getLoopbackAddressString(GetParam()),
                fake_upstreams_[0]->localAddress()->ip()->port()));

        discovery_response.add_resources()->PackFrom(cluster_load_assignment);
        eds_stream_->sendGrpcMessage(discovery_response);

        // Wait for the next request to make sure the first response was consumed.
        eds_stream_->waitForGrpcMessage(*dispatcher_, discovery_request);
      };
    }

    HttpIntegrationTest::initialize();
  }

protected:
  void performRequest(Http::TestHeaderMapImpl&& request_headers,
                      Http::TestHeaderMapImpl&& expected_request_headers,
                      Http::TestHeaderMapImpl&& response_headers,
                      Http::TestHeaderMapImpl&& expected_response_headers) {
    registerTestServerPorts({"http"});

    codec_client_ = makeHttpConnection(makeClientConnection(lookupPort("http")));
    codec_client_->makeHeaderOnlyRequest(request_headers, *response_);
    waitForNextUpstreamRequest();

    upstream_request_->encodeHeaders(response_headers, true);
    response_->waitForEndStream();

    compareHeaders(upstream_request_->headers(), expected_request_headers);
    compareHeaders(response_->headers(), expected_response_headers);
  }

  void compareHeaders(Http::TestHeaderMapImpl&& headers,
                      Http::TestHeaderMapImpl& expected_headers) {
    headers.remove(Envoy::Http::LowerCaseString{"content-length"});
    headers.remove(Envoy::Http::LowerCaseString{"date"});
    headers.remove(Envoy::Http::LowerCaseString{"x-envoy-expected-rq-timeout-ms"});
    headers.remove(Envoy::Http::LowerCaseString{"x-envoy-upstream-service-time"});
    headers.remove(Envoy::Http::LowerCaseString{"x-forwarded-proto"});
    headers.remove(Envoy::Http::LowerCaseString{"x-request-id"});
    headers.remove(Envoy::Http::LowerCaseString{"x-envoy-internal"});

    EXPECT_EQ(expected_headers, headers);
  }

  bool use_eds_{false};
  FakeHttpConnectionPtr eds_connection_;
  FakeStreamPtr eds_stream_;
};

INSTANTIATE_TEST_CASE_P(IpVersions, HeaderIntegrationTest,
                        testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

// Validate that downstream request headers are passed upstream and upstream response headers are
// passed downstream.
TEST_P(HeaderIntegrationTest, TestRequestAndResponseHeaderPassThrough) {
  initializeFilter(HeaderMode::Append, false);
  performRequest(
      Http::TestHeaderMapImpl{
          {":method", "GET"},
          {":path", "/"},
          {":scheme", "http"},
          {":authority", "no-headers.com"},
          {"x-request-foo", "downstram"},
      },
      Http::TestHeaderMapImpl{
          {":authority", "no-headers.com"},
          {"x-request-foo", "downstram"},
          {":path", "/"},
          {":method", "GET"},
      },
      Http::TestHeaderMapImpl{
          {"server", "envoy"},
          {"content-length", "0"},
          {":status", "200"},
          {"x-return-foo", "upstream"},
      },
      Http::TestHeaderMapImpl{
          {"server", "envoy"},
          {"x-return-foo", "upstream"},
          {":status", "200"},
      });
}

// Validates the virtual host appends upstream request headers and appends/removes upstream
// response headers.
TEST_P(HeaderIntegrationTest, TestVirtualHostAppendHeaderManipulation) {
  initializeFilter(HeaderMode::Append, false);
  performRequest(
      Http::TestHeaderMapImpl{
          {":method", "GET"},
          {":path", "/vhost-only"},
          {":scheme", "http"},
          {":authority", "vhost-headers.com"},
          {"x-vhost-request", "downstream"},
      },
      Http::TestHeaderMapImpl{
          {":authority", "vhost-headers.com"},
          {"x-vhost-request", "downstream"},
          {"x-vhost-request", "vhost"},
          {":path", "/vhost-only"},
          {":method", "GET"},
      },
      Http::TestHeaderMapImpl{
          {"server", "envoy"},
          {"content-length", "0"},
          {":status", "200"},
          {"x-vhost-response", "upstream"},
          {"x-vhost-response-remove", "upstream"},
      },
      Http::TestHeaderMapImpl{
          {"server", "envoy"},
          {"x-vhost-response", "upstream"},
          {"x-vhost-response", "vhost"},
          {":status", "200"},
      });
}

// Validates the virtual host replaces request headers and replaces upstream response headers.
TEST_P(HeaderIntegrationTest, TestVirtualHostReplaceHeaderManipulation) {
  initializeFilter(HeaderMode::Replace, false);
  performRequest(
      Http::TestHeaderMapImpl{
          {":method", "GET"},
          {":path", "/vhost-only"},
          {":scheme", "http"},
          {":authority", "vhost-headers.com"},
          {"x-vhost-request", "downstream"},
          {"x-unmodified", "downstream"},
      },
      Http::TestHeaderMapImpl{
          {":authority", "vhost-headers.com"},
          {"x-unmodified", "downstream"},
          {"x-vhost-request", "vhost"},
          {":path", "/vhost-only"},
          {":method", "GET"},
      },
      Http::TestHeaderMapImpl{
          {"server", "envoy"},
          {"content-length", "0"},
          {":status", "200"},
          {"x-vhost-response", "upstream"},
          {"x-unmodified", "upstream"},
      },
      Http::TestHeaderMapImpl{
          {"server", "envoy"},
          {"x-unmodified", "upstream"},
          {"x-vhost-response", "vhost"},
          {":status", "200"},
      });
}

// Validates the route appends request headers and appends/removes upstream response headers.
TEST_P(HeaderIntegrationTest, TestRouteAppendHeaderManipulation) {
  initializeFilter(HeaderMode::Append, false);
  performRequest(
      Http::TestHeaderMapImpl{
          {":method", "GET"},
          {":path", "/route-only"},
          {":scheme", "http"},
          {":authority", "route-headers.com"},
          {"x-route-request", "downstream"},
      },
      Http::TestHeaderMapImpl{
          {":authority", "route-headers.com"},
          {"x-route-request", "downstream"},
          {"x-route-request", "route"},
          {":path", "/route-only"},
          {":method", "GET"},
      },
      Http::TestHeaderMapImpl{
          {"server", "envoy"},
          {"content-length", "0"},
          {":status", "200"},
          {"x-route-response", "upstream"},
          {"x-route-response-remove", "upstream"},
      },
      Http::TestHeaderMapImpl{
          {"server", "envoy"},
          {"x-route-response", "upstream"},
          {"x-route-response", "route"},
          {":status", "200"},
      });
}

// Validates the route replaces request headers and replaces/removes upstream response headers.
TEST_P(HeaderIntegrationTest, TestRouteReplaceHeaderManipulation) {
  initializeFilter(HeaderMode::Replace, false);
  performRequest(
      Http::TestHeaderMapImpl{
          {":method", "GET"},
          {":path", "/route-only"},
          {":scheme", "http"},
          {":authority", "route-headers.com"},
          {"x-route-request", "downstream"},
          {"x-unmodified", "downstream"},
      },
      Http::TestHeaderMapImpl{
          {":authority", "route-headers.com"},
          {"x-unmodified", "downstream"},
          {"x-route-request", "route"},
          {":path", "/route-only"},
          {":method", "GET"},
      },
      Http::TestHeaderMapImpl{
          {"server", "envoy"},
          {"content-length", "0"},
          {":status", "200"},
          {"x-route-response", "upstream"},
          {"x-route-response-remove", "upstream"},
          {"x-unmodified", "upstream"},
      },
      Http::TestHeaderMapImpl{
          {"server", "envoy"},
          {"x-unmodified", "upstream"},
          {"x-route-response", "route"},
          {":status", "200"},
      });
}

// Validates the relationship between virtual host and route header manipulations when appending.
TEST_P(HeaderIntegrationTest, TestVirtualHostAndRouteAppendHeaderManipulation) {
  initializeFilter(HeaderMode::Append, false);
  performRequest(
      Http::TestHeaderMapImpl{
          {":method", "GET"},
          {":path", "/vhost-and-route"},
          {":scheme", "http"},
          {":authority", "vhost-headers.com"},
          {"x-vhost-request", "downstream"},
          {"x-route-request", "downstream"},
      },
      Http::TestHeaderMapImpl{
          {":authority", "vhost-headers.com"},
          {"x-vhost-request", "downstream"},
          {"x-route-request", "downstream"},
          {"x-route-request", "route"},
          {"x-vhost-request", "vhost"},
          {":path", "/vhost-and-route"},
          {":method", "GET"},
      },
      Http::TestHeaderMapImpl{
          {"server", "envoy"},
          {"content-length", "0"},
          {":status", "200"},
          {"x-vhost-response", "upstream"},
          {"x-vhost-response-remove", "upstream"},
          {"x-route-response", "upstream"},
          {"x-route-response-remove", "upstream"},
      },
      Http::TestHeaderMapImpl{
          {"server", "envoy"},
          {"x-vhost-response", "upstream"},
          {"x-route-response", "upstream"},
          {"x-route-response", "route"},
          {"x-vhost-response", "vhost"},
          {":status", "200"},
      });
}

// Validates the relationship between virtual host and route header manipulations when replacing.
TEST_P(HeaderIntegrationTest, TestVirtualHostAndRouteReplaceHeaderManipulation) {
  initializeFilter(HeaderMode::Replace, false);
  performRequest(
      Http::TestHeaderMapImpl{
          {":method", "GET"},
          {":path", "/vhost-and-route"},
          {":scheme", "http"},
          {":authority", "vhost-headers.com"},
          {"x-vhost-request", "downstream"},
          {"x-route-request", "downstream"},
          {"x-unmodified", "request"},
      },
      Http::TestHeaderMapImpl{
          {":authority", "vhost-headers.com"},
          {"x-unmodified", "request"},
          {"x-route-request", "route"},
          {"x-vhost-request", "vhost"},
          {":path", "/vhost-and-route"},
          {":method", "GET"},
      },
      Http::TestHeaderMapImpl{
          {"server", "envoy"},
          {"content-length", "0"},
          {":status", "200"},
          {"x-vhost-response", "upstream"},
          {"x-route-response", "upstream"},
          {"x-unmodified", "response"},
      },
      Http::TestHeaderMapImpl{
          {"server", "envoy"},
          {"x-unmodified", "response"},
          {"x-route-response", "route"},
          {"x-vhost-response", "vhost"},
          {":status", "200"},
      });
}

// Validates the relationship between route configuration, virtual host and route header
// manipulations when appending.
TEST_P(HeaderIntegrationTest, TestRouteConfigVirtualHostAndRouteAppendHeaderManipulation) {
  initializeFilter(HeaderMode::Append, true);
  performRequest(
      Http::TestHeaderMapImpl{
          {":method", "GET"},
          {":path", "/vhost-and-route"},
          {":scheme", "http"},
          {":authority", "vhost-headers.com"},
          {"x-routeconfig-request", "downstream"},
          {"x-vhost-request", "downstream"},
          {"x-route-request", "downstream"},
      },
      Http::TestHeaderMapImpl{
          {":authority", "vhost-headers.com"},
          {"x-routeconfig-request", "downstream"},
          {"x-vhost-request", "downstream"},
          {"x-route-request", "downstream"},
          {"x-route-request", "route"},
          {"x-vhost-request", "vhost"},
          {"x-routeconfig-request", "routeconfig"},
          {":path", "/vhost-and-route"},
          {":method", "GET"},
      },
      Http::TestHeaderMapImpl{
          {"server", "envoy"},
          {"content-length", "0"},
          {":status", "200"},
          {"x-routeconfig-response", "upstream"},
          {"x-routeconfig-response-remove", "upstream"},
          {"x-vhost-response", "upstream"},
          {"x-vhost-response-remove", "upstream"},
          {"x-route-response", "upstream"},
          {"x-route-response-remove", "upstream"},
      },
      Http::TestHeaderMapImpl{
          {"server", "envoy"},
          {"x-routeconfig-response", "upstream"},
          {"x-vhost-response", "upstream"},
          {"x-route-response", "upstream"},
          {"x-route-response", "route"},
          {"x-vhost-response", "vhost"},
          {"x-routeconfig-response", "routeconfig"},
          {":status", "200"},
      });
}

// Validates the relationship between route configuration, virtual host and route header
// manipulations when replacing.
TEST_P(HeaderIntegrationTest, TestRouteConfigVirtualHostAndRouteReplaceHeaderManipulation) {
  initializeFilter(HeaderMode::Replace, true);
  performRequest(
      Http::TestHeaderMapImpl{
          {":method", "GET"},
          {":path", "/vhost-and-route"},
          {":scheme", "http"},
          {":authority", "vhost-headers.com"},
          {"x-routeconfig-request", "downstream"},
          {"x-vhost-request", "downstream"},
          {"x-route-request", "downstream"},
          {"x-unmodified", "request"},
      },
      Http::TestHeaderMapImpl{
          {":authority", "vhost-headers.com"},
          {"x-unmodified", "request"},
          {"x-route-request", "route"},
          {"x-vhost-request", "vhost"},
          {"x-routeconfig-request", "routeconfig"},
          {":path", "/vhost-and-route"},
          {":method", "GET"},
      },
      Http::TestHeaderMapImpl{
          {"server", "envoy"},
          {"content-length", "0"},
          {":status", "200"},
          {"x-routeconfig-response", "upstream"},
          {"x-vhost-response", "upstream"},
          {"x-route-response", "upstream"},
          {"x-unmodified", "response"},
      },
      Http::TestHeaderMapImpl{
          {"server", "envoy"},
          {"x-unmodified", "response"},
          {"x-route-response", "route"},
          {"x-vhost-response", "vhost"},
          {"x-routeconfig-response", "routeconfig"},
          {":status", "200"},
      });
}

// Validates that upstream host metadata can be emitted in headers.
TEST_P(HeaderIntegrationTest, TestDynamicHeaders) {
  prepareEDS();
  initializeFilter(HeaderMode::Replace, true);
  performRequest(
      Http::TestHeaderMapImpl{
          {":method", "GET"},
          {":path", "/vhost-and-route"},
          {":scheme", "http"},
          {":authority", "vhost-headers.com"},
          {"x-routeconfig-request", "downstream"},
          {"x-vhost-request", "downstream"},
          {"x-route-request", "downstream"},
          {"x-unmodified", "request"},
      },
      Http::TestHeaderMapImpl{
          {":authority", "vhost-headers.com"},
          {"x-unmodified", "request"},
          {"x-route-request", "route"},
          {"x-vhost-request", "vhost"},
          {"x-routeconfig-request", "routeconfig"},
          {":path", "/vhost-and-route"},
          {":method", "GET"},
      },
      Http::TestHeaderMapImpl{
          {"server", "envoy"},
          {"content-length", "0"},
          {":status", "200"},
          {"x-routeconfig-response", "upstream"},
          {"x-vhost-response", "upstream"},
          {"x-route-response", "upstream"},
          {"x-unmodified", "response"},
      },
      Http::TestHeaderMapImpl{
          {"server", "envoy"},
          {"x-unmodified", "response"},
          {"x-route-response", "route"},
          {"x-route-dynamic", "metadata-value"},
          {"x-vhost-response", "vhost"},
          {"x-vhost-dynamic", "metadata-value"},
          {"x-routeconfig-response", "routeconfig"},
          {"x-routeconfig-dynamic", "metadata-value"},
          {":status", "200"},
      });
}

} // namespace Envoy
