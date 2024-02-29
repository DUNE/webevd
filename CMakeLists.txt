# ======================================================================
#  webevd main build file
#
#  cd .../path/to/build/directory
#  source .../path/to/webevd/ups/setup_for_development <-d|-p>
#  buildtool [-I /install/path] -bt[i] -jN
# ======================================================================

cmake_minimum_required(VERSION 3.20 FATAL_ERROR)

find_package(cetmodules REQUIRED)
project(webevd VERSION 09.83.01 LANGUAGES CXX)

include(CetCMakeEnv)
cet_cmake_env()

cet_set_compiler_flags(DIAGS CAUTIOUS
  WERROR
  NO_UNDEFINED
  EXTRA_FLAGS -pedantic -Wno-unused-local-typedefs
)

cet_report_compiler_flags(REPORT_THRESHOLD VERBOSE)

find_package(art_root_io REQUIRED EXPORT)
find_package(gallery REQUIRED EXPORT)
find_package(art REQUIRED EXPORT)
find_package(canvas REQUIRED EXPORT)
find_package(fhiclcpp REQUIRED EXPORT)

find_package(lardata REQUIRED EXPORT)
find_package(larcore REQUIRED EXPORT)
find_package(lardataalg REQUIRED EXPORT)
find_package(lardataobj REQUIRED EXPORT)
find_package(larcorealg REQUIRED EXPORT)
find_package(larcoreobj REQUIRED EXPORT)

find_package(nusimdata REQUIRED EXPORT)

find_package(PNG REQUIRED EXPORT)
find_package(ROOT COMPONENTS Core Physics REQUIRED EXPORT)
find_package(Threads REQUIRED EXPORT)
find_package(ZLIB REQUIRED EXPORT)

# macros for dictionary and simple_plugin
include(ArtDictionary)
include(ArtMake)
include(BuildPlugins)


add_subdirectory(webevd)
add_subdirectory(test)

# packaging utility
cet_cmake_config()
