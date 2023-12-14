#include <hwloc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <numeric>
#include <optional>
#include <random>
#include <thread>
#include <type_traits>
#include <vector>

std::size_t round_trips = 1000;
std::size_t repeat = 15;
bool symmetric = true;
bool randomize = true;

#ifdef __cpp_lib_hardware_interference_size
    using std::hardware_constructive_interference_size;
#else
    constexpr std::size_t hardware_constructive_interference_size = 64;
#endif

static_assert(hardware_constructive_interference_size >= alignof(std::atomic_size_t));

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
  cmd_args.erase(cmd_args.begin());
  if (auto opt = std::find_if(
        std::begin(cmd_args),
        std::end(cmd_args),
        [](const auto& opt) { return opt == "--help" or opt == "-h"; });
      opt != cmd_args.end()) {
    std::cout << "USAGE: cpu_latency [options]\n\n"
              << "Measures average the time (ns) that it takes to send/receive a Compare-And-Swap (CAS) message among all the cores\n"
              << "The results are streamed to 'stdout' as a comma-separated values (CSV) format.\n\n"
              << "OPTIONS:\n\n"
              << "-rt, --round-trips   <int>   Number of times to send and receive messages from core A to core B              (Default: "<< round_trips <<")\n"
              << "-r,  --repeat        <int>   Number of times to repeat the experiment per core                               (Default: "<< repeat <<")\n"
              << "-s,  --symmetric     <bool>  Whether to measure ping-pong latency from core A to core B but not the oposite  (Default: "<< (symmetric ? "true" : "false") << ")\n"
              << "     --randomize     <bool>  Whether to randomize the order of cores to measure                              (Default: "<< (randomize ? "true" : "false") << ")\n"
              << "-h,  --help                  Display available options\n"
              << std::endl;
    return 0;
  }

  auto to_int = [](const auto& s) -> std::optional<int> {
    int value{};
    if (std::from_chars(s.data(), s.data() + s.size(), value).ec == std::errc{})
      return value;
    return std::nullopt;
  };

  auto to_bool = [](const auto& s) -> std::optional<bool> {
    if (s == "true")
      return true;
    else if (s == "false")
      return false;
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
      symmetric = to_bool(*opt_val).value();;
      cmd_args.erase(opt, std::next(opt_val));
    } else {
      throw;
    }
  }

  if (auto opt = std::find_if(
        std::begin(cmd_args),
        std::end(cmd_args),
        [](const auto& opt) { return opt == "--randomize" or opt == "-r"; });
      opt != cmd_args.end()) {
    if (auto opt_val = std::next(opt); opt_val != cmd_args.end()) {
      randomize = to_bool(*opt_val).value();
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

  std::map<std::array<std::size_t, 2>, void*> storage;
  std::vector<std::array<std::size_t, 2>> cpus;
  for (std::size_t i = 0; i != cores; ++i)
    for (std::size_t j = 0; j != cores; ++j)
      if (not skip_core(i, j)) {
        cpus.push_back({ i, j });
        auto obj = hwloc_get_obj_by_depth(topology, depth, i);
        // allocate memory bound to cpu i
        auto ptr = hwloc_alloc_membind_policy(
          topology,
          hardware_constructive_interference_size * 2,
          obj->cpuset,
          HWLOC_MEMBIND_BIND,
          0);
        if (not ptr)
          throw;
        storage[{ i, j }] = ptr;
      }

  std::random_device rd;
  std::mt19937 g(rd());

  std::map<std::array<std::size_t, 2>, std::chrono::nanoseconds> durations;
  std::thread ping_t{ [&]() {
    for (std::size_t s = 0; s != repeat; ++s) {
      sync.wait(0);
      if (randomize)
        std::shuffle(std::begin(cpus), std::end(cpus), g);
      sync.wait(0);
      for (auto [i, j] : cpus) {
        sync.wait(0);
        auto obj = hwloc_get_obj_by_depth(topology, depth, i);
        if (hwloc_set_cpubind(topology, obj->cpuset, HWLOC_CPUBIND_THREAD))
          throw;
        auto ptr = storage[{ i, j }];
        std::size_t sz = hardware_constructive_interference_size * 2;
        ptr = std::align(hardware_constructive_interference_size,
                         sizeof(std::atomic_size_t),
                         ptr,
                         sz);
        if (not ptr)
          throw;
        data_ptr = ::new (ptr) std::atomic_size_t();
        *data_ptr = std::numeric_limits<std::size_t>::max();
        sync.wait(0);
        std::size_t ping{ 0 };
        auto start = std::chrono::high_resolution_clock::now();
        data_ptr->store(0);
        for (std::size_t k = 0; k != (2 * round_trips) + 2; k += 2) {
          ping = k;
          while (
            not data_ptr->compare_exchange_strong(ping,
                                                  ping + 1,
                                                  std::memory_order_relaxed,
                                                  std::memory_order_relaxed)) {
            ping = k;
          }
        }
        if (data_ptr->load() != 2 * round_trips + 1)
          throw;
        durations[{ i, j }] +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now() - start);
        std::destroy_at(data_ptr);
      }
    }
  } };

  std::thread pong_t{ [&]() {
    for (std::size_t s = 0; s != repeat; ++s) {
      sync.wait(1);
      sync.wait(1);
      for (auto [i, j] : cpus) {
        sync.wait(1);
        auto obj = hwloc_get_obj_by_depth(topology, depth, j);
        if (hwloc_set_cpubind(topology, obj->cpuset, HWLOC_CPUBIND_THREAD))
          throw;
        sync.wait(1);
        std::size_t pong{ 1 };
        for (std::size_t k = 0; k != (2 * round_trips); k += 2) {
          pong = k + 1;
          while (
            not data_ptr->compare_exchange_strong(pong,
                                                  pong + 1,
                                                  std::memory_order_relaxed,
                                                  std::memory_order_relaxed)) {
            pong = k + 1;
          }
        }
      }
    }
  } };

  ping_t.join();
  pong_t.join();

  for (auto [i, j] : cpus)
    hwloc_free(
      topology, storage[{ i, j }], hardware_constructive_interference_size * 2);

  hwloc_topology_destroy(topology);

  for (std::size_t i = 0; i != cores; ++i) {
    for (std::size_t j = 0; j != cores; ++j) {
      if (auto it = durations.find({ i, j }); it != durations.end())
        std::cout << (it->second.count() / (2. * round_trips * repeat));
      std::cout << (j + 1 == cores ? "\n" : ",");
    }
  }

  return 0;
}
