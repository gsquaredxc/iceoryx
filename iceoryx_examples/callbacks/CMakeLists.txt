# Copyright (c) 2021 - 2022 by Apex.AI Inc. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)
project(example_callbacks)

include(GNUInstallDirs)

find_package(iceoryx_posh CONFIG REQUIRED)
find_package(iceoryx_hoofs CONFIG REQUIRED)

get_target_property(ICEORYX_CXX_STANDARD iceoryx_posh::iceoryx_posh CXX_STANDARD)

include(IceoryxPlatform)
include(IceoryxPackageHelper)

iox_add_executable(
    TARGET  iox-cpp-callbacks-publisher
    FILES   ./ice_callbacks_publisher.cpp
    LIBS    iceoryx_posh::iceoryx_posh
)

iox_add_executable(
    TARGET  iox-cpp-callbacks-subscriber
    FILES   ./ice_callbacks_subscriber.cpp
    LIBS    iceoryx_posh::iceoryx_posh
)

iox_add_executable(
    TARGET  iox-cpp-callbacks-listener-as-class-member
    FILES   ./ice_callbacks_listener_as_class_member.cpp
    LIBS    iceoryx_posh::iceoryx_posh
)
