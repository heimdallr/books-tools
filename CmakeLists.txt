cmake_minimum_required(VERSION 3.12)

set(PRODUCT_VERSION 2.6.0)

project(FLibraryTools VERSION ${PRODUCT_VERSION})

if (NOT DEFINED CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Debug)
endif ()
set(CMAKE_CONFIGURATION_TYPES ${CMAKE_BUILD_TYPE})

set(BUILDSCRIPTS_ROOT ${CMAKE_SOURCE_DIR}/src/ext/scripts CACHE STRING "BUILDSCRIPTS_ROOT")
set(FOUNDATION_ROOT ${CMAKE_SOURCE_DIR}/src/ext/foundation)

include(${BUILDSCRIPTS_ROOT}/cmake/conan.cmake)
conan_install(
    ${CMAKE_SOURCE_DIR}/src/home/script/conan
    ${CMAKE_SOURCE_DIR}/src/ext/scripts/conan/profiles/msvc2022_x86_64_${CMAKE_BUILD_TYPE}
    )
set(CMAKE_TOOLCHAIN_FILE "${CMAKE_BINARY_DIR}/conan_toolchain.cmake")
include("${CMAKE_BINARY_DIR}/conan_toolchain.cmake")

if(MSVC)
    if (NOT ${CMAKE_CXX_FLAGS} STREQUAL "")
        string(REGEX REPLACE "/W3" " " CMAKE_C_FLAGS ${CMAKE_CXX_FLAGS})
        string(REGEX REPLACE "/W3" " " CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
    endif()

	add_compile_options(
		/MP     # multi-processor compilation
		/W4		# warnings level 4
		/WX		# warning is error
		/we5038 # reorder
		/wd4250 # inherits via dominance
		/wd4251 # member (ala std::unique_ptr<some>) needs to have dll-interface to be used by clients of class 'some'
		/wd4275 # non dll-interface class 'ISome' used as base for dll-interface class 'Some'
		/wd4503	# decorated name length exceeded, name was truncated
		/bigobj # number of sections exceeded object file format limit
	)
endif()

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(QT_MAJOR_VERSION 6)

include(${BUILDSCRIPTS_ROOT}/cmake/target.cmake)

find_package(plog REQUIRED)
find_package(Boost REQUIRED)
find_package(XercesC REQUIRED)
find_package(7zip REQUIRED)
find_package(ICU REQUIRED)
find_package(libjxl REQUIRED)
find_package(cimg REQUIRED)

set(QtModules Core Gui Widgets Network Svg)
find_package(Qt6 REQUIRED COMPONENTS ${QtModules})
include(QtInstallPaths)

include_directories(${CMAKE_CURRENT_LIST_DIR}/src/home)
include_directories(${CMAKE_CURRENT_LIST_DIR}/src/home/common)

include_directories(${FOUNDATION_ROOT})
include(${FOUNDATION_ROOT}/fnd/fnd.cmake)
include(${FOUNDATION_ROOT}/logging/logging.cmake)
include(${FOUNDATION_ROOT}/network/network.cmake)
include(${FOUNDATION_ROOT}/jxl/jxl.cmake)
include(${FOUNDATION_ROOT}/zip/zip.cmake)
include(${FOUNDATION_ROOT}/database/database.cmake)
include(${FOUNDATION_ROOT}/util/util.cmake)
include(${FOUNDATION_ROOT}/icu/icu.cmake)

string(TOUPPER ${CMAKE_BUILD_TYPE} CMAKE_BUILD_TYPE_UPPER)
file(WRITE "${CMAKE_BINARY_DIR}/var/7z" "${7zip_BIN_DIRS_${CMAKE_BUILD_TYPE_UPPER}}/")
file(WRITE "${CMAKE_BINARY_DIR}/var/version" ${PRODUCT_VERSION})

add_compile_definitions(PLOG_IMPORT)
add_compile_definitions(UNICODE _UNICODE NOMINMAX _USE_MATH_DEFINES)
add_compile_definitions(HYPODERMIC_OVERRIDE_CONSTRUCTOR_ARGUMENT_COUNT=32)
include_directories("src/ext/Hypodermic")

include("src/ext/sqlite.cmake")
include("src/home/lib/lib.cmake")
include("src/home/tool/tool.cmake")
