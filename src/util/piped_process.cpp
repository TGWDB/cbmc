/// \file piped_process.cpp
/// Subprocess communication with pipes.
/// \author Diffblue Ltd.

#ifdef _WIN32
#  include <tchar.h> // library for _tcscpy function
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
  sec_attr.lpSecurityDescriptor = NULL;

  // START NAMED PIPES STUFF
  // child_std_IN = CreateNamedPipe(
  //   lpszPipename_Child_IN,
  //   PIPE_ACCESS_DUPLEX,       // Read and write mode
  //   PIPE_NOWAIT,              // Do not block
  //   1,                        // One instance of the pipe?
  //   BUFSIZE, BUFSIZE,         // Output and input bufffer sizes
  //   0,                        // Timeout in ms, 0 = use system default
  //   &sec_attr);
  // SetHandleInformation(child_std_IN, HANDLE_FLAG_INHERIT, 0);
  // child_std_OUT = CreateNamedPipe(
  //   lpszPipename_Child_OUT,
  //   PIPE_ACCESS_DUPLEX,       // Read and write mode
  //   PIPE_NOWAIT,              // Do not block
  //   1,                        // One instance of the pipe?
  //   BUFSIZE, BUFSIZE,         // Output and input bufffer sizes
  //   0,                        // Timeout in ms, 0 = use system default
  //   &sec_attr);
  // SetHandleInformation(child_std_OUT, HANDLE_FLAG_INHERIT, 0);

  // // TODO, error handling for the above pipes
  // STARTUPINFO start_info;
  // BOOL success = FALSE;
  // ZeroMemory(&proc_info, sizeof(PROCESS_INFORMATION));
  // ZeroMemory( &start_info, sizeof(STARTUPINFO) );
  // start_info.cb = sizeof(STARTUPINFO);
  // start_info.hStdError = child_std_OUT;
  // start_info.hStdOutput = child_std_OUT;
  // start_info.hStdInput = child_std_IN;
  // start_info.dwFlags |= STARTF_USESTDHANDLES;

  // // TODO: properly do the construciton of the command line here!
  // TCHAR szCmdline[] = TEXT("echo hi");
  // success = CreateProcess(NULL,
  //     szCmdline,     // command line
  //     NULL,          // process security attributes
  //     NULL,          // primary thread security attributes
  //     TRUE,          // handles are inherited
  //     0,             // creation flags
  //     NULL,          // use parent's environment
  //     NULL,          // use parent's current directory
  //     &start_info,  // STARTUPINFO pointer
  //     &proc_info);  // receives PROCESS_INFORMATION

  // return;
  // END NAMED PIPES STUFF

  // Create a pipe for the child process's STDOUT and ensure the read handle
  // to the pipe for STDOUT is not inherited.
  if(
    !CreatePipe(&child_std_OUT_Rd, &child_std_OUT_Wr, &sec_attr, 0) ||
    !SetHandleInformation(child_std_OUT_Rd, HANDLE_FLAG_INHERIT, 0))
  {
    throw std::runtime_error("Input pipe creation failed");
  }
  // Create a pipe for the child process's STDIN and ensure the write handle
  // to the pipe for STDIN is not inherited.
  if(
    !CreatePipe(&child_std_IN_Rd, &child_std_IN_Wr, &sec_attr, 0) ||
    !SetHandleInformation(child_std_IN_Wr, HANDLE_FLAG_INHERIT, 0))
  {
    throw std::runtime_error("Output pipe creation failed");
  }
  // Create the child process
  STARTUPINFO start_info;
  BOOL success = FALSE;
  ZeroMemory(&proc_info, sizeof(PROCESS_INFORMATION));
  ZeroMemory(&start_info, sizeof(STARTUPINFO));
  start_info.cb = sizeof(STARTUPINFO);
  start_info.hStdError = child_std_OUT_Wr;
  start_info.hStdOutput = child_std_OUT_Wr;
  start_info.hStdInput = child_std_IN_Rd;
  start_info.dwFlags |= STARTF_USESTDHANDLES;
  // Unpack the command into a single string for Windows API
  std::string cmdline = "";
  for(int i = 0; i < commandvec.size(); i++)
  {
    cmdline.append(commandvec[i].c_str());
    cmdline.append(" ");
  }
  // Note that we do NOT free this since it becomes part of the child
  // and causes heap corruption in Windows if we free!
  TCHAR *szCmdline = (TCHAR *)malloc(sizeof(TCHAR) * cmdline.size());
  _tcscpy(szCmdline, cmdline.c_str());
  success = CreateProcess(
    NULL,         // application name, we only use the command below
    szCmdline,    // command line
    NULL,         // process security attributes
    NULL,         // primary thread security attributes
    TRUE,         // handles are inherited
    0,            // creation flags
    NULL,         // use parent's environment
    NULL,         // use parent's current directory
    &start_info, // STARTUPINFO pointer
    &proc_info); // receives PROCESS_INFORMATION
  // Close handles to the stdin and stdout pipes no longer needed by the
  // child process. If they are not explicitly closed, there is no way to
  // recognize that the child process has ended.
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
#ifdef _WIN32
  // CloseHandle(child_std_OUT);
  // CloseHandle(child_std_IN);
  CloseHandle(child_std_OUT_Rd);
  CloseHandle(child_std_IN_Wr);
  CloseHandle(proc_info.hProcess);
  CloseHandle(proc_info.hThread);
  //TODO: kill child!
#else
  // Close the parent side of the remaning pipes
  fclose(command_stream);
  // Note that the above will call close(pipe_input[1]);
  close(pipe_output[0]);
  // Send signal to the child process to terminate
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
  if(!WriteFile(
    child_std_IN_Wr, message.c_str(), message.size(), NULL, NULL))
  {
    // Error handling with GetLastError ?
    return send_responset::FAILED;
  }
#else
  // send message to solver process
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
    success = nbytes > 0;   // 0 is error, -1 is nothing to read
#endif
    if(nbytes > 0)
    {
      response.append(buff, nbytes);
    }
  }
  return response;
// #ifdef _WIN32
//   // A lot of this could be unified with the Linux/MacOS version, TODO
//   BOOL success = TRUE;
//   DWORD nbytes;
//   while(success)
//   {
//     success = ReadFile(child_std_OUT_Rd, buff, BUFSIZE, &nbytes, NULL);
//     if(!success || nbytes == 0)
//     // This has some old error handling stuff, but it's not properly done
//     // left for information.
//     // {
//     //   nbytes = GetLastError(); // error code only
//     //   std::ostringstream stream;
//     //   stream << nbytes;
//     //   response.append(" error code: ");
//     //   response.append(stream.str());
//     //   return response;
//     // }
//     // else if(nbytes == 0)
//     {
//       return response;
//     }
//     response.append(buff, nbytes);
//   }
// #else
//   int nbytes;
//   while(true)
//   {
//     nbytes = read(pipe_output[0], buff, BUFSIZE);
//     switch(nbytes)
//     {
//     case -1:
//       // Nothing more to read in the pipe
//       return response;
//     case 0:
//       // Pipe is closed.
//       process_state = statet::STOPPED;
//       return response;
//     default:
//       // Read some bytes, append them to the response and continue
//       response.append(buff, nbytes);
//     }
//   }
// #endif
//   UNREACHABLE;
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
  while(timeout < 0 || waited_time >= timeout)
  {
    if(PeekNamedPipe(child_std_OUT_Rd, NULL, 0, NULL, NULL, NULL))
    {
      return true;
    }
// TODO, make this define and choice better
#  define WIN_POLL_WAIT 10
    Sleep(WIN_POLL_WAIT);
    waited_time += WIN_POLL_WAIT;
  }
  return false;
  // UNIMPLEMENTED_FEATURE(
  //   "Pipe IPC on windows: can_receive(optionalt<std::size_t> wait_time)");
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
  return false;
#endif
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
