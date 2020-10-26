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

#ifndef PRIVACY_NET_KRYPTON_UTILS_TIME_UTIL_H_
#define PRIVACY_NET_KRYPTON_UTILS_TIME_UTIL_H_

#include "google/protobuf/duration.proto.h"
#include "privacy/net/krypton/utils/status.h"
#include "third_party/absl/status/status.h"
#include "third_party/absl/status/statusor.h"
#include "third_party/absl/time/time.h"

namespace privacy {
namespace krypton {
namespace utils {

absl::Status ToProtoDuration(absl::Duration d,
                             google::protobuf::Duration* proto);
}  // namespace utils
}  // namespace krypton
}  // namespace privacy

#endif  // PRIVACY_NET_KRYPTON_UTILS_TIME_UTIL_H_