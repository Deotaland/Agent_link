# Copyright (c) Meta Platforms, Inc. and affiliates.
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

# Vendored from facebookincubator/muse-gadget-sdk (esp32/components/noise_core).
# Local change: the sources are compiled only for the Muse transport. agent_link requires this
# component on every build (a REQUIRES cannot depend on Kconfig), so on the other transports it
# registers as an empty, header-only component.
if(CONFIG_AGENT_LINK_TRANSPORT_MUSE)
    set(srcs
        "src/ClientSession.cpp"
        "src/InitiatorHandshake.cpp"
        "src/PsaCryptoBackend.cpp"
        "src/ServiceCodec.cpp"
        "src/Status.cpp"
        "src/Transport.cpp"
        "src/TransportFrameCodec.cpp")
else()
    set(srcs)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mbedtls
)

if(srcs)
    target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    )
endif()
