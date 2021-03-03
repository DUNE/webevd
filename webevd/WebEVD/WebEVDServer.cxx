// Chris Backhouse - bckhouse@fnal.gov

#include "webevd/WebEVD/WebEVDServer.h"

#include "webevd/WebEVD/PNGArena.h"

#include "webevd/WebEVD/JSONFormatter.h"

#include "webevd/WebEVD/ThreadsafeGalleryEvent.h"

#include "webevd/WebEVD/TruthText.h"

#include <string>

#include "fhiclcpp/ParameterSet.h"
#include "art/Framework/Principal/Handle.h"

#include "art/Framework/Principal/Event.h"
#include "gallery/Event.h"

#include "lardataobj/RecoBase/Hit.h"
#include "lardataobj/RecoBase/OpFlash.h"
#include "lardataobj/RecoBase/SpacePoint.h"
#include "lardataobj/RecoBase/Wire.h"
#include "lardataobj/RecoBase/Track.h"
#include "lardataobj/RecoBase/Vertex.h"

#include "nusimdata/SimulationBase/MCParticle.h"
#include "nusimdata/SimulationBase/MCTruth.h"

#include "lardataobj/RawData/RawDigit.h"
#include "lardataobj/RawData/raw.h" // Uncompress()

#include "larcorealg/Geometry/GeometryCore.h"
#include "lardataalg/DetectorInfo/DetectorPropertiesData.h"

#include "garsoft/Geometry/GeometryCore.h"
#include "garsoft/ReconstructionDataProducts/Hit.h"
#include "garsoft/ReconstructionDataProducts/CaloHit.h"
#include "garsoft/ReconstructionDataProducts/Cluster.h"
#include "garsoft/ReconstructionDataProducts/TPCCluster.h"
#include "garsoft/ReconstructionDataProducts/Track.h"
#include "garsoft/ReconstructionDataProducts/TrackTrajectory.h"
#include "garsoft/ReconstructionDataProducts/Vertex.h"

#include <sys/types.h>
#include <sys/socket.h>
//#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */

#include <sys/types.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "signal.h"

#include "zlib.h"

#include <thread>

namespace std{
  bool operator<(const art::InputTag& a, const art::InputTag& b)
  {
    return (std::make_tuple(a.label(), a.instance(), a.process()) <
            std::make_tuple(b.label(), b.instance(), b.process()));
  }
}

namespace evd
{

// ----------------------------------------------------------------------------
template<class T> WebEVDServer<T>::WebEVDServer()
  : fSock(0)
{
}

// ----------------------------------------------------------------------------
template<class T> WebEVDServer<T>::~WebEVDServer()
{
  if(fSock) close(fSock);
}

short swap_byte_order(short x)
{
  char* cx = (char*)&x;
  std::swap(cx[0], cx[1]);
  return x;
}

void write_ok200(int sock,
                 const std::string content = "text/html",
                 bool gzip = false)
{
  std::string str =
    "HTTP/1.0 200 OK\r\n"
    "Server: WebEVD/1.0.0\r\n"
    "Content-Type: "+content+"\r\n";

  if(gzip) str += "Content-Encoding: gzip\r\n";

  str += "\r\n";

  write(sock, &str.front(), str.size());
}

void write_notfound404(int sock)
{
  const char str[] =
    "HTTP/1.0 404 Not Found\r\n"
    "Server: WebEVD/1.0.0\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "404. Huh?\r\n";

  write(sock, str, strlen(str));
}

void write_unimp501(int sock)
{
  const char str[] =
    "HTTP/1.0 501 Not Implemented\r\n"
    "Server: WebEVD/1.0.0\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "I don't know how to do that\r\n";

  write(sock, str, strlen(str));
}

std::string read_all(int sock)
{
  std::string ret;

  std::vector<char> buf(1024*1024);
  while(true){
    const int nread = read(sock, &buf.front(), buf.size());
    if(nread == 0) return ret;
    ret.insert(ret.end(), buf.begin(), buf.begin()+nread);
    // Only handle GETs, so no need to wait for payload (for which we'd need to
    // check Content-Length too).
    if(ret.find("\r\n\r\n") != std::string::npos) return ret;
  }
}

EResult err(const char* call)
{
  std::cout << call << "() error " << errno << " = " << strerror(errno) << std::endl;
  return kERROR;
  //  return errno;
}

Result HandleCommand(std::string cmd, int sock)
{
  EResult code = kERROR;
  int run = -1, subrun = -1, evt = -1;
  bool traces = false;

  if(cmd == "/QUIT") code = kQUIT;
  if(cmd == "/NEXT") code = kNEXT;
  if(cmd == "/PREV") code = kPREV;
  if(cmd == "/NEXT_TRACES"){ code = kNEXT; traces = true;}
  if(cmd == "/PREV_TRACES"){ code = kPREV; traces = true;}

  if(cmd.find("/seek/") == 0 ||
     cmd.find("/seek_traces/") == 0){
    if(cmd.find("/seek_traces/") == 0) traces = true;

    code = kSEEK;
    char* ctx;
    strtok_r(cmd.data(), "/", &ctx); // consumes the "seek" text
    run    = atoi(strtok_r(0, "/", &ctx));
    subrun = atoi(strtok_r(0, "/", &ctx));
    evt    = atoi(strtok_r(0, "/", &ctx));
    // if this goes wrong we get zeros, which seems a reasonable fallback
  }

  write_ok200(sock, "text/html", false);

  const int delay = (code == kQUIT) ? 2000 : 0;
  const std::string txt = (code == kQUIT) ? "Goodbye!" : "Please wait...";
  const std::string next = traces ? "/traces.html" : "/";

  // The script tag to set the style is a pretty egregious layering violation,
  // but doing more seems overkill for a simple interstitial page.
  const std::string msg = TString::Format("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><script>setTimeout(function(){window.location.replace('%s');}, %d);</script></head><body><script>if(window.sessionStorage.theme != 'lighttheme'){document.body.style.backgroundColor='black';document.body.style.color='white';}</script><h1>%s</h1></body></html>", next.c_str(), delay, txt.c_str()).Data();

  write(sock, msg.c_str(), msg.size());
  close(sock);

  if(code == kSEEK){
    return Result(kSEEK, run, subrun, evt);
  }
  else{
    return code;
  }
}

// ----------------------------------------------------------------------------
std::string FindWebDir()
{
  std::string webdir;

  // For development purposes we prefer to serve the files from the source
  // directory, which allows them to be live-edited with just a refresh of the
  // browser window to see them.
  if(getenv("MRB_SOURCE")) cet::search_path("MRB_SOURCE").find_file("webevd/webevd/WebEVD/web/", webdir);
  // Otherwise, serve the files from where they get installed
  if(webdir.empty() && getenv("PRODUCTS") && getenv("WEBEVD_VERSION")) cet::search_path("PRODUCTS").find_file("webevd/"+std::string(getenv("WEBEVD_VERSION"))+"/webevd/", webdir);

  if(webdir.empty()){
    std::cout << "Unable to find webevd files under $MRB_SOURCE or $PRODUCTS" << std::endl;
    abort();
  }

  return webdir;
}

class ILazy
{
public:
  virtual void Serialize(JSONFormatter& json) = 0;
  virtual PNGArena& GetArena() = 0;
};

// ----------------------------------------------------------------------------
void _HandleGetPNG(std::string doc, int sock, ILazy* digs, ILazy* wires)
{
  const std::string mime = "image/png";

  // Parse the filename
  char* ctx;

  const char* pName = strtok_r(&doc.front(), "_", &ctx);
  const char* pIdx = strtok_r(0, "_", &ctx);
  const char* pDim = strtok_r(0, ".", &ctx);

  if(!pName || !pIdx || !pDim){
    write_notfound404(sock);
    close(sock);
    return;
  }

  const std::string name(pName);
  const int idx = atoi(pIdx);
  const int dim = atoi(pDim);

  PNGArena* arena = 0;
  if(name == "/dig") arena = &digs->GetArena();
  if(name == "/wire") arena = &wires->GetArena();

  if(!arena || idx >= int(arena->data.size()) || dim > PNGArena::kArenaSize){
    write_notfound404(sock);
    close(sock);
    return;
  }

  write_ok200(sock, mime, false);
  FILE* f = fdopen(sock, "wb");
  arena->WritePNGBytes(f, idx, dim);
  fclose(f);
}

// ----------------------------------------------------------------------------
void gzip_buffer(unsigned char* src,
                 int length,
                 std::vector<unsigned char>& dest,
                 int level)
{
  // C++20 will allow to use designated initializers here
  z_stream strm;
  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;
  strm.opaque = Z_NULL;

  strm.next_in = src;
  strm.avail_in = length;

  // The 16 here is the secret sauce to get gzip header and trailer for some
  // reason...
  deflateInit2(&strm, level, Z_DEFLATED, 15 | 16, 9, Z_DEFAULT_STRATEGY);

  // If we allocate a big enough buffer we can deflate in one pass
  dest.resize(deflateBound(&strm, length));

  strm.next_out = dest.data();
  strm.avail_out = dest.size();

  deflate(&strm, Z_FINISH);

  dest.resize(dest.size() - strm.avail_out);

  deflateEnd(&strm);
}

// ----------------------------------------------------------------------------
void write_compressed_buffer(unsigned char* src,
                             int length,
                             int sock,
                             int level,
                             const std::string& name)
{
  std::vector<unsigned char> dest;
  gzip_buffer(src, length, dest, level);

  std::cout << "Writing " << length << " bytes (compressed to " << dest.size() << ") for " << name << "\n" << std::endl;

  write(sock, dest.data(), dest.size());
}

// ----------------------------------------------------------------------------
void write_compressed_file(const std::string& loc, int fd_out, int level)
{
  int fd_in = open(loc.c_str(), O_RDONLY);

  // Map in the whole file
  struct stat st;
  fstat(fd_in, &st);
  unsigned char* src = (unsigned char*)mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd_in, 0);

  write_compressed_buffer(src, st.st_size, fd_out, level, loc);

  munmap(src, st.st_size);

  close(fd_in);
}

// ----------------------------------------------------------------------------
bool endswith(const std::string& s, const std::string& suffix)
{
  return s.rfind(suffix)+suffix.size() == s.size();
}

// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const TVector3& v)
{
  return json << std::vector<double>{v.X(), v.Y(), v.Z()};
}

// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const geo::Point_t& pt)
{
  return json << std::vector<double>{pt.X(), pt.Y(), pt.Z()};
}

// ----------------------------------------------------------------------------

std::string ToString(const art::InputTag& tag)
{
  std::string ret = tag.label();
  if(!tag.instance().empty()) ret += ":"+tag.instance();
  if(!tag.process().empty()) ret += ":"+tag.process();
  return ret;
}

// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const art::InputTag& tag)
{
  return json << ToString(tag);
}

// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const geo::OpDetID& id)
{
  return json << std::string(id);
}


// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const geo::PlaneID& plane)
{
  return json << std::string(plane);
}

// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const recob::Hit& hit)
{
  return json << Dict<unsigned int, double>("wire", geo::WireID(hit.WireID()).Wire,
                                            "tick", hit.PeakTime(),
                                            "rms", hit.RMS(),
                                            "peakamp", hit.PeakAmplitude());
}

// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const recob::Vertex& vtx)
{
  return json << vtx.position();
}

// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const gar::rec::Vertex& vtx)
{
  return json << TVector3(vtx.Position());
}

// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const simb::MCTruth& mct)
{
  // Don't show MCTruth for cosmic rays, which can be extremely
  // lengthy. Ideally we should exclude them from the list entirely, but this
  // requires less change to the structure of the code.
  if(mct.Origin() == simb::kCosmicRay) return json << std::string();

  return json << MCTruthShortText(mct);
}

// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const recob::SpacePoint& sp)
{
  return json << Dict<geo::Point_t>("pos", sp.position());
}

// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const gar::rec::Hit& hit)
{
  return json << Dict<TVector3>("pos", TVector3(hit.Position()[0], hit.Position()[1], hit.Position()[2]));
}

// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const gar::rec::CaloHit& hit)
{
  return json << Dict<TVector3, double>("pos", TVector3(hit.Position()[0], hit.Position()[1], hit.Position()[2]), "radius", 2);
}

// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const gar::rec::Cluster& clust)
{
  return json << Dict<TVector3, double>("pos", TVector3(clust.Position()[0], clust.Position()[1], clust.Position()[2]), "radius", 5);
}

// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const gar::rec::TPCCluster& clust)
{
  return json << Dict<TVector3>("pos", TVector3(clust.Position()[0], clust.Position()[1], clust.Position()[2]));
}

// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const recob::Track& track)
{
  std::vector<geo::Point_t> pts;

  const recob::TrackTrajectory& traj = track.Trajectory();
  for(unsigned int j = traj.FirstValidPoint(); j <= traj.LastValidPoint(); ++j){
    if(traj.HasValidPoint(j)) pts.push_back(traj.LocationAtPoint(j));
  }

  return json << Dict<std::vector<geo::Point_t>>("positions", pts);
}

// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const gar::rec::Track& trk)
{
  const std::vector<geo::Point_t> pts = {{trk.Vertex()[0], trk.Vertex()[1], trk.Vertex()[2]},
                                         {trk.End()[0], trk.End()[1], trk.End()[2]}};

  return json << Dict<std::vector<geo::Point_t>>("positions", pts);
}

// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const gar::rec::TrackTrajectory& traj)
{
  // TODO difference between FWD and BAK?
  return json << Dict<std::vector<TVector3>>("positions", traj.getBAKTrajectory());
}

// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const simb::MCParticle& part)
{
  const int apdg = abs(part.PdgCode());
  std::vector<TVector3> pts;
  if(apdg != 12 && apdg != 14 && apdg != 16){ // skip decay neutrinos
    for(unsigned int j = 0; j < part.NumberTrajectoryPoints(); ++j){
      pts.emplace_back(part.Vx(j), part.Vy(j), part.Vz(j));
    }
  }

  return json << Dict<int, std::vector<TVector3>>("pdg", apdg,
                                                  "positions", pts);
}

// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const recob::OpFlash& flash)
{
  return json << Dict<double>("tcenter", flash.Time(),
                              "twidth", flash.TimeWidth(),
                              "ycenter", flash.YCenter(),
                              "ywidth", flash.YWidth(),
                              "zcenter", flash.ZCenter(),
                              "zwidth", flash.ZWidth(),
                              "totpe", flash.TotalPE());
}

// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const geo::CryostatGeo& cryo)
{
  return json << Dict<geo::Point_t, std::string>("shape", "cuboid",
                                                 "min", cryo.BoundingBox().Min(),
                                                 "max", cryo.BoundingBox().Max());
}

// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const geo::OpDetGeo& opdet)
{
  const geo::Point_t center = opdet.GetCenter();
  const geo::Vector_t size(opdet.Width()/2, opdet.Height()/2, opdet.Length()/2);
  return json << Dict<geo::OpDetID, geo::Point_t, std::string>("shape", "cuboid",
                                                               "name", opdet.ID(),
                                                               "min", center-size,
                                                               "max", center+size);
}

// ----------------------------------------------------------------------------
JSONFormatter& operator<<(JSONFormatter& json, const PNGView& v)
{
  std::vector<Dict<int, std::string>> blocks;

  for(unsigned int ix = 0; ix < v.blocks.size(); ++ix){
    for(unsigned int iy = 0; iy < v.blocks[ix].size(); ++iy){
      const png_byte* b = v.blocks[ix][iy];
      if(!b) continue;

      int dataidx = 0;
      for(unsigned int d = 0; d < v.arena.data.size(); ++d){
        if(b >= &v.arena.data[d]->front() &&
           b <  &v.arena.data[d]->front() + 4*PNGArena::kArenaSize*PNGArena::kArenaSize){
          dataidx = d;
          break;
        }
      }

      const int texdx = ((b-&v.arena.data[dataidx]->front())/4)%PNGArena::kArenaSize;
      const int texdy = ((b-&v.arena.data[dataidx]->front())/4)/PNGArena::kArenaSize;

      blocks.emplace_back("x", ix*PNGArena::kBlockSize,
                          "y", iy*PNGArena::kBlockSize,
                          "dx", PNGArena::kBlockSize,
                          "dy", PNGArena::kBlockSize,
                          "fname", v.arena.name+"_"+std::to_string(dataidx),
                          "texdim", PNGArena::kArenaSize,
                          "u", texdx,
                          "v", texdy,
                          "du", PNGArena::kBlockSize,
                          "dv", PNGArena::kBlockSize);
    }
  }

  return json << Dict<decltype(blocks)>("blocks", blocks);
}

// ----------------------------------------------------------------------------
template<class TProd, class TEvt> void
SerializeProduct(const TEvt& evt, JSONFormatter& json)
{
  Dict<const std::vector<TProd>*> dict;

  for(const art::InputTag& tag: evt.template getInputTags<std::vector<TProd>>()){
    typename TEvt::template HandleT<std::vector<TProd>> prods; // deduce handle type
    evt.getByLabel(tag, prods);

    dict[ToString(tag)] = prods.product();
  }

  json << dict;
}

// ----------------------------------------------------------------------------
template<class TProd, class TEvt> void
SerializeProductByLabel(const TEvt& evt,
                        const std::string& in_label,
                        JSONFormatter& json)
{
  typename TEvt::template HandleT<std::vector<TProd>> prods; // deduce handle type
  evt.getByLabel(in_label, prods);

  if(prods.isValid()){
    json << *prods;
  }
  else{
    json << std::vector<int>();
  }
}

// ----------------------------------------------------------------------------
template<class T> void SerializeEventID(const T& evt, JSONFormatter& json)
{
  json << Dict<int>("run", evt.run(),
                    "subrun", evt.subRun(),
                    "evt", evt.event());
}

// ----------------------------------------------------------------------------
void SerializeEventID(const ThreadsafeGalleryEvent& evt, JSONFormatter& json)
{
  SerializeEventID(evt.eventAuxiliary(), json);
}

// ----------------------------------------------------------------------------
Dict<Dict<int, double, TVector3>> SerializePlanes(const gar::geo::GeometryCore* geom,
                                                  const detinfo::DetectorPropertiesData& detprop)
{
  Dict<Dict<int, double, TVector3>> ret;
  /*
  for(geo::PlaneID plane: geom->IteratePlaneIDs()){
    const geo::PlaneGeo& planegeo = geom->Plane(plane);

    const double tick_origin = detprop.ConvertTicksToX(0, plane);
    const double tick_pitch = detprop.ConvertTicksToX(1, plane) - tick_origin;

    ret[std::string(plane)] = Dict<int, double, TVector3>
      ("view", int(planegeo.View()),
       "nwires", int(planegeo.Nwires()),
       "pitch", planegeo.WirePitch(),
       "nticks", int(detprop.NumberTimeSamples()),
       "tick_origin", tick_origin,
       "tick_pitch", tick_pitch,
       "center", planegeo.GetCenter(),
       "across", planegeo.GetIncreasingWireDirection(),
       "wiredir", planegeo.GetWireDirection(),
       "depth", planegeo.Depth(),
       "width", planegeo.Width(),
       "depthdir", planegeo.DepthDir(),
       "widthdir", planegeo.WidthDir(),
       "normal", planegeo.GetNormalDirection());
  }
  */
  return ret;
}

typedef Dict<TVector3, std::string, double, int> ShapeDict_t;

// ----------------------------------------------------------------------------
ShapeDict_t CuboidGeom(TVector3 center, TVector3 halfsize)
{
  return {"shape", "cuboid",
      "min", center-halfsize,
      "max", center+halfsize};
}

// ----------------------------------------------------------------------------
ShapeDict_t CuboidGeom(double cx, double cy, double cz, double dx, double dy, double dz)
{
  return CuboidGeom(TVector3(cx, cy, cz), TVector3(dx, dy, dz));
}

// ----------------------------------------------------------------------------
ShapeDict_t CylinderGeom(TVector3 center, double radius, double length)
{
  return {"shape", "cylinder",
      "center", center,
      "radius", radius,
      "length", length};
}

// ----------------------------------------------------------------------------
ShapeDict_t CylinderGeom(double cx, double cy, double cz, double radius, double length)
{
  return CylinderGeom(TVector3(cx, cy, cz), radius, length);
}

// ----------------------------------------------------------------------------
ShapeDict_t PrismGeom(TVector3 center, double radius, double length, int sides)
{
  return {"shape", "prism",
      "center", center,
      "radius", radius,
      "length", length,
      "sides", sides};
}

// ----------------------------------------------------------------------------
ShapeDict_t PrismGeom(double cx, double cy, double cz, double radius, double length, int sides)
{
  return PrismGeom(TVector3(cx, cy, cz), radius, length, sides);
}

// ----------------------------------------------------------------------------
void SerializeGeometry(const gar::geo::GeometryCore* geom,
                       const detinfo::DetectorPropertiesData& detprop,
                       JSONFormatter& json)
{
  const auto planes = SerializePlanes(geom, detprop);

  std::vector<const geo::CryostatGeo*> cryos;
  //  for(const geo::CryostatGeo& cryo: geom->IterateCryostats()) cryos.push_back(&cryo);

  std::vector<const geo::OpDetGeo*> opdets;
  //  for(unsigned int i = 0; i < geom->NOpDets(); ++i) opdets.push_back(&geom->OpDetGeoFromOpDet(i)); // IterateOpDets() doesn't seem to exist

  const TVector3 origin(geom->GetOriginX(), geom->GetOriginY(), geom->GetOriginZ());

  const std::vector<ShapeDict_t> mpd =
    {CuboidGeom(geom->GetMPDX(), geom->GetMPDY(), geom->GetMPDZ(), geom->GetMPDHalfWidth(), geom->GetMPDHalfHeight(), .5*geom->GetMPDLength())};

  const std::vector<ShapeDict_t> lar =
    {CuboidGeom(geom->GetLArTPCX(), geom->GetLArTPCY(), geom->GetLArTPCZ(), geom->GetLArTPCHalfWidth(), geom->GetLArTPCHalfHeight(), .5*geom->GetLArTPCLength())};

  const std::vector<ShapeDict_t> active_lar =
    {CuboidGeom(geom->GetActiveLArTPCX(), geom->GetActiveLArTPCY(), geom->GetActiveLArTPCZ(), geom->GetActiveLArTPCHalfWidth(), geom->GetActiveLArTPCHalfHeight(), .5*geom->GetActiveLArTPCLength())};

  const TVector3 tpccent(geom->TPCXCent(), geom->TPCYCent(), geom->TPCZCent());
  const std::vector<ShapeDict_t> tpc =
    {CylinderGeom(tpccent, geom->TPCRadius(), geom->TPCLength())};

  const std::vector<ShapeDict_t> garlite =
    {CylinderGeom(geom->GArLiteXCent(), geom->GArLiteYCent(), geom->GArLiteZCent(), geom->GArLiteRadius(), geom->GArLiteLength())};

  const double rocMidR = (geom->GetIROCOuterRadius()+geom->GetOROCInnerRadius())/2;
  const std::vector<ShapeDict_t> roc =
    {CylinderGeom(tpccent, geom->GetIROCInnerRadius(), geom->TPCLength()),
     CylinderGeom(tpccent, rocMidR,                    geom->TPCLength()),
//   CylinderGeom(tpccent, geom->GetOROCPadHeightChangeRadius(), geom->TPCLength()),
     CylinderGeom(tpccent, geom->GetOROCOuterRadius(), geom->TPCLength())};

  const std::vector<ShapeDict_t> cathode =
    {CylinderGeom(tpccent, geom->GetOROCOuterRadius(), 0.1)};

  const std::vector<ShapeDict_t> ecal =
    {PrismGeom(tpccent, geom->GetECALInnerBarrelRadius(), geom->TPCLength(), geom->GetECALInnerSymmetry()),
     PrismGeom(tpccent, geom->GetECALOuterBarrelRadius(), geom->TPCLength(), geom->GetECALInnerSymmetry())};

  const std::vector<ShapeDict_t> muid =
    {PrismGeom(tpccent, geom->GetMuIDInnerBarrelRadius(), geom->TPCLength(), geom->GetMuIDInnerSymmetry()),
     PrismGeom(tpccent, geom->GetMuIDOuterBarrelRadius(), geom->TPCLength(), geom->GetMuIDInnerSymmetry())};

  const TVector3 endcapcentp((geom->GetECALEndcapStartX()+geom->GetECALEndcapOuterX())/2, tpccent.Y(), tpccent.Z());
  const TVector3 endcapcentn(-endcapcentp.X(), tpccent.Y(), tpccent.Z());
  const double dx = geom->GetECALEndcapOuterX() - geom->GetECALEndcapStartX();

  const std::vector<ShapeDict_t> endcap =
    {CylinderGeom(endcapcentp, geom->GetECALInnerEndcapRadius(), dx),
     CylinderGeom(endcapcentp, geom->GetECALOuterEndcapRadius(), dx),
     CylinderGeom(endcapcentn, geom->GetECALInnerEndcapRadius(), dx),
     CylinderGeom(endcapcentn, geom->GetECALOuterEndcapRadius(), dx)};


  Dict<TVector3,
       std::vector<ShapeDict_t>,
       decltype(&planes),
       std::vector<const geo::CryostatGeo*>,
       std::vector<const geo::OpDetGeo*>> dict("origin", origin,
                                               "planes", &planes,
                                               "Cryostats", cryos,
                                               "OpDets", opdets,
                                               "MPD", mpd,
                                               "ROCs", roc,
                                               "Cathode", cathode);

  if(geom->HasLArTPCDetector()){
    dict["LAr"] = lar;
    dict["Active&nbsp;LAr"] = active_lar;
  }
  if(geom->HasTrackerScDetector()) dict["GArLite"] = garlite;
  if(geom->HasECALDetector()){
    dict["ECAL"] = ecal;
    dict["ECAL&nbsp;endcaps"] = endcap;
  }
  if(geom->HasMuonDetector()) dict["MuID"] = muid;
  if(geom->HasGasTPCDetector()) dict["TPC"] = tpc;

  json << dict;
}

// ----------------------------------------------------------------------------
template<class T> void
SerializeHits(const T& evt, const gar::geo::GeometryCore* geom, JSONFormatter& json)
{
  std::map<art::InputTag, std::map<geo::PlaneID, std::vector<recob::Hit>>> plane_hits;

  /*
  for(art::InputTag tag: evt.template getInputTags<std::vector<recob::Hit>>()){
    typename T::template HandleT<std::vector<recob::Hit>> hits; // deduce handle type
    evt.getByLabel(tag, hits);

    for(const recob::Hit& hit: *hits){
      // Would possibly be right for disambiguated hits?
      //    const geo::WireID wire(hit.WireID());

      for(geo::WireID wire: geom->ChannelToWire(hit.Channel())){
        const geo::PlaneID plane(wire);

        // Correct for disambiguated hits
        //      plane_hits[plane].push_back(hit);

        // Otherwise we have to update the wire number
        plane_hits[tag][plane].emplace_back(hit.Channel(), hit.StartTick(), hit.EndTick(), hit.PeakTime(), hit.SigmaPeakTime(), hit.RMS(), hit.PeakAmplitude(), hit.SigmaPeakAmplitude(), hit.SummedADC(), hit.Integral(), hit.SigmaIntegral(), hit.Multiplicity(), hit.LocalIndex(), hit.GoodnessOfFit(), hit.DegreesOfFreedom(), hit.View(), hit.SignalType(), wire);
      }
    }
  } // end for tag
  */

  json << plane_hits;
}

// ----------------------------------------------------------------------------
template<class T> std::map<int, std::vector<T>> ToSnippets(const std::vector<T>& adcs, T pedestal = 0)
{
  std::vector<T> snip;
  snip.reserve(adcs.size());

  std::map<int, std::vector<T>> snips;

  int t = 0;
  for(T adc: adcs){
    if(adc == 0){
      if(!snip.empty()){
        snips[t-snip.size()] = snip;
        snip.clear();
      }
    }
    else{
      snip.push_back(adc - pedestal);
    }

    ++t;
  } // end for adc

  // Save last in-progress snippet if necessary
  if(!snip.empty()) snips[t-snip.size()] = snip;

  // this is a bit of a hack to teach the viewer how long the full trace
  // is
  snips[adcs.size()] = {};

  return snips;
}

// ----------------------------------------------------------------------------
template<class T> void SerializeDigitTraces(const T& evt,
                                            const gar::geo::GeometryCore* geom,
                                            JSONFormatter& json)
{
  // [tag][plane][wire index][t0]
  std::map<art::InputTag, std::map<geo::PlaneID, std::map<int, std::map<int, std::vector<short>>>>> traces;

  /*
  for(art::InputTag tag: evt.template getInputTags<std::vector<raw::RawDigit>>()){
    typename T::template HandleT<std::vector<raw::RawDigit>> digs; // deduce handle type
    evt.getByLabel(tag, digs);

    for(const raw::RawDigit& dig: *digs){
      for(geo::WireID wire: geom->ChannelToWire(dig.Channel())){
        const geo::PlaneID plane(wire);

        raw::RawDigit::ADCvector_t adcs(dig.Samples());
        raw::Uncompress(dig.ADCs(), adcs, dig.Compression());

        traces[tag][plane][wire.Wire] = ToSnippets(adcs, short(dig.GetPedestal()));
      } // end for wire
    } // end for dig
  } // end for tag
  */

  json << traces;
}

// ----------------------------------------------------------------------------
template<class T> void SerializeWireTraces(const T& evt,
                                           const gar::geo::GeometryCore* geom,
                                           JSONFormatter& json)
{
  // [tag][plane][wire][t0]
  std::map<art::InputTag, std::map<geo::PlaneID, std::map<int, std::map<int, std::vector<float>>>>> traces;

  /*
  for(art::InputTag tag: evt.template getInputTags<std::vector<recob::Wire>>()){
    typename T::template HandleT<std::vector<recob::Wire>> wires; // deduce handle type
    evt.getByLabel(tag, wires);

    for(const recob::Wire& rbwire: *wires){
      // Place all wire traces on the first wire (== channel) they are found on
      const geo::WireID wire =  geom->ChannelToWire(rbwire.Channel())[0];
      const geo::PlaneID plane(wire);

      traces[tag][plane][wire.Wire] = ToSnippets(rbwire.Signal());
    } // end for rbwire
  } // end for tag
  */

  json << traces;
}


// ----------------------------------------------------------------------------
template<class T> void _HandleGetJSON(std::string doc, int sock, const T* evt, const gar::geo::GeometryCore* geom, const detinfo::DetectorPropertiesData* detprop, ILazy* digs, ILazy* wires)
{
  const std::string mime = "application/json";

  std::stringstream ss;
  JSONFormatter json(ss);

  /***/if(doc == "/evtid.json")       SerializeEventID(*evt, json);
  //  else if(doc == "/tracks.json")      SerializeProduct<recob::Track>(*evt, json);
  //  else if(doc == "/tracks.json")      SerializeProduct<gar::rec::Track>(*evt, json);
  else if(doc == "/tracks.json")      SerializeProduct<gar::rec::TrackTrajectory>(*evt, json);
  else if(doc == "/spacepoints.json") SerializeProduct<recob::SpacePoint>(*evt, json);
  else if(doc == "/garhits.json")     SerializeProduct<gar::rec::Hit>(*evt, json);
  else if(doc == "/calohits.json")    SerializeProduct<gar::rec::CaloHit>(*evt, json);
  else if(doc == "/clusts.json")      SerializeProduct<gar::rec::Cluster>(*evt, json);
  else if(doc == "/tpcclusts.json")   SerializeProduct<gar::rec::TPCCluster>(*evt, json);
  //  else if(doc == "/vtxs.json")        SerializeProduct<recob::Vertex>(*evt, json);
  else if(doc == "/vtxs.json")        SerializeProduct<gar::rec::Vertex>(*evt, json);
  else if(doc == "/trajs.json")       SerializeProductByLabel<simb::MCParticle>(*evt, /*"largeant"*/"edepconvert", json);
  else if(doc == "/mctruth.json")     SerializeProduct<simb::MCTruth>(*evt, json);
  else if(doc == "/opflashes.json")   SerializeProduct<recob::OpFlash>(*evt, json);
  else if(doc == "/hits.json")        SerializeHits(*evt, geom, json);
  else if(doc == "/geom.json")        SerializeGeometry(geom, *detprop, json);
  else if(doc == "/digs.json")        digs->Serialize(json);
  else if(doc == "/wires.json")       wires->Serialize(json);
  else if(doc == "/dig_traces.json")  SerializeDigitTraces(*evt, geom, json);
  else if(doc == "/wire_traces.json") SerializeWireTraces(*evt, geom, json);
  else{
    write_notfound404(sock);
    close(sock);
    return;
  }

  std::string response = ss.str();
  write_ok200(sock, mime, true);
  write_compressed_buffer((unsigned char*)response.data(), response.size(), sock, Z_DEFAULT_COMPRESSION, doc);
  close(sock);
}

// ----------------------------------------------------------------------------
template<class T> void _HandleGet(std::string doc, int sock, const T* evt, ILazy* digs, ILazy* wires, const gar::geo::GeometryCore* geom, const detinfo::DetectorPropertiesData* detprop)
{
  if(doc == "/") doc = "/index.html";

  if(endswith(doc, ".png")){
    _HandleGetPNG(doc, sock, digs, wires);
    return;
  }

  if(endswith(doc, ".json")){
    _HandleGetJSON(doc, sock, evt, geom, detprop, digs, wires);
    return;
  }

  // TODO - more sophisticated MIME type handling
  std::string mime = "text/html";
  if(endswith(doc, ".js" )) mime = "application/javascript";
  if(endswith(doc, ".css")) mime = "text/css";
  if(endswith(doc, ".ico")) mime = "image/vnd.microsoft.icon";

  // Otherwise it must be a physical file

  // Don't accidentally serve any file we shouldn't
  const std::set<std::string> whitelist = {"/evd.css", "/evd.js", "/traces.js", "/favicon.ico", "/index.html", "/traces.html"};

  if(whitelist.count(doc)){
    write_ok200(sock, mime, true);
    write_compressed_file(FindWebDir()+doc, sock, Z_DEFAULT_COMPRESSION);
  }
  else{
    write_notfound404(sock);
  }

  close(sock);
}

// ----------------------------------------------------------------------------
template<class T> int WebEVDServer<T>::EnsureListen()
{
  if(fSock != 0) return 0;

  char host[1024];
  gethostname(host, 1024);
  char* user = getlogin();

  std::cout << "\n------------------------------------------------------------\n" << std::endl;

  // E1071 is DUNE :)
  int port = 1071;

  // Search for an open port up-front
  while(system(TString::Format("ss -an | grep -q %d", port).Data()) == 0) ++port;


  fSock = socket(AF_INET, SOCK_STREAM, 0);
  if(fSock == -1) return err("socket");

  // Reuse port immediately even if a previous instance just aborted.
  const int one = 1;
  if(setsockopt(fSock, SOL_SOCKET, SO_REUSEADDR,
                &one, sizeof(one)) != 0) return err("setsockopt");

  sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = swap_byte_order(port);
  addr.sin_addr.s_addr = INADDR_ANY;

  if(bind(fSock, (sockaddr*)&addr, sizeof(addr)) != 0) return err("bind");

  if(listen(fSock, 128/*backlog*/) != 0) return err("listen");


  std::cout << "First run" << std::endl;
  std::cout << "ssh -L "
            << port << ":localhost:" << port << " "
            << user << "@" << host << std::endl << std::endl;
  std::cout << "and then navigate to localhost:" << port << " in your favorite browser." << std::endl << std::endl;
  //  std::cout << "Press Ctrl-C here when done." << std::endl;

  return 0;
}

template<class T> class LazyDigits: public ILazy
{
public:
  LazyDigits(const T& evt, const gar::geo::GeometryCore* geom)
    : fEvt(&evt), fGeom(geom), fArena("dig")
  {
  }

  virtual void Serialize(JSONFormatter& json) override
  {
    Init();
    json << fImgs;
  }

  virtual PNGArena& GetArena() override
  {
    Init();
    return fArena;
  }

protected:
  void Init()
  {
    std::lock_guard guard(fLock);

    if(!fEvt || !fGeom) return; // already init'd
    /*
    for(art::InputTag tag: fEvt->template getInputTags<std::vector<raw::RawDigit>>()){
      typename T::template HandleT<std::vector<raw::RawDigit>> digs; // deduce handle type
      fEvt->getByLabel(tag, digs);

      for(const raw::RawDigit& dig: *digs){
        for(geo::WireID wire: fGeom->ChannelToWire(dig.Channel())){
          //        const geo::TPCID tpc(wire);
          const geo::PlaneID plane(wire);

          const geo::WireID w0 = fGeom->GetBeginWireID(plane);
          const unsigned int Nw = fGeom->Nwires(plane);

          if(fImgs[tag].count(plane) == 0){
            fImgs[tag].emplace(plane, PNGView(fArena, Nw, dig.Samples()));
          }

          PNGView& bytes = fImgs[tag].find(plane)->second;

          raw::RawDigit::ADCvector_t adcs(dig.Samples());
          raw::Uncompress(dig.ADCs(), adcs, dig.Compression());

          for(unsigned int tick = 0; tick < adcs.size(); ++tick){
            const int adc = adcs[tick] ? int(adcs[tick])-dig.GetPedestal() : 0;

            if(adc != 0){
              // alpha
              bytes(wire.Wire-w0.Wire, tick, 3) = std::min(abs(adc), 255);
              if(adc > 0){
                // red
                bytes(wire.Wire-w0.Wire, tick, 0) = 255;
              }
              else{
                // blue
                bytes(wire.Wire-w0.Wire, tick, 2) = 255;
              }
            }
          } // end for tick
        } // end for wire
      } // end for dig
    } // end for tag
    */
    fEvt = 0;
    fGeom = 0;
  }

  const T* fEvt;
  const gar::geo::GeometryCore* fGeom;

  std::mutex fLock;
  PNGArena fArena;

  std::map<art::InputTag, std::map<geo::PlaneID, PNGView>> fImgs;
};

template<class T> class LazyWires: public ILazy
{
public:
  LazyWires(const T& evt, const gar::geo::GeometryCore* geom)
    : fEvt(&evt), fGeom(geom), fArena("wire")
  {
  }

  virtual void Serialize(JSONFormatter& json) override
  {
    Init();
    json << fImgs;
  }

  virtual PNGArena& GetArena() override
  {
    Init();
    return fArena;
  }

protected:
  void Init()
  {
    std::lock_guard guard(fLock);

    if(!fEvt || !fGeom) return; // already init'd
    /*
    for(art::InputTag tag: fEvt->template getInputTags<std::vector<recob::Wire>>()){
      typename T::template HandleT<std::vector<recob::Wire>> wires; // deduce handle type
      fEvt->getByLabel(tag, wires);

      for(const recob::Wire& rbwire: *wires){
        for(geo::WireID wire: fGeom->ChannelToWire(rbwire.Channel())){
          //        const geo::TPCID tpc(wire);
          const geo::PlaneID plane(wire);

          const geo::WireID w0 = fGeom->GetBeginWireID(plane);
          const unsigned int Nw = fGeom->Nwires(plane);

          if(fImgs[tag].count(plane) == 0){
            fImgs[tag].emplace(plane, PNGView(fArena, Nw, rbwire.NSignal()));
          }

          PNGView& bytes = fImgs[tag].find(plane)->second;

          const auto adcs = rbwire.Signal();
          for(unsigned int tick = 0; tick < adcs.size(); ++tick){
            if(adcs[tick] <= 0) continue;

            // green channel
            bytes(wire.Wire-w0.Wire, tick, 1) = 128; // dark green
            // alpha channel
            bytes(wire.Wire-w0.Wire, tick, 3) = std::max(0, std::min(int(10*adcs[tick]), 255));
          } // end for tick
        } // end for wire
      } // end for rbwire
    } // end for tag
    */
    fEvt = 0;
    fGeom = 0;
  }

protected:
  const T* fEvt;
  const gar::geo::GeometryCore* fGeom;

  std::mutex fLock;
  PNGArena fArena;

  std::map<art::InputTag, std::map<geo::PlaneID, PNGView>> fImgs;
};

// ----------------------------------------------------------------------------
template<class T> Result WebEVDServer<T>::
serve(const T& evt,
      //      const geo::GeometryCore* geom,
      const gar::geo::GeometryCore* geom,
      const detinfo::DetectorPropertiesData& detprop)
{
  // Don't want a sigpipe signal when the browser hangs up on us. This way we
  // will get an error return from the write() call instead.
  signal(SIGPIPE, SIG_IGN);

  if(EnsureListen() != 0) return kERROR;

  LazyDigits<T> digs(evt, geom);
  LazyWires<T> wires(evt, geom);

  std::list<std::thread> threads;

  while(true){
    int sock = accept(fSock, 0, 0);
    if(sock == -1) return err("accept");

    std::string req = read_all(sock);

    std::cout << req << std::endl;

    char* ctx;
    char* verb = strtok_r(&req.front(), " ", &ctx);

    if(verb && std::string(verb) == "GET"){
      char* freq = strtok_r(0, " ", &ctx);
      std::string sreq(freq);

      if(sreq == "/NEXT" ||
         sreq == "/PREV" ||
         sreq == "/NEXT_TRACES" ||
         sreq == "/PREV_TRACES" ||
         sreq == "/QUIT" ||
         sreq.find("/seek/") == 0 ||
         sreq.find("/seek_traces/") == 0){
        for(std::thread& t: threads) t.join();
        return HandleCommand(sreq, sock);
      }
      else{
        threads.emplace_back(_HandleGet<T>, sreq, sock, &evt, &digs, &wires, geom, &detprop);
      }
    }
    else{
      write_unimp501(sock);
      close(sock);
    }
  }

  // unreachable
}

template class WebEVDServer<art::Event>;
// Don't provide an instantiation for gallery::Event. Callers must wrap it in
// the threadsafe wrapper.
template class WebEVDServer<ThreadsafeGalleryEvent>;

} // namespace
