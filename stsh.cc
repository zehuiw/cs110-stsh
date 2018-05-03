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
#include <assert.h>
using namespace std;

static STSHJobList joblist; // the one piece of global data we need so signal handlers can access it
static void changeProcessStatus(pid_t pid, STSHJobState stat);
static void sigIntStopHandler(int sig);
static void sigchildHandler(int sig);
static void builtinFg(const pipeline& pipeline);
static void builtinSignals(const pipeline& pipeline, const string cmdName, int sig);
static void builtinBg(const pipeline& pipeline);
static void transferTerminalControl(pid_t pgid);
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
  case 2: builtinFg(pipeline); break; 
  case 3: builtinBg(pipeline); break;
  case 4: builtinSignals(pipeline, "slay", SIGINT); break;
  case 5: builtinSignals(pipeline, "halt", SIGTSTP); break;
  case 6: builtinSignals(pipeline, "cont", SIGCONT); break;
  case 7: cout << joblist; break;
  default: throw STSHException("Internal Error: Builtin command not supported."); // or not implemented yet
  }
  
  return true;
}

static void builtinFg(const pipeline& pipeline){
  char* arg = pipeline.commands[0].tokens[0];
  if(arg == NULL) throw STSHException("Usage: fg <jobid>.");
  if(strcmp(arg, "0") == 0) throw STSHException("fg 0: No such job.");
  int jobid = atoi(arg);
  if(jobid == 0) throw STSHException("Usage: fg <jobid>.");
  if(!joblist.containsJob(jobid)) {
    cout << "jobid: " << jobid << endl;    
    throw STSHException("fg " + to_string(jobid) + ": No such job.");
  }

  sigset_t additions, existingmask;
  sigemptyset(&additions);
  sigaddset(&additions, SIGINT);
  sigaddset(&additions, SIGTSTP);
  sigaddset(&additions, SIGCONT);
  sigaddset(&additions, SIGCHLD);
  sigprocmask(SIG_BLOCK, &additions, &existingmask);
  STSHJob& job = joblist.getJob(jobid);
  std::vector<STSHProcess> &processes = job.getProcesses();

  
  if(job.getState() == kForeground){ //foreground: continue
    for(STSHProcess proc : processes)
      kill(proc.getID(), SIGCONT);
  } else{//background: bring to foreground
    if(processes.size() > 0){
      job.setState(kForeground);
      pid_t gid = processes[0].getID();
      kill(-gid, SIGCONT); 
    }  
  }
  while(joblist.hasForegroundJob()) sigsuspend(&existingmask);
  sigprocmask(SIG_UNBLOCK, &additions, NULL);
}

static void builtinBg(const pipeline& pipeline){
  char* arg = pipeline.commands[0].tokens[0];
  if(arg == NULL) throw STSHException("Usage: bg <jobid>.");
  if(strcmp(arg, "0") == 0) throw STSHException("fg 0: No such job.");
  int jobid = atoi(arg);
  if(jobid == 0) throw STSHException("Usage: fg <jobid>.");
  if(!joblist.containsJob(jobid)) {
    cout << "jobid: " << jobid << endl;    
    throw STSHException("fg " + to_string(jobid) + ": No such job.");
  }
  STSHJob &job = joblist.getJob(jobid);
  std::vector<STSHProcess> &processes = job.getProcesses();
  for(STSHProcess proc : processes)
    kill(proc.getID(), SIGCONT);
}

static void builtinSignals(const pipeline& p, const string cmdName, int sig){
  char* arg1 = p.commands[0].tokens[0];
  if(arg1 == NULL) throw STSHException("Usage: " + cmdName + " <jobid> <index> | <pid>.");
  char* arg2 = p.commands[0].tokens[1];
  int arg1_int = atoi(arg1);
  if(arg2 == NULL){
    if(!joblist.containsProcess(arg1_int)) throw STSHException("No process with pid " + to_string(arg1_int));
    kill(arg1_int, sig);
  } else{
    int arg2_int = atoi(arg2);
    if(!joblist.containsJob(arg1_int)) throw STSHException("No job with id " + to_string(arg1_int));
    STSHJob& job = joblist.getJob(arg1_int);
    vector<STSHProcess>& processes = job.getProcesses();
    if(arg2_int >= processes.size()) throw STSHException("Job " + to_string(arg1_int) + "doesn't have a pid at index " + to_string(arg2_int));
    STSHProcess proc = processes[arg2_int];
    if(!job.containsProcess(proc.getID())) throw STSHException("Job " + to_string(arg1_int) + "doesn't have a pid at index " + to_string(arg2_int));
    kill(proc.getID(), sig);
  }
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
  installSignalHandler(SIGCHLD, sigchildHandler);
  installSignalHandler(SIGINT, sigIntStopHandler);
  installSignalHandler(SIGTSTP, sigIntStopHandler);
}

static void changeProcessStatus(pid_t pid, STSHProcessState stat){
  STSHJob &job = joblist.getJobWithProcess(pid);
  assert(job.containsProcess(pid));
  STSHProcess& proc = job.getProcess(pid);
  proc.setState(stat);
  joblist.synchronize(job);
}
static void sigIntStopHandler(int sig){
  if(joblist.hasForegroundJob()){
    STSHJob& job = joblist.getForegroundJob();
    vector<STSHProcess>& processes = job.getProcesses();
    for(STSHProcess proc : processes)
      kill(proc.getID(), sig);
  }
}

static void sigchildHandler(int sig){
  while(true){
    int status;
    pid_t pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
    if(pid <= 0) break;
    if(WIFEXITED(status) | WIFSIGNALED(status)) changeProcessStatus(pid, kTerminated);
    if(WIFSTOPPED(status)) changeProcessStatus(pid, kStopped);
    if(WIFCONTINUED(status)) changeProcessStatus(pid, kRunning);   
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
  pid_t groupid = 0;
  int fds[p.commands.size() - 1][2];
  for(size_t i = 0; i < p.commands.size() - 1; i++) pipe(fds[i]);
  int inputfd = -1, outputfd = -1;
  if(!p.input.empty()) inputfd = open(p.input.c_str(), O_RDONLY);
  if(!p.output.empty()) outputfd = open(p.output.c_str(), O_CREAT | O_TRUNC, 0644);

  for(size_t i = 0; i < p.commands.size(); i++){
    pid_t pid = fork();
    if(i == 0) groupid = pid;
    job.addProcess(STSHProcess(pid, p.commands[i]));
    //child process
    if(pid == 0){
      if(i == 0 && inputfd >= 0){
          dup2(inputfd, STDIN_FILENO);
          close(inputfd);
      }else if(i > 0){
        dup2(fds[i - 1][0], STDIN_FILENO);
        close(fds[i - 1][0]);
      }
      if(i == p.commands.size() - 1 && outputfd >= 0){
        dup2(outputfd, STDOUT_FILENO);
        close(outputfd);
      }else if(i != p.commands.size() - 1){
        dup2(fds[i][1], STDOUT_FILENO);
        close(fds[i][1]);
      }

      for(int t = 0; t < t.commands.size() - 1; t++){
        close(fds[i][0]); close(fds[i][1]);
      }
      setpgid(getpid(), groupid);
      char* argv[kMaxArguments + 2] = {NULL};
      argv[0] = const_cast<char*>(p.commands[i].command);
      for(size_t j = 0; j < kMaxArguments + 1 && p.commands[i].tokens[j] != NULL; j++) argv[j + 1] = p.commands[i].tokens[j];
      int err = execvp(argv[0], argv);
      if(err < 0) throw STSHException("./" + std::string(argv[0]) + ": command not found");      
    }
  }
  for(int t = 0; t < t.commands.size() - 1; t++)
        close(fds[i][0]); close(fds[i][1]);
  if(!p.background){
   sigset_t additions, existingmask;
   sigemptyset(&additions);
   sigaddset(&additions, SIGCHLD);
   sigaddset(&additions, SIGCHLD);
   sigaddset(&additions, SIGINT);
   sigaddset(&additions, SIGTSTP);
   sigaddset(&additions, SIGCONT);
   sigprocmask(SIG_BLOCK, &additions, &existingmask);

   if(joblist.hasForegroundJob()){
    transferTerminalControl(groupid);
   }

 	 while(joblist.hasForegroundJob() && joblist.getForegroundJob().getNum() == job.getNum())
     sigsuspend(&existingmask);        
  sigprocmask(SIG_UNBLOCK, &additions, &existingmask);
  } 
  else{ // background job
    cout << "[" << job.getNum() << "] ";
    vector<STSHProcess>& processes = job.getProcesses();
    for(size_t i = 0; i < processes.size(); i++)
      cout << processes[i].getID() << " ";
    cout << endl;
  }

  //give shell back to parent
  transferTerminalControl(getpgid(getpid()));
}

static void transferTerminalControl(pid_t pgid){
  int err = tcsetpgrp(STDIN_FILENO, pgid);
  if(err == -1 && errno != ENOTTY){
    throw STSHException("tcsetpgrp: A serious problem happens");
  }  
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
