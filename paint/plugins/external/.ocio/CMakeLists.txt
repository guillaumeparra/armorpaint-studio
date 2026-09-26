# Lot A1 : libstudio_ocio, shim C au-dessus d'OpenColorIO v2 (ABI : ../studio_ocio_lib.h).
# Construit par ../tools/build_ocio.sh, pas par amake (qui ne compile ni ne lie de C++ depuis external/).
#
# Une seule bibliothèque partagée, qui embarque OCIO et toutes ses dépendances en statique (expat, yaml-cpp,
# pystring, Imath, zlib, minizip-ng, sse2neon sur Apple ARM), toutes construites depuis leurs sources : aucune
# bibliothèque système requise hors toolchain. Elle n'exporte que les symboles studio_ocio_lib_* : ni OCIO ni Imath
# ne peuvent entrer en collision avec d'autres copies chargées dans le processus.

cmake_minimum_required(VERSION 3.24)
project(studio_ocio LANGUAGES C CXX)

# Version d'OCIO : ici et nulle part ailleurs. 2.5.x = VFX Reference Platform CY2026.
set(STUDIO_OCIO_TAG "v2.5.2" CACHE STRING "Tag OpenColorIO embarqué")

# OCIO pose CMAKE_CXX_STANDARD dans le cache (share/cmake/utils/CppVersion.cmake) : il doit y être déjà.
set(CMAKE_CXX_STANDARD 17 CACHE STRING "Standard C++ du shim et d'OCIO")
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Code statique absorbé dans une bibliothèque partagée, visibilité cachée propagée aux dépendances d'OCIO.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

set(BUILD_SHARED_LIBS         OFF CACHE BOOL "" FORCE)
set(OCIO_INSTALL_EXT_PACKAGES ALL CACHE STRING "" FORCE) # dépendances depuis les sources, jamais du système
set(OCIO_BUILD_APPS           OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_TESTS          OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_GPU_TESTS      OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_PYTHON         OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_DOCS           OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_JAVA           OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_OPENFX         OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_NUKE           OFF CACHE BOOL "" FORCE)
set(OCIO_WARNING_AS_ERROR     OFF CACHE BOOL "" FORCE)
set(OCIO_USE_SOVERSION        OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(OpenColorIO
	GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/OpenColorIO.git
	GIT_TAG        "${STUDIO_OCIO_TAG}"
	GIT_SHALLOW    TRUE
	EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(OpenColorIO)

add_library(studio_ocio SHARED studio_ocio_lib.cpp)
target_link_libraries(studio_ocio PRIVATE OpenColorIO)

# La visibilité cachée ne suffit pas (des archives C absorbées peuvent exporter par défaut) : la liste d'export
# imposée à l'éditeur de liens est le verrou. tests : ../.tests/ocio.sh vérifie ce qui sort.
if(APPLE)
	target_link_options(studio_ocio PRIVATE
		"LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/studio_ocio_lib.symbols"
		"LINKER:-dead_strip")
	set_property(TARGET studio_ocio APPEND PROPERTY LINK_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/studio_ocio_lib.symbols")
elseif(UNIX)
	target_link_options(studio_ocio PRIVATE
		"LINKER:--version-script,${CMAKE_CURRENT_SOURCE_DIR}/studio_ocio_lib.map"
		"LINKER:--exclude-libs,ALL"
		"LINKER:--gc-sections")
	set_property(TARGET studio_ocio APPEND PROPERTY LINK_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/studio_ocio_lib.map")
endif()
