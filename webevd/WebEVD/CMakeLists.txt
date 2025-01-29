cet_make_library(LIBRARY_NAME ThreadsafeGalleryEvent INTERFACE
  SOURCE ThreadsafeGalleryEvent.h
  LIBRARIES INTERFACE
  gallery::gallery
  Threads::Threads
)

cet_make_library(LIBRARY_NAME JSONFormatter INTERFACE
  SOURCE JSONFormatter.h
  LIBRARIES INTERFACE
  ROOT::Physics
)

cet_make_library(BASENAME_ONLY
  SOURCE
  PNGArena.cxx
  TruthText.cxx
  WebEVDServer.cxx
  LIBRARIES
  PUBLIC
  art::Framework_Principal
  PRIVATE
  webevd::JSONFormatter
  webevd::ThreadsafeGalleryEvent
  lardataalg::DetectorInfo
  lardataobj::RawData
  lardataobj::RecoBase
  lardataobj::Simulation
  larcorealg::Geometry
  larcore::headers
  nusimdata::SimulationBase
  gallery::gallery
  fhiclcpp::fhiclcpp
  PNG::PNG
  ROOT::Core
  Threads::Threads
  ZLIB::ZLIB
)

cet_build_plugin(InputSeeker art::service
  LIBRARIES
  PRIVATE
  art_root_io::RootInput_source
)

cet_build_plugin(WebEVD art::EDAnalyzer
  LIBRARIES PRIVATE
  webevd::WebEVD_InputSeeker_service
  webevd::WebEVD
  lardata::DetectorPropertiesService
  larcore::Geometry_Geometry_service
  art::Framework_Services_Registry
  fhiclcpp::fhiclcpp
)

install_headers()
install_source()

project_variable(WEB_DIR webevd CONFIG DOCSTRING "\
Location of WebEVD web support files\
")

file(GLOB webevd_files web/* )
install(FILES ${webevd_files}
  DESTINATION ${${CETMODULES_CURRENT_PROJECT_NAME}_WEB_DIR}
)

add_subdirectory(gallery)
add_subdirectory(fcl)
