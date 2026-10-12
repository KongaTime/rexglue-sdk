/**
 * @file        core/startup_ps5.cpp
 * @brief       PS5: the default startup log and state folder (weak, for a
 *              title to replace)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/startup_prospero.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

extern "C" __attribute__((weak)) const char* prospero_state_root() {
  return "/app0";
}

extern "C" __attribute__((weak)) void prospero_startup_log(const char* message) {
  if (!message) {
    return;
  }
  std::fprintf(stderr, "[startup] %s\n", message);
  std::fflush(stderr);
  // Folders 0777 and files 0666, so FTP can read them back
  const std::string logs = std::string(prospero_state_root()) + "/logs";
  mkdir(logs.c_str(), 0777);
  const std::string path = logs + "/startup.log";
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (fd < 0) {
    return;
  }
  std::string line(message);
  line.push_back('\n');
  const char* data = line.data();
  size_t left = line.size();
  while (left) {
    const ssize_t written = write(fd, data, left);
    if (written <= 0) {
      break;
    }
    data += written;
    left -= size_t(written);
  }
  close(fd);
}
