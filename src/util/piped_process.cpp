/// \file piped_process.cpp
/// Subprocess communication with pipes.
/// \author Diffblue Ltd.

#ifdef _WIN32
#  include "run.h"     // for Windows arg quoting
#  include "unicode.h" // for widen function
#  include <tchar.h>   // library for _tcscpy function
#else
#  include <fcntl.h>  // library for fcntl function
#  include <poll.h>   // library for poll function
#  include <signal.h> // library for kill function
#  include <string.h> // library for strerror function (on linux)
#  include <unistd.h> // library for read/write/sleep/etc. functions
#endif

#include <iostream>
#include <vector>

#include "invariant.h"
#include "optional.h"
#include "piped_process.h"
#include "string_utils.h"

#define BUFSIZE 2048

piped_processt::piped_processt(const std::vector<std::string> commandvec)
{
  // Default state
  process_state = statet::NOT_CREATED;
#ifdef _WIN32
  // Security attributes for pipe creation
  SECURITY_ATTRIBUTES sec_attr;
  // Ensure pipes are inherited
  sec_attr.nLength = sizeof(SECURITY_ATTRIBUTES);
  sec_attr.bInheritHandle = TRUE;
  // This sets the security to the default for the current session access token
  // See following link for details
  // https://docs.microsoft.com/en-us/previous-versions/windows/desktop/legacy/aa379560(v=vs.85) //NOLINT
  sec_attr.lpSecurityDescriptor = NULL;
  // Use named pipes to allow non-blocking read
  // Seed random numbers
  srand((unsigned)time(NULL));
  // Build the base name for the pipes
  std::string base_name = "\\\\.\\pipe\\cbmc\\SMT2\\child\\";
  // Use a random number here, a [G/U]UID would be better, but much more
  // annoying to handle on Windows and do the conversion.
  base_name.append(std::to_string(rand())); // NOLINT
  std::string tmp_name = base_name;
  tmp_name.append("\\IN");
  LPCSTR tmp_str = tmp_name.c_str();
  child_std_IN_Rd = CreateNamedPipe(
    tmp_str,
    PIPE_ACCESS_INBOUND,          // Reading for us
    PIPE_TYPE_BYTE | PIPE_NOWAIT, // Bytes and non-blocking
    PIPE_UNLIMITED_INSTANCES,     // Probably doesn't matter
    BUFSIZE,
    BUFSIZE,    // Output and input bufffer sizes
    0,          // Timeout in ms, 0 = use system default
    &sec_attr); // For inheritance by child
  if(child_std_IN_Rd == INVALID_HANDLE_VALUE)
  {
    throw std::runtime_error("Input pipe creation failed for child_std_IN_Rd");
  }
  // Connect to the other side of the pipe
  child_std_IN_Wr = CreateFileA(
    tmp_str,
    GENERIC_WRITE,                                  // Write side
    FILE_SHARE_READ | FILE_SHARE_WRITE,             // Shared read/write
    &sec_attr,                                      // Need this for inherit
    OPEN_EXISTING,                                  // Opening other end
    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING, // Normal, but don't buffer
    NULL);
  if(child_std_IN_Wr == INVALID_HANDLE_VALUE)
  {
    throw std::runtime_error("Input pipe creation failed for child_std_IN_Wr");
  }
  if(!SetHandleInformation(child_std_IN_Rd, HANDLE_FLAG_INHERIT, 0))
  {
    throw std::runtime_error(
      "Input pipe creation failed on SetHandleInformation");
  }
  tmp_name = base_name;
  tmp_name.append("\\OUT");
  tmp_str = tmp_name.c_str();
  child_std_OUT_Rd = CreateNamedPipe(
    tmp_str,
    PIPE_ACCESS_INBOUND,          // Reading for us
    PIPE_TYPE_BYTE | PIPE_NOWAIT, // Bytes and non-blocking
    PIPE_UNLIMITED_INSTANCES,     // Probably doesn't matter
    BUFSIZE,
    BUFSIZE,    // Output and input bufffer sizes
    0,          // Timeout in ms, 0 = use system default
    &sec_attr); // For inheritance by child
  if(child_std_OUT_Rd == INVALID_HANDLE_VALUE)
  {
    throw std::runtime_error(
      "Output pipe creation failed for child_std_OUT_Rd");
  }
  child_std_OUT_Wr = CreateFileA(
    tmp_str,
    GENERIC_WRITE,                                  // Write side
    FILE_SHARE_READ | FILE_SHARE_WRITE,             // Shared read/write
    &sec_attr,                                      // Need this for inherit
    OPEN_EXISTING,                                  // Opening other end
    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING, // Normal, but don't buffer
    NULL);
  if(child_std_OUT_Wr == INVALID_HANDLE_VALUE)
  {
    throw std::runtime_error(
      "Output pipe creation failed for child_std_OUT_Wr");
  }
  if(!SetHandleInformation(child_std_OUT_Rd, HANDLE_FLAG_INHERIT, 0))
  {
    throw std::runtime_error(
      "Output pipe creation failed on SetHandleInformation");
  }
  // Create the child process
  STARTUPINFOW start_info;
  BOOL success = FALSE;
  ZeroMemory(&proc_info, sizeof(PROCESS_INFORMATION));
  ZeroMemory(&start_info, sizeof(STARTUPINFOW));
  start_info.cb = sizeof(STARTUPINFOW);
  start_info.hStdError = child_std_OUT_Wr;
  start_info.hStdOutput = child_std_OUT_Wr;
  start_info.hStdInput = child_std_IN_Rd;
  start_info.dwFlags |= STARTF_USESTDHANDLES;
  // Unpack the command into a single string for Windows API
  std::wstring cmdline = widen(commandvec[0]);
  for(int i = 1; i < commandvec.size(); i++)
  {
    cmdline.append(L" ");
    cmdline.append(quote_windows_arg(widen(commandvec[i])));
  }
  // Note that we do NOT free this since it becomes part of the child
  // and causes heap corruption in Windows if we free!
  WCHAR *szCmdline =
    reinterpret_cast<WCHAR *>(malloc(sizeof(WCHAR) * cmdline.size()));
  wcscpy(szCmdline, cmdline.c_str());
  success = CreateProcessW(
    NULL,        // application name, we only use the command below
    szCmdline,   // command line
    NULL,        // process security attributes
    NULL,        // primary thread security attributes
    TRUE,        // handles are inherited
    0,           // creation flags
    NULL,        // use parent's environment
    NULL,        // use parent's current directory
    &start_info, // STARTUPINFO pointer
    &proc_info); // receives PROCESS_INFORMATION
  // Close handles to the stdin and stdout pipes no longer needed by the
  // child process. If they are not explicitly closed, there is no way to
  // recognize that the child process has ended (but maybe we don't care).
  CloseHandle(child_std_OUT_Wr);
  CloseHandle(child_std_IN_Rd);
#else
  if(pipe(pipe_input) == -1)
  {
    throw std::runtime_error("Input pipe creation failed");
  }
  if(pipe(pipe_output) == -1)
  {
    throw std::runtime_error("Output pipe creation failed");
  }
  if(fcntl(pipe_output[0], F_SETFL, O_NONBLOCK) < 0)
  {
    throw std::runtime_error("Setting pipe non-blocking failed");
  }
  // Create a new process for the child that will execute the
  // command and receive information via pipes.
  pid = fork();
  if(pid == 0)
  {
    // child process here
    // Close pipes that will be used by the parent so we do
    // not have our own copies and conflicts.
    close(pipe_input[1]);
    close(pipe_output[0]);
    // Duplicate pipes so we have the ones we need.
    dup2(pipe_input[0], STDIN_FILENO);
    dup2(pipe_output[1], STDOUT_FILENO);
    dup2(pipe_output[1], STDERR_FILENO);
    // Create a char** for the arguments (all the contents of commandvec
    // except the first element, i.e. the command itself).
    char **args =
      reinterpret_cast<char **>(malloc((commandvec.size()) * sizeof(char *)));
    // Add all the arguments to the args array of char *.
    unsigned long i = 0;
    while(i < commandvec.size())
    {
      args[i] = strdup(commandvec[i].c_str());
      i++;
    }
    args[i] = NULL;
    execvp(commandvec[0].c_str(), args);
    // The args variable will be handled by the OS if execvp succeeds, but
    // if execvp fails then we should free it here (just in case the runtime
    // error below continues execution.)
    while(i > 0)
    {
      i--;
      free(args[i]);
    }
    free(args);
    // Only reachable if execvp failed
    std::cerr << "Launching " << commandvec[0]
              << " failed with error: " << strerror(errno) << std::endl;
    abort();
  }
  else
  {
    // parent process here
    // Close pipes to be used by the child process
    close(pipe_input[0]);
    close(pipe_output[1]);
    // Get stream for sending to the child process
    command_stream = fdopen(pipe_input[1], "w");
  }
#endif
  process_state = statet::CREATED;
}

piped_processt::~piped_processt()
{
  // Close the parent side of the remaining pipes
  // and kill child process
#ifdef _WIN32
  TerminateProcess(proc_info.hProcess, 0);
  // Disconnecting the pipes also kicks the client off, it should be killed
  // by now, but this will also force the client off.
  // Note that pipes are cleaned up by Windows when all handles to the pipe
  // are closed. Disconnect may be superfluous here.
  DisconnectNamedPipe(child_std_OUT_Rd);
  DisconnectNamedPipe(child_std_IN_Wr);
  CloseHandle(child_std_OUT_Rd);
  CloseHandle(child_std_IN_Wr);
  CloseHandle(proc_info.hProcess);
  CloseHandle(proc_info.hThread);
#else
  fclose(command_stream);
  // Note that the above will call close(pipe_input[1]);
  close(pipe_output[0]);
  kill(pid, SIGTERM);
#endif
}

piped_processt::send_responset piped_processt::send(const std::string &message)
{
  if(process_state != statet::CREATED)
  {
    return send_responset::ERRORED;
  }
#ifdef _WIN32
  if(!WriteFile(child_std_IN_Wr, message.c_str(), message.size(), NULL, NULL))
  {
    // Error handling with GetLastError ?
    return send_responset::FAILED;
  }
#else
  int send_status = fputs(message.c_str(), command_stream);
  fflush(command_stream);
  if(send_status == EOF)
  {
    return send_responset::FAILED;
  }
#endif
  return send_responset::SUCCEEDED;
}

std::string piped_processt::receive()
{
  INVARIANT(
    process_state == statet::CREATED,
    "Can only receive() from a fully initialised process");
  std::string response = std::string("");
  char buff[BUFSIZE];
  bool success = true;
#ifdef _WIN32
  DWORD nbytes;
#else
  int nbytes;
#endif
  while(success)
  {
#ifdef _WIN32
    success = ReadFile(child_std_OUT_Rd, buff, BUFSIZE, &nbytes, NULL);
#else
    nbytes = read(pipe_output[0], buff, BUFSIZE);
    success = nbytes > 0; // 0 is error, -1 is nothing to read
#endif
    if(nbytes > 0)
    {
      response.append(buff, nbytes);
    }
  }
  return response;
}

std::string piped_processt::wait_receive()
{
  // can_receive(PIPED_PROCESS_INFINITE_TIMEOUT) waits an ubounded time until
  // there is some data
  can_receive(PIPED_PROCESS_INFINITE_TIMEOUT);
  return receive();
}

piped_processt::statet piped_processt::get_status()
{
  return process_state;
}

bool piped_processt::can_receive(optionalt<std::size_t> wait_time)
{
  // unwrap the optional argument here
  int timeout;
  if(wait_time.has_value())
  {
    timeout = wait_time.value();
  }
  else
  {
    timeout = -1;
  }
#ifdef _WIN32
  int waited_time = 0;
  // The next four need to be initialised for compiler warnings
  DWORD buffer = 0;
  LPDWORD nbytes = 0;
  LPDWORD rbytes = 0;
  LPDWORD rmbytes = 0;
  while(timeout < 0 || waited_time >= timeout)
  {
    PeekNamedPipe(child_std_OUT_Rd, &buffer, 1, nbytes, rbytes, rmbytes);
    if(buffer != 0)
    {
      return true;
    }
// TODO make this define and choice better
#  define WIN_POLL_WAIT 10
    Sleep(WIN_POLL_WAIT);
    waited_time += WIN_POLL_WAIT;
  }
#else
  int ready;
  struct pollfd fds // NOLINT
  {
    pipe_output[0], POLLIN, 0
  };
  nfds_t nfds = POLLIN;
  ready = poll(&fds, nfds, timeout);
  switch(ready)
  {
  case -1:
    // Error case
    // Further error handling could go here
    process_state = statet::ERRORED;
    // fallthrough intended
  case 0:
    // Timeout case
    // Do nothing for timeout and error fallthrough, default function behaviour
    // is to return false.
    break;
  default:
    // Found some events, check for POLLIN
    if(fds.revents & POLLIN)
    {
      // we can read from the pipe here
      return true;
    }
    // Some revent we did not ask for or check for, can't read though.
  }
#endif
  return false;
}

bool piped_processt::can_receive()
{
  return can_receive(0);
}

void piped_processt::wait_receivable(int wait_time)
{
  while(process_state == statet::CREATED && !can_receive(0))
  {
#ifdef _WIN32
    Sleep(wait_time);
#else
    usleep(wait_time);
#endif
  }
}
