// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the );
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an  BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "privacy/net/krypton/session.h"

#include <memory>
#include <string>
#include <type_traits>

#include "net/proto2/contrib/parse_proto/parse_text_proto.h"
#include "privacy/net/krypton/add_egress_request.h"
#include "privacy/net/krypton/add_egress_response.h"
#include "privacy/net/krypton/auth.h"
#include "privacy/net/krypton/auth_and_sign_response.h"
#include "privacy/net/krypton/crypto/session_crypto.h"
#include "privacy/net/krypton/crypto/suite.h"
#include "privacy/net/krypton/datapath_interface.h"
#include "privacy/net/krypton/egress_manager.h"
#include "privacy/net/krypton/fd_packet_pipe.h"
#include "privacy/net/krypton/pal/http_fetcher_interface.h"
#include "privacy/net/krypton/pal/mock_oauth_interface.h"
#include "privacy/net/krypton/pal/mock_timer_interface.h"
#include "privacy/net/krypton/pal/mock_vpn_service_interface.h"
#include "privacy/net/krypton/proto/debug_info.proto.h"
#include "privacy/net/krypton/proto/krypton_config.proto.h"
#include "privacy/net/krypton/proto/network_info.proto.h"
#include "privacy/net/krypton/proto/network_type.proto.h"
#include "privacy/net/krypton/timer_manager.h"
#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/memory/memory.h"
#include "third_party/absl/status/status.h"
#include "third_party/absl/status/statusor.h"
#include "third_party/absl/strings/string_view.h"
#include "third_party/absl/synchronization/notification.h"
#include "third_party/absl/time/time.h"
#include "third_party/absl/types/optional.h"
#include "third_party/jsoncpp/value.h"
#include "third_party/jsoncpp/writer.h"

namespace privacy {
namespace krypton {
namespace {

constexpr int kValidTunFd = 0xbeef;
constexpr int kInvalidFd = -1;
constexpr int kValidNetworkFd = 0xbeef + 1;

using ::testing::_;
using ::testing::ByMove;
using ::testing::DoAll;
using ::testing::Eq;
using ::testing::EqualsProto;
using ::testing::Invoke;
using ::testing::InvokeWithoutArgs;
using ::testing::Optional;
using ::testing::Return;
using ::testing::status::IsOk;
using ::testing::status::StatusIs;

// Checks that a given NetworkInfo is equal to the one passed in.
MATCHER_P(NetworkInfoEquals, expected, "") {
  auto actual = arg;

  if (!actual) {
    return false;
  }

  return expected.network_id() == actual->network_id() &&
         expected.network_type() == actual->network_type();
}

// Checks that a given PacketPipe has the given file descriptor.
MATCHER_P(PacketPipeHasFd, fd, "") {
  auto fd_packet_pipe = arg;

  auto status_or_fd = fd_packet_pipe->GetFd();
  if (!status_or_fd.ok()) {
    return false;
  }

  return status_or_fd.value() == fd;
}

// Helper macro for returning a PacketPipe wrapping a file descriptor, since
// it's complicated and used in many locations.
#define RETURN_TEST_PIPE(fd)                                   \
  Return(ByMove(absl::StatusOr<std::unique_ptr<FdPacketPipe>>( \
      std::make_unique<FdPacketPipe>(fd))))

// Mock the Auth.
class MockAuth : public Auth {
 public:
  using Auth::Auth;
  MOCK_METHOD(void, Start, (bool), (override));
  MOCK_METHOD(std::shared_ptr<AuthAndSignResponse>, auth_response, (),
              (const, override));
};

// Mock the Egress Management.
class MockEgressManager : public EgressManager {
 public:
  using EgressManager::EgressManager;
  MOCK_METHOD(absl::Status, GetEgressNodeForBridge,
              (std::shared_ptr<AuthAndSignResponse>), (override));
  MOCK_METHOD(absl::StatusOr<std::shared_ptr<AddEgressResponse>>,
              GetEgressSessionDetails, (), (const, override));
  MOCK_METHOD(absl::Status, GetEgressNodeForPpnIpSec,
              (const AddEgressRequest::PpnDataplaneRequestParams&), (override));
};

class MockHttpFetcherInterface : public HttpFetcherInterface {
 public:
  MOCK_METHOD(HttpResponse, PostJson, (const HttpRequest&), (override));
};

class MockSessionNotification : public Session::NotificationInterface {
 public:
  MOCK_METHOD(void, ControlPlaneConnected, (), (override));
  MOCK_METHOD(void, StatusUpdated, (), (override));
  MOCK_METHOD(void, ControlPlaneDisconnected, (const absl::Status&),
              (override));
  MOCK_METHOD(void, PermanentFailure, (const absl::Status&), (override));
  MOCK_METHOD(void, DatapathConnected, (), (override));
  MOCK_METHOD(void, DatapathDisconnected,
              (const NetworkInfo&, const absl::Status&), (override));
};

class MockDatapath : public DatapathInterface {
 public:
  MOCK_METHOD(absl::Status, Start,
              (std::shared_ptr<AddEgressResponse>, const BridgeTransformParams&,
               CryptoSuite),
              (override));
  MOCK_METHOD(void, Stop, (), (override));
  MOCK_METHOD(bool, is_running, (), (const, override));
  MOCK_METHOD(void, RegisterNotificationHandler,
              (DatapathInterface::NotificationInterface * notification),
              (override));
  MOCK_METHOD(absl::Status, SwitchNetwork,
              (uint32, const std::vector<std::string>&,
               absl::optional<NetworkInfo>, const PacketPipe*,
               const PacketPipe*, int),
              (override));
  MOCK_METHOD(absl::Status, Rekey, (const std::string&, const std::string&),
              (override));
};

class SessionTest : public ::testing::Test {
 public:
  void SetUp() override {
    EXPECT_CALL(datapath_, RegisterNotificationHandler)
        .WillOnce(
            Invoke([&](DatapathInterface::NotificationInterface* notification) {
              datapath_notification_ = notification;
            }));
    fake_auth_and_sign_response_ = std::make_shared<AuthAndSignResponse>();
    fake_add_egress_response_ = std::make_shared<AddEgressResponse>();

    HttpResponse fake_add_egress_http_response;
    fake_add_egress_http_response.mutable_status()->set_code(200);
    fake_add_egress_http_response.mutable_status()->set_message("OK");
    fake_add_egress_http_response.set_json_body(R"string({
      "ppn_dataplane": {
        "user_private_ip": [{
          "ipv4_range": "10.2.2.123/32",
          "ipv6_range": "fec2:0001::3/64"
        }],
        "egress_point_sock_addr": ["64.9.240.165:2153", "[2604:ca00:f001:4::5]:2153"],
        "egress_point_public_value": "a22j+91TxHtS5qa625KCD5ybsyzPR1wkTDWHV2qSQQc=",
        "server_nonce": "Uzt2lEzyvZYzjLAP3E+dAA==",
        "uplink_spi": 1234,
        "expiry": "2020-08-07T01:06:13+00:00"
      }
    })string");
    ASSERT_OK(fake_add_egress_response_->DecodeFromProto(
        fake_add_egress_http_response));

    session_ = absl::make_unique<Session>(
        &auth_, &egress_manager_, &datapath_, &vpn_service_, &timer_manager_,
        absl::nullopt, &config_, &notification_thread_);
    session_->RegisterNotificationHandler(&notification_);
  }

  void TearDown() override {
    auth_.Stop();
    egress_manager_.Stop();
  }

  virtual void ExpectSuccessfulAddEgress() {
    EXPECT_CALL(egress_manager_, GetEgressNodeForBridge)
        .WillOnce(
            Invoke([&](std::shared_ptr<AuthAndSignResponse> /*auth_response*/) {
              notification_thread_.Post(
                  [this] { session_->EgressAvailable(false); });
              return absl::OkStatus();
            }));
    EXPECT_OK(
        egress_manager_.SaveEgressDetailsTestOnly(fake_add_egress_response_));
  }

  void ExpectSuccessfulAuth() {
    EXPECT_CALL(auth_, Start).WillOnce(Invoke([&]() {
      notification_thread_.Post(
          [this] { session_->AuthSuccessful(is_rekey_); });
    }));
    EXPECT_CALL(auth_, auth_response)
        .WillRepeatedly(Return(fake_auth_and_sign_response_));
  }

  virtual void ExpectSuccessfulDatapathInit() {
    EXPECT_CALL(timer_interface_, StartTimer(_, absl::Minutes(5)))
        .WillOnce(Return(absl::OkStatus()));

    EXPECT_CALL(notification_, ControlPlaneConnected());

    EXPECT_CALL(egress_manager_, GetEgressSessionDetails)
        .WillRepeatedly(Invoke([&]() { return fake_add_egress_response_; }));

    EXPECT_CALL(datapath_, Start(fake_add_egress_response_, _, _))
        .WillOnce(::testing::DoAll(
            InvokeWithoutArgs(&done_, &absl::Notification::Notify),
            Return(absl::OkStatus())));
  }

  void StartSessionAndConnectDatapathOnCellular() {
    ExpectSuccessfulAuth();
    ExpectSuccessfulAddEgress();
    ExpectSuccessfulDatapathInit();

    session_->Start();
    WaitInitial();
    network_fd_counter_ += 1;
    EXPECT_CALL(egress_manager_, GetEgressSessionDetails)
        .WillRepeatedly(Invoke([&]() { return fake_add_egress_response_; }));
    EXPECT_CALL(vpn_service_,
                CreateTunnel(EqualsProto(R"pb(tunnel_ip_addresses {
                                                ip_family: IPV4
                                                ip_range: "10.2.2.123"
                                                prefix: 32
                                              }
                                              tunnel_ip_addresses {
                                                ip_family: IPV6
                                                ip_range: "fec2:0001::3"
                                                prefix: 64
                                              }
                                              tunnel_dns_addresses {
                                                ip_family: IPV4
                                                ip_range: "8.8.8.8"
                                                prefix: 32
                                              }
                                              tunnel_dns_addresses {
                                                ip_family: IPV4
                                                ip_range: "8.8.8.4"
                                                prefix: 32
                                              }
                                              tunnel_dns_addresses {
                                                ip_family: IPV6
                                                ip_range: "2001:4860:4860::8888"
                                                prefix: 128
                                              }
                                              tunnel_dns_addresses {
                                                ip_family: IPV6
                                                ip_range: "2001:4860:4860::8844"
                                                prefix: 128
                                              }
                                              is_metered: false)pb")))
        .WillOnce(RETURN_TEST_PIPE(++tun_fd_counter_));
    EXPECT_CALL(vpn_service_,
                CreateProtectedNetworkSocket(EqualsProto(
                    R"pb(network_id: 1234 network_type: CELLULAR)pb")))
        .WillOnce(RETURN_TEST_PIPE(network_fd_counter_));

    NetworkInfo expected_network_info;
    expected_network_info.set_network_id(1234);
    expected_network_info.set_network_type(NetworkType::CELLULAR);
    EXPECT_CALL(datapath_,
                SwitchNetwork(1234, _, NetworkInfoEquals(expected_network_info),
                              _, PacketPipeHasFd(tun_fd_counter_), _))
        .WillOnce(Return(absl::OkStatus()));

    NetworkInfo network_info;
    network_info.set_network_id(1234);
    network_info.set_network_type(NetworkType::CELLULAR);
    EXPECT_OK(session_->SetNetwork(network_info));
    WaitForNotifications();
    EXPECT_CALL(notification_, DatapathConnected());

    session_->DatapathEstablished();
    EXPECT_THAT(session_->active_network_info(),
                NetworkInfoEquals(expected_network_info));
    EXPECT_THAT(session_->active_tun_fd_test_only(), Optional(tun_fd_counter_));
  }

  void WaitInitial() {
    ASSERT_TRUE(done_.WaitForNotificationWithTimeout(absl::Seconds(3)));
  }

  void WaitForNotifications() {
    absl::Mutex lock;
    absl::CondVar condition;
    absl::MutexLock l(&lock);
    notification_thread_.Post([&condition] { condition.SignalAll(); });
    condition.Wait(&lock);
  }

  KryptonConfig config_{proto2::contrib::parse_proto::ParseTextProtoOrDie(
      R"pb(zinc_url: "http://www.example.com/auth"
           service_type: "service_type"
           ipsec_datapath: false
           bridge_over_ppn: false
           enable_blind_signing: false)pb")};

  int tun_fd_counter_ = kValidTunFd;  // Starting value of tun fd.
  int network_fd_counter_ = kValidTunFd + 1000;
  MockSessionNotification notification_;
  MockHttpFetcherInterface http_fetcher_;
  MockOAuth oauth_;
  utils::LooperThread notification_thread_{"Session Test"};
  MockAuth auth_{&config_, &http_fetcher_, &oauth_, &notification_thread_};
  MockEgressManager egress_manager_{"http://www.example.com/addegress",
                                    &http_fetcher_, &notification_thread_};

  MockDatapath datapath_;
  MockTimerInterface timer_interface_;
  TimerManager timer_manager_{&timer_interface_};

  MockVpnService vpn_service_;
  std::unique_ptr<Session> session_;
  std::shared_ptr<AuthAndSignResponse> fake_auth_and_sign_response_;
  std::shared_ptr<AddEgressResponse> fake_add_egress_response_;
  DatapathInterface::NotificationInterface* datapath_notification_;
  bool is_rekey_ = false;
  absl::Notification done_;
};

TEST_F(SessionTest, AuthenticationFailure) {
  absl::Notification done;
  EXPECT_CALL(auth_, Start).WillOnce(Invoke([&]() {
    notification_thread_.Post(
        [this] { session_->AuthFailure(absl::InternalError("Some error")); });
  }));
  EXPECT_CALL(notification_, ControlPlaneDisconnected(::testing::_))
      .WillOnce(InvokeWithoutArgs(&done, &absl::Notification::Notify));
  session_->Start();
  EXPECT_TRUE(done.WaitForNotificationWithTimeout(absl::Seconds(3)));
  EXPECT_EQ(Session::State::kSessionError, session_->state());
}

TEST_F(SessionTest, AuthenticationPermanentFailure) {
  absl::Notification done;
  EXPECT_CALL(auth_, Start).WillOnce(Invoke([&]() {
    notification_thread_.Post([this] {
      session_->AuthFailure(absl::PermissionDeniedError("Some error"));
    });
  }));

  EXPECT_CALL(notification_, PermanentFailure(::testing::_))
      .WillOnce(InvokeWithoutArgs(&done, &absl::Notification::Notify));

  session_->Start();
  EXPECT_TRUE(done.WaitForNotificationWithTimeout(absl::Seconds(3)));
  EXPECT_EQ(Session::State::kPermanentError, session_->state());
}

// This test assumes Authentication was successful.
TEST_F(SessionTest, AddEgressFailure) {
  absl::Notification done;
  ExpectSuccessfulAuth();

  EXPECT_CALL(egress_manager_, GetEgressNodeForBridge)
      .WillOnce(Invoke([&](std::shared_ptr<AuthAndSignResponse>) {
        return absl::NotFoundError("Add Egress Failure");
      }));
  EXPECT_CALL(notification_, ControlPlaneDisconnected(::testing::_))
      .WillOnce(InvokeWithoutArgs(&done, &absl::Notification::Notify));
  session_->Start();
  EXPECT_TRUE(done.WaitForNotificationWithTimeout(absl::Seconds(3)));
  EXPECT_THAT(session_->latest_status(),
              StatusIs(absl::StatusCode::kNotFound, "Add Egress Failure"));
}

TEST_F(SessionTest, DatapathInitFailure) {
  absl::Notification done;
  ExpectSuccessfulAuth();
  ExpectSuccessfulAddEgress();

  EXPECT_CALL(egress_manager_, GetEgressSessionDetails)
      .WillRepeatedly(Invoke([&]() { return fake_add_egress_response_; }));

  EXPECT_CALL(datapath_, Start(fake_add_egress_response_, _, _))
      .WillOnce(::testing::DoAll(
          InvokeWithoutArgs(&done, &absl::Notification::Notify),
          Return(absl::InvalidArgumentError("Initialization error"))));

  session_->Start();
  EXPECT_TRUE(done.WaitForNotificationWithTimeout(absl::Seconds(3)));
  EXPECT_THAT(session_->latest_status(),
              StatusIs(util::error::INVALID_ARGUMENT, "Initialization error"));

  EXPECT_EQ(session_->state(), Session::State::kSessionError);
}

TEST_F(SessionTest, DatapathInitSuccessful) {
  ExpectSuccessfulAuth();
  ExpectSuccessfulAddEgress();
  ExpectSuccessfulDatapathInit();

  session_->Start();

  WaitInitial();
  EXPECT_THAT(session_->latest_status(), IsOk());

  EXPECT_EQ(session_->state(), Session::State::kConnected);
}

TEST_F(SessionTest, InitialDatapathEndpointChangeAndNoNetworkAvailable) {
  ExpectSuccessfulAuth();
  ExpectSuccessfulAddEgress();
  ExpectSuccessfulDatapathInit();

  session_->Start();

  WaitInitial();
  EXPECT_CALL(egress_manager_, GetEgressSessionDetails)
      .WillRepeatedly(Invoke([&]() {
        EXPECT_OK(egress_manager_.SaveEgressDetailsTestOnly(
            fake_add_egress_response_));
        return fake_add_egress_response_;
      }));
  EXPECT_CALL(vpn_service_,
              CreateTunnel(EqualsProto(R"pb(tunnel_ip_addresses {
                                              ip_family: IPV4
                                              ip_range: "10.2.2.123"
                                              prefix: 32
                                            }
                                            tunnel_ip_addresses {
                                              ip_family: IPV6
                                              ip_range: "fec2:0001::3"
                                              prefix: 64
                                            }
                                            tunnel_dns_addresses {
                                              ip_family: IPV4
                                              ip_range: "8.8.8.8"
                                              prefix: 32
                                            }
                                            tunnel_dns_addresses {
                                              ip_family: IPV4
                                              ip_range: "8.8.8.4"
                                              prefix: 32
                                            }
                                            tunnel_dns_addresses {
                                              ip_family: IPV6
                                              ip_range: "2001:4860:4860::8888"
                                              prefix: 128
                                            }
                                            tunnel_dns_addresses {
                                              ip_family: IPV6
                                              ip_range: "2001:4860:4860::8844"
                                              prefix: 128
                                            }
                                            is_metered: false)pb")))
      .WillOnce(RETURN_TEST_PIPE(++tun_fd_counter_));

  EXPECT_CALL(vpn_service_,
              CreateProtectedNetworkSocket(EqualsProto(R"pb(network_type:
                                                                CELLULAR)pb")))
      .WillOnce(RETURN_TEST_PIPE(tun_fd_counter_));

  NetworkInfo expected_network_info;
  expected_network_info.set_network_type(NetworkType::CELLULAR);
  EXPECT_CALL(datapath_,
              SwitchNetwork(1234, _, NetworkInfoEquals(expected_network_info),
                            _, PacketPipeHasFd(tun_fd_counter_), _))
      .WillOnce(Return(absl::OkStatus()));

  NetworkInfo network_info;
  network_info.set_network_type(NetworkType::CELLULAR);

  EXPECT_OK(session_->SetNetwork(network_info));

  EXPECT_CALL(notification_, DatapathConnected());
  session_->DatapathEstablished();

  // No Network available.
  EXPECT_CALL(datapath_, SwitchNetwork(1234, _, Eq(absl::nullopt), _,
                                       PacketPipeHasFd(tun_fd_counter_), _))
      .WillOnce(Return(absl::OkStatus()));
  EXPECT_OK(session_->SetNetwork(absl::nullopt));
}

TEST_F(SessionTest, SwitchNetworkToSameNetworkType) {
  StartSessionAndConnectDatapathOnCellular();

  // Switch network to same type.
  network_fd_counter_ += 1;
  NetworkInfo new_network_info;
  new_network_info.set_network_type(NetworkType::CELLULAR);

  EXPECT_CALL(vpn_service_,
              CreateProtectedNetworkSocket(EqualsProto(R"pb(network_type:
                                                                CELLULAR)pb")))
      .WillOnce(RETURN_TEST_PIPE(network_fd_counter_));
  // Expect no tunnel fd change.
  EXPECT_CALL(datapath_,
              SwitchNetwork(1234, _, NetworkInfoEquals(new_network_info), _,
                            PacketPipeHasFd(tun_fd_counter_), _))
      .WillOnce(Return(absl::OkStatus()));

  EXPECT_OK(session_->SetNetwork(new_network_info));
  // Check all the parameters are correct in the session.
  EXPECT_THAT(session_->active_network_info(),
              NetworkInfoEquals(new_network_info));
  EXPECT_THAT(session_->active_tun_fd_test_only(), Optional(tun_fd_counter_));
}

TEST_F(SessionTest, DatapathReattemptFailure) {
  StartSessionAndConnectDatapathOnCellular();

  NetworkInfo expected_network_info;
  expected_network_info.set_network_id(1234);
  expected_network_info.set_network_type(NetworkType::CELLULAR);
  absl::Status status = absl::InternalError("Some error");
  for (int i = 0; i < 3; ++i) {
    // Initial failure
    EXPECT_CALL(timer_interface_, StartTimer(_, absl::Milliseconds(500)))
        .WillOnce(Return(absl::OkStatus()));

    session_->DatapathFailed(status, network_fd_counter_);

    EXPECT_CALL(vpn_service_, CreateProtectedNetworkSocket(
                                  EqualsProto(R"pb(network_type: CELLULAR
                                                   network_id: 1234)pb")))
        .WillOnce(RETURN_TEST_PIPE(network_fd_counter_));

    // 2 Attempts on V6, 2 attempts on V4.  V6 preferred over v4.
    if (i < 2) {
      EXPECT_CALL(
          datapath_,
          SwitchNetwork(1234,
                        ::testing::ElementsAre("[2604:ca00:f001:4::5]:2153"),
                        NetworkInfoEquals(expected_network_info), _,
                        PacketPipeHasFd(tun_fd_counter_), _))
          .WillOnce(Return(absl::OkStatus()));
    } else {
      EXPECT_CALL(
          datapath_,
          SwitchNetwork(1234, ::testing::ElementsAre("64.9.240.165:2153"),
                        NetworkInfoEquals(expected_network_info), _,
                        PacketPipeHasFd(tun_fd_counter_), _))
          .WillOnce(Return(absl::OkStatus()));
    }

    session_->AttemptDatapathReconnect();
  }
  // Reattempt not done as we reached the max reattempts.
  EXPECT_CALL(notification_, DatapathDisconnected(_, status));

  session_->DatapathFailed(status, network_fd_counter_);
}

TEST_F(SessionTest, DatapathFailureAndSuccessfulBeforeReattempt) {
  StartSessionAndConnectDatapathOnCellular();

  EXPECT_CALL(timer_interface_, StartTimer(_, absl::Milliseconds(500)))
      .WillOnce(Return(absl::OkStatus()));

  session_->DatapathFailed(absl::InternalError("Some error"),
                           network_fd_counter_);

  // Datapath Successful.
  WaitForNotifications();
  EXPECT_CALL(notification_, DatapathConnected);
  session_->DatapathEstablished();
  EXPECT_EQ(-1, session_->DatapathReattemptTimerIdTestOnly());
  EXPECT_EQ(0, session_->DatapathReattemptCountTestOnly());
}

TEST_F(SessionTest, SwitchNetworkToDifferentNetworkType) {
  StartSessionAndConnectDatapathOnCellular();

  // Switch network to different type.
  network_fd_counter_ += 1;
  NetworkInfo new_network_info;
  new_network_info.set_network_type(NetworkType::WIFI);

  EXPECT_CALL(vpn_service_, CreateProtectedNetworkSocket(
                                EqualsProto(R"pb(network_type: WIFI)pb")))
      .WillOnce(RETURN_TEST_PIPE(network_fd_counter_));
  EXPECT_CALL(datapath_,
              SwitchNetwork(1234, _, NetworkInfoEquals(new_network_info), _,
                            PacketPipeHasFd(tun_fd_counter_), _))
      .WillOnce(Return(absl::OkStatus()));

  EXPECT_OK(session_->SetNetwork(new_network_info));
  // Check all the parameters are correct in the session.
  EXPECT_THAT(session_->active_network_info(),
              NetworkInfoEquals(new_network_info));
  EXPECT_THAT(session_->active_tun_fd_test_only(), Optional(tun_fd_counter_));
}

TEST_F(SessionTest, TestEndpointChangeBeforeEstablishingSession) {
  absl::Notification done;
  // Switch network after auth is successful and before session is in
  // connected state.
  EXPECT_CALL(auth_, Start).WillOnce(Invoke([&]() {
    NetworkInfo network_info;
    network_info.set_network_type(NetworkType::CELLULAR);
    notification_thread_.Post([this, network_info]() {
      ASSERT_OK(session_->SetNetwork(network_info));
    });

    notification_thread_.Post([this]() { session_->AuthSuccessful(false); });
  }));
  EXPECT_CALL(auth_, auth_response)
      .WillRepeatedly(Return(fake_auth_and_sign_response_));

  ExpectSuccessfulAddEgress();
  EXPECT_CALL(vpn_service_, CreateTunnel(_))
      .WillOnce(RETURN_TEST_PIPE(++tun_fd_counter_));
  EXPECT_CALL(notification_, ControlPlaneConnected());

  EXPECT_CALL(egress_manager_, GetEgressSessionDetails)
      .WillRepeatedly(Invoke([&]() { return fake_add_egress_response_; }));

  EXPECT_CALL(datapath_, Start(fake_add_egress_response_, _, _))
      .WillOnce(::testing::DoAll(
          InvokeWithoutArgs(&done, &absl::Notification::Notify),
          Return(absl::OkStatus())));

  EXPECT_CALL(vpn_service_,
              CreateProtectedNetworkSocket(EqualsProto(R"pb(network_type:
                                                                CELLULAR)pb")))
      .WillOnce(RETURN_TEST_PIPE(network_fd_counter_));
  NetworkInfo expected_network_info;
  expected_network_info.set_network_type(NetworkType::CELLULAR);
  EXPECT_CALL(datapath_,
              SwitchNetwork(1234, _, NetworkInfoEquals(expected_network_info),
                            _, PacketPipeHasFd(tun_fd_counter_), _))
      .WillOnce(Return(absl::OkStatus()));

  session_->Start();
  done.WaitForNotificationWithTimeout(absl::Seconds(3));
  EXPECT_CALL(notification_, DatapathConnected());
  session_->DatapathEstablished();
}

TEST_F(SessionTest, PopulatesDebugInfo) {
  session_->Start();

  SessionDebugInfo debug_info;
  session_->GetDebugInfo(&debug_info);

  EXPECT_THAT(debug_info, EqualsProto(R"pb(
                state: "kInitialized"
                status: "OK"
                successful_rekeys: 0
                network_switches: 1
              )pb"));
}

// Tests Bridge dataplane and PPN control plane.
class BridgeOnPpnSession : public SessionTest {
 public:
  void SetUp() override {
    config_.set_bridge_over_ppn(true);
    EXPECT_CALL(datapath_, RegisterNotificationHandler)
        .WillOnce(
            Invoke([&](DatapathInterface::NotificationInterface* notification) {
              datapath_notification_ = notification;
            }));
    fake_auth_and_sign_response_ = std::make_shared<AuthAndSignResponse>();
    fake_add_egress_response_ = std::make_shared<AddEgressResponse>();

    HttpResponse fake_add_egress_http_response;
    fake_add_egress_http_response.mutable_status()->set_code(200);
    fake_add_egress_http_response.mutable_status()->set_message("OK");
    fake_add_egress_http_response.set_json_body(R"string({
      "ppn_dataplane": {
        "user_private_ip": [{
          "ipv4_range": "10.2.2.123/32",
          "ipv6_range": "fec2:0001::3/64"
        }],
        "egress_point_sock_addr": ["64.9.240.165:2153", "[2604:ca00:f001:4::5]:2153"],
        "egress_point_public_value": "a22j+91TxHtS5qa625KCD5ybsyzPR1wkTDWHV2qSQQc=",
        "server_nonce": "Uzt2lEzyvZYzjLAP3E+dAA==",
        "uplink_spi": 123,
        "expiry": "2020-08-07T01:06:13+00:00"
      }
    })string");
    ASSERT_OK(fake_add_egress_response_->DecodeFromProto(
        fake_add_egress_http_response));
    session_ = absl::make_unique<Session>(
        &auth_, &egress_manager_, &datapath_, &vpn_service_, &timer_manager_,
        absl::nullopt, &config_, &notification_thread_);
    crypto::SessionCrypto remote;
    auto remote_key = remote.GetMyKeyMaterial();
    EXPECT_OK(session_->MutableCryptoTestOnly()->SetRemoteKeyMaterial(
        remote_key.public_value, remote_key.nonce));
    session_->RegisterNotificationHandler(&notification_);
  }

  void ExpectSuccessfulAddEgress() override {
    EXPECT_CALL(egress_manager_, GetEgressNodeForPpnIpSec)
        .WillOnce(Invoke(
            [&](const AddEgressRequest::PpnDataplaneRequestParams& /*params*/) {
              notification_thread_.Post(
                  [this]() { session_->EgressAvailable(is_rekey_); });

              return absl::OkStatus();
            }));
    EXPECT_OK(
        egress_manager_.SaveEgressDetailsTestOnly(fake_add_egress_response_));
  }

  void ExpectSuccessfulDatapathInit() override {
    EXPECT_CALL(timer_interface_, StartTimer(_, absl::Minutes(5)))
        .WillOnce(Return(absl::OkStatus()));

    EXPECT_CALL(notification_, ControlPlaneConnected());

    EXPECT_CALL(egress_manager_, GetEgressSessionDetails)
        .WillRepeatedly(Invoke([&]() { return fake_add_egress_response_; }));

    EXPECT_CALL(datapath_, Start(fake_add_egress_response_, _, _))
        .WillOnce(::testing::DoAll(
            InvokeWithoutArgs(&done_, &absl::Notification::Notify),
            Return(absl::OkStatus())));
  }

  void BringDatapathToConnected() {
    ExpectSuccessfulAuth();
    ExpectSuccessfulAddEgress();
    ExpectSuccessfulDatapathInit();

    session_->Start();

    WaitInitial();
    EXPECT_THAT(session_->latest_status(), IsOk());

    EXPECT_EQ(session_->state(), Session::State::kConnected);

    EXPECT_CALL(egress_manager_, GetEgressSessionDetails)
        .WillRepeatedly(Invoke([&]() { return fake_add_egress_response_; }));
    EXPECT_CALL(vpn_service_,
                CreateTunnel(EqualsProto(R"pb(tunnel_ip_addresses {
                                                ip_family: IPV4
                                                ip_range: "10.2.2.123"
                                                prefix: 32
                                              }
                                              tunnel_ip_addresses {
                                                ip_family: IPV6
                                                ip_range: "fec2:0001::3"
                                                prefix: 64
                                              }
                                              tunnel_dns_addresses {
                                                ip_family: IPV4
                                                ip_range: "8.8.8.8"
                                                prefix: 32
                                              }
                                              tunnel_dns_addresses {
                                                ip_family: IPV4
                                                ip_range: "8.8.8.4"
                                                prefix: 32
                                              }
                                              tunnel_dns_addresses {
                                                ip_family: IPV6
                                                ip_range: "2001:4860:4860::8888"
                                                prefix: 128
                                              }
                                              tunnel_dns_addresses {
                                                ip_family: IPV6
                                                ip_range: "2001:4860:4860::8844"
                                                prefix: 128
                                              }
                                              is_metered: false)pb")))
        .WillOnce(RETURN_TEST_PIPE(++tun_fd_counter_));

    EXPECT_CALL(vpn_service_, CreateProtectedNetworkSocket(
                                  EqualsProto(R"pb(network_type: CELLULAR)pb")))
        .WillOnce(RETURN_TEST_PIPE(tun_fd_counter_));
    NetworkInfo expected_network_info;
    expected_network_info.set_network_type(NetworkType::CELLULAR);
    EXPECT_CALL(datapath_,
                SwitchNetwork(123, _, NetworkInfoEquals(expected_network_info),
                              _, PacketPipeHasFd(tun_fd_counter_), _))
        .WillOnce(Return(absl::OkStatus()));

    NetworkInfo network_info;
    network_info.set_network_type(NetworkType::CELLULAR);
    EXPECT_OK(session_->SetNetwork(network_info));

    EXPECT_CALL(notification_, DatapathConnected());
    session_->DatapathEstablished();
  }
};

TEST_F(BridgeOnPpnSession, DatapathInitSuccessful) {
  BringDatapathToConnected();
}

TEST_F(BridgeOnPpnSession, DatapathPermanentFailure) {
  BringDatapathToConnected();

  EXPECT_CALL(notification_, DatapathDisconnected(_, _));
  session_->DatapathPermanentFailure(absl::InvalidArgumentError("some error"));
}

TEST_F(BridgeOnPpnSession, TestRekey) {
  ExpectSuccessfulAuth();
  ExpectSuccessfulAddEgress();
  ExpectSuccessfulDatapathInit();

  session_->Start();
  WaitInitial();

  is_rekey_ = true;
  absl::Notification rekey_done;
  ExpectSuccessfulAuth();
  ExpectSuccessfulAddEgress();
  EXPECT_CALL(datapath_, Rekey(_, _))
      .WillOnce(
          DoAll(InvokeWithoutArgs(&rekey_done, &absl::Notification::Notify),
                Return(absl::OkStatus())));
  session_->DoRekey();
  rekey_done.WaitForNotificationWithTimeout(absl::Seconds(3));
  SessionDebugInfo debug_info;
  session_->GetDebugInfo(&debug_info);
}

}  // namespace
}  // namespace krypton
}  // namespace privacy
