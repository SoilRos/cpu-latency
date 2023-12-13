#include <hwloc.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <iostream>
#include <memory>
#include <algorithm>
#include <numeric>
#include <optional>
#include <thread>
#include <mutex>
#include <vector>
#include <condition_variable>

std::size_t round_trips = 1000;
std::size_t repeat = 300;
bool symmetric = true;

class Barrier
{
  std::atomic_char acounter, bcounter;
  std::vector<int> dir;

public:
  Barrier(int P_)
    : dir(P_, 0)
  {
    acounter.store(0);
    bcounter.store(0);
  }

  void wait(int i)
  {
    if (dir[i] == 0) {
      acounter++;
      while (acounter.load() < dir.size())
        ;
      bcounter++;
      while (bcounter.load() < dir.size())
        ;
    } else {
      acounter--;
      while (acounter.load() > 0)
        ;
      bcounter--;
      while (bcounter.load() > 0)
        ;
    }
    dir[i] = 1 - dir[i]; // reverse direction in next round
  }
};

int
main(int argc, char** argv)
{
  std::vector<std::string> cmd_args(argv, argv + argc);
  const std::string prog_path{ cmd_args[0] };
  cmd_args.erase(cmd_args.begin());
  if (auto opt = std::find_if(std::begin(cmd_args), std::end(cmd_args),
                                      [](const auto& opt) {
                                        return opt == "--help" or
                                               opt == "-h";
                                      });
      opt != cmd_args.end()) {
    std::cout << "USAGE: " << prog_path << " [options]\n\n"
              << "Measures average the time (ns) that it takes to send/receive a Compare-And-Swap (CAS) message among all the cores\n"
              << "The results are streamed to 'stdout' as a comma-separated values (CSV) format.\n\n"
              << "OPTIONS:\n\n"
              << "-rt, --round-trips   <int>   Number of times to send and receive messages from core A to core B              (Default: "<< round_trips <<")\n"
              << "-r,  --repeat        <int>   Number of times to repeat the experiment per core                               (Default: "<< repeat <<")\n"
              << "-s,  --symmetric     <bool>  Whether to measure ping-pong latency from core A to core B but not the oposite  (Default: "<< (symmetric ? "true" : "false") << ")\n"
              << "-h,  --help                  Display available options\n"
              << std::endl;
    return 0;
  }

  auto to_int = [](const auto& s) -> std::optional<int> {
    int value{};
    if (std::from_chars(s.data(), s.data() + s.size(), value).ec == std::errc{})
      return value;
    else
      return std::nullopt;
  };

  if (auto opt = std::find_if(
        std::begin(cmd_args),
        std::end(cmd_args),
        [](const auto& opt) { return opt == "--repeat" or opt == "-r"; });
      opt != cmd_args.end()) {
    if (auto opt_val = std::next(opt); opt_val != cmd_args.end()) {
      repeat = to_int(*opt_val).value();
      cmd_args.erase(opt, std::next(opt_val));
    } else {
      throw;
    }
  }

  if (auto opt = std::find_if(
        std::begin(cmd_args),
        std::end(cmd_args),
        [](const auto& opt) { return opt == "--round-trips" or opt == "-rt"; });
      opt != cmd_args.end()) {
    if (auto opt_val = std::next(opt); opt_val != cmd_args.end()) {
      round_trips = to_int(*opt_val).value();
      cmd_args.erase(opt, std::next(opt_val));
    } else {
      throw;
    }
  }

  if (auto opt = std::find_if(
        std::begin(cmd_args),
        std::end(cmd_args),
        [](const auto& opt) { return opt == "--symmetric" or opt == "-s"; });
      opt != cmd_args.end()) {
    if (auto opt_val = std::next(opt); opt_val != cmd_args.end()) {
      if (*opt_val == "true")
        symmetric = true;
      else if (*opt_val == "false")
        symmetric = false;
      else
        throw;
      cmd_args.erase(opt, std::next(opt_val));
    } else {
      throw;
    }
  }

  if (not cmd_args.empty()) {
    std::cerr << "unkown parameters:" << std::endl;
    for (auto& opt : cmd_args)
      std::cerr << " * " << opt << std::endl;
    throw;
  }

  // initialize
  hwloc_topology_t topology;
  hwloc_topology_init(&topology);
  hwloc_topology_load(topology);
  auto depth = hwloc_get_type_or_below_depth(topology, HWLOC_OBJ_PU);
  auto cores = hwloc_get_nbobjs_by_depth(topology, depth);

  // main object to send messages
  std::atomic_size_t* data_ptr;

  auto skip_core = [](auto i, auto j) {
    return (j == i) or (symmetric and j >= i);
  };

  Barrier sync{ 2 };

  std::thread ping_t{ [&]() {
    std::vector<std::chrono::nanoseconds> durations;
    for (std::size_t i = 0; i != cores; ++i) {
      for (std::size_t j = 0; j != cores; ++j) {
        sync.wait(0);
        if (skip_core(i, j)) {
          if (j + 1 != cores)
            std::cout << ",";
          continue;
        }
        durations.assign(repeat, std::chrono::nanoseconds{ 0 });
        auto obj = hwloc_get_obj_by_depth(topology, depth, i);
        auto cpuset = hwloc_bitmap_dup(obj->cpuset);
        // bind thread to cpu i
        if (hwloc_set_cpubind(topology, cpuset, HWLOC_CPUBIND_THREAD))
          throw;
        // allocate memory bound to cpu i
        auto storage = hwloc_alloc_membind_policy(topology,
                                                  sizeof(std::atomic_size_t),
                                                  cpuset,
                                                  HWLOC_MEMBIND_DEFAULT,
                                                  HWLOC_MEMBIND_THREAD);
        if (not storage)
          throw;
        hwloc_bitmap_free(cpuset);
        data_ptr = ::new (storage) std::atomic_size_t();
        *data_ptr = std::numeric_limits<std::size_t>::max();
        for (std::size_t s = 0; s != repeat; ++s) {
          sync.wait(0);
          std::size_t ping{ 0 };
          auto start = std::chrono::high_resolution_clock::now();
          data_ptr->store(0);
          for (std::size_t k = 0; k != (2 * round_trips) + 2; k += 2) {
            ping = k;
            while (not data_ptr->compare_exchange_strong(
              ping,
              ping + 1,
              std::memory_order_relaxed,
              std::memory_order_relaxed)) {
              ping = k;
            }
          }
          durations[s] = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now() - start);
          if (data_ptr->load() != 2 * round_trips + 1)
            throw;
        }
        auto sum = std::accumulate(std::begin(durations),
                                   std::end(durations),
                                   std::chrono::nanoseconds{ 0 });
        std::cout << sum.count() / (2. * round_trips * repeat);
        if (j + 1 != cores)
          std::cout << ",";
        std::destroy_at(data_ptr);
        hwloc_free(topology, storage, sizeof(std::atomic_size_t));
      }
      std::cout << std::endl;
    }
  } };

  std::thread pong_t{ [&]() {
    for (std::size_t i = 0; i != cores; ++i) {
      for (std::size_t j = 0; j != cores; ++j) {
        sync.wait(1);
        if (skip_core(i, j))
          continue;
        auto obj = hwloc_get_obj_by_depth(topology, depth, j);
        auto cpuset = hwloc_bitmap_dup(obj->cpuset);
        if (hwloc_set_cpubind(topology, cpuset, HWLOC_CPUBIND_THREAD))
          throw;
        hwloc_bitmap_free(cpuset);
        for (std::size_t s = 0; s != repeat; ++s) {
          sync.wait(1);
          std::size_t pong{ 1 };
          for (std::size_t k = 0; k != (2 * round_trips); k += 2) {
            pong = k + 1;
            while (not data_ptr->compare_exchange_strong(
              pong,
              pong + 1,
              std::memory_order_relaxed,
              std::memory_order_relaxed)) {
              pong = k + 1;
            }
          }
        }
      }
    }
  } };

  ping_t.join();
  pong_t.join();

  hwloc_topology_destroy(topology);
  return 0;
}
