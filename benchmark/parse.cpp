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
const size_t BYTES_PER_BLOCK = 64;

ostream& verbose() {
  return *verbose_stream;
}

void print_usage(ostream& out) {
  out << "Usage: " << exe_name << " [-vt] [-n #] [-s STAGE] [-a ARCH] <jsonfile> ..." << endl;
  out << endl;
  out << "Runs the parser against the given json files in a loop, measuring speed and other statistics." << endl;
  out << endl;
  out << "Options:" << endl;
  out << endl;
  out << "-n #       - Number of iterations per file. Default: 1000" << endl;
  out << "-t         - Tabbed data output" << endl;
  out << "-v         - Verbose output." << endl;
  out << "-s STAGE   - Stop after the given stage." << endl;
  out << "             -s stage1 - Stop after find_structural_bits." << endl;
  out << "             -s all    - Run all stages." << endl;
  out << "-a ARCH    - Use the parser with the designated architecture (HASWELL, WESTMERE" << endl;
  out << "             or ARM64). By default, detects best supported architecture." << endl;
}

void exit_usage(string message) {
  cerr << message << endl;
  cerr << endl;
  print_usage(cerr);
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

  int32_t iterations = 200;

  bool verbose = false;
  bool tabbed_output = false;

  option_struct(int argc, char **argv) {
    #ifndef _MSC_VER
      int c;

      while ((c = getopt(argc, argv, "vtn:a:s:")) != -1) {
        switch (c) {
        case 'n':
          iterations = atoi(optarg);
          break;
        case 't':
          tabbed_output = true;
          break;
        case 'v':
          verbose = true;
          break;
        case 'a':
          architecture = parse_architecture(optarg);
          if (architecture == Architecture::UNSUPPORTED) {
            exit_usage(string("Unsupported option value -a ") + optarg + ": expected -a HASWELL, WESTMERE or ARM64");
          }
          break;
        case 's':
          if (!strcmp(optarg, "stage1")) {
            stage1_only = true;
          } else if (!strcmp(optarg, "all")) {
            stage1_only = false;
          } else {
            exit_usage(string("Unsupported option value -s ") + optarg + ": expected -s stage1 or all");
          }
          break;
        default:
          exit_error("Unexpected argument " + c);
        }
      }
    #else
      int optind = 1;
    #endif

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
      if (options.tabbed_output) {
        exit_error("tabbed_output (-t) flag only works under linux.\n");
      }
    #endif
  }
};

struct json_stats {
  size_t bytes = 0;
  size_t blocks = 0;
  size_t structurals = 0;
  size_t blocks_with_utf8 = 0;
  size_t blocks_with_utf8_flipped = 0;
  size_t blocks_with_0_structurals = 0;
  size_t blocks_with_0_structurals_flipped = 0;
  size_t blocks_with_8_structurals = 0;
  size_t blocks_with_8_structurals_flipped = 0;
  size_t blocks_with_16_structurals = 0;
  size_t blocks_with_16_structurals_flipped = 0;

  json_stats(const padded_string& json, const ParsedJson& pj) {
    bytes = json.size();
    blocks = bytes / BYTES_PER_BLOCK;
    if (bytes % BYTES_PER_BLOCK > 0) { blocks++; } // Account for remainder block
    structurals = pj.n_structural_indexes;

    // Calculate stats on blocks that will trigger utf-8 if statements / mispredictions
    bool last_block_has_utf8 = false;
    for (size_t block=0; block<blocks; block++) {
      // Find utf-8 in the block
      size_t block_start = block*BYTES_PER_BLOCK;
      size_t block_end = block+BYTES_PER_BLOCK;
      if (block_end > json.size()) { block_end = json.size(); }
      bool block_has_utf8 = false;
      for (size_t i=block_start; i<block_end; i++) {
        if (json.data()[i] & 0x80) {
          block_has_utf8 = true;
          break;
        }
      }
      if (block_has_utf8) {
        blocks_with_utf8++;
      }
      if (block > 0 && last_block_has_utf8 != block_has_utf8) {
        blocks_with_utf8_flipped++;
      }
      last_block_has_utf8 = block_has_utf8;
    }

    // Calculate stats on blocks that will trigger structural count if statements / mispredictions
    bool last_block_has_0_structurals = false;
    bool last_block_has_8_structurals = false;
    bool last_block_has_16_structurals = false;
    size_t structural=0;
    for (size_t block=0; block<blocks; block++) {
      // Count structurals in the block
      int block_structurals=0;
      while (structural < pj.n_structural_indexes && pj.structural_indexes[structural] < (block+1)*BYTES_PER_BLOCK) {
        block_structurals++;
        structural++;
      }

      bool block_has_0_structurals = block_structurals == 0;
      if (block_has_0_structurals) {
        blocks_with_0_structurals++;
      }
      if (block > 0 && last_block_has_0_structurals != block_has_0_structurals) {
        blocks_with_0_structurals_flipped++;
      }
      last_block_has_0_structurals = block_has_0_structurals;

      bool block_has_8_structurals = block_structurals >= 8;
      if (block_has_8_structurals) {
        blocks_with_8_structurals++;
      }
      if (block > 0 && last_block_has_8_structurals != block_has_8_structurals) {
        blocks_with_8_structurals_flipped++;
      }
      last_block_has_8_structurals = block_has_8_structurals;

      bool block_has_16_structurals = block_structurals >= 16;
      if (block_has_16_structurals) {
        blocks_with_16_structurals++;
      }
      if (block > 0 && last_block_has_16_structurals != block_has_16_structurals) {
        blocks_with_16_structurals_flipped++;
      }
      last_block_has_16_structurals = block_has_16_structurals;
    }
  }
};

padded_string load_json(char *filename) {
  try {
    verbose() << "[verbose] loading " << filename << endl;
    padded_string json = simdjson::get_corpus(filename);
    verbose() << "[verbose] loaded " << filename << " (" << json.size() << " bytes)" << endl;
    return json;
  } catch (const exception &) { // caught by reference to base
    exit_error(string("Could not load the file ") + filename);
    exit(EXIT_FAILURE); // This is not strictly necessary but removes the warning
  }
}

struct progress_bar {
  int max_value;
  int total_ticks;
  double ticks_per_value;
  int next_tick;
  progress_bar(int _max_value, int _total_ticks) : max_value(_max_value), total_ticks(_total_ticks), ticks_per_value(double(_total_ticks)/_max_value), next_tick(0) {}

  void print(int value) {
    double ticks = value*ticks_per_value;
    if (ticks >= total_ticks) {
      ticks = total_ticks-1;
    }
    // if (ticks >= next_tick) {
      printf("\r[");
      int tick;
      for (tick=0;tick<=ticks; tick++) {
        printf("=");
      }
      if (tick<total_ticks) {
        printf(">");
        tick++;
      }
      for (;tick<total_ticks;tick++) {
        printf(" ");
      }
      printf("]");
      next_tick = tick;
    // }
  }
  void print_finish() {
    print(100);
    printf("\n");
  }
};

struct benchmarker {
  // JSON text from loading the file. Owns the memory.
  const padded_string json;
  // JSON filename
  char *filename;
  // Parser that will parse the JSON file
  const json_parser& parser;
  // Event collector that can be turned on to measure cycles, missed branches, etc.
  event_collector& collector;

  // Statistics about the JSON file independent of its speed (amount of utf-8, structurals, etc.).
  // Loaded on first parse.
  json_stats* stats;
  // Speed and event summary for full parse (not including allocation)
  event_aggregate all_stages;
  // Speed and event summary for stage 1
  event_aggregate stage1;
  // Speed and event summary for stage 2
  event_aggregate stage2;
  // Speed and event summary for allocation
  event_aggregate allocate_stage;

  benchmarker(char *_filename, const json_parser& _parser, event_collector& _collector)
    : json(load_json(_filename)), filename(_filename), parser(_parser), collector(_collector), stats(NULL) {}

  ~benchmarker() {
    if (stats) {
      delete stats;
    }
  }

  int iterations() const {
    return all_stages.iterations;
  }

  void run_iteration() {
    // Allocate ParsedJson
    collector.start();
    ParsedJson pj;
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

    // Calculate stats the first time we parse
    if (stats == NULL) {
      stats = new json_stats(json, pj);
    }
  }

  template<typename T>
  void print_aggregate(const char* prefix, const T& stage) const {
    printf("%s%-13s: %10.1f ns (%5.1f %%) - %8.4f ns per block - %8.4f ns per byte - %8.4f ns per structural - %8.3f GB/s\n",
      prefix,
      "Speed",
      stage.elapsed_ns(), // per file
      100.0 * stage.elapsed_sec() / all_stages.elapsed_sec(), // %
      stage.elapsed_ns() / stats->blocks, // per block
      stage.elapsed_ns() / stats->bytes, // per byte
      stage.elapsed_ns() / stats->structurals, // per structural
      (json.size() / 1000000000.0) / stage.elapsed_sec() // GB/s
    );

    if (collector.has_events()) {
      printf("%s%-13s: %5.2f (%5.2f %%) - %2.3f per block - %2.3f per byte - %2.3f per structural - %2.3f GHz est. frequency\n",
        prefix,
        "Cycles",
        stage.cycles(),
        100.0 * stage.cycles() / all_stages.cycles(),
        stage.cycles() / stats->blocks,
        stage.cycles() / stats->bytes,
        stage.cycles() / stats->structurals,
        (stage.cycles() / stage.elapsed_sec()) / 1000000000.0
      );

      printf("%s%-13s: %10f (%5.2f %%) - %2.2f per block - %2.2f per byte - %2.2f per structural - %2.2f per cycle\n",
        prefix,
        "Instructions",
        stage.instructions(),
        100.0 * stage.instructions() / all_stages.instructions(),
        stage.instructions() / stats->blocks,
        stage.instructions() / stats->bytes,
        stage.instructions() / stats->structurals,
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

  void print(bool tabbed_output) const {
    if (tabbed_output) {
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
      printf("%9lu blocks - %10lu bytes - %5lu structurals (%5.1f %%)\n", stats->bytes / BYTES_PER_BLOCK, stats->bytes, stats->structurals, 100.0 * stats->structurals / stats->bytes);
      if (stats) {
        printf("special blocks with: utf8 %9lu (%5.1f %%) - 0 structurals %9lu (%5.1f %%) - 8+ structurals %9lu (%5.1f %%) - 16+ structurals %9lu (%5.1f %%)\n",
          stats->blocks_with_utf8, 100.0 * stats->blocks_with_utf8 / stats->blocks,
          stats->blocks_with_0_structurals, 100.0 * stats->blocks_with_0_structurals / stats->blocks,
          stats->blocks_with_8_structurals, 100.0 * stats->blocks_with_8_structurals / stats->blocks,
          stats->blocks_with_16_structurals, 100.0 * stats->blocks_with_16_structurals / stats->blocks);
        printf("special block flips: utf8 %9lu (%5.1f %%) - 0 structurals %9lu (%5.1f %%) - 8+ structurals %9lu (%5.1f %%) - 16+ structurals %9lu (%5.1f %%)\n",
          stats->blocks_with_utf8_flipped, 100.0 * stats->blocks_with_utf8_flipped / stats->blocks,
          stats->blocks_with_0_structurals_flipped, 100.0 * stats->blocks_with_0_structurals_flipped / stats->blocks,
          stats->blocks_with_8_structurals_flipped, 100.0 * stats->blocks_with_8_structurals_flipped / stats->blocks,
          stats->blocks_with_16_structurals_flipped, 100.0 * stats->blocks_with_16_structurals_flipped / stats->blocks);
      }
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
  // Read options
  exe_name = argv[0];
  option_struct options(argc, argv);
  if (options.verbose) {
    verbose_stream = &cout;
  }

  // Start collecting events. We put this early so if it prints an error message, it's the
  // first thing printed.
  event_collector collector;

  // Print preamble
  if (!options.tabbed_output) {
    printf("number of iterations %u \n", options.iterations);
  }

  // Set up benchmarkers by reading all files
  json_parser parser(options.architecture);
  vector<benchmarker*> benchmarkers;
  for (size_t i=0; i<options.files.size(); i++) {
    benchmarkers.push_back(new benchmarker(options.files[i], parser, collector));
  }

  // Run the benchmarks
  progress_bar progress(options.iterations, 50);
  for (int iteration = 0; iteration < options.iterations; iteration++) {
    if (!options.verbose) { progress.print(iteration); }
    // Benchmark each file once per iteration
    for (size_t i=0; i<options.files.size(); i++) {
      verbose() << "[verbose] " << benchmarkers[i]->filename << " iteration #" << iteration << endl;
      benchmarkers[i]->run_iteration();
    }
  }
  if (!options.verbose) { progress.print_finish(); }

  for (size_t i=0; i<options.files.size(); i++) {
    benchmarkers[i]->print(options.tabbed_output);
    delete benchmarkers[i];
  }

  return EXIT_SUCCESS;
}
