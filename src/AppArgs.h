#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "platform/HttpApi.h"

namespace patina {

// Command-line options shared by main.cpp and the GUI.
struct Args {
  std::string file;
  int port = 7860;
  bool headless = false;
  bool mcp = false;
  bool server = true;
  int resolution = 2048;
  bool verbose = false;

  // HTTP API (--http ...)
  bool http = false;
  bool httpGiven = false;  // any --http* option on the command line (overrides the GUI setting)
  int httpPort = 7870;
  bool lan = false;
  std::string bind;
  std::vector<std::string> allow;
  std::string token;
  bool noToken = false;
  std::string workspace;
  bool sandbox = false;
  uint64_t maxUploadMB = 4096;
  std::vector<std::string> corsOrigins;

  // patina run job.json / patina call cmd '{...}'
  std::string mode;  // "", "run", "call"
  std::string jobFile;
  std::string callCmd, callArgs;
  std::map<std::string, std::string> vars;  // --set name=value
  std::string report;                       // --report out.json
  std::string progress;                     // --progress out.jsonl: a line per finished file (batches, 0.30)
  bool quiet = false;

  net::HttpApiOptions httpOptions() const {
    net::HttpApiOptions o;
    o.port = httpPort;
    o.lan = lan;
    o.bind = bind;
    o.allow = allow;
    o.token = token;
    o.requireToken = !noToken;
    o.workspace = workspace;
    o.sandbox = sandbox;
    o.maxUploadMB = maxUploadMB;
    o.corsOrigins = corsOrigins;
    return o;
  }
};

}  // namespace patina
