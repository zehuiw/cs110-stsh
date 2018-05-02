/**
 * File: stsh.cc
 * -------------
 * Defines the entry point of the stsh executable.
 */

#include "stsh-parser/stsh-parse.h"
#include "stsh-parser/stsh-readline.h"
#include "stsh-parser/stsh-parse-exception.h"
#include "stsh-signal.h"
#include "stsh-job-list.h"
#include "stsh-job.h"
#include "stsh-process.h"
#include <cstring>
#include <iostream>
#include <string>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>  // for fork
#include <signal.h>  // for kill
#include <sys/wait.h>
using namespace std;

static STSHJobList joblist; // the one piece of global data we need so signal handlers can access it

/**
 * Function: handleBuiltin
 * -----------------------
 * Examines the leading command of the provided pipeline to see if
 * it's a shell builtin, and if so, handles and executes it.  handleBuiltin
 * returns true if the command is a builtin, and false otherwise.
 */
static const string kSupportedBuiltins[] = {"quit", "exit", "fg", "bg", "slay", "halt", "cont", "jobs"};
static const size_t kNumSupportedBuiltins = sizeof(kSupportedBuiltins)/sizeof(kSupportedBuiltins[0]);
static bool handleBuiltin(const pipeline& pipeline) {
  const string& command = pipeline.commands[0].command;
  auto iter = find(kSupportedBuiltins, kSupportedBuiltins + kNumSupportedBuiltins, command);
  if (iter == kSupportedBuiltins + kNumSupportedBuiltins) return false;
  size_t index = iter - kSupportedBuiltins;

  switch (index) {
  case 0:
  case 1: exit(0);
  case 7: cout << joblist; break;
  default: throw STSHException("Internal Error: Builtin command not supported."); // or not implemented yet
  }
  
  return true;
}

/**
 * Function: installSignalHandlers
 * -------------------------------
 * Installs user-defined signals handlers for four signals
 * (once you've implemented signal handlers for SIGCHLD, 
 * SIGINT, and SIGTSTP, you'll add more installSignalHandler calls) and 
 * ignores two others.
 */
static void installSignalHandlers() {
  installSignalHandler(SIGQUIT, [](int sig) { exit(0); });
  installSignalHandler(SIGTTIN, SIG_IGN);
  installSignalHandler(SIGTTOU, SIG_IGN);
//  signal(SIGCHLD, reapForegroundProcess);
}

static void reapForegroundProcess(int sig){
 if(joblist.hasForegroundJob()){
    STSHJob fjob = joblist.getForegroundJob();
    std::vector<STSHProcess> &processes = fjob.getProcesses();
    for(size_t i = 0; i < processes.size(); i++){
      STSHProcess &process = processes[i];
      int status;
      int pid = waitpid(-1, &status, WNOHANG);
      if(pid < 0) continue;
      if(WIFEXITED(status)) process.setState(kTerminated);
      else if(WIFSTOPPED(status)) process.setState(kStopped);
    }
    joblist.synchronize(fjob);
  }
}
  
      



/**
 * Function: createJob
 * -------------------
 * Creates a new job on behalf of the provided pipeline.
 */
static void createJob(const pipeline& p) {
//  cout << p; // remove this line once you get started
//  /* STSHJob& job = */ joblist.addJob(kForeground);

  STSHJob& job = joblist.addJob(p.background ? kBackground : kForeground);
  for(command cmd : p.commands){
//    cout << "cmd:" << endl << cmd.command  << cmd.tokens << endl;
    int pid = fork();
    job.addProcess(STSHProcess(pid, cmd));
    if(pid == 0){
      setpgid(0, 0);
      char* argv[kMaxArguments + 2] = {NULL};
      argv[0] = const_cast<char*>(cmd.command);
      for(size_t i = 0; i < kMaxArguments + 1 && cmd.tokens[i] != NULL; i++)
	argv[i + 1] = cmd.tokens[i];
      int err = execvp(argv[0], argv);
      if(err < 0) throw STSHException("Command not found");
      
    }
  }
   sigset_t additions, existingmask;
   sigemptyset(&additions);
   sigaddset(&additions, SIGCHLD);
   sigaddset(&additions, SIGCHLD);
   sigaddset(&additions, SIGINT);
   sigaddset(&additions, SIGTSTP);
   sigaddset(&additions, SIGCONT);
  	 sigprocmask(SIG_BLOCK, &additions, &existingmask);
 	 while(joblist.hasForegroundJob() && joblist.getForegroundJob().getNum() == job.getNum())
         sigsuspend(&existingmask);
        
        sigprocmask(SIG_UNBLOCK, &additions, &existingmask);
   

  
}

/**
 * Function: main
 * --------------
 * Defines the entry point for a process running stsh.
 * The main function is little more than a read-eval-print
 * loop (i.e. a repl).  
 */
int main(int argc, char *argv[]) {
  pid_t stshpid = getpid();
  installSignalHandlers();
  rlinit(argc, argv);
  while (true) {
    string line;
    if (!readline(line)) break;
    if (line.empty()) continue;
    try {
      pipeline p(line);
      bool builtin = handleBuiltin(p);
      if (!builtin) createJob(p);
    } catch (const STSHException& e) {
      cerr << e.what() << endl;
      if (getpid() != stshpid) exit(0); // if exception is thrown from child process, kill it
    }
  }

  return 0;
}
