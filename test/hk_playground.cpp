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

  for (int i = 0; dev_list[i]; i++) {
    ibv_device *device = dev_list[i];
    std::cout
      << ::ibv_get_device_name(device)
      << " GUID:"
      << ::ibv_get_device_guid(device)
      << std::endl;
  }

  ::ibv_free_device_list(dev_list);

  return 0;
}
