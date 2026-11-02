cmake_minimum_required(VERSION 3.21)

project(phi-transport-mqtt
    VERSION 0.1.0
    DESCRIPTION "MQTT endpoint transport for phi-core: devices, rooms and scenes on a broker, commands from it"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
include(GNUInstallDirs)
include(CTest)

find_package(nlohmann_json REQUIRED)
find_package(phi-runtime-mqtt CONFIG REQUIRED)
find_package(phi-transport-api 2.2.0 CONFIG QUIET)
if(NOT TARGET phicore::transport-api AND NOT TARGET phi_transport_api)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../phi-transport-api/CMakeLists.txt")
        add_subdirectory(
            "${CMAKE_CURRENT_SOURCE_DIR}/../phi-transport-api"
            "${CMAKE_CURRENT_BINARY_DIR}/_deps/phi-transport-api"
        )
    endif()
endif()
if(TARGET phicore::transport-api)
    set(PHI_TRANSPORT_API_TARGET phicore::transport-api)
elseif(TARGET phi_transport_api)
    set(PHI_TRANSPORT_API_TARGET phi_transport_api)
else()
    message(FATAL_ERROR "phi-transport-api target not found. Install phi-transport-api-dev or place the repo next to this project.")
endif()

# Everything the transport does, as one static library the plugin and the
# tests link: the model of the house, the topic mapping, the payloads, the
# command parser and the retained ledger need no broker and no core; the
# broker link and the transport class sit on top of them.
add_library(phi_mqtt_core STATIC
    src/broker.cpp
    src/broker.h
    src/command.cpp
    src/command.h
    src/config.cpp
    src/config.h
    src/model.cpp
    src/model.h
    src/mqtttransport.cpp
    src/mqtttransport.h
    src/payload.cpp
    src/payload.h
    src/retained.cpp
    src/retained.h
    src/topics.cpp
    src/topics.h
)
set_target_properties(phi_mqtt_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(phi_mqtt_core PUBLIC src)
target_compile_definitions(phi_mqtt_core PRIVATE PHI_MQTT_VERSION="${PROJECT_VERSION}")
target_link_libraries(phi_mqtt_core
    PUBLIC
        nlohmann_json::nlohmann_json
        ${PHI_TRANSPORT_API_TARGET}
        phi::runtime_mqtt
)
target_compile_options(phi_mqtt_core PRIVATE -Wall -Wextra)

add_library(phi_transport_mqtt MODULE src/plugin.cpp)
target_link_libraries(phi_transport_mqtt PRIVATE phi_mqtt_core ${PHI_TRANSPORT_API_TARGET})
target_link_options(phi_transport_mqtt PRIVATE -Wl,--gc-sections -Wl,--as-needed -Wl,--exclude-libs,ALL)
set_target_properties(phi_transport_mqtt PROPERTIES
    OUTPUT_NAME "phi_transport_mqtt"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/transports"
)

install(TARGETS phi_transport_mqtt
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/phi/plugins/transports
)

if(BUILD_TESTING)
    # The model, the mapping, the payloads and the commands, without a broker.
    add_executable(mqtt_tests tests/mqtt_tests.cpp)
    target_link_libraries(mqtt_tests PRIVATE phi_mqtt_core)
    add_test(NAME mqtt_tests COMMAND mqtt_tests)

    # The plugin against a mosquitto started on a free port, with a fake core
    # facade. Skipped (exit 77) where /usr/sbin/mosquitto is missing.
    add_executable(mqtt_integration_tests tests/integration_tests.cpp)
    target_link_libraries(mqtt_integration_tests PRIVATE phi_mqtt_core ${PHI_TRANSPORT_API_TARGET})
    add_test(NAME mqtt_integration_tests COMMAND mqtt_integration_tests)
    set_tests_properties(mqtt_integration_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()
