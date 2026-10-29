cmake_minimum_required(VERSION 3.21)
project(UXWebviewHost VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
	message(FATAL_ERROR "UXWebviewHost builds on Windows only until the macOS/Linux milestones")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Locate the CEF binary distribution: explicit -DUXWV_CEF_ROOT wins, otherwise
# the version pinned in ../cef.lock.json is used.
if(NOT DEFINED UXWV_CEF_ROOT)
	set(_lockFile "${CMAKE_CURRENT_SOURCE_DIR}/../cef.lock.json")
	if(NOT EXISTS "${_lockFile}")
		message(FATAL_ERROR "native/cef.lock.json not found")
	endif()
	file(READ "${_lockFile}" _lockJson)
	string(REGEX MATCH "\"version\"[ \t\r\n]*:[ \t\r\n]*\"([^\"]+)\"" _lockMatch "${_lockJson}")
	set(_cefVersion "${CMAKE_MATCH_1}")
	set(UXWV_CEF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../cef/${_cefVersion}/windows64")
endif()

if(NOT IS_DIRECTORY "${UXWV_CEF_ROOT}")
	message(FATAL_ERROR "CEF not found at '${UXWV_CEF_ROOT}'. Run Scripts/FetchCef.ps1 first.")
endif()

set(CEF_ROOT "${UXWV_CEF_ROOT}")
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)

# The binary distribution ships libcef.lib as an import library; cefsimple
# defines it as an imported target, so do the same.
add_library(libcef_lib STATIC IMPORTED)
set_target_properties(libcef_lib PROPERTIES IMPORTED_LOCATION "${CEF_LIB_RELEASE}")

add_subdirectory("${CEF_ROOT}/libcef_dll" "${CMAKE_BINARY_DIR}/libcef_dll_wrapper")

add_executable(UXWebviewHost
	src/main.cpp
	src/app/HostApp.cpp
	src/browser/BrowserClient.cpp
	src/frame/FramePublisher.cpp
	src/ipc/IpcServer.cpp
	src/util/Log.cpp
)

SET_EXECUTABLE_TARGET_PROPERTIES(UXWebviewHost)
target_include_directories(UXWebviewHost PRIVATE
	"${CEF_ROOT}/include"
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
	"${CMAKE_CURRENT_SOURCE_DIR}/../include"
)
target_compile_definitions(UXWebviewHost PRIVATE
	UNICODE
	_UNICODE
	NOMINMAX
	WIN32_LEAN_AND_MEAN
)
target_link_libraries(UXWebviewHost PRIVATE
	libcef_lib
	libcef_dll_wrapper
	d3d11
	dxgi
)

SET_CEF_TARGET_OUT_DIR()
COPY_FILES(UXWebviewHost "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR_RELEASE}" "${CEF_TARGET_OUT_DIR}")

# CEF_RESOURCE_FILES mixes files and the locales directory, which the
# copy_if_different helper cannot handle. They live in Resources/, not Release/.
set(CEF_RESOURCES_DIR "${CEF_ROOT}/Resources")
foreach(_resource ${CEF_RESOURCE_FILES})
	if(_resource STREQUAL "locales")
		add_custom_command(TARGET UXWebviewHost POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_directory
				"${CEF_RESOURCES_DIR}/locales"
				"${CEF_TARGET_OUT_DIR}/locales"
		)
	else()
		add_custom_command(TARGET UXWebviewHost POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"${CEF_RESOURCES_DIR}/${_resource}"
				"${CEF_TARGET_OUT_DIR}/${_resource}"
		)
	endif()
endforeach()
