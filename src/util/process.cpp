#include "util/process.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace hoodie::util {

ProcessResult run_process(const std::vector<std::string>& argv) {
  ProcessResult result;
  if (argv.empty()) {
    result.output = "run_process: empty argv";
    return result;
  }

  int pipe_fd[2];
  if (pipe(pipe_fd) != 0) {
    result.output = std::string("pipe failed: ") + std::strerror(errno);
    return result;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    close(pipe_fd[0]);
    close(pipe_fd[1]);
    result.output = std::string("fork failed: ") + std::strerror(errno);
    return result;
  }

  if (pid == 0) {
    // Child: redirect stdout/stderr into the pipe and exec.
    close(pipe_fd[0]);
    dup2(pipe_fd[1], STDOUT_FILENO);
    dup2(pipe_fd[1], STDERR_FILENO);
    close(pipe_fd[1]);

    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (const std::string& arg : argv) raw.push_back(const_cast<char*>(arg.c_str()));
    raw.push_back(nullptr);
    execvp(raw[0], raw.data());
    _exit(127);  // exec failed
  }

  close(pipe_fd[1]);
  char buffer[4096];
  ssize_t count = 0;
  while ((count = read(pipe_fd[0], buffer, sizeof(buffer))) > 0) {
    result.output.append(buffer, static_cast<std::size_t>(count));
  }
  close(pipe_fd[0]);

  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return result;
}

}  // namespace hoodie::util
