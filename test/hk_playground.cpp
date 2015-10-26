// random assorted code for learning/testing

#include <inttypes.h>
#include <errno.h>
#include <infiniband/verbs.h>

#include <iostream>

int main(int argc, char **argv) {
  (void) argc;
  (void) argv;
  int ret = ::ibv_fork_init();
  if (ret) {
    std::cerr << "Failure: ibv_fork_init: errno=" << ret << std::endl;
    return 1;
  }

  ibv_device **dev_list;
  dev_list = ::ibv_get_device_list(nullptr);

  if (!dev_list) {
    int errsave = errno;
    std::cerr << "Failure: ibv_get_device_list: errno=" << errsave << std::endl;
    return 1;
  }

  const uint32_t kSize = 1U << 22;
  char* array = new char[kSize];
  for (int i = 0; dev_list[i]; i++) {
    ibv_device *device = dev_list[i];
    std::cout
      << ::ibv_get_device_name(device)
      << " GUID:"
      << ::ibv_get_device_guid(device)
      << std::endl;
    ibv_context *context = ::ibv_open_device(device);
    if (!context) {
      std::cerr << "huh? couldn't open device: " << errno << std::endl;
      continue;
    }

    std::cout << "  ok, opened device" << std::endl;
    ibv_pd* pd = ::ibv_alloc_pd(context);
    if (pd) {
      std::cout << "  ok, created pd" << std::endl;

      ibv_mr* mr = ::ibv_reg_mr(pd, array, kSize, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
      if (mr) {
        std::cout << "  ok, reg mr" << std::endl;
        ::ibv_dereg_mr(mr);
      } else {
        std::cerr << "huh? couldn't reg mr: " << errno << std::endl;
      }
    } else {
      std::cerr << "huh? couldn't create pd: " << errno << std::endl;
    }

    if (::ibv_close_device(context)) {
      std::cerr << "huh? couldn't close device: " << errno << std::endl;
    }
  }
  ::ibv_free_device_list(dev_list);
  delete[] array;

  return 0;
}
