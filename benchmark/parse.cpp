#include "json_parser.h"
#include "event_counter.h"

#include <cassert>
#include <cctype>
#ifndef _MSC_VER
#include <dirent.h>
#include <unistd.h>
#endif
#include <cinttypes>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "linux-perf-events.h"
#ifdef __linux__
#include <libgen.h>
#endif
//#define DEBUG
#include "simdjson/common_defs.h"
#include "simdjson/isadetection.h"
#include "simdjson/jsonioutil.h"
#include "simdjson/jsonparser.h"
#include "simdjson/parsedjson.h"
#include "simdjson/stage1_find_marks.h"
#include "simdjson/stage2_build_tape.h"

#include <functional>

using namespace simdjson;
using std::cerr;
using std::cout;
using std::endl;
using std::string;
using std::to_string;
using std::vector;
using std::ostream;
using std::ofstream;
using std::exception;

// Stash the exe_name in main() for functions to use
char* exe_name;

// Initialize "verbose" to go nowhere. We'll read options in main() and set to cout if verbose is true.
std::ofstream dev_null;
ostream *verbose_stream = &dev_null;
const double BYTES_PER_LOOP = 64.0;

ostream& verbose() {
  return *verbose_stream;
}

void print_usage() {
  cerr << "Usage: " << exe_name << " <jsonfile>" << endl;
}
void exit_usage(string message) {
  cerr << message << endl;
  cerr << endl;
  print_usage();
  exit(EXIT_FAILURE);
}
void exit_error(string message) {
  cerr << message << endl;
  exit(EXIT_FAILURE);
  abort();
}

struct option_struct {
  vector<char*> files;
  Architecture architecture = Architecture::UNSUPPORTED;
  bool stage1_only = false;

  int32_t warmup_iterations = -1;
  int32_t iterations = -1;

  bool verbose = false;
  bool dump = false;
  bool json_output = false;
  bool just_data = false;

  option_struct(int argc, char **argv) {
    #ifndef _MSC_VER
      int c;

      while ((c = getopt(argc, argv, "1vdtn:w:as")) != -1) {
        switch (c) {
        case 'n':
          iterations = atoi(optarg);
          break;
        case 'w':
          warmup_iterations = atoi(optarg);
          break;
        case 'a':
          architecture = parse_architecture(optarg);
          if (architecture == Architecture::UNSUPPORTED) {
            exit_usage(string("Unsupported architecture string ") + optarg + ": expected -s=HASWELL, WESTMERE or ARM64");
          }
          break;
        case 's':
          if (!strcmp(optarg, "stage1")) {
            stage1_only = true;
          } else {
            exit_usage(string("Unsupported stage '") + optarg + "': expected -s=stage1");
          }
          break;
        case 't':
          just_data = true;
          break;
        case 'v':
          verbose = true;
          break;
        case 'd':
          dump = true;
          break;
        case 'j':
          json_output = true;
          break;
        default:
          exit_error("Unexpected argument " + c);
        }
      }
    #else
      int optind = 1;
    #endif

    // Handle defaults
    if (warmup_iterations == -1) {
      #if defined(DEBUG)
        warmup_iterations = 0;
      #else
        warmup_iterations = 1;
      #endif
    }
    if (iterations == -1) {
      #if defined(DEBUG)
        iterations = 1;
      #else
        iterations = 10;
      #endif
    }

    // If architecture is not specified, pick the best supported architecture by default
    if (architecture == Architecture::UNSUPPORTED) {
      architecture = find_best_supported_architecture();
    }

    // All remaining arguments are considered to be files
    for (int i=optind; i<argc; i++) {
      files.push_back(argv[i]);
    }
    if (files.empty()) {
      exit_usage("No files specified");
    }

    #if !defined(__linux__)
      if (options.just_data) {
        exit_error("just_data (-t) flag only works under linux.\n");
      }
    #endif
  }
};

padded_string load_file(char *filename) {
  try {
    verbose() << "[verbose] loading " << filename << endl;
    padded_string p = simdjson::get_corpus(filename);
    verbose() << "[verbose] loaded " << filename << " (" << p.size() << " bytes)" << endl;
    return p;
  } catch (const exception &) { // caught by reference to base
    exit_error(string("Could not load the file ") + filename);
    exit(EXIT_FAILURE); // This is not strictly necessary but removes the warning
  }
}

struct benchmarker {
  // input
  json_parser& parser;
  char *filename;
  event_collector& collector;

  // data
  padded_string json;
  ParsedJson pj;

  // stats
  event_aggregate all_stages;
  event_aggregate allocate_stage;
  event_aggregate stage1;
  event_aggregate stage2;

  benchmarker(json_parser& _parser, char *_filename, event_collector& _collector)
    : parser(_parser), filename(_filename), collector(_collector), 
      json(load_file(_filename)) {}

  int iterations() {
    return all_stages.iterations;
  }

  void warmup_iteration() {
    // Allocate ParsedJson
    bool allocok = pj.allocate_capacity(json.size());

    if (!allocok) {
      exit_error(string("Unable to allocate_stage ") + to_string(json.size()) + " bytes for the JSON result.");
    }
    verbose() << "[verbose] allocated memory for parsed JSON " << endl;

    // Stage 1 (find structurals)
    int result = parser.stage1((const uint8_t *)json.data(), json.size(), pj);

    if (result != simdjson::SUCCESS) {
      exit_error(string("Failed to parse ") + filename + " during stage 1: " + pj.get_error_message());
    }

    // Stage 2 (unified machine)
    result = parser.stage2((const uint8_t *)json.data(), json.size(), pj);

    if (result != simdjson::SUCCESS) {
      exit_error(string("Failed to parse ") + filename + " during stage 2: " + pj.get_error_message());
    }
  }

  void run_iteration() {
    // Allocate ParsedJson
    collector.start();
    bool allocok = pj.allocate_capacity(json.size());
    event_count allocate_count = collector.end();
    allocate_stage << allocate_count;

    if (!allocok) {
      exit_error(string("Unable to allocate_stage ") + to_string(json.size()) + " bytes for the JSON result.");
    }
    verbose() << "[verbose] allocated memory for parsed JSON " << endl;

    // Stage 1 (find structurals)
    collector.start();
    int result = parser.stage1((const uint8_t *)json.data(), json.size(), pj);
    event_count stage1_count = collector.end();
    stage1 << stage1_count;

    if (result != simdjson::SUCCESS) {
      exit_error(string("Failed to parse ") + filename + " during stage 1: " + pj.get_error_message());
    }

    // Stage 2 (unified machine)
    collector.start();
    result = parser.stage2((const uint8_t *)json.data(), json.size(), pj);
    event_count stage2_count = collector.end();
    stage2 << stage2_count;

    all_stages << (stage1_count + stage2_count);

    if (result != simdjson::SUCCESS) {
      exit_error(string("Failed to parse ") + filename + " during stage 2: " + pj.get_error_message());
    }
  }

  template<typename T>
  void print_aggregate(const char* prefix, const T& stage) const {
    printf("%s%-13s: %10.1f ns (%5.1f %%) - %8.4f ns per loop - %8.4f ns per byte - %8.4f ns per structural - %8.3f GB/s\n",
      prefix,
      "Speed",
      stage.elapsed_ns(), // per file
      100.0 * stage.elapsed_sec() / all_stages.elapsed_sec(), // %
      stage.elapsed_ns() / (json.size() / BYTES_PER_LOOP), // per loop
      stage.elapsed_ns() / json.size(), // per byte
      stage.elapsed_ns() / pj.n_structural_indexes, // per structural
      (json.size() / 1000000000.0) / stage.elapsed_sec() // GB/s
    );

    if (collector.has_events()) {
      printf("%s%-13s: %5.2f (%5.2f %%) - %2.3f per loop - %2.3f per byte - %2.3f per structural - %2.3f GHz est. frequency\n",
        prefix,
        "Cycles",
        stage.cycles(),
        100.0 * stage.cycles() / all_stages.cycles(),
        stage.cycles() / (json.size() / BYTES_PER_LOOP),
        stage.cycles() / json.size(),
        stage.cycles() / pj.n_structural_indexes,
        (stage.cycles() / stage.elapsed_sec()) / 1000000000.0
      );

      printf("%s%-13s: %10f (%5.2f %%) - %2.2f per loop - %2.2f per byte - %2.2f per structural - %2.2f per cycle\n",
        prefix,
        "Instructions",
        stage.instructions(),
        100.0 * stage.instructions() / all_stages.instructions(),
        stage.instructions() / (json.size() / BYTES_PER_LOOP),
        stage.instructions() / json.size(),
        stage.instructions() / pj.n_structural_indexes,
        stage.instructions() / stage.cycles()
      );

      // NOTE: removed cycles/miss because it is a somewhat misleading stat
      printf("%s%-13s: %2.2f branch misses (%5.2f %%) - %2.2f cache misses (%5.2f %%) - %2.2f cache references\n",
        prefix,
        "Misses",
        stage.branch_misses(),
        100.0 * stage.branch_misses() / all_stages.branch_misses(),
        stage.cache_misses(),
        100.0 * stage.cache_misses() / all_stages.cache_misses(),
        stage.cache_references()
      );
    }
  }

  void print(bool just_data) const {
    if (just_data) {
      double speedinGBs = (json.size() / 1000000000.0) / all_stages.best.elapsed_sec();
      if (collector.has_events()) {
        printf("\"%s\"\t%f\t%f\t%f\t%f\t%f\n",
                ::basename(filename),
                allocate_stage.best.cycles() / json.size(),
                stage1.best.cycles() / json.size(),
                stage2.best.cycles() / json.size(),
                all_stages.best.cycles() / json.size(),
                speedinGBs);
      } else {
        printf("\"%s\"\t\t\t\t\t%f\n",
                ::basename(filename),
                speedinGBs);
      }
    } else {
      printf("\n");
      printf("%s\n", filename);
      printf("%s\n", string(strlen(filename), '=').c_str());
      printf("%11.1f loops - %10lu bytes - %5d structurals (%5.1f %%)\n", json.size() / BYTES_PER_LOOP, json.size(), pj.n_structural_indexes, (double)pj.n_structural_indexes / json.size());
      printf("\n");
      printf("All Stages\n");
      print_aggregate("|    "   , all_stages.best);
      //          printf("|- Allocation\n");
      // print_aggregate("|    ", allocate_stage.best);
              printf("|- Stage 1\n");
      print_aggregate("|    ", stage1.best);
              printf("|- Stage 2\n");
      print_aggregate("|    ", stage2.best);
    }
  }
};

int main(int argc, char *argv[]) {
  exe_name = argv[0];
  option_struct options(argc, argv);
  if (options.verbose) {
    verbose_stream = &cout;
  }

  event_collector collector;

  if (!options.just_data) {
    printf("number of warmups %u \n", options.warmup_iterations);
    printf("number of iterations %u \n", options.iterations);
  }

  json_parser parser(options.architecture);
  for (size_t i=0; i<options.files.size(); i++) {
    benchmarker benchmarker(parser, options.files[i], collector);

    for (int iteration = 0; iteration < options.warmup_iterations; iteration++) {
      verbose() << "[verbose] warmup iteration #" << iteration << endl;
      benchmarker.warmup_iteration();
    }

    for (int iteration = 0; iteration < options.iterations; iteration++) {
      verbose() << "[verbose] event iteration #" << iteration << endl;
      benchmarker.run_iteration();
    }

    benchmarker.print(options.just_data);
  }

  return EXIT_SUCCESS;
}
