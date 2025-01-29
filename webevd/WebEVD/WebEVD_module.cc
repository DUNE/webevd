// Chris Backhouse - bckhouse@fnal.gov

#include "art/Framework/Core/EDAnalyzer.h"
#include "art/Framework/Core/ModuleMacros.h"
#include "fhiclcpp/ParameterSet.h"

#include "art/Framework/Services/Registry/ServiceHandle.h"
#include "larcore/Geometry/Geometry.h"
#include "larcore/Geometry/WireReadout.h"
#include "lardata/DetectorInfoServices/DetectorPropertiesService.h"

#include "webevd/WebEVD/InputSeeker.h"
#include "webevd/WebEVD/WebEVDServer.h"

namespace evd {

  class WebEVD : public art::EDAnalyzer {
  public:
    explicit WebEVD(const fhicl::ParameterSet& pset);

    void analyze(const art::Event& evt) override;
    void endJob() override;

  protected:
    evd::WebEVDServer<art::Event> fServer;

    const geo::GeometryCore* fGeom = art::ServiceHandle<geo::Geometry>().get();
    const geo::WireReadoutGeom* fWireReadoutGeom = &art::ServiceHandle<geo::WireReadout>()->Get();
  };

  DEFINE_ART_MODULE(WebEVD)

  // ---------------------------------------------------------------------------
  WebEVD::WebEVD(const fhicl::ParameterSet& pset) : EDAnalyzer(pset) {}

  void WebEVD::endJob() { std::cout << "Ran out of events. Goodbye!" << std::endl; }

  void WebEVD::analyze(const art::Event& evt)
  {
    auto const detProp = art::ServiceHandle<detinfo::DetectorPropertiesService>()->DataFor(evt);
    const Result res = fServer.serve(evt, fGeom, fWireReadoutGeom, detProp);

    switch (res.code) {
    case kNEXT:
      std::cout << "Next clicked in GUI. Going to next event" << std::endl;
      // nothing, allow art to proceed to next
      return;

    case kPREV:
      std::cout << "Prev clicked in GUI. Going to previous event" << std::endl;
      // Because we will automatically increment by one
      art::ServiceHandle<InputSeeker>()->seekToEvent(-2);
      return;

    case kQUIT:
      // TODO give fServer a chance to clean up
      std::cout << "Quit clicked in GUI. Goodbye!" << std::endl;
      exit(0);

    case kERROR: std::cout << "Error. Quitting" << std::endl; exit(1);

    case kSEEK:
      std::cout << "User requested seek to " << res.run << ":" << res.subrun << ":" << res.event
                << std::endl;
      art::ServiceHandle<InputSeeker>()->seekToEvent(art::EventID(res.run, res.subrun, res.event));
      return;

    default: std::cout << "Unhandled result code " << res.code << "!" << std::endl; abort();
    }
  }

} // namespace
